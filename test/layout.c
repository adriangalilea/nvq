// Compiles only when NVIDIA's nvml.h agrees with the layouts src/nvml.h asserts. Build against the
// CUDA toolkit's header: cc -std=c11 -I/usr/local/cuda/include -fsyntax-only test/layout.c
#include <nvml.h>
#include <stddef.h>

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
