// Compiles only when NVIDIA's headers agree with what nvq declares: every struct layout, constant and
// function signature nvq uses. Run against each CUDA toolkit on the box:
//   cc -std=c11 -Werror -I/usr/local/cuda-X/include -fsyntax-only test/layout.c
// The function lists are nvq's own (src/*_funcs.h), so a signature nvq gets wrong cannot hide here.
#define __CUDA_API_VERSION_INTERNAL // declare every versioned CUDA symbol, not only the current aliases
#include <cuda.h>
#include <nvml.h>
#include <stddef.h>

#include "../src/cuda_funcs.h"
#include "../src/nvml_funcs.h"

// Each function nvq calls, assigned to a pointer of the signature nvq declares: any difference in
// return type, arity, order, pointer level or const is an incompatible-pointer error.
#define CHECK(r, n, a) static r(*const check_##n) a = n;
NVML_FUNCS(CHECK)
CUDA_FUNCS(CHECK)
#ifdef nvmlTemperature_v1 // headers from 12.9 on carry the versioned temperature query
NVML_CURRENT_FUNCS(CHECK)
_Static_assert(sizeof(nvmlTemperature_t) == 12 && offsetof(nvmlTemperature_t, temperature) == 8, "nvmlTemperature_t");
_Static_assert(nvmlTemperature_v1 == (12u | 1u << 24), "nvmlTemperature_v1");
#else
CHECK(nvmlReturn_t, nvmlDeviceGetCurrentClocksEventReasons, (nvmlDevice_t, unsigned long long *))
#endif
// NVML 13 deprecates the names these replaced; nvq still calls them on drivers that lack the
// replacements, so their signatures are checked like any other, with the deprecation acknowledged.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
NVML_REPLACED_FUNCS(CHECK)
#pragma GCC diagnostic pop

// nvq stands int in for NVIDIA's enums: they must be int-sized.
_Static_assert(sizeof(nvmlReturn_t) == 4 && sizeof(nvmlTemperatureSensors_t) == 4 && sizeof(nvmlClockType_t) == 4, "enum sizes");
_Static_assert(sizeof(nvmlPstates_t) == 4 && sizeof(nvmlEnableState_t) == 4, "enum sizes");
_Static_assert(sizeof(CUresult) == 4 && sizeof(CUdevice_attribute) == 4 && sizeof(CUdevice) == 4, "enum sizes");
_Static_assert(sizeof(CUdeviceptr) == 8 && sizeof(CUuuid) == 16, "CUDA types");

_Static_assert(sizeof(nvmlMemory_t) == 24, "nvmlMemory_t");
_Static_assert(sizeof(nvmlUtilization_t) == 8, "nvmlUtilization_t");
_Static_assert(sizeof(nvmlPciInfo_t) == 68 && offsetof(nvmlPciInfo_t, busId) == 36, "nvmlPciInfo_t");
_Static_assert(sizeof(nvmlProcessInfo_t) == 24 && offsetof(nvmlProcessInfo_t, usedGpuMemory) == 8, "nvmlProcessInfo_t");
_Static_assert(sizeof(nvmlEventData_t) == 32 && offsetof(nvmlEventData_t, eventData) == 16, "nvmlEventData_t");
_Static_assert(NVML_ERROR_GPU_IS_LOST == 15 && NVML_ERROR_LIB_RM_VERSION_MISMATCH == 18, "return codes");
_Static_assert(NVML_ERROR_DRIVER_NOT_LOADED == 9 && NVML_ERROR_TIMEOUT == 10, "return codes");
_Static_assert(NVML_ERROR_GPU_NOT_FOUND == 28 && NVML_ERROR_INVALID_STATE == 29, "return codes");
_Static_assert(nvmlEventTypeXidCriticalError == 0x8 && nvmlEventTypeDoubleBitEccError == 0x2, "event types");
_Static_assert(NVML_CLOCK_SM == 1 && NVML_CLOCK_MEM == 2 && NVML_TEMPERATURE_GPU == 0, "enums");
// The throttle-named macros exist in every header since the rename kept them as aliases.
_Static_assert(nvmlClocksThrottleReasonHwSlowdown == 0x8 && nvmlClocksThrottleReasonHwThermalSlowdown == 0x40, "reasons");
_Static_assert(nvmlClocksThrottleReasonHwPowerBrakeSlowdown == 0x80 && nvmlClocksThrottleReasonDisplayClockSetting == 0x100, "reasons");

_Static_assert(CU_DEVICE_ATTRIBUTE_MAX_THREADS_PER_BLOCK == 1 && CU_DEVICE_ATTRIBUTE_MAX_BLOCK_DIM_X == 2, "attributes");
_Static_assert(CU_DEVICE_ATTRIBUTE_MAX_GRID_DIM_X == 5 && CU_DEVICE_ATTRIBUTE_MAX_SHARED_MEMORY_PER_BLOCK == 8, "attributes");
_Static_assert(CU_DEVICE_ATTRIBUTE_TOTAL_CONSTANT_MEMORY == 9 && CU_DEVICE_ATTRIBUTE_WARP_SIZE == 10, "attributes");
_Static_assert(CU_DEVICE_ATTRIBUTE_MAX_REGISTERS_PER_BLOCK == 12 && CU_DEVICE_ATTRIBUTE_CLOCK_RATE == 13, "attributes");
_Static_assert(CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT == 16 && CU_DEVICE_ATTRIBUTE_INTEGRATED == 18, "attributes");
_Static_assert(CU_DEVICE_ATTRIBUTE_COMPUTE_MODE == 20 && CU_DEVICE_ATTRIBUTE_CONCURRENT_KERNELS == 31, "attributes");
_Static_assert(CU_DEVICE_ATTRIBUTE_ECC_ENABLED == 32 && CU_DEVICE_ATTRIBUTE_MEMORY_CLOCK_RATE == 36, "attributes");
_Static_assert(CU_DEVICE_ATTRIBUTE_GLOBAL_MEMORY_BUS_WIDTH == 37 && CU_DEVICE_ATTRIBUTE_L2_CACHE_SIZE == 38, "attributes");
_Static_assert(CU_DEVICE_ATTRIBUTE_MAX_THREADS_PER_MULTIPROCESSOR == 39 && CU_DEVICE_ATTRIBUTE_ASYNC_ENGINE_COUNT == 40, "attributes");
_Static_assert(CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR == 75 && CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR == 76, "attributes");
_Static_assert(CU_DEVICE_ATTRIBUTE_MAX_SHARED_MEMORY_PER_MULTIPROCESSOR == 81 && CU_DEVICE_ATTRIBUTE_MAX_REGISTERS_PER_MULTIPROCESSOR == 82, "attributes");
_Static_assert(CU_DEVICE_ATTRIBUTE_MULTI_GPU_BOARD == 84 && CU_DEVICE_ATTRIBUTE_MAX_SHARED_MEMORY_PER_BLOCK_OPTIN == 97, "attributes");
_Static_assert(CU_DEVICE_ATTRIBUTE_MAX_BLOCKS_PER_MULTIPROCESSOR == 106 && CU_DEVICE_ATTRIBUTE_MAX_PERSISTING_L2_CACHE_SIZE == 108, "attributes");
