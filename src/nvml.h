// The slice of NVML nvq uses, declared here so nvq builds without NVIDIA's headers and loads the
// library at run time: a box without a driver still runs nvq and is told so. Layouts and constants
// are NVML's (nvml.h, API v12/13); a mismatch is a wrong answer, so keep them byte-exact.
#pragma once

// NVIDIA's enums are int-sized; test/layout.c asserts it, so int stands in for each here.
typedef int nvmlReturn_t;
typedef int nvmlTemperatureSensors_t;
typedef int nvmlClockType_t;
typedef int nvmlPstates_t;
typedef int nvmlEnableState_t;
typedef struct nvmlDevice_st *nvmlDevice_t;
typedef struct nvmlEventSet_st *nvmlEventSet_t;

enum {
    NVML_SUCCESS = 0,
    NVML_ERROR_NOT_SUPPORTED = 3,
    NVML_ERROR_DRIVER_NOT_LOADED = 9,
    NVML_ERROR_TIMEOUT = 10,
    NVML_ERROR_GPU_IS_LOST = 15,
    NVML_ERROR_INSUFFICIENT_SIZE = 7,
};

typedef struct { unsigned long long total, free, used; } nvmlMemory_t;
typedef struct { unsigned int gpu, memory; } nvmlUtilization_t;
typedef struct {
    char busIdLegacy[16];
    unsigned int domain, bus, device, pciDeviceId, pciSubSystemId;
    char busId[32];
} nvmlPciInfo_t;
typedef struct {
    unsigned int pid;
    unsigned long long usedGpuMemory;
    unsigned int gpuInstanceId, computeInstanceId;
} nvmlProcessInfo_t;
typedef struct {
    nvmlDevice_t device;
    unsigned long long eventType, eventData;
    unsigned int gpuInstanceId, computeInstanceId;
} nvmlEventData_t;

// Versioned struct of nvmlDeviceGetTemperatureV (NVML 12.9+): version = sizeof | 1 << 24.
typedef struct {
    unsigned int version;
    nvmlTemperatureSensors_t sensorType;
    int temperature;
} nvmlTemperature_t;
#define NVQ_TEMPERATURE_V1 ((unsigned int)(sizeof(nvmlTemperature_t) | (1u << 24)))

// The same numbers are asserted against NVIDIA's own nvml.h by test/layout.c.
#include <stddef.h>
_Static_assert(sizeof(nvmlMemory_t) == 24, "nvmlMemory_t");
_Static_assert(sizeof(nvmlUtilization_t) == 8, "nvmlUtilization_t");
_Static_assert(sizeof(nvmlPciInfo_t) == 68 && offsetof(nvmlPciInfo_t, busId) == 36, "nvmlPciInfo_t");
_Static_assert(sizeof(nvmlProcessInfo_t) == 24 && offsetof(nvmlProcessInfo_t, usedGpuMemory) == 8, "nvmlProcessInfo_t");
_Static_assert(sizeof(nvmlEventData_t) == 32 && offsetof(nvmlEventData_t, eventData) == 16, "nvmlEventData_t");
_Static_assert(sizeof(nvmlTemperature_t) == 12 && offsetof(nvmlTemperature_t, temperature) == 8, "nvmlTemperature_t");

#define NVML_TEMPERATURE_GPU 0
#define NVML_CLOCK_SM 1
#define NVML_CLOCK_MEM 2
#define NVML_EVENT_SINGLE_BIT_ECC 0x1ULL
#define NVML_EVENT_DOUBLE_BIT_ECC 0x2ULL
#define NVML_EVENT_XID 0x8ULL

#include "nvml_funcs.h"

// NVML return codes by name: the stable vocabulary of nvq's errors.
static inline const char *nvml_code(nvmlReturn_t r) {
    switch (r) {
    case 0: return "success";
    case 1: return "uninitialized";
    case 2: return "invalid_argument";
    case 3: return "not_supported";
    case 4: return "no_permission";
    case 5: return "already_initialized";
    case 6: return "not_found";
    case 7: return "insufficient_size";
    case 8: return "insufficient_power";
    case 9: return "driver_not_loaded";
    case 10: return "timeout";
    case 11: return "irq_issue";
    case 12: return "library_not_found";
    case 13: return "function_not_found";
    case 14: return "corrupted_inforom";
    case 15: return "gpu_is_lost";
    case 16: return "reset_required";
    case 17: return "operating_system";
    case 18: return "lib_rm_version_mismatch";
    case 19: return "in_use";
    case 20: return "memory";
    case 21: return "no_data";
    case 22: return "vgpu_ecc_not_supported";
    case 23: return "insufficient_resources";
    case 24: return "freq_not_supported";
    case 25: return "argument_version_mismatch";
    case 26: return "deprecated";
    case 27: return "not_ready";
    case 28: return "gpu_not_found";
    case 29: return "invalid_state";
    }
    return "unknown";
}

// Clock-limit reason bits (nvmlClocksEventReason*), named.
static const struct { unsigned long long bit; const char *name; } nvml_reasons[] = {
    {0x1, "gpu_idle"},      {0x2, "applications_clocks"}, {0x4, "sw_power_cap"},
    {0x8, "hw_slowdown"},   {0x10, "sync_boost"},         {0x20, "sw_thermal"},
    {0x40, "hw_thermal"},   {0x80, "hw_power_brake"},     {0x100, "display_clocks"},
};

// What an Xid means, for the ones a mining rig meets. Source: NVIDIA's Xid catalog.
static inline const char *xid_meaning(unsigned long long xid) {
    switch (xid) {
    case 8: return "gpu stopped processing (timeout)";
    case 13: return "graphics engine exception";
    case 31: return "gpu memory page fault";
    case 32: return "invalid or corrupted push buffer stream";
    case 38: return "driver firmware error";
    case 43: return "gpu stopped processing";
    case 45: return "preemptive cleanup after a previous error";
    case 48: return "double bit ecc error";
    case 56: return "display engine error";
    case 61: return "internal micro-controller breakpoint";
    case 62: return "internal micro-controller halt";
    case 63: return "ecc page retirement or row remapping";
    case 64: return "ecc page retirement or row remapper failure";
    case 69: return "graphics engine class error";
    case 74: return "nvlink error";
    case 79: return "gpu has fallen off the bus";
    case 92: return "high single-bit ecc error rate";
    case 94: return "contained ecc error";
    case 95: return "uncontained ecc error";
    case 109: return "context switch timeout";
    case 119: return "gsp rpc timeout";
    case 120: return "gsp error";
    }
    return "";
}
