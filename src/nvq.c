// nvq: NVIDIA cards as JSON, for programs. See README.md for the contract.
//
//   nvq list               every card the driver knows, with what NVML can say about each
//   nvq probe <uuid|all>   can the card run a kernel: CUDA init, context, alloc, PTX JIT, launch, verify
//   nvq watch [--every-ms N]  Xid and ECC events as they happen, lost cards, optional samples
//   nvq schema             every command, its output and exit codes, as JSON Schema
//   nvq version
//
// The roster comes from the kernel (/proc/driver/nvidia/gpus): it lists every card the driver bound,
// lost ones included, and never hangs. NVML adds the details per card by bus id, CUDA proves the card
// computes. Both libraries are loaded at run time, so nvq runs on a box without a driver and says so.
//
// A hung driver call cannot be interrupted from inside the process; nvq bounds itself with a deadline
// and `probe all` isolates every card in its own child, killed when its deadline passes.
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cuda.h"
#include "json.h"
#include "nvml.h"

#define NVQ_SCHEMA 1

// schema/nvq.schema.json, compiled in by the Makefile: the binary describes itself exactly.
static const unsigned char schema_json[] = {
#include "schema.inc"
};
#ifndef NVQ_VERSION
#define NVQ_VERSION "dev"
#endif

enum { EXIT_OK = 0, EXIT_CARD = 1, EXIT_USAGE = 2, EXIT_DRIVER = 3, EXIT_PROBE = 4, EXIT_DEADLINE = 5 };

#define PROC_GPUS "/proc/driver/nvidia/gpus"
#define PROC_VERSION "/proc/driver/nvidia/version"
#define MAX_CARDS 64

// ---- failure ----------------------------------------------------------------------------------

// A whole-command failure: one document {"nvq":1,"error":{code,detail}} and an exit code.
static void fail(int code, const char *err, const char *detail) {
    jo(NULL);
    ju("nvq", NVQ_SCHEMA);
    jo("error");
    js("code", err);
    js("detail", detail ? detail : "");
    jeo();
    jeo();
    jline();
    exit(code);
}

// The deadline: a driver call that never returns ends here. Async-signal-safe by construction.
static const char deadline_doc[] = "{\"nvq\":1,\"error\":{\"code\":\"deadline\",\"detail\":\"a driver call did not return in time\"}}\n";
static void on_deadline(int sig) {
    (void)sig;
    ssize_t w = write(STDOUT_FILENO, deadline_doc, sizeof deadline_doc - 1);
    (void)w;
    _exit(EXIT_DEADLINE);
}

static void arm_deadline(long ms) {
    if (ms <= 0) return;
    signal(SIGALRM, on_deadline);
    struct itimerval t = {{0, 0}, {ms / 1000, (ms % 1000) * 1000}};
    setitimer(ITIMER_REAL, &t, NULL);
}

static double now_unix(void) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static double now_mono_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

// ---- the kernel's roster ----------------------------------------------------------------------

typedef struct {
    char bus[16];   // "0000:01:00.0", lowercase
    char uuid[48];  // "GPU-…", lowercase
    char model[96];
    int minor;      // /dev/nvidia<minor>
} Card;

static void trim(char *s) {
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    memmove(s, p, strlen(p) + 1);
    for (size_t n = strlen(s); n > 0 && isspace((unsigned char)s[n - 1]); n--) s[n - 1] = 0;
}

static void lower(char *s) {
    for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

static int card_cmp(const void *a, const void *b) { return strcmp(((const Card *)a)->bus, ((const Card *)b)->bus); }

// roster fills cards from the kernel and returns how many. No driver module: a failure, exit 3.
static int roster(Card *cards) {
    DIR *d = opendir(PROC_GPUS);
    if (!d) fail(EXIT_DRIVER, "driver_not_loaded", "no " PROC_GPUS ": the nvidia kernel module is not loaded");
    int n = 0;
    for (struct dirent *e; (e = readdir(d));) {
        if (e->d_name[0] == '.') continue;
        if (n == MAX_CARDS) fail(EXIT_DRIVER, "too_many_cards", "more than 64 cards");
        char path[512];
        snprintf(path, sizeof path, PROC_GPUS "/%s/information", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        Card c = {.minor = -1};
        char line[256];
        while (fgets(line, sizeof line, f)) {
            char *colon = strchr(line, ':');
            if (!colon) continue;
            *colon = 0;
            char *val = colon + 1;
            trim(line);
            trim(val);
            if (!strcmp(line, "Model")) snprintf(c.model, sizeof c.model, "%s", val);
            else if (!strcmp(line, "GPU UUID")) snprintf(c.uuid, sizeof c.uuid, "%s", val);
            else if (!strcmp(line, "Bus Location")) snprintf(c.bus, sizeof c.bus, "%s", val);
            else if (!strcmp(line, "Device Minor")) c.minor = atoi(val);
        }
        fclose(f);
        if (!c.bus[0]) snprintf(c.bus, sizeof c.bus, "%.15s", e->d_name);
        lower(c.bus);
        lower(c.uuid);
        // The UUID prefix is "gpu-" once lowered; the world writes it "GPU-".
        if (!strncmp(c.uuid, "gpu-", 4)) memcpy(c.uuid, "GPU-", 4);
        cards[n++] = c;
    }
    closedir(d);
    qsort(cards, n, sizeof *cards, card_cmp);
    return n;
}

// The kernel module's version, "590.48.01", from /proc. Empty when unreadable.
static void kernel_driver(char *out, size_t size) {
    out[0] = 0;
    FILE *f = fopen(PROC_VERSION, "r");
    if (!f) return;
    char line[512];
    if (fgets(line, sizeof line, f)) {
        char *p = strstr(line, "Kernel Module");
        if (p) {
            p += strlen("Kernel Module");
            while (*p == ' ' || *p == '\t') p++;
            size_t n = strcspn(p, " \t\n");
            snprintf(out, size, "%.*s", (int)n, p);
        }
    }
    fclose(f);
}

// ---- NVML -------------------------------------------------------------------------------------

static struct {
#define DECL(r, n, a) r(*n) a;
    NVML_FUNCS(DECL)
    NVML_CURRENT_FUNCS(DECL)
    NVML_REPLACED_FUNCS(DECL)
#undef DECL
} nv;

// The card's temperature through whichever query the driver has: the versioned one (NVML 12.9+), else
// the one it replaced.
static nvmlReturn_t card_temp(nvmlDevice_t d, unsigned int *c) {
    if (!nv.nvmlDeviceGetTemperatureV) return nv.nvmlDeviceGetTemperature(d, NVML_TEMPERATURE_GPU, c);
    nvmlTemperature_t t = {.version = NVQ_TEMPERATURE_V1, .sensorType = NVML_TEMPERATURE_GPU};
    nvmlReturn_t r = nv.nvmlDeviceGetTemperatureV(d, &t);
    if (r == NVML_SUCCESS) *c = (unsigned int)t.temperature;
    return r;
}

static nvmlReturn_t card_reasons(nvmlDevice_t d, unsigned long long *bits) {
    if (nv.nvmlDeviceGetCurrentClocksEventReasons) return nv.nvmlDeviceGetCurrentClocksEventReasons(d, bits);
    return nv.nvmlDeviceGetCurrentClocksThrottleReasons(d, bits);
}

// nvml_open loads and initialises NVML. On failure it returns the error code and fills detail;
// the caller decides whether that is fatal (watch) or a fact to report beside the roster (list).
static const char *nvml_open(char *detail, size_t size) {
    void *h = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        snprintf(detail, size, "%s", dlerror());
        return "library_not_found";
    }
#define LOAD(r, n, a)                                                                              \
    nv.n = (r(*) a)dlsym(h, #n);                                                                   \
    if (!nv.n) {                                                                                   \
        snprintf(detail, size, "libnvidia-ml.so.1 has no %s: driver too old for nvq", #n);         \
        return "function_not_found";                                                               \
    }
    NVML_FUNCS(LOAD)
#undef LOAD
#define LOAD_OPTIONAL(r, n, a) nv.n = (r(*) a)dlsym(h, #n);
    NVML_CURRENT_FUNCS(LOAD_OPTIONAL)
    NVML_REPLACED_FUNCS(LOAD_OPTIONAL)
#undef LOAD_OPTIONAL
    if (!nv.nvmlDeviceGetTemperatureV && !nv.nvmlDeviceGetTemperature) {
        snprintf(detail, size, "libnvidia-ml.so.1 has neither nvmlDeviceGetTemperatureV nor nvmlDeviceGetTemperature");
        return "function_not_found";
    }
    if (!nv.nvmlDeviceGetCurrentClocksEventReasons && !nv.nvmlDeviceGetCurrentClocksThrottleReasons) {
        snprintf(detail, size, "libnvidia-ml.so.1 has no clock event or throttle reasons query");
        return "function_not_found";
    }
    nvmlReturn_t r = nv.nvmlInit_v2();
    if (r != NVML_SUCCESS) {
        snprintf(detail, size, "nvmlInit_v2 returned %d", r);
        return nvml_code(r);
    }
    return NULL;
}

// Field outcomes collected while a card is written, emitted at its end.
typedef struct {
    const char *unsupported[32];
    int nu;
    const char *errname[32];
    const char *errcode[32];
    int ne;
} Outcomes;

// ok reports whether a field call succeeded; otherwise it files the field under unsupported or errors.
static int ok(Outcomes *o, const char *field, nvmlReturn_t r) {
    if (r == NVML_SUCCESS) return 1;
    if (r == NVML_ERROR_NOT_SUPPORTED) o->unsupported[o->nu++] = field;
    else {
        o->errname[o->ne] = field;
        o->errcode[o->ne++] = nvml_code(r);
    }
    return 0;
}

static void write_reasons(const char *key, unsigned long long bits) {
    ja(key);
    for (size_t i = 0; i < sizeof nvml_reasons / sizeof *nvml_reasons; i++)
        if (bits & nvml_reasons[i].bit) js(NULL, nvml_reasons[i].name);
    jea();
}

// card_handle looks a roster card up in NVML. NULL with *r set when NVML cannot reach it.
static nvmlDevice_t card_handle(const Card *c, nvmlReturn_t *r) {
    nvmlDevice_t d = NULL;
    *r = nv.nvmlDeviceGetHandleByPciBusId_v2(c->bus, &d);
    if (*r != NVML_SUCCESS) return NULL;
    // A cheap read that touches the device: a card that fell off the bus answers gpu_is_lost here.
    char uuid[96];
    *r = nv.nvmlDeviceGetUUID(d, uuid, sizeof uuid);
    return *r == NVML_SUCCESS ? d : NULL;
}

// write_card renders one card; returns 1 when the card answered.
static int write_card(int index, const Card *c, int nvml_up) {
    jo(NULL);
    ji("index", index);
    js("uuid", c->uuid);
    js("bus", c->bus);
    ji("minor", c->minor);
    js("model", c->model);
    if (!nvml_up) {
        js("state", "unknown");
        jeo();
        return 0;
    }
    nvmlReturn_t r;
    nvmlDevice_t d = card_handle(c, &r);
    if (!d) {
        js("state", r == NVML_ERROR_GPU_IS_LOST ? "lost" : "error");
        js("error", nvml_code(r));
        jeo();
        return 0;
    }
    js("state", "ok");
    Outcomes o = {0};

    int major, minor;
    if (ok(&o, "compute", nv.nvmlDeviceGetCudaComputeCapability(d, &major, &minor))) {
        char cc[16];
        snprintf(cc, sizeof cc, "%d.%d", major, minor);
        js("compute", cc);
    }
    nvmlMemory_t mem;
    if (ok(&o, "memory", nv.nvmlDeviceGetMemoryInfo(d, &mem))) {
        jo("memoryMiB");
        ju("total", mem.total >> 20);
        ju("used", mem.used >> 20);
        jeo();
    }
    unsigned int mw;
    int power_open = 0;
    if (ok(&o, "powerLimit", nv.nvmlDeviceGetEnforcedPowerLimit(d, &mw))) {
        jo("powerW");
        power_open = 1;
        jf("limit", mw / 1000.0);
    }
    if (ok(&o, "powerDraw", nv.nvmlDeviceGetPowerUsage(d, &mw))) {
        if (!power_open) jo("powerW"), power_open = 1;
        jf("draw", mw / 1000.0);
    }
    if (power_open) jeo();
    unsigned int v;
    if (ok(&o, "tempC", card_temp(d, &v))) ju("tempC", v);
    if (ok(&o, "fanPct", nv.nvmlDeviceGetFanSpeed(d, &v))) ju("fanPct", v);
    unsigned int sm, memclk;
    nvmlReturn_t rs = nv.nvmlDeviceGetClockInfo(d, NVML_CLOCK_SM, &sm);
    nvmlReturn_t rm = nv.nvmlDeviceGetClockInfo(d, NVML_CLOCK_MEM, &memclk);
    if (ok(&o, "clockSm", rs) & ok(&o, "clockMem", rm)) {
        jo("clocksMHz");
        ju("sm", sm);
        ju("mem", memclk);
        jeo();
    }
    nvmlUtilization_t u;
    if (ok(&o, "utilization", nv.nvmlDeviceGetUtilizationRates(d, &u))) {
        jo("utilPct");
        ju("gpu", u.gpu);
        ju("mem", u.memory);
        jeo();
    }
    int ps;
    if (ok(&o, "pstate", nv.nvmlDeviceGetPerformanceState(d, &ps))) ji("pstate", ps);
    unsigned int gen, width, maxgen, maxwidth;
    nvmlReturn_t r1 = nv.nvmlDeviceGetCurrPcieLinkGeneration(d, &gen);
    nvmlReturn_t r2 = nv.nvmlDeviceGetCurrPcieLinkWidth(d, &width);
    nvmlReturn_t r3 = nv.nvmlDeviceGetMaxPcieLinkGeneration(d, &maxgen);
    nvmlReturn_t r4 = nv.nvmlDeviceGetMaxPcieLinkWidth(d, &maxwidth);
    unsigned int replays;
    nvmlReturn_t r5 = nv.nvmlDeviceGetPcieReplayCounter(d, &replays);
    if (ok(&o, "pcieGen", r1) & ok(&o, "pcieWidth", r2) & ok(&o, "pcieMaxGen", r3) & ok(&o, "pcieMaxWidth", r4)) {
        jo("pcie");
        ju("gen", gen);
        ju("width", width);
        ju("maxGen", maxgen);
        ju("maxWidth", maxwidth);
        // Link-level retransmissions since the driver loaded: a rising count is a failing riser or slot.
        if (ok(&o, "pcieReplays", r5)) ju("replays", replays);
        jeo();
    } else ok(&o, "pcieReplays", r5);
    unsigned long long mj;
    if (ok(&o, "energyJ", nv.nvmlDeviceGetTotalEnergyConsumption(d, &mj))) jf("energyJ", mj / 1000.0);
    int pm;
    if (ok(&o, "persistence", nv.nvmlDeviceGetPersistenceMode(d, &pm))) jb("persistence", pm);
    unsigned long long reasons;
    if (ok(&o, "limits", card_reasons(d, &reasons))) write_reasons("limits", reasons);
    nvmlProcessInfo_t procs[128];
    unsigned int nprocs = 128;
    if (ok(&o, "processes", nv.nvmlDeviceGetComputeRunningProcesses_v3(d, &nprocs, procs))) {
        ja("processes");
        for (unsigned int i = 0; i < nprocs; i++) {
            jo(NULL);
            ju("pid", procs[i].pid);
            // NVML reports usedGpuMemory as all-ones when it cannot see it (another container).
            if (procs[i].usedGpuMemory != ~0ULL) ju("usedMiB", procs[i].usedGpuMemory >> 20);
            jeo();
        }
        jea();
    }
    if (o.nu) {
        ja("unsupported");
        for (int i = 0; i < o.nu; i++) js(NULL, o.unsupported[i]);
        jea();
    }
    if (o.ne) {
        jo("errors");
        for (int i = 0; i < o.ne; i++) js(o.errname[i], o.errcode[i]);
        jeo();
    }
    jeo();
    return 1;
}

static int cmd_list(void) {
    Card cards[MAX_CARDS];
    int n = roster(cards);
    char driver[64], detail[512];
    kernel_driver(driver, sizeof driver);
    const char *err = nvml_open(detail, sizeof detail);

    jo(NULL);
    ju("nvq", NVQ_SCHEMA);
    js("driver", driver);
    if (err) {
        jo("error");
        js("code", err);
        js("detail", detail);
        jeo();
    } else {
        char v[96];
        if (nv.nvmlSystemGetNVMLVersion(v, sizeof v) == NVML_SUCCESS) js("nvml", v);
        int cv;
        if (nv.nvmlSystemGetCudaDriverVersion_v2(&cv) == NVML_SUCCESS) {
            snprintf(v, sizeof v, "%d.%d", cv / 1000, cv % 1000 / 10);
            js("cuda", v);
        }
    }
    int answered = 0;
    ja("cards");
    for (int i = 0; i < n; i++) answered += write_card(i, &cards[i], !err);
    jea();
    jeo();
    jline();
    if (err) return EXIT_DRIVER;
    return answered == n ? EXIT_OK : EXIT_CARD;
}

// ---- CUDA probe -------------------------------------------------------------------------------

static struct {
#define DECL(r, n, a) r(*n) a;
    CUDA_FUNCS(DECL)
#undef DECL
} cu;

static const char *cu_name(CUresult r) {
    const char *s = NULL;
    if (cu.cuGetErrorName && cu.cuGetErrorName(r, &s) == 0 && s) return s;
    return "CUDA_ERROR_UNKNOWN";
}

static void format_uuid(const CUuuid *u, char *out) {
    const unsigned char *b = (const unsigned char *)u->bytes;
    sprintf(out, "GPU-%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2], b[3],
            b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

// Every CUdevice_attribute below MAX_ATTR, read once; the sheet picks the ones it names.
enum { MAX_ATTR = 128 };
typedef struct {
    CUdevice dev;
    int v[MAX_ATTR];
    int ok[MAX_ATTR];
} Attrs;

static const char *missing[48];
static int nmissing;

static void attr_i(const Attrs *a, const char *key, int id) {
    if (a->ok[id]) ji(key, a->v[id]);
    else missing[nmissing++] = key;
}

static void attr_b(const Attrs *a, const char *key, int id) {
    if (a->ok[id]) jb(key, a->v[id]);
    else missing[nmissing++] = key;
}

static void attr_dim3(const Attrs *a, const char *key, int x) {
    if (!a->ok[x] || !a->ok[x + 1] || !a->ok[x + 2]) {
        missing[nmissing++] = key;
        return;
    }
    ja(key);
    for (int i = 0; i < 3; i++) ji(NULL, a->v[x + i]);
    jea();
}

// write_sheet renders what a kernel author sizes launches by. Units are in the keys; derived values
// (maxWarpsPerSM, peakGBs) follow from the attributes beside them and nothing else.
static void write_sheet(const Attrs *a, size_t total) {
    int major = a->v[ATTR_COMPUTE_CAPABILITY_MAJOR], minor = a->v[ATTR_COMPUTE_CAPABILITY_MINOR];
    char text[32];
    jo("arch");
    snprintf(text, sizeof text, "%d.%d", major, minor);
    js("compute", text);
    snprintf(text, sizeof text, "sm_%d%d", major, minor);
    js("sm", text);
    js("family", cc_family(major, minor));
    jeo();

    jo("layout");
    attr_i(a, "sms", ATTR_MULTIPROCESSOR_COUNT);
    attr_i(a, "warpSize", ATTR_WARP_SIZE);
    attr_i(a, "maxThreadsPerBlock", ATTR_MAX_THREADS_PER_BLOCK);
    attr_i(a, "maxThreadsPerSM", ATTR_MAX_THREADS_PER_MULTIPROCESSOR);
    attr_i(a, "maxBlocksPerSM", ATTR_MAX_BLOCKS_PER_MULTIPROCESSOR);
    if (a->ok[ATTR_MAX_THREADS_PER_MULTIPROCESSOR] && a->ok[ATTR_WARP_SIZE])
        ji("maxWarpsPerSM", a->v[ATTR_MAX_THREADS_PER_MULTIPROCESSOR] / a->v[ATTR_WARP_SIZE]);
    attr_dim3(a, "maxBlockDim", ATTR_MAX_BLOCK_DIM_X);
    attr_dim3(a, "maxGridDim", ATTR_MAX_GRID_DIM_X);
    jeo();

    jo("registers");
    attr_i(a, "perBlock", ATTR_MAX_REGISTERS_PER_BLOCK);
    attr_i(a, "perSM", ATTR_MAX_REGISTERS_PER_MULTIPROCESSOR);
    jeo();

    jo("sharedBytes");
    attr_i(a, "perBlock", ATTR_MAX_SHARED_MEMORY_PER_BLOCK);
    attr_i(a, "perBlockOptin", ATTR_MAX_SHARED_MEMORY_PER_BLOCK_OPTIN);
    attr_i(a, "perSM", ATTR_MAX_SHARED_MEMORY_PER_MULTIPROCESSOR);
    jeo();

    jo("memory");
    ju("totalMiB", total >> 20);
    attr_i(a, "busWidthBits", ATTR_GLOBAL_MEMORY_BUS_WIDTH);
    if (a->ok[ATTR_MEMORY_CLOCK_RATE]) ji("clockMHz", a->v[ATTR_MEMORY_CLOCK_RATE] / 1000);
    else missing[nmissing++] = "memory.clockMHz";
    // Double data rate: two transfers per clock across the bus.
    if (a->ok[ATTR_MEMORY_CLOCK_RATE] && a->ok[ATTR_GLOBAL_MEMORY_BUS_WIDTH])
        jf("peakGBs", a->v[ATTR_MEMORY_CLOCK_RATE] * 1e3 * 2 * (a->v[ATTR_GLOBAL_MEMORY_BUS_WIDTH] / 8.0) / 1e9);
    attr_i(a, "l2Bytes", ATTR_L2_CACHE_SIZE);
    attr_i(a, "l2PersistingBytes", ATTR_MAX_PERSISTING_L2_CACHE_SIZE);
    attr_i(a, "constantBytes", ATTR_TOTAL_CONSTANT_MEMORY);
    jeo();

    if (a->ok[ATTR_CLOCK_RATE]) ji("smClockMHz", a->v[ATTR_CLOCK_RATE] / 1000);
    else missing[nmissing++] = "smClockMHz";
    attr_i(a, "copyEngines", ATTR_ASYNC_ENGINE_COUNT);
    attr_b(a, "concurrentKernels", ATTR_CONCURRENT_KERNELS);
    if (a->ok[ATTR_COMPUTE_MODE]) js("computeMode", compute_mode(a->v[ATTR_COMPUTE_MODE]));
    else missing[nmissing++] = "computeMode";
    attr_b(a, "ecc", ATTR_ECC_ENABLED);
    attr_b(a, "integrated", ATTR_INTEGRATED);
    attr_b(a, "multiGpuBoard", ATTR_MULTI_GPU_BOARD);
    if (nmissing) {
        ja("unsupported");
        for (int i = 0; i < nmissing; i++) js(NULL, missing[i]);
        jea();
    }
}

enum { MAX_STEPS = 32 };
typedef struct {
    const char *name[MAX_STEPS];
    double ms[MAX_STEPS];
    int n;
} Steps;

static void probe_fail(const char *uuid, Steps *s, const char *step, const char *err) {
    jo(NULL);
    ju("nvq", NVQ_SCHEMA);
    js("uuid", uuid);
    jb("ok", 0);
    js("step", step);
    js("error", err);
    jo("ms");
    for (int i = 0; i < s->n; i++) jf(s->name[i], s->ms[i]);
    jeo();
    jeo();
    jline();
    exit(EXIT_PROBE);
}

// The probe steps in order; each is timed, the first failure names its step.
#define STEP(label, call)                                                                          \
    do {                                                                                           \
        if (s.n == MAX_STEPS) abort();                                                             \
        double t0 = now_mono_ms();                                                                 \
        CUresult rc = (call);                                                                      \
        s.name[s.n] = label;                                                                       \
        s.ms[s.n++] = now_mono_ms() - t0;                                                          \
        if (rc != 0) probe_fail(uuid, &s, label, cu_name(rc));                                     \
    } while (0)

static int cmd_probe_one(const char *uuid) {
    Steps s = {0};
    // Only this card is visible to CUDA: a broken neighbour cannot hang this card's cuInit.
    setenv("CUDA_VISIBLE_DEVICES", uuid, 1);
    void *h = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!h) probe_fail(uuid, &s, "load", "library_not_found");
#define LOAD(r, n, a)                                                                              \
    cu.n = (r(*) a)dlsym(h, #n);                                                                   \
    if (!cu.n) probe_fail(uuid, &s, "load", "function_not_found: " #n);
    CUDA_FUNCS(LOAD)
#undef LOAD
    STEP("init", cu.cuInit(0));
    int count = 0;
    cu.cuDeviceGetCount(&count);
    if (count != 1) probe_fail(uuid, &s, "device", count == 0 ? "not_visible" : "more_than_one_visible");
    CUdevice dev;
    CUuuid cuid;
    char seen[64];
    STEP("device", cu.cuDeviceGet(&dev, 0));
    STEP("uuid", cu.cuDeviceGetUuid_v2(&cuid, dev));
    format_uuid(&cuid, seen);
    if (strcmp(seen, uuid)) probe_fail(uuid, &s, "uuid", "uuid_mismatch");
    char name[128];
    size_t total;
    int drv;
    STEP("name", cu.cuDeviceGetName(name, sizeof name, dev));
    STEP("memory", cu.cuDeviceTotalMem_v2(&total, dev));
    STEP("driver", cu.cuDriverGetVersion(&drv));
    Attrs at = {.dev = dev};
    double t0 = now_mono_ms();
    for (int id = 0; id < MAX_ATTR; id++) at.ok[id] = cu.cuDeviceGetAttribute(&at.v[id], id, dev) == 0;
    s.name[s.n] = "attributes";
    s.ms[s.n++] = now_mono_ms() - t0;
    if (!at.ok[ATTR_COMPUTE_CAPABILITY_MAJOR] || !at.ok[ATTR_COMPUTE_CAPABILITY_MINOR])
        probe_fail(uuid, &s, "attributes", "no_compute_capability");

    CUcontext ctx;
    CUdeviceptr buf;
    CUmodule mod;
    CUfunction fn;
    enum { THREADS = 256 };
    unsigned int value = 0x5eed0000, host[THREADS];
    void *args[] = {&buf, &value};
    STEP("context", cu.cuCtxCreate_v2(&ctx, 0, dev));
    STEP("alloc", cu.cuMemAlloc_v2(&buf, sizeof host));
    STEP("memset", cu.cuMemsetD32_v2(buf, 0, THREADS));
    STEP("jit", cu.cuModuleLoadData(&mod, probe_ptx));
    STEP("function", cu.cuModuleGetFunction(&fn, mod, "nvq_probe"));
    STEP("launch", cu.cuLaunchKernel(fn, 1, 1, 1, THREADS, 1, 1, 0, NULL, args, NULL));
    STEP("sync", cu.cuCtxSynchronize());
    STEP("copy", cu.cuMemcpyDtoH_v2(host, buf, sizeof host));
    for (unsigned int i = 0; i < THREADS; i++)
        if (host[i] != value + i) probe_fail(uuid, &s, "verify", "wrong_result");
    STEP("free", cu.cuMemFree_v2(buf));
    STEP("unload", cu.cuModuleUnload(mod));
    STEP("teardown", cu.cuCtxDestroy_v2(ctx));

    jo(NULL);
    ju("nvq", NVQ_SCHEMA);
    js("uuid", uuid);
    jb("ok", 1);
    js("name", name);
    char text[32];
    snprintf(text, sizeof text, "%d.%d", drv / 1000, drv % 1000 / 10);
    js("cuda", text);
    write_sheet(&at, total);
    jo("ms");
    for (int i = 0; i < s.n; i++) jf(s.name[i], s.ms[i]);
    jeo();
    jeo();
    jline();
    return EXIT_OK;
}

// probe all: one child per card, all at once, each killed at the deadline. A child's document is
// copied verbatim; a child that said nothing in time is reported as a hang.
static int cmd_probe_all(long deadline_ms) {
    Card cards[MAX_CARDS];
    int n = roster(cards);
    pid_t pid[MAX_CARDS];
    int fd[MAX_CARDS];
    static char out[MAX_CARDS][16384];
    size_t len[MAX_CARDS];
    int open_fds = 0;
    char self[4096];
    ssize_t sl = readlink("/proc/self/exe", self, sizeof self - 1);
    if (sl < 0) fail(EXIT_PROBE, "operating_system", "cannot read /proc/self/exe");
    self[sl] = 0;
    char dl[32];
    snprintf(dl, sizeof dl, "%ld", deadline_ms);
    for (int i = 0; i < n; i++) {
        int p[2];
        if (pipe(p)) fail(EXIT_PROBE, "operating_system", strerror(errno));
        pid[i] = fork();
        if (pid[i] < 0) fail(EXIT_PROBE, "operating_system", strerror(errno));
        if (pid[i] == 0) {
            dup2(p[1], STDOUT_FILENO);
            close(p[0]);
            close(p[1]);
            execl(self, "nvq", "--deadline-ms", dl, "probe", cards[i].uuid, (char *)NULL);
            _exit(127);
        }
        close(p[1]);
        fd[i] = p[0];
        fcntl(fd[i], F_SETFL, O_NONBLOCK);
        len[i] = 0;
        open_fds++;
    }
    double end = now_mono_ms() + deadline_ms + 2000; // the child's own deadline fires first
    while (open_fds > 0 && now_mono_ms() < end) {
        struct pollfd pf[MAX_CARDS];
        int map[MAX_CARDS], m = 0;
        for (int i = 0; i < n; i++)
            if (fd[i] >= 0) pf[m] = (struct pollfd){fd[i], POLLIN, 0}, map[m++] = i;
        poll(pf, m, 100);
        for (int k = 0; k < m; k++) {
            if (!pf[k].revents) continue;
            int i = map[k];
            ssize_t r = read(fd[i], out[i] + len[i], sizeof out[i] - 1 - len[i]);
            if (r > 0) len[i] += r;
            else if (r == 0 || errno != EAGAIN) close(fd[i]), fd[i] = -1, open_fds--;
        }
    }
    int all_ok = 1;
    jo(NULL);
    ju("nvq", NVQ_SCHEMA);
    ja("probes");
    for (int i = 0; i < n; i++) {
        int status = 0, reaped = 0;
        if (fd[i] >= 0) kill(pid[i], SIGKILL), close(fd[i]);
        // A child inside a driver call that never returns sleeps uninterruptibly: SIGKILL is queued
        // but it cannot die until the call returns. Wait 2 s, then leave it and say so.
        for (double until = now_mono_ms() + 2000; !reaped && now_mono_ms() < until; usleep(10000))
            reaped = waitpid(pid[i], &status, WNOHANG) == pid[i];
        out[i][len[i]] = 0;
        while (len[i] > 0 && out[i][len[i] - 1] == '\n') out[i][--len[i]] = 0;
        int child_ok = reaped && WIFEXITED(status) && WEXITSTATUS(status) == EXIT_OK;
        all_ok &= child_ok;
        jsep(NULL);
        if (reaped && len[i] > 0 && out[i][0] == '{' && out[i][len[i] - 1] == '}') {
            fputs(out[i], stdout); // the child's own document
        } else {
            jcomma[++jdepth] = 0, fputc('{', stdout);
            ju("nvq", NVQ_SCHEMA);
            js("uuid", cards[i].uuid);
            jb("ok", 0);
            if (!reaped) {
                js("step", "hang");
                js("error", "stuck_in_driver");
                ji("pid", pid[i]);
            } else if (fd[i] >= 0) {
                js("step", "hang");
                js("error", "killed_at_deadline");
            } else {
                js("step", "crash");
                js("error", "no_output");
                if (WIFSIGNALED(status)) ji("signal", WTERMSIG(status));
                if (WIFEXITED(status)) ji("exit", WEXITSTATUS(status));
            }
            jeo();
        }
    }
    jea();
    jeo();
    jline();
    return all_ok ? EXIT_OK : EXIT_PROBE;
}

// ---- watch ------------------------------------------------------------------------------------

static void event_head(const char *event, const Card *c) {
    jo(NULL);
    ju("nvq", NVQ_SCHEMA);
    js("event", event);
    jf("at", now_unix());
    if (c) {
        js("uuid", c->uuid);
        js("bus", c->bus);
    }
}

static _Noreturn void cmd_watch(long every_ms) {
    Card cards[MAX_CARDS];
    int n = roster(cards);
    char detail[512];
    const char *err = nvml_open(detail, sizeof detail);
    if (err) fail(EXIT_DRIVER, err, detail);
    nvmlDevice_t dev[MAX_CARDS];
    int lost[MAX_CARDS] = {0};
    nvmlEventSet_t set;
    nvmlReturn_t r = nv.nvmlEventSetCreate(&set);
    if (r) fail(EXIT_DRIVER, nvml_code(r), "nvmlEventSetCreate");

    event_head("start", NULL);
    ja("cards");
    for (int i = 0; i < n; i++) {
        jo(NULL);
        js("uuid", cards[i].uuid);
        js("bus", cards[i].bus);
        dev[i] = card_handle(&cards[i], &r);
        if (!dev[i]) {
            lost[i] = 1;
            js("state", r == NVML_ERROR_GPU_IS_LOST ? "lost" : "error");
            js("error", nvml_code(r));
            jeo();
            continue;
        }
        unsigned long long supported = 0, want = NVML_EVENT_XID | NVML_EVENT_DOUBLE_BIT_ECC;
        nv.nvmlDeviceGetSupportedEventTypes(dev[i], &supported);
        r = nv.nvmlDeviceRegisterEvents(dev[i], want & supported, set);
        js("state", "ok");
        ja("events");
        if (r == NVML_SUCCESS) {
            if (want & supported & NVML_EVENT_XID) js(NULL, "xid");
            if (want & supported & NVML_EVENT_DOUBLE_BIT_ECC) js(NULL, "ecc_double");
        }
        jea();
        if (r != NVML_SUCCESS) js("eventsError", nvml_code(r));
        jeo();
    }
    jea();
    jeo();
    jline();

    double next_sample = now_mono_ms() + every_ms;
    for (;;) {
        nvmlEventData_t ev;
        r = nv.nvmlEventSetWait_v2(set, &ev, 1000);
        if (r == NVML_SUCCESS) {
            int i = 0;
            while (i < n && dev[i] != ev.device) i++;
            const Card *c = i < n ? &cards[i] : NULL;
            if (ev.eventType == NVML_EVENT_XID) {
                event_head("xid", c);
                ju("xid", ev.eventData);
                js("meaning", xid_meaning(ev.eventData));
            } else {
                event_head("ecc_double", c);
                ju("data", ev.eventData);
            }
            jeo();
            jline();
        } else if (r != NVML_ERROR_TIMEOUT) {
            event_head("wait_error", NULL);
            js("error", nvml_code(r));
            jeo();
            jline();
            sleep(1);
        }
        // Health: a card that stops answering is reported once, the moment it is seen.
        for (int i = 0; i < n; i++) {
            if (lost[i]) continue;
            unsigned int t;
            r = card_temp(dev[i], &t);
            if (r == NVML_ERROR_GPU_IS_LOST || r == NVML_ERROR_DRIVER_NOT_LOADED) {
                lost[i] = 1;
                event_head("lost", &cards[i]);
                js("error", nvml_code(r));
                jeo();
                jline();
            }
        }
        if (every_ms > 0 && now_mono_ms() >= next_sample) {
            next_sample += every_ms;
            for (int i = 0; i < n; i++) {
                if (lost[i]) continue;
                event_head("sample", &cards[i]);
                unsigned int v, mw;
                nvmlUtilization_t u;
                unsigned long long reasons;
                if (card_temp(dev[i], &v) == NVML_SUCCESS) ju("tempC", v);
                if (nv.nvmlDeviceGetPowerUsage(dev[i], &mw) == NVML_SUCCESS) jf("powerW", mw / 1000.0);
                if (nv.nvmlDeviceGetClockInfo(dev[i], NVML_CLOCK_SM, &v) == NVML_SUCCESS) ju("smMHz", v);
                if (nv.nvmlDeviceGetUtilizationRates(dev[i], &u) == NVML_SUCCESS) ju("utilPct", u.gpu);
                if (nv.nvmlDeviceGetFanSpeed(dev[i], &v) == NVML_SUCCESS) ju("fanPct", v);
                unsigned long long mj;
                if (nv.nvmlDeviceGetTotalEnergyConsumption(dev[i], &mj) == NVML_SUCCESS) jf("energyJ", mj / 1000.0);
                if (nv.nvmlDeviceGetPcieReplayCounter(dev[i], &v) == NVML_SUCCESS) ju("pcieReplays", v);
                if (card_reasons(dev[i], &reasons) == NVML_SUCCESS) write_reasons("limits", reasons);
                jeo();
                jline();
            }
        }
    }
}

// ---- main -------------------------------------------------------------------------------------

static void usage(void) {
    fputs("usage: nvq [--deadline-ms N] list | probe <GPU-uuid|all> | watch [--every-ms N] | schema | version\n"
          "       nvq schema describes every command, its output and its exit codes as JSON Schema\n",
          stderr);
    exit(EXIT_USAGE);
}

static long number(const char *s) {
    char *end;
    long v = strtol(s, &end, 10);
    if (*s == 0 || *end != 0 || v < 0) usage();
    return v;
}

int main(int argc, char **argv) {
    long deadline = -1;
    int i = 1;
    if (i + 1 < argc && !strcmp(argv[i], "--deadline-ms")) deadline = number(argv[i + 1]), i += 2;
    if (i >= argc) usage();
    const char *cmd = argv[i++];
    if (!strcmp(cmd, "schema")) {
        if (i != argc) usage();
        fwrite(schema_json, 1, sizeof schema_json, stdout);
        fflush(stdout);
        return EXIT_OK;
    }
    if (!strcmp(cmd, "version")) {
        if (i != argc) usage();
        jo(NULL);
        ju("nvq", NVQ_SCHEMA);
        js("version", NVQ_VERSION);
        jeo();
        jline();
        return EXIT_OK;
    }
    if (!strcmp(cmd, "list")) {
        if (i != argc) usage();
        arm_deadline(deadline < 0 ? 10000 : deadline);
        return cmd_list();
    }
    if (!strcmp(cmd, "probe")) {
        if (i + 1 != argc) usage();
        long d = deadline < 0 ? 60000 : deadline;
        if (!strcmp(argv[i], "all")) return cmd_probe_all(d);
        if (strncmp(argv[i], "GPU-", 4) || strlen(argv[i]) != 40) usage();
        arm_deadline(d);
        return cmd_probe_one(argv[i]);
    }
    if (!strcmp(cmd, "watch")) {
        long every = 0;
        if (i + 2 == argc && !strcmp(argv[i], "--every-ms")) every = number(argv[i + 1]);
        else if (i != argc) usage();
        if (deadline >= 0) usage(); // watch runs until killed
        cmd_watch(every);
    }
    usage();
}
