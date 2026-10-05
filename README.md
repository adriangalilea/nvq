# nvq

NVIDIA cards as JSON, for programs. One static-ish binary, no driver needed to start, every failure a stable code.

nvidia-smi is a tool for people: text tables, `[N/A]` in place of numbers, a hang when a card falls off the bus, and a different shape per driver. nvq asks the same libraries (NVML, the CUDA driver API) and answers in one documented JSON shape, with a deadline on every call and each card's probe in its own process.

```
$ nvq list
{"nvq":1,"driver":"590.48.01","nvml":"13.590.48.01","cuda":"13.1","cards":[{"index":0,
 "uuid":"GPU-00000000-1111-2222-3333-444444444444","bus":"0000:01:00.0","minor":0,
 "model":"NVIDIA GeForce RTX 3090","state":"ok","compute":"8.6","memoryMiB":{"total":24576,"used":1503},
 "powerW":{"limit":260.000,"draw":128.180},"tempC":44,"fanPct":70,"clocksMHz":{"sm":1500,"mem":10001},
 "utilPct":{"gpu":100,"mem":0},"pstate":2,"pcie":{"gen":4,"width":16,"maxGen":4,"maxWidth":16},
 "persistence":true,"limits":[],"processes":[{"pid":2073697,"usedMiB":1046}]}]}
```

## Install

Linux, amd64 or arm64, glibc 2.17 or newer (every distro an NVIDIA driver supports):

```
curl -fsSLO https://github.com/adriangalilea/nvq/releases/latest/download/nvq-linux-amd64
curl -fsSL https://github.com/adriangalilea/nvq/releases/latest/download/nvq-linux-amd64.sha256 | sha256sum -c
install -m 755 nvq-linux-amd64 /usr/local/bin/nvq
```

Pin a version in anything you ship: `releases/download/v0.1.0/…`. Building yourself: `make` (a C11 compiler, nothing else).

## Commands

| Command | Answers | Touches the card |
|---|---|---|
| `nvq list` | every card the kernel driver bound, lost ones included, with NVML's view of each: compute capability, memory, power, temperature, fan, clocks, utilisation, PCIe link (a riser shows as gen 1 x1), persistence, clock limits, the processes holding it | no: NVML reads only, safe on a busy card |
| `nvq probe <GPU-uuid>` | can it compute: CUDA init with only that card visible, context, allocation, PTX JIT, kernel launch, result verified, each step timed; plus the card's capability sheet (SMs, warps, registers, shared memory, L2, bus width, peak bandwidth) | yes: a short CUDA context |
| `nvq probe all` | probe on every card at once, each in its own process: a hung card cannot stall the others | yes |
| `nvq watch [--every-ms N]` | one JSON line per event until killed: Xid errors (named: 79 is "gpu has fallen off the bus"), double-bit ECC, a card lost the moment NVML stops reaching it, optional samples | no |
| `nvq schema` | the contract below, as JSON | no |
| `nvq version` | `{"nvq":1,"version":"v0.1.0"}` | no |

`--deadline-ms N` (before the command) bounds `list` (default 10 s) and `probe` (60 s). A driver call that does not return ends there with `{"nvq":1,"error":{"code":"deadline",…}}`. One stuck inside the kernel cannot be interrupted at all: `probe all` reports that child as `stuck_in_driver` with its pid instead of hanging with it.

## The contract

`nvq schema` prints a JSON Schema (2020-12) of every output, plus `x-commands`: each command's usage, default deadline and the meaning of every exit code. It is the same file as [`schema/nvq.schema.json`](schema/nvq.schema.json), compiled into the binary, so a program can ask the binary it has what it speaks. Generate types for your language from it; the Go types in this repo are tested against it.

Rules every document follows:

- `"nvq": 1` is the schema version. Fields are only ever added under it; a removal or a change of meaning bumps it. Ignore keys you do not know.
- A value the card cannot report is absent, never 0, and its name is listed in `unsupported`. A query that failed is under `errors` with its code.
- Failures are codes, not prose: NVML's return codes by name (`gpu_is_lost`, `lib_rm_version_mismatch`, `driver_not_loaded`, …) and CUDA's error names (`CUDA_ERROR_INVALID_PTX`).
- A lost card is a fact in `list` (`"state":"lost"`), not a failed command. NVML unusable still lists every card from the kernel, `"state":"unknown"`, with the reason in `error`.

Exit codes: 0 ok, 1 a card is lost or erred (the document says which), 2 usage, 3 driver or NVML unusable, 4 a probe failed, 5 deadline.

## Go

```go
import "github.com/adriangalilea/nvq"

n := nvq.NVQ{Path: "/usr/local/bin/nvq"}
list, err := n.List(ctx)
probes, err := n.ProbeAll(ctx)
for ev, err := range n.Watch(ctx, 10*time.Second) { … }
```

Optional values are pointers. A failed probe step is a `Probe` with `OK: false`; `*nvq.Error` carries the code and exit for a command that failed as a whole.

## Testing

`make test` stages every failure nvq names (a version mismatch after a driver upgrade, a card off the bus, a hung call, a call deaf to signals, a driver older than nvq, a PTX JIT failure) with scripted `libnvidia-ml.so.1` and `libcuda.so.1` from `test/`, on a Linux box with the driver loaded. `test/run.sh --real` adds the box's real cards, read-only. `NVQ_FIXTURES=dir` keeps every output; those are `test/fixtures`, which `go test` validates against the schema and decodes strictly into the Go types. `test/layout.c` checks nvq's NVML declarations against NVIDIA's own header.
