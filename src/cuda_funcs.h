// Every CUDA driver API function nvq calls, in NVIDIA's own type names. src/cuda.h declares those types
// for the build; test/layout.c includes this list after NVIDIA's real cuda.h and fails to compile if
// any signature here differs from the driver's.
#pragma once

#define CUDA_FUNCS(X)                                                                              \
    X(CUresult, cuInit, (unsigned int))                                                            \
    X(CUresult, cuDriverGetVersion, (int *))                                                       \
    X(CUresult, cuDeviceGetCount, (int *))                                                         \
    X(CUresult, cuDeviceGet, (CUdevice *, int))                                                    \
    X(CUresult, cuDeviceGetUuid_v2, (CUuuid *, CUdevice))                                          \
    X(CUresult, cuDeviceGetName, (char *, int, CUdevice))                                          \
    X(CUresult, cuDeviceGetAttribute, (int *, CUdevice_attribute, CUdevice))                       \
    X(CUresult, cuDeviceTotalMem_v2, (size_t *, CUdevice))                                         \
    X(CUresult, cuCtxCreate_v2, (CUcontext *, unsigned int, CUdevice))                             \
    X(CUresult, cuCtxDestroy_v2, (CUcontext))                                                      \
    X(CUresult, cuCtxSynchronize, (void))                                                          \
    X(CUresult, cuMemAlloc_v2, (CUdeviceptr *, size_t))                                            \
    X(CUresult, cuMemFree_v2, (CUdeviceptr))                                                       \
    X(CUresult, cuMemsetD32_v2, (CUdeviceptr, unsigned int, size_t))                               \
    X(CUresult, cuMemcpyDtoH_v2, (void *, CUdeviceptr, size_t))                                    \
    X(CUresult, cuModuleLoadData, (CUmodule *, const void *))                                      \
    X(CUresult, cuModuleGetFunction, (CUfunction *, CUmodule, const char *))                       \
    X(CUresult, cuModuleUnload, (CUmodule))                                                        \
    X(CUresult, cuLaunchKernel,                                                                    \
      (CUfunction, unsigned int, unsigned int, unsigned int, unsigned int, unsigned int,           \
       unsigned int, unsigned int, CUstream, void **, void **))                                    \
    X(CUresult, cuGetErrorName, (CUresult, const char **))
