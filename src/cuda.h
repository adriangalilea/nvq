// The slice of the CUDA driver API (libcuda.so.1) nvq's probe uses, loaded at run time like NVML.
#pragma once
#include <stddef.h>

typedef int CUresult;
typedef int CUdevice;
typedef struct CUctx_st *CUcontext;
typedef struct CUmod_st *CUmodule;
typedef struct CUfunc_st *CUfunction;
typedef struct CUstream_st *CUstream;
typedef unsigned long long CUdeviceptr;
typedef struct { char bytes[16]; } CUuuid;

// CUdevice_attribute values nvq reads (identical in cuda.h 12.4 through 13.1; test/layout.c asserts).
enum {
    ATTR_MAX_THREADS_PER_BLOCK = 1,
    ATTR_MAX_BLOCK_DIM_X = 2,
    ATTR_MAX_BLOCK_DIM_Y = 3,
    ATTR_MAX_BLOCK_DIM_Z = 4,
    ATTR_MAX_GRID_DIM_X = 5,
    ATTR_MAX_GRID_DIM_Y = 6,
    ATTR_MAX_GRID_DIM_Z = 7,
    ATTR_MAX_SHARED_MEMORY_PER_BLOCK = 8,
    ATTR_TOTAL_CONSTANT_MEMORY = 9,
    ATTR_WARP_SIZE = 10,
    ATTR_MAX_REGISTERS_PER_BLOCK = 12,
    ATTR_CLOCK_RATE = 13, // kHz
    ATTR_MULTIPROCESSOR_COUNT = 16,
    ATTR_INTEGRATED = 18,
    ATTR_COMPUTE_MODE = 20,
    ATTR_CONCURRENT_KERNELS = 31,
    ATTR_ECC_ENABLED = 32,
    ATTR_MEMORY_CLOCK_RATE = 36, // kHz
    ATTR_GLOBAL_MEMORY_BUS_WIDTH = 37, // bits
    ATTR_L2_CACHE_SIZE = 38,
    ATTR_MAX_THREADS_PER_MULTIPROCESSOR = 39,
    ATTR_ASYNC_ENGINE_COUNT = 40,
    ATTR_COMPUTE_CAPABILITY_MAJOR = 75,
    ATTR_COMPUTE_CAPABILITY_MINOR = 76,
    ATTR_MAX_SHARED_MEMORY_PER_MULTIPROCESSOR = 81,
    ATTR_MAX_REGISTERS_PER_MULTIPROCESSOR = 82,
    ATTR_MULTI_GPU_BOARD = 84,
    ATTR_MAX_SHARED_MEMORY_PER_BLOCK_OPTIN = 97,
    ATTR_MAX_BLOCKS_PER_MULTIPROCESSOR = 106,
    ATTR_MAX_PERSISTING_L2_CACHE_SIZE = 108,
};

// The architecture family of a compute capability, the name people say.
static inline const char *cc_family(int major, int minor) {
    switch (major) {
    case 3: return "kepler";
    case 5: return "maxwell";
    case 6: return "pascal";
    case 7: return minor >= 5 ? "turing" : "volta";
    case 8: return minor == 9 ? "ada" : "ampere";
    case 9: return "hopper";
    case 10: case 11: case 12: return "blackwell";
    }
    return "unknown";
}

static inline const char *compute_mode(int m) {
    switch (m) {
    case 0: return "default";
    case 2: return "prohibited";
    case 3: return "exclusive_process";
    }
    return "unknown";
}

#define CUDA_FUNCS(X)                                                                              \
    X(CUresult, cuInit, (unsigned int))                                                            \
    X(CUresult, cuDriverGetVersion, (int *))                                                       \
    X(CUresult, cuDeviceGetCount, (int *))                                                         \
    X(CUresult, cuDeviceGet, (CUdevice *, int))                                                    \
    X(CUresult, cuDeviceGetUuid_v2, (CUuuid *, CUdevice))                                          \
    X(CUresult, cuDeviceGetName, (char *, int, CUdevice))                                          \
    X(CUresult, cuDeviceGetAttribute, (int *, int, CUdevice))                                      \
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
      (CUfunction, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, CUstream, \
       void **, void **))                                                                          \
    X(CUresult, cuGetErrorName, (CUresult, const char **))

// The probe kernel: every thread of one block writes value + its index. PTX for sm_50, so the driver
// JITs it for whatever card is there: the probe proves the JIT path a fat binary falls back to.
static const char probe_ptx[] =
    ".version 6.0\n"
    ".target sm_50\n"
    ".address_size 64\n"
    ".visible .entry nvq_probe(.param .u64 out, .param .u32 value)\n"
    "{\n"
    "  .reg .u32 %r<4>;\n"
    "  .reg .u64 %rd<4>;\n"
    "  ld.param.u64 %rd1, [out];\n"
    "  cvta.to.global.u64 %rd1, %rd1;\n"
    "  ld.param.u32 %r1, [value];\n"
    "  mov.u32 %r2, %tid.x;\n"
    "  add.u32 %r3, %r1, %r2;\n"
    "  mul.wide.u32 %rd2, %r2, 4;\n"
    "  add.u64 %rd3, %rd1, %rd2;\n"
    "  st.global.u32 [%rd3], %r3;\n"
    "  ret;\n"
    "}\n";
