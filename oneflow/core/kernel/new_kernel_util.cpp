/*
Copyright 2020 The OneFlow Authors. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/
#include "oneflow/core/kernel/new_kernel_util.h"
#include "oneflow/core/memory/memory_case.pb.h"
#include "oneflow/core/register/blob.h"

namespace oneflow {

template<>
void Memcpy<DeviceType::kCPU>(DeviceCtx* ctx, void* dst, const void* src, size_t sz,
                              cudaMemcpyKind kind

) {
  if (dst == src) { return; }
  memcpy(dst, src, sz);
}

template<>
void Memset<DeviceType::kCPU>(DeviceCtx* ctx, void* dst, const char value, size_t sz) {
  memset(dst, value, sz);
}

void WithHostBlobAndStreamSynchronizeEnv(DeviceCtx* ctx, Blob* blob,
                                         std::function<void(Blob*)> Callback) {
#ifdef WITH_CUDA
  char* host_raw_dptr = nullptr;
  CudaCheck(cudaMallocHost(&host_raw_dptr, blob->AlignedTotalByteSize()));
  Blob host_blob(MemoryCase(), &blob->blob_desc(), host_raw_dptr);
  Callback(&host_blob);
  Memcpy<DeviceType::kGPU>(ctx, blob->mut_dptr(), host_blob.dptr(), blob->ByteSizeOfBlobBody(),
                           cudaMemcpyHostToDevice);
  CudaCheck(cudaStreamSynchronize(ctx->cuda_stream()));
  CudaCheck(cudaFreeHost(host_raw_dptr));
#else
  UNIMPLEMENTED();
#endif
}

}  // namespace oneflow
