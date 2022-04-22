/******************************************************************************
 * Copyright (c) 2011-2021, NVIDIA CORPORATION.  All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the NVIDIA CORPORATION nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL NVIDIA CORPORATION BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 ******************************************************************************/

#include "fmha.h"
#include <cstring>
#include <string>
#include <exception>
#include <stdexcept>
#include <mutex>
#include <memory>
#include "cuda.h"
#include "cuda_runtime.h"
#include "math.h"
#include "fmhalib.h"
#include "dlfcn.h"

using SeedIncFuncPtr = void (*)(uint64_t, uint64_t *, const int64_t **, uint64_t*, bool*);

#define ASSERT_CHECK(__cond)                             \
      do {                                               \
        const bool __cond_var = (__cond);                \
        if (!__cond_var) {                               \
          ::std::string __err_msg = ::std::string("`") + \
                #__cond + "` check failed at " +         \
		__FILE__ + ":" +                         \
		::std::to_string(__LINE__);              \
          throw std::runtime_error(__err_msg);           \
        }                                                \
      } while (0)


thread_local std::unique_ptr<char[]> fmhalib_err_msg;  

#ifdef __cplusplus
extern "C" {
#endif

const char *fmhalib_version() {
   return "0.1";
}

static void fmhalib_set_error(const char *msg) {
  if (msg == nullptr || *msg == '\0') {
    msg = "unknown error";
  }

  auto n = strlen(msg);
  std::unique_ptr<char[]> new_err_msg(new char[n+1]);
  std::strcpy(new_err_msg.get(), msg);
  fmhalib_err_msg = std::move(new_err_msg);
}

const char *fmhalib_error() {
  return fmhalib_err_msg.get();
}

#define FMHALIB_BEGIN_FUNC try {
#define FMHALIB_END_FUNC } catch (::std::exception &__e) { fmhalib_set_error(__e.what()); } catch (...) { fmhalib_set_error(nullptr); }

static void set_params(Fused_multihead_attention_fprop_params &params,
                       // sizes
                       const size_t b,
                       const size_t s,
                       const size_t h,
                       const size_t d,
                       // device pointers
                       void *qkv_packed_d,
                       void *cu_seqlens_d,
                       void *o_packed_d,
                       void *s_d,
                       float p_dropout) {

    Data_type acc_type = DATA_TYPE_FP32;
    Data_type data_type = DATA_TYPE_FP16;

    // Reset the parameters
    memset(&params, 0, sizeof(params));

    // Set the pointers and strides.
    params.qkv_ptr = qkv_packed_d;
    params.qkv_stride_in_bytes = get_size_in_bytes(h * 3 * d, data_type);
    params.o_ptr = o_packed_d;
    params.o_stride_in_bytes = get_size_in_bytes(h * d, data_type);

    params.cu_seqlens = static_cast<int *>(cu_seqlens_d);

    // S = softmax(P)
    params.s_ptr = s_d;
    params.s_stride_in_bytes = get_size_in_bytes(b * h * s, data_type);

    // Set the dimensions.
    params.b = b;
    params.h = h;
    params.s = s;
    params.d = d;

    // Set the different scale values.
    const float scale_bmm1 = 1.f / sqrtf(d);
    constexpr float scale_softmax = 1.f;
    constexpr float scale_bmm2 = 1.f;

    set_alpha(params.scale_bmm1, scale_bmm1, data_type);
    set_alpha(params.scale_softmax, scale_softmax, acc_type);
    set_alpha(params.scale_bmm2, scale_bmm2, data_type);

    // Set this to probability of keeping an element to simplify things.
    params.p_dropout = 1.f - p_dropout;
    params.rp_dropout = 1.f / params.p_dropout;
    ASSERT_CHECK(p_dropout < 1.f);
    set_alpha(params.scale_dropout, params.rp_dropout, data_type);
}

static int fmhalib_bwd_nl_num_chunks(const int batch_size) {
    int num_chunks = 2;
    if( batch_size == 1 ) {
        num_chunks = 4;
    }else if( batch_size == 2 ) {
        num_chunks = 3;
    }
    return num_chunks;
}

int fmhalib_seq_len(const int max_seq_len) {
    if (max_seq_len < 0) {
      return -1;
    }

    const int seq_lens[] = {128, 256, 384, 512};
    constexpr int n = sizeof(seq_lens) / sizeof(seq_lens[0]);
#pragma unroll n
    for (int i = 0; i < n; ++i) {
      if (max_seq_len <= seq_lens[i]) {
	return seq_lens[i];
      }
    }
    return -1;
}

static void SetPhiloxCudaState(at::PhiloxCudaState *state, SeedIncFuncPtr seed_inc_func, uint64_t increment) {
    uint64_t rnd_seed;
    const int64_t *offset_ptr;
    uint64_t rnd_offset;
    bool is_device_rnd;
    seed_inc_func(increment, &rnd_seed, &offset_ptr, &rnd_offset, &is_device_rnd);
    if (is_device_rnd) {
        *state = at::PhiloxCudaState(rnd_seed, const_cast<int64_t *>(offset_ptr), static_cast<uint32_t>(rnd_offset));
    } else {
        *state = at::PhiloxCudaState(rnd_seed, rnd_offset);
    }
}

static cudaDeviceProp g_prop;

static cudaDeviceProp *GetCurrentDeviceProperties() {
    static std::once_flag flag;   
    std::call_once(flag, [] {
      int dev_id;
      ASSERT_CHECK(cudaGetDevice(&dev_id) == cudaSuccess);
      ASSERT_CHECK(cudaGetDeviceProperties(&g_prop, dev_id) == cudaSuccess);
    });
    return &g_prop;
}   

static void SetZero(void *ptr, size_t sizeof_type, std::initializer_list<int> shapes, cudaStream_t stream) {
    size_t n = sizeof_type;
    for (int s : shapes) n *= s;
    ASSERT_CHECK(cudaMemsetAsync(ptr, 0, n, stream) == cudaSuccess); 
}

int seq_len_round(int real_seq_len) {
   if (real_seq_len > 384) {
     return 512;
   } else if (real_seq_len > 128) {
     return 384; 
   } else if (real_seq_len > 0) {
     return 128; 
   } else {
     ASSERT_CHECK(false);
   }
}

void fmhalib_fwd(const void *qkv_ptr,
                 const void *cu_seqlens_ptr,
                 const void *host_cu_seqlens_ptr,
                 const int total,
                 const int num_heads,
                 const int head_size,
                 const int batch_size,
                 const float p_dropout,
                 const int max_seq_len,
                 const bool is_training,
                 const bool is_nl,
                 const bool zero_tensors,
		 SeedIncFuncPtr seed_inc_func,
                 cudaStream_t stream,
                 void *ctx_ptr, // {total, num_heads, head_size}
                 void *s_ptr) { // {batch_size, num_heads, seq_len, seq_len}
    FMHALIB_BEGIN_FUNC
    auto dprops = GetCurrentDeviceProperties(); 
    ASSERT_CHECK(dprops->major == 8 && dprops->minor == 0);
    
    /*
     *[3, 3, 2, 1]
     *[0, 3, 6, 8, 9]
     *seq_len_group_idx: [0, 6, 8, 9]
     * 
     * */
    const int group_size = 3;
    std::vector<int> seq_len_per_sample(batch_size); 
    std::vector<int> seq_len_group_idx(group_size);
    std::vector<int> group_len(group_size);
    
    for (int i = 0; i < group_size; i++) {
      seq_len_group_idx[i] = static_cast<const int*>(host_cu_seqlens_ptr)[batch_size];
      group_len[i] = 0; 
    }

    int cur_group = 0;
    int cur_group_len = 1;
    int cur_idx = 0;
    seq_len_group_idx[cur_idx++] = 0; 
    for (int i = 0; i < batch_size; i++) {
      seq_len_per_sample[i] = static_cast<const int*>(host_cu_seqlens_ptr)[i + 1] - static_cast<const int*>(host_cu_seqlens_ptr)[i];
      // round so as the elements in array is among [512, 384, 128].
      seq_len_per_sample[i] = seq_len_round(seq_len_per_sample[i]);
      if (i > 0) {
        if (seq_len_per_sample[i] != seq_len_per_sample[i - 1]) {
	  seq_len_group_idx[cur_idx++] = static_cast<const int*>(host_cu_seqlens_ptr)[i];
          group_len[cur_group++] = cur_group_len;
          cur_group_len = 1;	
	} else {
          cur_group_len += 1; 
	} 
      }
    }
    seq_len_group_idx[cur_idx] = static_cast<const int*>(host_cu_seqlens_ptr)[batch_size];
    group_len[cur_group] = cur_group_len;

#if 1 
    printf("batch_size = %d\n", batch_size); 
    printf("group_size = %d, %d, %d\n", group_len[0], group_len[1], group_len[2]);
#endif
    if (group_len[0] + group_len[1] + group_len[2] != batch_size) {
        ASSERT_CHECK(false);
    }
#if 1 
    //for (int i = 0; i < batch_size; i++) {
    //  printf("seq_len_per_sample[%d] = %d\n", i, seq_len_per_sample[i]);
    //}
    for (int i = 0; i < group_size; i++) {
      printf("seq_len_group_idx[%d] = %d\n", i, seq_len_group_idx[i]);
    }
#endif

    Launch_params<Fused_multihead_attention_fprop_params> launch_params_128(dprops, stream, is_training, is_nl);
    Launch_params<Fused_multihead_attention_fprop_params> launch_params_384(dprops, stream, is_training, is_nl);
    Launch_params<Fused_multihead_attention_fprop_params> launch_params_512(dprops, stream, is_training, is_nl);
    
    int seq_len_512 = 512;
    int seq_len_384 = 384;
    int seq_len_128 = 128;
    auto launch_512 = &run_fmha_fp16_512_64_sm80;
    auto launch_384 = &run_fmha_fp16_384_64_sm80;
    auto launch_128 = &run_fmha_fp16_128_64_sm80;
    
    ASSERT_CHECK(batch_size > 0);
    ASSERT_CHECK(head_size == 64);

    if( zero_tensors ) {
        SetZero(ctx_ptr, 2, {total, num_heads, head_size}, stream);  
        // SetZero(s_ptr, 2, {batch_size, num_heads, seq_len, seq_len}, stream);
        SetZero(s_ptr, 2, {batch_size, num_heads, 512, 512}, stream);
    }
    
    set_params(launch_params_512.params,
               group_len[0], // batch_size,
               seq_len_512,
               num_heads,
               head_size,
               const_cast<void*>(qkv_ptr),
               const_cast<void*>(cu_seqlens_ptr),
               ctx_ptr,
               s_ptr,
               p_dropout);
#if 1 
    if (group_len[1] > 0) {
      int qkv_offset = seq_len_group_idx[1] * head_size * num_heads * 3;
      const __half* new_qkv_ptr = static_cast<const __half*>(qkv_ptr) + qkv_offset;
      const int* new_cu_seqlens_ptr = static_cast<const int*>(cu_seqlens_ptr) + group_len[0];
      // {total, num_heads, head_size}
      __half* new_ctx_ptr = static_cast<__half*>(ctx_ptr) + seq_len_group_idx[1] * num_heads * head_size;
      // batch_size, num_heads, seq_len, seq_len
      __half* new_s_ptr = static_cast<__half*>(s_ptr) + group_len[0] * num_heads * 512 * 512; 
      set_params(launch_params_384.params,
               group_len[1], // batch_size,
               seq_len_384,
               num_heads,
               head_size,
               const_cast<void*>(static_cast<const void*>(new_qkv_ptr)),
               const_cast<void*>(static_cast<const void*>(new_cu_seqlens_ptr)),
               static_cast<void*>(new_ctx_ptr),
               static_cast<void*>(new_s_ptr),
               p_dropout);
    }
    if (group_len[2] > 0) {
      int qkv_offset = seq_len_group_idx[2] * head_size * num_heads * 3;
      const __half* new_qkv_ptr = static_cast<const __half*>(qkv_ptr) + qkv_offset;
      const int* new_cu_seqlens_ptr = static_cast<const int*>(cu_seqlens_ptr) + group_len[0] + group_len[1]; 
      __half* new_ctx_ptr = static_cast<__half*>(ctx_ptr) + seq_len_group_idx[2] * num_heads * head_size;
      __half* new_s_ptr = static_cast<__half*>(s_ptr) + (group_len[0] + group_len[1]) * num_heads * 512 * 512; 
      set_params(launch_params_128.params,
               group_len[2], // batch_size,
               seq_len_128,
               num_heads,
               head_size,
               const_cast<void*>(static_cast<const void*>(new_qkv_ptr)),
               const_cast<void*>(static_cast<const void*>(new_cu_seqlens_ptr)),
               static_cast<void*>(new_ctx_ptr),
               static_cast<void*>(new_s_ptr),
               p_dropout);
    }
#endif
    launch_512(launch_params_512, /*configure=*/ true);
    
    if (group_len[1] > 0) {
      launch_384(launch_params_384, /*configure=*/ true);
    }
    if (group_len[2] > 0) {
      launch_128(launch_params_128, /*configure=*/ true);
    }

    if ( is_training ) {
        int64_t counter_offset = launch_params_512.elts_per_thread;
        SetPhiloxCudaState(&launch_params_512.params.philox_args, seed_inc_func, counter_offset);
	if (group_len[1] > 0) {
          int64_t counter_offset = launch_params_384.elts_per_thread;
          SetPhiloxCudaState(&launch_params_384.params.philox_args, seed_inc_func, counter_offset);
	}	
	if (group_len[2] > 0) {
          int64_t counter_offset = launch_params_128.elts_per_thread;
          SetPhiloxCudaState(&launch_params_128.params.philox_args, seed_inc_func, counter_offset);
	}	
    }
    
    launch_512(launch_params_512, /*configure=*/ false);
    if (group_len[1] > 0) {
      launch_384(launch_params_384, /*configure=*/ false);
    }
    if (group_len[2] > 0) {
      launch_128(launch_params_128, /*configure=*/ false);
    }
    FMHALIB_END_FUNC
}

static void fmhalib_bwd_nl(const void *dout_ptr,
                           const void *qkv_ptr,
                           const void *cu_seqlens_ptr,
                           const int total,
                           const int num_heads,
                           const int head_size,
                           const int batch_size,
                           const float p_dropout,         // probability to drop
                           const int max_seq_len,         // max sequence length to choose the kernel
                           const bool zero_tensors,
                           cudaStream_t stream,
                           void *softmax_ptr,             // will be overwritten
                           void *dqkv_ptr,
                           void *dkv_ptr) {
    FMHALIB_BEGIN_FUNC
    ASSERT_CHECK(batch_size > 0);
    ASSERT_CHECK(head_size == 64);

    int seq_len = 512;
    auto launch = &run_fmha_dgrad_fp16_512_64_sm80_nl;

    if( zero_tensors ) {
        SetZero(dqkv_ptr, 2, {total, num_heads, 3, head_size}, stream); 
    }

    Fused_multihead_attention_fprop_params params;

    set_params(params,
               batch_size,
               seq_len,
               num_heads,
               head_size,
               const_cast<void*>(qkv_ptr),
               const_cast<void*>(cu_seqlens_ptr),
               const_cast<void*>(dout_ptr),     // o_ptr = dout
               softmax_ptr,  // softmax gets overwritten by dP!
               p_dropout);

    params.dkv_ptr = dkv_ptr;

    Data_type acc_type = DATA_TYPE_FP32;
    set_alpha(params.scale_bmm1, 1.f, acc_type);
    set_alpha(params.scale_softmax, 1.f / sqrtf(head_size), acc_type);
    set_alpha(params.scale_bmm2, 1.f, DATA_TYPE_FP16);
    params.dqkv_ptr = dqkv_ptr;

    auto num_chunks = fmhalib_bwd_nl_num_chunks(batch_size);
    launch(params, num_chunks, stream);

    //SPLIT-K reduction of num_chunks dK, dV parts

    // The equivalent of the following Pytorch code:
    // using namespace torch::indexing;
    // at::Tensor view_out = dqkv.index({Slice(), Slice(1, None, None)});
    // torch::sum_out(view_out, dkv, 1);

    const int hidden_size = num_heads * head_size;
    fmha_run_noloop_reduce(
        dqkv_ptr, dkv_ptr, static_cast<const int *>(cu_seqlens_ptr), hidden_size, batch_size, total, num_chunks, stream);
    FMHALIB_END_FUNC
}


void fmhalib_bwd(const void *dout_ptr,
                 const void *qkv_ptr,
                 const void *cu_seqlens_ptr,
                 const void *host_cu_seqlens_ptr,
                 const int total,
                 const int num_heads,
                 const int head_size,
                 const int batch_size,
                 const float p_dropout,
                 const int max_seq_len,
                 const bool is_nl,
                 const bool zero_tensors,
                 cudaStream_t stream,
                 void *softmax_ptr,  // will be overwritten
                 void *dqkv_ptr,
                 void *workspace,
                 uint64_t *workspace_size) {
    if (dqkv_ptr == nullptr) {
        if (is_nl) {
            int num_chunks = fmhalib_bwd_nl_num_chunks(batch_size); 
            *workspace_size = static_cast<uint64_t>(total) * num_chunks * 2 * num_heads * head_size * 2;  
        } else {
            *workspace_size = 0;
        }
        return;
    }

    if (is_nl) {
        fmhalib_bwd_nl(dout_ptr, qkv_ptr, cu_seqlens_ptr, total, num_heads, head_size,
            batch_size, p_dropout, max_seq_len, zero_tensors, stream, softmax_ptr,
            dqkv_ptr, workspace);
        return;
    }

    FMHALIB_BEGIN_FUNC
    auto dprops = GetCurrentDeviceProperties();
    ASSERT_CHECK(dprops->major == 8 && dprops->minor == 0);
    
    const int group_size = 3;
    std::vector<int> seq_len_per_sample(batch_size); 
    std::vector<int> seq_len_group_idx(group_size);
    std::vector<int> group_len(group_size);
    
    for (int i = 0; i < group_size; i++) {
      seq_len_group_idx[i] = static_cast<const int*>(host_cu_seqlens_ptr)[batch_size];
      group_len[i] = 0; 
    }

    int cur_group = 0;
    int cur_group_len = 1;
    int cur_idx = 0;
    seq_len_group_idx[cur_idx++] = 0; 
    for (int i = 0; i < batch_size; i++) {
      seq_len_per_sample[i] = static_cast<const int*>(host_cu_seqlens_ptr)[i + 1] - static_cast<const int*>(host_cu_seqlens_ptr)[i];
      // round so as the elements in array is among [512, 384, 128].
      seq_len_per_sample[i] = seq_len_round(seq_len_per_sample[i]);
      if (i > 0) {
        if (seq_len_per_sample[i] != seq_len_per_sample[i - 1]) {
	  seq_len_group_idx[cur_idx++] = static_cast<const int*>(host_cu_seqlens_ptr)[i];
          group_len[cur_group++] = cur_group_len;
          cur_group_len = 1;	
	} else {
          cur_group_len += 1; 
	} 
      }
    }
    seq_len_group_idx[cur_idx] = static_cast<const int*>(host_cu_seqlens_ptr)[batch_size];
    group_len[cur_group] = cur_group_len;
    
    printf("limin: begin backward\n");
#if 1
    printf("batch_size = %d\n", batch_size); 
    printf("group_size = %d, %d, %d\n", group_len[0], group_len[1], group_len[2]);
#endif
    if (group_len[0] + group_len[1] + group_len[2] != batch_size) {
        ASSERT_CHECK(false);
    }

    int seq_len_512 = 512;
    int seq_len_384 = 384;
    int seq_len_128 = 128;
    auto launch_512 = &run_fmha_dgrad_fp16_512_64_sm80;
    auto launch_384 = &run_fmha_dgrad_fp16_384_64_sm80;
    auto launch_128 = &run_fmha_dgrad_fp16_128_64_sm80;
    
    ASSERT_CHECK(batch_size > 0);
    ASSERT_CHECK(head_size == 64);

    if( zero_tensors ) {
        SetZero(dqkv_ptr, 2, {total, num_heads, 3, head_size}, stream);
    }

    Fused_multihead_attention_fprop_params params;
    Fused_multihead_attention_fprop_params params_384;
    Fused_multihead_attention_fprop_params params_128;

    set_params(params,
               group_len[0], //batch_size,
               seq_len_512, // seq_len,
               num_heads,
               head_size,
               const_cast<void*>(qkv_ptr),
               const_cast<void*>(cu_seqlens_ptr),
               const_cast<void*>(dout_ptr),     // we set o_ptr to dout
               softmax_ptr,  // softmax gets overwritten by dP!
               p_dropout);
#if 1 
    int qkv_offset = seq_len_group_idx[1] * head_size * num_heads * 3;
    const __half* new_qkv_ptr = static_cast<const __half*>(qkv_ptr) + qkv_offset;
    const int* new_cu_seqlens_ptr = static_cast<const int*>(cu_seqlens_ptr) + group_len[0];
    const __half* new_dout_ptr = static_cast<const __half*>(dout_ptr) + qkv_offset;
    // limin-todo: 
    __half* new_softmax_ptr = static_cast<__half*>(softmax_ptr) + group_len[0] * num_heads * 512 * 512; 
    if (group_len[1] > 0) {
      set_params(params_384,
               group_len[1], // batch_size,
               seq_len_384, // seq_len,
               num_heads,
               head_size,
               const_cast<void*>(static_cast<const void*>(new_qkv_ptr)),
               const_cast<void*>(static_cast<const void*>(new_cu_seqlens_ptr)),
               const_cast<void*>(static_cast<const void*>(new_dout_ptr)),     // we set o_ptr to dout
               static_cast<void*>(new_softmax_ptr),  // softmax gets overwritten by dP!
               p_dropout);
    }
    int qkv_offset_2 = seq_len_group_idx[2] * head_size * num_heads * 3;
    const __half* new_qkv_ptr_2 = static_cast<const __half*>(qkv_ptr) + qkv_offset_2;
    const int* new_cu_seqlens_ptr_2 = static_cast<const int*>(cu_seqlens_ptr) + group_len[0] + group_len[1];
    const __half* new_dout_ptr_2 = static_cast<const __half*>(dout_ptr) + qkv_offset_2;
    // {batch_size, num_heads, seq_len, seq_len}
    __half* new_softmax_ptr_2 = static_cast<__half*>(softmax_ptr) + (group_len[0] + group_len[1]) * num_heads * 512 * 512; 
    if (group_len[2] > 0) {
      set_params(params_128,
               group_len[2], // batch_size,
               seq_len_128, // seq_len,
               num_heads,
               head_size,
               const_cast<void*>(static_cast<const void*>(new_qkv_ptr_2)),
               const_cast<void*>(static_cast<const void*>(new_cu_seqlens_ptr_2)),
               const_cast<void*>(static_cast<const void*>(new_dout_ptr_2)),     // we set o_ptr to dout
	       // limin-todo: 
               static_cast<void*>(new_softmax_ptr_2),  // softmax gets overwritten by dP!
               p_dropout);
    }
#endif

    // we're re-using these scales
    Data_type acc_type = DATA_TYPE_FP32;
    // limin-todo: 
    set_alpha(params.scale_bmm1, 1.f, acc_type);
    set_alpha(params.scale_softmax, 1.f / sqrtf(head_size), acc_type);
    set_alpha(params.scale_bmm2, 1.f, DATA_TYPE_FP16);
#if 1 
    set_alpha(params_384.scale_bmm1, 1.f, acc_type);
    set_alpha(params_384.scale_softmax, 1.f / sqrtf(head_size), acc_type);
    set_alpha(params_384.scale_bmm2, 1.f, DATA_TYPE_FP16);
    
    set_alpha(params_128.scale_bmm1, 1.f, acc_type);
    set_alpha(params_128.scale_softmax, 1.f / sqrtf(head_size), acc_type);
    set_alpha(params_128.scale_bmm2, 1.f, DATA_TYPE_FP16);
#endif

    // limin-todo: 
    params.dqkv_ptr = dqkv_ptr;
#if 1 
    params_384.dqkv_ptr = static_cast<void*>(static_cast<__half*>(dqkv_ptr) + qkv_offset);
    params_128.dqkv_ptr = static_cast<void*>(static_cast<__half*>(dqkv_ptr) + qkv_offset_2);
#endif
    launch_512(params, stream);
#if 1 
    if (group_len[1] > 0) {
      launch_384(params_384, stream);
    }
    if (group_len[2] > 0) {
      launch_128(params_128, stream);
    }
#endif
    FMHALIB_END_FUNC
}

#ifdef __cplusplus
}
#endif
