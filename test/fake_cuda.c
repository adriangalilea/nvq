// A scripted libcuda.so.1. NVQ_FAKE picks: ok | hang (an interruptible wait: the deadline ends it) |
// stuck (every signal blocked, as a call inside the driver: only SIGKILL from outside ends it) | jit.
// Device memory is host memory; the "kernel" does what the probe's PTX does.
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/cuda.h"

static int is(const char *m) {
    const char *v = getenv("NVQ_FAKE");
    return v && !strcmp(v, m);
}

CUresult cuInit(unsigned int f) {
    (void)f;
    if (is("hang")) for (;;) pause();
    if (is("stuck")) {
        sigset_t all;
        sigfillset(&all);
        sigprocmask(SIG_BLOCK, &all, NULL);
        for (;;) pause();
    }
    return 0;
}
CUresult cuDriverGetVersion(int *v) { *v = 12040; return 0; }
CUresult cuDeviceGetCount(int *n) { *n = 1; return 0; }
CUresult cuDeviceGet(CUdevice *d, int i) { *d = i; return 0; }
// The UUID CUDA_VISIBLE_DEVICES names, as the driver would report it.
CUresult cuDeviceGetUuid_v2(CUuuid *u, CUdevice d) {
    (void)d;
    const char *s = getenv("CUDA_VISIBLE_DEVICES");
    unsigned char *b = (unsigned char *)u->bytes;
    int k = 0;
    for (const char *p = s + 4; *p && k < 16; p++) {
        if (*p == '-') continue;
        unsigned int x;
        sscanf(p, "%2x", &x);
        b[k++] = (unsigned char)x;
        p++;
    }
    return 0;
}
CUresult cuDeviceGetName(char *n, int len, CUdevice d) { (void)d; snprintf(n, len, "Fake Card"); return 0; }
CUresult cuDeviceGetAttribute(int *v, int a, CUdevice d) {
    (void)d;
    switch (a) {
    case 75: *v = 8; return 0;
    case 76: *v = 9; return 0;
    case 16: *v = 128; return 0;
    case 10: *v = 32; return 0;
    case 39: *v = 1536; return 0;
    case 36: *v = 10501000; return 0;
    case 37: *v = 384; return 0;
    }
    return 1; // CUDA_ERROR_INVALID_VALUE: an attribute this "driver" does not know
}
CUresult cuDeviceTotalMem_v2(size_t *b, CUdevice d) { (void)d; *b = (size_t)24 << 30; return 0; }
CUresult cuCtxCreate_v2(CUcontext *c, unsigned int f, CUdevice d) { (void)f; (void)d; *c = (CUcontext)1; return 0; }
CUresult cuCtxDestroy_v2(CUcontext c) { (void)c; return 0; }
CUresult cuCtxSynchronize(void) { return 0; }
CUresult cuMemAlloc_v2(CUdeviceptr *p, size_t n) { *p = (CUdeviceptr)malloc(n); return 0; }
CUresult cuMemFree_v2(CUdeviceptr p) { free((void *)p); return 0; }
CUresult cuMemsetD32_v2(CUdeviceptr p, unsigned int v, size_t n) {
    for (size_t i = 0; i < n; i++) ((unsigned int *)p)[i] = v;
    return 0;
}
CUresult cuMemcpyDtoH_v2(void *h, CUdeviceptr p, size_t n) { memcpy(h, (void *)p, n); return 0; }
CUresult cuModuleLoadData(CUmodule *m, const void *img) {
    (void)img;
    if (is("jit")) return 218; // CUDA_ERROR_INVALID_PTX
    *m = (CUmodule)1;
    return 0;
}
CUresult cuModuleGetFunction(CUfunction *f, CUmodule m, const char *n) { (void)m; (void)n; *f = (CUfunction)1; return 0; }
CUresult cuModuleUnload(CUmodule m) { (void)m; return 0; }
CUresult cuLaunchKernel(CUfunction f, unsigned gx, unsigned gy, unsigned gz, unsigned bx, unsigned by, unsigned bz,
                        unsigned shared, CUstream s, void **args, void **extra) {
    (void)f, (void)gx, (void)gy, (void)gz, (void)by, (void)bz, (void)shared, (void)s, (void)extra;
    unsigned int *out = (unsigned int *)*(CUdeviceptr *)args[0];
    unsigned int value = *(unsigned int *)args[1];
    for (unsigned i = 0; i < bx; i++) out[i] = value + i;
    return 0;
}
CUresult cuGetErrorName(CUresult r, const char **s) {
    *s = r == 218 ? "CUDA_ERROR_INVALID_PTX" : r == 1 ? "CUDA_ERROR_INVALID_VALUE" : "CUDA_ERROR_UNKNOWN";
    return 0;
}
