// A scripted libnvidia-ml.so.1 for the failures a real card cannot be asked to show on demand.
// NVQ_FAKE picks one: ok | mismatch | lost | hang (interruptible) | stuck (every signal blocked, as a
// call inside the driver). Built with -DOLD it lacks one function, as a driver older than nvq would.
#define _GNU_SOURCE
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/nvml.h"

static const char *mode(void) {
    const char *m = getenv("NVQ_FAKE");
    return m ? m : "ok";
}
static int is(const char *m) { return !strcmp(mode(), m); }

// A driver call stuck in the kernel: no signal reaches it, only SIGKILL ends the process.
static void stuck(void) {
    sigset_t all;
    sigfillset(&all);
    sigprocmask(SIG_BLOCK, &all, NULL);
    for (;;) pause();
}

static struct nvmlDevice_st { int unused; } card;

nvmlReturn_t nvmlInit_v2(void) { return is("mismatch") ? 18 : 0; }
nvmlReturn_t nvmlShutdown(void) { return 0; }
nvmlReturn_t nvmlSystemGetDriverVersion(char *v, unsigned int n) { strncpy(v, "550.0.fake", n); return 0; }
nvmlReturn_t nvmlSystemGetNVMLVersion(char *v, unsigned int n) { strncpy(v, "12.550.fake", n); return 0; }
nvmlReturn_t nvmlSystemGetCudaDriverVersion_v2(int *v) { *v = 12040; return 0; }
nvmlReturn_t nvmlDeviceGetHandleByPciBusId_v2(const char *bus, nvmlDevice_t *d) { (void)bus; *d = &card; return 0; }
nvmlReturn_t nvmlDeviceGetName(nvmlDevice_t d, char *v, unsigned int n) { (void)d; strncpy(v, "Fake", n); return 0; }
nvmlReturn_t nvmlDeviceGetUUID(nvmlDevice_t d, char *v, unsigned int n) {
    (void)d;
    if (is("lost")) return NVML_ERROR_GPU_IS_LOST;
    if (is("hang")) for (;;) pause();
    if (is("stuck")) stuck();
    strncpy(v, "GPU-fake", n);
    return 0;
}
nvmlReturn_t nvmlDeviceGetPciInfo_v3(nvmlDevice_t d, nvmlPciInfo_t *p) { (void)d; memset(p, 0, sizeof *p); return 0; }
nvmlReturn_t nvmlDeviceGetCudaComputeCapability(nvmlDevice_t d, int *a, int *b) { (void)d; *a = 8; *b = 6; return 0; }
nvmlReturn_t nvmlDeviceGetMemoryInfo(nvmlDevice_t d, nvmlMemory_t *m) { (void)d; m->total = 24ULL << 30; m->used = 1ULL << 30; m->free = 23ULL << 30; return 0; }
nvmlReturn_t nvmlDeviceGetEnforcedPowerLimit(nvmlDevice_t d, unsigned int *v) { (void)d; *v = 300000; return 0; }
nvmlReturn_t nvmlDeviceGetPowerUsage(nvmlDevice_t d, unsigned int *v) { (void)d; *v = 250500; return 0; }
nvmlReturn_t nvmlDeviceGetTemperature(nvmlDevice_t d, int s, unsigned int *v) { (void)d; (void)s; *v = 61; return 0; }
// A card whose fan the driver cannot read: absent, never 0.
nvmlReturn_t nvmlDeviceGetFanSpeed(nvmlDevice_t d, unsigned int *v) { (void)d; (void)v; return NVML_ERROR_NOT_SUPPORTED; }
nvmlReturn_t nvmlDeviceGetClockInfo(nvmlDevice_t d, int c, unsigned int *v) { (void)d; *v = c == 1 ? 1695 : 9751; return 0; }
nvmlReturn_t nvmlDeviceGetUtilizationRates(nvmlDevice_t d, nvmlUtilization_t *u) { (void)d; u->gpu = 100; u->memory = 40; return 0; }
nvmlReturn_t nvmlDeviceGetPerformanceState(nvmlDevice_t d, int *v) { (void)d; *v = 2; return 0; }
// A mining riser: x1 at gen 1 on a card that can do x16 gen 4.
nvmlReturn_t nvmlDeviceGetCurrPcieLinkGeneration(nvmlDevice_t d, unsigned int *v) { (void)d; *v = 1; return 0; }
nvmlReturn_t nvmlDeviceGetCurrPcieLinkWidth(nvmlDevice_t d, unsigned int *v) { (void)d; *v = 1; return 0; }
nvmlReturn_t nvmlDeviceGetMaxPcieLinkGeneration(nvmlDevice_t d, unsigned int *v) { (void)d; *v = 4; return 0; }
nvmlReturn_t nvmlDeviceGetMaxPcieLinkWidth(nvmlDevice_t d, unsigned int *v) { (void)d; *v = 16; return 0; }
nvmlReturn_t nvmlDeviceGetPersistenceMode(nvmlDevice_t d, int *v) { (void)d; *v = 0; return 0; }
nvmlReturn_t nvmlDeviceGetCurrentClocksEventReasons(nvmlDevice_t d, unsigned long long *v) { (void)d; *v = 0x4 | 0x20; return 0; }
#ifndef OLD
nvmlReturn_t nvmlDeviceGetComputeRunningProcesses_v3(nvmlDevice_t d, unsigned int *n, nvmlProcessInfo_t *p) {
    (void)d;
    *n = 2;
    p[0] = (nvmlProcessInfo_t){.pid = 4242, .usedGpuMemory = 3ULL << 30};
    p[1] = (nvmlProcessInfo_t){.pid = 4343, .usedGpuMemory = ~0ULL};
    return 0;
}
#endif
nvmlReturn_t nvmlDeviceGetSupportedEventTypes(nvmlDevice_t d, unsigned long long *v) { (void)d; *v = NVML_EVENT_XID; return 0; }
nvmlReturn_t nvmlEventSetCreate(nvmlEventSet_t *s) { *s = (nvmlEventSet_t)&card; return 0; }
nvmlReturn_t nvmlDeviceRegisterEvents(nvmlDevice_t d, unsigned long long t, nvmlEventSet_t s) { (void)d; (void)t; (void)s; return 0; }
// One Xid 79 on the first wait, then a lost card: the fall-off-the-bus sequence.
static int waits;
nvmlReturn_t nvmlEventSetWait_v2(nvmlEventSet_t s, nvmlEventData_t *e, unsigned int ms) {
    (void)s;
    if (waits++ == 0) {
        *e = (nvmlEventData_t){.device = &card, .eventType = NVML_EVENT_XID, .eventData = 79};
        return 0;
    }
    usleep(ms * 1000);
    return NVML_ERROR_TIMEOUT;
}
