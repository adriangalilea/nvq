# nvq

NVIDIA cards as JSON, for programs. One binary, no driver needed to start, every failure a stable code, every call bounded by a deadline.

```sh
nvq list
```

```json
{
  "nvq": 1,
  "driver": "590.48.01",
  "nvml": "13.590.48.01",
  "cuda": "13.1",
  "cards": [
    {
      "index": 0,
      "uuid": "GPU-00000000-1111-2222-3333-444444444444",
      "bus": "0000:01:00.0",
      "minor": 0,
      "model": "NVIDIA GeForce RTX 3090",
      "state": "ok",
      "compute": "8.6",
      "memoryMiB": { "total": 24576, "used": 2957 },
      "powerW": { "limit": 260.000, "draw": 230.728 },
      "tempC": 49,
      "fanPct": 70,
      "clocksMHz": { "sm": 1500, "mem": 10001 },
      "utilPct": { "gpu": 100, "mem": 2 },
      "pstate": 2,
      "pcie": { "gen": 4, "width": 16, "maxGen": 4, "maxWidth": 16, "replays": 0 },
      "energyJ": 15640697.951,
      "persistence": true,
      "limits": [],
      "processes": [{ "pid": 2133245, "usedMiB": 2500 }]
    }
  ]
}
```

Real output from an RTX 3090 ([`test/fixtures/list.real__list.json`](test/fixtures/list.real__list.json)), indented here; nvq prints one line. A card that fell off the bus keeps its identity (`uuid`, `bus`, `model`) and says why, `"state": "lost", "error": "gpu_is_lost"`, with nothing it can no longer read.

## Why

Programs that run on GPU machines (miners, schedulers, health checks, stats exporters) need facts about the cards: which exist, which fell off the bus, what each can do, what is holding it. Three things decide how to get them.

- **A dead card hangs the caller.** When a card falls off the PCIe bus, NVML and CUDA calls on it can block inside the kernel driver forever. In-process, that freezes the program asking. Out of process, the caller kills a child at a deadline and carries on with the other cards. This is the reason nvq is a separate binary and not a library.
- **The answer must be readable by anything.** A rig's supervisor may be Go today, a bash hook beside it, a Python tool for analysis, something else next year. JSON on stdout, with a schema, is the one interface all of them read the same way.
- **Failures must be data.** "Driver/library version mismatch" after an upgrade without a reboot, a card lost, a field the card cannot report: each needs a stable name a program can branch on, never prose to parse.

## Alternatives considered

| | What it is | Why not, for this job |
|---|---|---|
| **nvidia-smi** | NVIDIA's CLI over NVML | Made for people: tables, `[N/A]` and `[Not Supported]` in place of numbers, output that shifts between drivers, error text to parse. Hangs with the card, and `--query-gpu` has no event stream (Xid) and no capability sheet. |
| **[NVIDIA/go-nvml](https://github.com/NVIDIA/go-nvml)** | Official Go bindings to NVML, generated from `nvml.h` | The right pick for a Go program that wants NVML in-process and accepts cgo. It costs what nvq exists for: a stuck call blocks the goroutine forever (the program still needs a watchdog process); cgo ends static cross-builds (`GOOS=linux go build` from a laptop); only Go can use it. nvq took its best idea: declarations checked against NVIDIA's own header (below). |
| **[gorgonia/cu](https://github.com/gorgonia/cu)** | Go bindings to the CUDA driver API | A different job: launching your own kernels from Go. No NVML (no temperature, power, holders, Xid, lost cards), cgo plus the CUDA toolkit to build, in-process. |
| **DCGM** | NVIDIA's datacenter GPU manager | The industrial version of `nvq watch`: health policies, diagnostics, a daemon. Heavy for a mining rig or a workstation, and a service to run, not a tool to call. nvq borrows its sense of what matters (Xid, ECC, PCIe replays, energy). |
| **pynvml / nvidia-ml-py** | Python bindings to NVML | Python only, in-process, the same hang. |

nvq's own costs, stated: a process per query (about 12 ms for `list` on an RTX 3090; `watch` is one long-lived process), a second binary to ship, and the NVML and CUDA declarations it keeps itself (about fifty functions and structs, checked against NVIDIA's headers in CI).

## Layout

| | |
|---|---|
| `src/` | nvq itself, C11, no dependency beyond libc and the driver's own libraries, loaded at run time |
| `schema/` | the contract every output follows, compiled into the binary |
| `test/` | the ground truth every client is held to: scripted driver libraries that stage each failure, and `fixtures/`, real output from them and from real cards |
| `go/` | the Go client, with the released binaries embedded |
| `python/` | the Python client, types generated from the schema; its wheels carry the binary |

## Install

Linux, amd64 or arm64, glibc 2.17 or newer (every distro an NVIDIA driver supports):

```
curl -fsSLO https://github.com/adriangalilea/nvq/releases/latest/download/nvq-linux-amd64
curl -fsSL https://github.com/adriangalilea/nvq/releases/latest/download/nvq-linux-amd64.sha256 | sha256sum -c
install -m 755 nvq-linux-amd64 /usr/local/bin/nvq
```

Pin a version in anything you ship: `releases/download/v0.3.0/…`. Each release also carries `nvq.schema.json`. A Go or Python program needs none of this: its client carries the binary (below). `pip install nvq` (or `uv tool install nvq`) also puts `nvq` on PATH. Building yourself: `make` (a C11 compiler, nothing else).

## Commands

| Command | Answers | Touches the card |
|---|---|---|
| `nvq list` | every card the kernel driver bound, lost ones included, with NVML's view of each: compute capability, memory, power, energy used, temperature, fan, clocks, utilisation, PCIe link and its replay count (a riser shows as gen 1 x1, a failing one as rising replays), persistence, clock limits, the processes holding it | no: NVML reads only, safe on a busy card |
| `nvq probe <GPU-uuid>` | can it compute: CUDA init with only that card visible, context, allocation, PTX JIT, kernel launch, result verified, each step timed; plus the card's capability sheet (SMs, warps, registers, shared memory, L2, bus width, peak bandwidth) | yes: a short CUDA context |
| `nvq probe all` | probe on every card at once, each in its own process: a hung card cannot stall the others | yes |
| `nvq watch [--every-ms N]` | one JSON line per event until killed: Xid errors (named: 79 is "gpu has fallen off the bus"), double-bit ECC, a card lost the moment NVML stops reaching it, optional samples | no |
| `nvq schema` | the contract below, as JSON | no |
| `nvq version` | `{"nvq":1,"version":"v0.3.0"}` | no |

`--deadline-ms N` (before the command) bounds `list` (default 10 s) and `probe` (60 s). A driver call that does not return ends there with `{"nvq":1,"error":{"code":"deadline",…}}`. One stuck inside the kernel cannot be interrupted at all: `probe all` reports that child as `stuck_in_driver` with its pid instead of hanging with it.

## The contract

`nvq schema` prints a JSON Schema (2020-12) of every output, plus `x-commands`: each command's usage, default deadline and the meaning of every exit code. It is the same file as [`schema/nvq.schema.json`](schema/nvq.schema.json), compiled into the binary, so a program can ask the binary it has what it speaks.

- `"nvq": 1` is the schema version. Fields are only ever added under it; a removal or a change of meaning bumps it. Ignore keys you do not know.
- A value the card cannot report is absent, never 0, and its name is listed in `unsupported`. A query that failed is under `errors` with its code.
- Failures are codes, not prose: NVML's return codes by name (`gpu_is_lost`, `lib_rm_version_mismatch`, `driver_not_loaded`, …) and CUDA's error names (`CUDA_ERROR_INVALID_PTX`).
- A lost card is a fact in `list` (`"state":"lost"`), not a failed command. NVML unusable still lists every card from the kernel, `"state":"unknown"`, with the reason in `error`.
- Counters (`energyJ`, `pcie.replays`) are cumulative since the driver loaded: take differences between two readings.

Exit codes: 0 ok, 1 a card is lost or erred (the document says which), 2 usage, 3 driver or NVML unusable, 4 a probe failed, 5 deadline.

## Clients

The product is the binary and its schema; a client is a convenience. It runs nvq, reads one JSON document (a line, for `watch`), and turns an error document into the language's error, so any language gets one in a page: roll your own wherever you are, generating its types from `nvq schema`. The two here exist to carry the binary into their package managers.

No client writes a document type by hand. `make types` generates every client's types from `schema/nvq.schema.json`, they are committed, and CI regenerates them and fails on any difference: a field added to the schema reaches every client in the same commit, and none can drift. What a client writes by hand is only what no schema change touches: running the process and telling documents apart. Each is a directory at the root, held to the same ground truth: every file in `test/fixtures` validates against the schema and reads through the client, and the binary a client ships prints exactly that schema.

**Go** (`go/`, module `github.com/adriangalilea/nvq/go`, package `nvq`): `go get github.com/adriangalilea/nvq/go@v0.3.0` is the whole install. The module carries the release's binaries for linux/amd64 and linux/arm64; the first call writes the one for this machine into the user cache under its sha256 and every later call reuses it, after checking its bytes. `go.sum` pins those bytes, so a program runs exactly the nvq its version was released with: no download at run time, no compiler, nothing on PATH. `NVQ{Path: …}` runs another binary instead.

```go
var n nvq.NVQ
list, err := n.List(ctx)
probes, err := n.ProbeAll(ctx)
for ev, err := range n.Watch(ctx, 10*time.Second) {
	switch e := ev.(type) {
	case nvq.EventXid:
		log.Print(e.Xid, e.Meaning)
	case nvq.EventLost:
		…
	}
}
```

Types are generated by [go-jsonschema](https://github.com/atombender/go-jsonschema) into `go/documents.go`, one per schema definition: optional values are pointers, enums are typed constants (`nvq.CodeGPUIsLost`), and decoding checks required fields, constants, enums and patterns. A `Probe` is a `ProbeOK` or a `ProbeFailed` (a failed step is an answer, not an error), an `Event` one of the `Event*` types, read with a type switch. `*nvq.Error` carries the code and exit for a command that failed as a whole. The one list kept by hand, `"event"` value to type, is tested against the schema's event definitions.

**Python** (`python/`, package `nvq` on PyPI, 3.12+): `pip install nvq` is the whole install. The wheels for linux x86_64 and aarch64 carry that release's binary, so a pinned version runs exactly the nvq it was released with; any other platform gets a wheel without one, and `NVQ(path=…)` names a binary there.

```python
n = nvq.NVQ()
cards = n.list(timeout=15)["cards"]
probes = n.probe_all()
for ev in n.watch(every=10.0): …
```

Documents are the parsed JSON, keys as the contract names them (`card["tempC"]`); an absent key is a value the card cannot report. Their types are `TypedDict`s generated by [datamodel-code-generator](https://github.com/koxudaxi/datamodel-code-generator) into `python/src/nvq/documents.py`, so a type checker knows every key, every error code and every event kind, and narrows on `ev["event"]` and `p["ok"]`. `nvq.Error` carries the code and exit; `timeout` kills nvq from outside.

TypeScript and the rest: no client yet. The first project that needs one adds it the same way, types generated from the schema, held to the fixtures.

## Releasing

`gh workflow run release.yml -f version=vX.Y.Z`. CI builds both binaries and the three Python wheels, runs both clients' tests against the binaries, places them in `go/bin` and commits them if their bytes changed, tags `vX.Y.Z` (the binary) and `go/vX.Y.Z` (the Go module), publishes the GitHub release, then the wheels to PyPI (Trusted Publishing: the `pypi` environment, no stored token).

## How it stays correct

- `make test` stages every failure nvq names (a version mismatch after a driver upgrade, a card off the bus, a hung call, a call deaf to signals, a driver older than nvq, a PTX JIT failure) with scripted `libnvidia-ml.so.1` and `libcuda.so.1` from `test/`, on a Linux box with the driver loaded. `test/run.sh --real` adds the box's real cards, read-only.
- `NVQ_FIXTURES=dir` keeps every output (card UUIDs masked); those are `test/fixtures`, which every client's test validates against the schema and reads through the client.
- `make types` regenerates every client's types from the schema; CI fails if that changes a committed file.
- `make layout` compiles nvq's NVML and CUDA declarations against NVIDIA's own headers: struct layouts, constants and every function signature (each function nvq calls is assigned to a pointer of the signature nvq declares, so a wrong parameter is a compile error). CI runs it against CUDA 12.5, 12.9, 13.1 and 13.4. Where NVML replaced a function (`nvmlDeviceGetTemperature` by the versioned `nvmlDeviceGetTemperatureV` in 12.9, the throttle-named reasons query by the event-named one in driver 535), nvq calls the replacement when the driver exports it and the old one otherwise, and both signatures are checked; a new deprecation in a future header prints as a warning, the next migration.
- Releases are built in manylinux2014 and CI asserts no symbol newer than glibc 2.17.
