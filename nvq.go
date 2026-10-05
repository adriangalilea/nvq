// Package nvq runs the nvq binary and decodes what it says about NVIDIA cards.
//
//	n := nvq.NVQ{Path: "/opt/tool/nvq"}
//	list, err := n.List(ctx)       // every card, lost ones included; no CUDA context opened
//	probe, err := n.ProbeAll(ctx)  // does each card run a kernel, and its capability sheet
//	for ev, err := range n.Watch(ctx, time.Second) { … }  // Xid events, lost cards, samples
//
// A value the card cannot report is a nil pointer, never zero. The contract is the JSON Schema the
// binary prints with `nvq schema` (schema/nvq.schema.json); these types mirror it, and the tests hold
// both to the binary's real output.
package nvq

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"iter"
	"os/exec"
	"strconv"
	"time"
)

// Schema is the document version this package reads.
const Schema = 1

// NVQ is a path to the binary. The zero value runs "nvq" from PATH.
type NVQ struct {
	Path string
	// Deadline bounds one list or probe inside nvq itself (--deadline-ms); 0 keeps nvq's default
	// (10 s list, 60 s probe). The context bounds the whole process from outside.
	Deadline time.Duration
}

// Error is a failure nvq reported as a document, or the process failing to produce one.
type Error struct {
	Code   string `json:"code"`   // a "code" from the schema, e.g. driver_not_loaded, lib_rm_version_mismatch, deadline
	Detail string `json:"detail"` //
	Exit   int    `json:"-"`      // nvq's exit code
}

func (e *Error) Error() string { return fmt.Sprintf("nvq: %s: %s (exit %d)", e.Code, e.Detail, e.Exit) }

// ---- list -------------------------------------------------------------------------------------

type List struct {
	NVQ    int    `json:"nvq"`
	Driver string `json:"driver"`
	NVML   string `json:"nvml,omitempty"`
	CUDA   string `json:"cuda,omitempty"`
	Error  *Error `json:"error,omitempty"` // NVML unusable: every card's State is "unknown"
	Cards  []Card `json:"cards"`
}

type Card struct {
	Index       int               `json:"index"`
	UUID        string            `json:"uuid"`
	Bus         string            `json:"bus"`
	Minor       int               `json:"minor"`
	Model       string            `json:"model"`
	State       string            `json:"state"` // ok | lost | error | unknown
	Error       string            `json:"error,omitempty"`
	Compute     string            `json:"compute,omitempty"`
	MemoryMiB   *CardMemory       `json:"memoryMiB,omitempty"`
	PowerW      *Power            `json:"powerW,omitempty"`
	TempC       *int              `json:"tempC,omitempty"`
	FanPct      *int              `json:"fanPct,omitempty"`
	ClocksMHz   *Clocks           `json:"clocksMHz,omitempty"`
	UtilPct     *Util             `json:"utilPct,omitempty"`
	PState      *int              `json:"pstate,omitempty"`
	PCIe        *PCIe             `json:"pcie,omitempty"`
	Persistence *bool             `json:"persistence,omitempty"`
	Limits      []string          `json:"limits,omitempty"`
	Processes   []Process         `json:"processes,omitempty"`
	Unsupported []string          `json:"unsupported,omitempty"`
	Errors      map[string]string `json:"errors,omitempty"`
}

type CardMemory struct {
	Total int64 `json:"total"`
	Used  int64 `json:"used"`
}

type Clocks struct {
	SM  int `json:"sm"`
	Mem int `json:"mem"`
}

type Util struct {
	GPU int `json:"gpu"`
	Mem int `json:"mem"`
}

type Power struct {
	Limit *float64 `json:"limit,omitempty"`
	Draw  *float64 `json:"draw,omitempty"`
}

type PCIe struct {
	Gen      int `json:"gen"`
	Width    int `json:"width"`
	MaxGen   int `json:"maxGen"`
	MaxWidth int `json:"maxWidth"`
}

type Process struct {
	PID     int    `json:"pid"`
	UsedMiB *int64 `json:"usedMiB,omitempty"`
}

// List returns every card the kernel bound. A lost card is a Card with State "lost", not an error;
// NVML being unusable returns the kernel's roster (State "unknown") together with an *Error.
func (n NVQ) List(ctx context.Context) (List, error) {
	var l List
	out, code, err := n.run(ctx, n.deadlineArgs("list")...)
	if err != nil {
		return l, err
	}
	if e := asError(out, code); e != nil && !bytes.Contains(out, []byte(`"cards"`)) {
		return l, e
	}
	if err := decode(out, &l); err != nil {
		return l, err
	}
	if l.Error != nil {
		l.Error.Exit = code
		return l, l.Error
	}
	return l, nil
}

// ---- probe ------------------------------------------------------------------------------------

// Probe is one card's answer: OK with its capability sheet, or the step that failed and why.
type Probe struct {
	NVQ  int    `json:"nvq"`
	UUID string `json:"uuid"`
	OK   bool   `json:"ok"`

	Step   string `json:"step,omitempty"`  // failed: the step, or hang / crash in ProbeAll
	Error  string `json:"error,omitempty"` // failed: a CUDA_ERROR_* name or nvq's own
	Signal *int   `json:"signal,omitempty"`
	Exit   *int   `json:"exit,omitempty"`
	PID    *int   `json:"pid,omitempty"` // stuck_in_driver: the child nvq could not reap

	Name              string     `json:"name,omitempty"`
	CUDA              string     `json:"cuda,omitempty"`
	Arch              *Arch      `json:"arch,omitempty"`
	Layout            *Layout    `json:"layout,omitempty"`
	Registers         *Registers `json:"registers,omitempty"`
	SharedBytes       *Shared    `json:"sharedBytes,omitempty"`
	Memory            *Memory    `json:"memory,omitempty"`
	SMClockMHz        *int       `json:"smClockMHz,omitempty"`
	CopyEngines       *int       `json:"copyEngines,omitempty"`
	ConcurrentKernels *bool      `json:"concurrentKernels,omitempty"`
	ComputeMode       string     `json:"computeMode,omitempty"`
	ECC               *bool      `json:"ecc,omitempty"`
	Integrated        *bool      `json:"integrated,omitempty"`
	MultiGPUBoard     *bool      `json:"multiGpuBoard,omitempty"`
	Unsupported       []string   `json:"unsupported,omitempty"`

	MS map[string]float64 `json:"ms,omitempty"` // milliseconds per step
}

type Arch struct {
	Compute string `json:"compute"` // "8.6"
	SM      string `json:"sm"`      // "sm_86"
	Family  string `json:"family"`  // "ampere"
}

type Layout struct {
	SMs                *int    `json:"sms,omitempty"`
	WarpSize           *int    `json:"warpSize,omitempty"`
	MaxThreadsPerBlock *int    `json:"maxThreadsPerBlock,omitempty"`
	MaxThreadsPerSM    *int    `json:"maxThreadsPerSM,omitempty"`
	MaxBlocksPerSM     *int    `json:"maxBlocksPerSM,omitempty"`
	MaxWarpsPerSM      *int    `json:"maxWarpsPerSM,omitempty"`
	MaxBlockDim        *[3]int `json:"maxBlockDim,omitempty"`
	MaxGridDim         *[3]int `json:"maxGridDim,omitempty"`
}

type Registers struct {
	PerBlock *int `json:"perBlock,omitempty"`
	PerSM    *int `json:"perSM,omitempty"`
}

type Shared struct {
	PerBlock      *int `json:"perBlock,omitempty"`
	PerBlockOptin *int `json:"perBlockOptin,omitempty"`
	PerSM         *int `json:"perSM,omitempty"`
}

type Memory struct {
	TotalMiB          int64    `json:"totalMiB"`
	BusWidthBits      *int     `json:"busWidthBits,omitempty"`
	ClockMHz          *int     `json:"clockMHz,omitempty"`
	PeakGBs           *float64 `json:"peakGBs,omitempty"`
	L2Bytes           *int64   `json:"l2Bytes,omitempty"`
	L2PersistingBytes *int64   `json:"l2PersistingBytes,omitempty"`
	ConstantBytes     *int64   `json:"constantBytes,omitempty"`
}

// Probe runs a kernel on one card, with only that card visible to CUDA. It opens a CUDA context:
// do not aim it at a card mid-work you care about. A failed step is a Probe with OK false, not an error.
func (n NVQ) Probe(ctx context.Context, uuid string) (Probe, error) {
	var p Probe
	out, code, err := n.run(ctx, n.deadlineArgs("probe", uuid)...)
	if err != nil {
		return p, err
	}
	if e := asError(out, code); e != nil {
		return p, e
	}
	return p, decode(out, &p)
}

// ProbeAll probes every card at once, each in its own process: a hung card cannot stall the others.
func (n NVQ) ProbeAll(ctx context.Context) ([]Probe, error) {
	var all struct {
		NVQ    int     `json:"nvq"`
		Probes []Probe `json:"probes"`
	}
	out, code, err := n.run(ctx, n.deadlineArgs("probe", "all")...)
	if err != nil {
		return nil, err
	}
	if e := asError(out, code); e != nil {
		return nil, e
	}
	if err := decode(out, &all); err != nil {
		return nil, err
	}
	return all.Probes, nil
}

// ---- watch ------------------------------------------------------------------------------------

// Event is one line of watch: start, xid, ecc_double, lost, sample or wait_error.
type Event struct {
	NVQ   int     `json:"nvq"`
	Event string  `json:"event"`
	At    float64 `json:"at"` // unix seconds
	UUID  string  `json:"uuid,omitempty"`
	Bus   string  `json:"bus,omitempty"`

	Cards   []WatchCard `json:"cards,omitempty"`   // start
	Xid     *int        `json:"xid,omitempty"`     // xid
	Meaning string      `json:"meaning,omitempty"` // xid: NVIDIA's name for it, "" when nvq has none
	Data    *uint64     `json:"data,omitempty"`    // ecc_double
	Error   string      `json:"error,omitempty"`   // lost, wait_error

	TempC   *int     `json:"tempC,omitempty"` // sample
	PowerW  *float64 `json:"powerW,omitempty"`
	SMMHz   *int     `json:"smMHz,omitempty"`
	UtilPct *int     `json:"utilPct,omitempty"`
	FanPct  *int     `json:"fanPct,omitempty"`
	Limits  []string `json:"limits,omitempty"`
}

type WatchCard struct {
	UUID        string   `json:"uuid"`
	Bus         string   `json:"bus"`
	State       string   `json:"state"`
	Error       string   `json:"error,omitempty"`
	Events      []string `json:"events,omitempty"`
	EventsError string   `json:"eventsError,omitempty"`
}

// Watch streams events until ctx ends or nvq exits. every > 0 adds a sample per card at that
// interval. The sequence ends with a non-nil error unless ctx was cancelled.
func (n NVQ) Watch(ctx context.Context, every time.Duration) iter.Seq2[Event, error] {
	return func(yield func(Event, error) bool) {
		args := []string{"watch"}
		if every > 0 {
			args = append(args, "--every-ms", strconv.FormatInt(every.Milliseconds(), 10))
		}
		cmd := exec.CommandContext(ctx, n.path(), args...)
		stdout, err := cmd.StdoutPipe()
		if err != nil {
			yield(Event{}, err)
			return
		}
		var stderr bytes.Buffer
		cmd.Stderr = &stderr
		if err := cmd.Start(); err != nil {
			yield(Event{}, err)
			return
		}
		// One exit path: the process is killed (a no-op once it exited) and reaped exactly once.
		stop := func() error {
			cmd.Process.Kill() //nolint:errcheck // already exited is fine
			return cmd.Wait()
		}
		sc := bufio.NewScanner(stdout)
		sc.Buffer(make([]byte, 64<<10), 1<<20)
		for sc.Scan() {
			line := sc.Bytes()
			if e := asError(line, 3); e != nil && !bytes.Contains(line, []byte(`"event"`)) {
				stop() //nolint:errcheck
				yield(Event{}, e)
				return
			}
			var ev Event
			if err := decode(line, &ev); err != nil {
				stop() //nolint:errcheck
				yield(Event{}, err)
				return
			}
			if !yield(ev, nil) {
				stop() //nolint:errcheck
				return
			}
		}
		err = stop()
		if ctx.Err() != nil {
			return
		}
		yield(Event{}, fmt.Errorf("nvq watch ended: %v: %s", err, bytes.TrimSpace(stderr.Bytes())))
	}
}

// ---- plumbing ---------------------------------------------------------------------------------

func (n NVQ) path() string {
	if n.Path == "" {
		return "nvq"
	}
	return n.Path
}

func (n NVQ) deadlineArgs(args ...string) []string {
	if n.Deadline > 0 {
		return append([]string{"--deadline-ms", strconv.FormatInt(n.Deadline.Milliseconds(), 10)}, args...)
	}
	return args
}

// run returns stdout and the exit code. A process that never wrote a document (not found, killed,
// a usage error) is an error here; any other exit is the document's to explain.
func (n NVQ) run(ctx context.Context, args ...string) ([]byte, int, error) {
	cmd := exec.CommandContext(ctx, n.path(), args...)
	var stderr bytes.Buffer
	cmd.Stderr = &stderr
	out, err := cmd.Output()
	code := 0
	var ee *exec.ExitError
	if errors.As(err, &ee) {
		code = ee.ExitCode()
	} else if err != nil {
		return nil, 0, fmt.Errorf("nvq %v: %w", args, err)
	}
	if len(bytes.TrimSpace(out)) == 0 {
		if ctx.Err() != nil {
			return nil, code, fmt.Errorf("nvq %v: %w", args, ctx.Err())
		}
		return nil, code, fmt.Errorf("nvq %v: exit %d, no document: %s", args, code, bytes.TrimSpace(stderr.Bytes()))
	}
	return out, code, nil
}

// asError reads a document that is the {"nvq":1,"error":{…}} shape; nil for any other document.
func asError(doc []byte, code int) *Error {
	var e struct {
		Error *Error `json:"error"`
	}
	if json.Unmarshal(doc, &e) != nil || e.Error == nil {
		return nil
	}
	e.Error.Exit = code
	return e.Error
}

func decode(doc []byte, v any) error {
	if err := json.Unmarshal(doc, v); err != nil {
		return fmt.Errorf("nvq: decoding %.200s: %w", doc, err)
	}
	var head struct{ NVQ int }
	json.Unmarshal(doc, &head) //nolint:errcheck // decoded above
	if head.NVQ != Schema {
		return fmt.Errorf("nvq: document version %d, this package reads %d", head.NVQ, Schema)
	}
	return nil
}
