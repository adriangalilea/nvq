// Every NVML function nvq calls, in NVIDIA's own type names. src/nvml.h declares those types for the
// build; test/layout.c includes this list after NVIDIA's real nvml.h and fails to compile if any
// signature here differs from the driver's. A driver missing one is too old for nvq:
// function_not_found, by name.
#pragma once

#define NVML_FUNCS(X)                                                                              \
    X(nvmlReturn_t, nvmlInit_v2, (void))                                                           \
    X(nvmlReturn_t, nvmlShutdown, (void))                                                          \
    X(nvmlReturn_t, nvmlSystemGetDriverVersion, (char *, unsigned int))                            \
    X(nvmlReturn_t, nvmlSystemGetNVMLVersion, (char *, unsigned int))                              \
    X(nvmlReturn_t, nvmlSystemGetCudaDriverVersion_v2, (int *))                                    \
    X(nvmlReturn_t, nvmlDeviceGetHandleByPciBusId_v2, (const char *, nvmlDevice_t *))              \
    X(nvmlReturn_t, nvmlDeviceGetName, (nvmlDevice_t, char *, unsigned int))                       \
    X(nvmlReturn_t, nvmlDeviceGetUUID, (nvmlDevice_t, char *, unsigned int))                       \
    X(nvmlReturn_t, nvmlDeviceGetPciInfo_v3, (nvmlDevice_t, nvmlPciInfo_t *))                      \
    X(nvmlReturn_t, nvmlDeviceGetCudaComputeCapability, (nvmlDevice_t, int *, int *))              \
    X(nvmlReturn_t, nvmlDeviceGetMemoryInfo, (nvmlDevice_t, nvmlMemory_t *))                       \
    X(nvmlReturn_t, nvmlDeviceGetEnforcedPowerLimit, (nvmlDevice_t, unsigned int *))               \
    X(nvmlReturn_t, nvmlDeviceGetPowerUsage, (nvmlDevice_t, unsigned int *))                       \
    X(nvmlReturn_t, nvmlDeviceGetTotalEnergyConsumption, (nvmlDevice_t, unsigned long long *))     \
    X(nvmlReturn_t, nvmlDeviceGetFanSpeed, (nvmlDevice_t, unsigned int *))                         \
    X(nvmlReturn_t, nvmlDeviceGetClockInfo, (nvmlDevice_t, nvmlClockType_t, unsigned int *))       \
    X(nvmlReturn_t, nvmlDeviceGetUtilizationRates, (nvmlDevice_t, nvmlUtilization_t *))            \
    X(nvmlReturn_t, nvmlDeviceGetPerformanceState, (nvmlDevice_t, nvmlPstates_t *))                \
    X(nvmlReturn_t, nvmlDeviceGetCurrPcieLinkGeneration, (nvmlDevice_t, unsigned int *))           \
    X(nvmlReturn_t, nvmlDeviceGetCurrPcieLinkWidth, (nvmlDevice_t, unsigned int *))                \
    X(nvmlReturn_t, nvmlDeviceGetMaxPcieLinkGeneration, (nvmlDevice_t, unsigned int *))            \
    X(nvmlReturn_t, nvmlDeviceGetMaxPcieLinkWidth, (nvmlDevice_t, unsigned int *))                 \
    X(nvmlReturn_t, nvmlDeviceGetPcieReplayCounter, (nvmlDevice_t, unsigned int *))                \
    X(nvmlReturn_t, nvmlDeviceGetPersistenceMode, (nvmlDevice_t, nvmlEnableState_t *))             \
    X(nvmlReturn_t, nvmlDeviceGetComputeRunningProcesses_v3,                                       \
      (nvmlDevice_t, unsigned int *, nvmlProcessInfo_t *))                                         \
    X(nvmlReturn_t, nvmlDeviceGetSupportedEventTypes, (nvmlDevice_t, unsigned long long *))        \
    X(nvmlReturn_t, nvmlEventSetCreate, (nvmlEventSet_t *))                                        \
    X(nvmlReturn_t, nvmlDeviceRegisterEvents, (nvmlDevice_t, unsigned long long, nvmlEventSet_t))  \
    X(nvmlReturn_t, nvmlEventSetWait_v2, (nvmlEventSet_t, nvmlEventData_t *, unsigned int))

// Functions NVML replaced: nvq calls the current one when the driver exports it, else the one before.
// The current names (NVML 12.9+ for temperature, driver 535+ for the reasons query):
#define NVML_CURRENT_FUNCS(X)                                                                      \
    X(nvmlReturn_t, nvmlDeviceGetTemperatureV, (nvmlDevice_t, nvmlTemperature_t *))                \
    X(nvmlReturn_t, nvmlDeviceGetCurrentClocksEventReasons, (nvmlDevice_t, unsigned long long *))
// The names they replaced, deprecated in NVML 13 and still the only ones on older drivers:
#define NVML_REPLACED_FUNCS(X)                                                                     \
    X(nvmlReturn_t, nvmlDeviceGetTemperature, (nvmlDevice_t, nvmlTemperatureSensors_t, unsigned int *)) \
    X(nvmlReturn_t, nvmlDeviceGetCurrentClocksThrottleReasons, (nvmlDevice_t, unsigned long long *))
