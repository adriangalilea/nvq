// Package nvq runs the nvq binary and decodes what it says about NVIDIA cards. On linux/amd64 and
// linux/arm64 the binary ships inside this module (binary.go), so importing it is the whole install.
//
//	var n nvq.NVQ                  // the module's own binary; NVQ{Path: …} runs another
//	list, err := n.List(ctx)       // every card, lost ones included; no CUDA context opened
//	probes, err := n.ProbeAll(ctx) // does each card run a kernel, and its capability sheet
//	for ev, err := range n.Watch(ctx, time.Second) { … }  // Xid events, lost cards, samples
//
// The document types (documents.go) are generated from the JSON Schema the binary prints with
// `nvq schema` (../schema/nvq.schema.json) by `make types`; CI regenerates them and fails on any
// difference. A value the card cannot report is a nil pointer, never zero. This file only runs the
// process and tells documents apart: a Probe is a ProbeOK or a ProbeFailed, an Event one of the
// Event* types, so a type switch reads them.
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

// NVQ is the binary to run: Path, or when empty the one this module carries (Binary).
type NVQ struct {
	Path string
	// Deadline bounds one list or probe inside nvq itself (--deadline-ms); 0 keeps nvq's default
	// (10 s list, 60 s probe). The context bounds the whole process from outside.
	Deadline time.Duration
}

// Error is a failure nvq reported as a document (an ErrorDocument, or list's error when NVML is
// unusable), with nvq's exit code.
type Error struct {
	Code   Code
	Detail string
	Exit   int
}

func (e *Error) Error() string { return fmt.Sprintf("nvq: %s: %s (exit %d)", e.Code, e.Detail, e.Exit) }

// List returns every card the kernel bound. A lost card is a Card with State "lost", not an error;
// NVML being unusable returns the kernel's roster (State "unknown") together with an *Error.
func (n NVQ) List(ctx context.Context) (ListOK, error) {
	var l ListOK
	out, code, err := n.run(ctx, n.deadlineArgs("list")...)
	if err != nil {
		return l, err
	}
	h, err := peek(out)
	if err != nil {
		return l, err
	}
	if h.Cards == nil {
		return l, failure(out, code)
	}
	if err := decode(out, &l); err != nil {
		return l, err
	}
	if l.Error != nil {
		return l, &Error{Code: l.Error.Code, Detail: l.Error.Detail, Exit: code}
	}
	return l, nil
}

// Probe runs a kernel on one card, with only that card visible to CUDA. It opens a CUDA context: do
// not aim it at a card mid-work you care about. The answer is a ProbeOK, or a ProbeFailed naming the
// step: a failed step is an answer, not an error.
func (n NVQ) Probe(ctx context.Context, uuid string) (Probe, error) {
	out, code, err := n.run(ctx, n.deadlineArgs("probe", uuid)...)
	if err != nil {
		return nil, err
	}
	return probe(out, code)
}

// ProbeAll probes every card at once, each in its own process: a hung card cannot stall the others.
// An entry is the child's own document: a ProbeOK, a ProbeFailed, or an ErrorDocument for a child
// that failed as a whole.
func (n NVQ) ProbeAll(ctx context.Context) ([]Probe, error) {
	out, code, err := n.run(ctx, n.deadlineArgs("probe", "all")...)
	if err != nil {
		return nil, err
	}
	h, err := peek(out)
	if err != nil {
		return nil, err
	}
	if h.Probes == nil {
		return nil, failure(out, code)
	}
	var entries []json.RawMessage
	if err := decode(h.Probes, &entries); err != nil {
		return nil, err
	}
	probes := make([]Probe, len(entries))
	for i, e := range entries {
		p, err := entry(e, code)
		if err != nil {
			return nil, err
		}
		probes[i] = p
	}
	return probes, nil
}

// Watch streams events until ctx ends or nvq exits. every > 0 adds an EventSample per card at that
// interval. The sequence ends with a non-nil error unless ctx was cancelled.
func (n NVQ) Watch(ctx context.Context, every time.Duration) iter.Seq2[Event, error] {
	return func(yield func(Event, error) bool) {
		args := []string{"watch"}
		if every > 0 {
			args = append(args, "--every-ms", strconv.FormatInt(every.Milliseconds(), 10))
		}
		bin, err := n.path()
		if err != nil {
			yield(nil, err)
			return
		}
		cmd := exec.CommandContext(ctx, bin, args...)
		stdout, err := cmd.StdoutPipe()
		if err != nil {
			yield(nil, err)
			return
		}
		var stderr bytes.Buffer
		cmd.Stderr = &stderr
		if err := cmd.Start(); err != nil {
			yield(nil, err)
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
			line := bytes.Clone(sc.Bytes())
			h, err := peek(line)
			if err != nil {
				stop() //nolint:errcheck
				yield(nil, err)
				return
			}
			if h.Event == "" {
				cmd.Wait() //nolint:errcheck // nvq exits after an error document; its code is the point
				yield(nil, failure(line, cmd.ProcessState.ExitCode()))
				return
			}
			kind, known := eventKinds[h.Event]
			if !known {
				stop() //nolint:errcheck
				yield(nil, fmt.Errorf("nvq: event %q is not in this package's schema", h.Event))
				return
			}
			ev, err := kind(line)
			if err != nil {
				stop() //nolint:errcheck
				yield(nil, fmt.Errorf("nvq: decoding %.200s: %w", line, err))
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
		yield(nil, fmt.Errorf("nvq watch ended: %v: %s", err, bytes.TrimSpace(stderr.Bytes())))
	}
}

// eventKinds maps each "event" value to its generated type. A test holds its keys to the schema's
// event definitions, so a kind added there fails until it is added here.
var eventKinds = map[string]func([]byte) (any, error){
	"start":      one[EventStart],
	"xid":        one[EventXid],
	"ecc_double": one[EventECCDouble],
	"lost":       one[EventLost],
	"wait_error": one[EventWaitError],
	"sample":     one[EventSample],
}

// ---- plumbing ---------------------------------------------------------------------------------

// head is what tells documents apart: an ErrorDocument has error and none of the others.
type head struct {
	NVQ    int             `json:"nvq"`
	Event  string          `json:"event"`
	OK     *bool           `json:"ok"`
	Cards  json.RawMessage `json:"cards"`
	Probes json.RawMessage `json:"probes"`
}

func peek(doc []byte) (head, error) {
	var h head
	if err := json.Unmarshal(doc, &h); err != nil {
		return h, fmt.Errorf("nvq: not JSON: %.200s: %w", doc, err)
	}
	if h.NVQ != Schema {
		return h, fmt.Errorf("nvq: document version %d, this package reads %d: %.200s", h.NVQ, Schema, doc)
	}
	return h, nil
}

func probe(doc []byte, code int) (Probe, error) {
	h, err := peek(doc)
	if err != nil {
		return nil, err
	}
	switch {
	case h.OK == nil:
		return nil, failure(doc, code)
	case *h.OK:
		return one[ProbeOK](doc)
	default:
		return one[ProbeFailed](doc)
	}
}

// one decodes doc as a T, the generated type's own checks included: a Probe or an Event holding it,
// or nil and the error.
func one[T any](doc []byte) (any, error) {
	var v T
	if err := decode(doc, &v); err != nil {
		return nil, err
	}
	return v, nil
}

// entry is one of probe all's: a child that failed as a whole left an ErrorDocument, kept as such.
func entry(doc []byte, code int) (Probe, error) {
	h, err := peek(doc)
	if err != nil {
		return nil, err
	}
	if h.OK != nil {
		return probe(doc, code)
	}
	return one[ErrorDocument](doc)
}

// failure turns an ErrorDocument into an *Error; a document that is not one is a decoding error.
func failure(doc []byte, code int) error {
	var d ErrorDocument
	if err := decode(doc, &d); err != nil {
		return err
	}
	return &Error{Code: d.Error.Code, Detail: d.Error.Detail, Exit: code}
}

func decode(doc []byte, v any) error {
	if err := json.Unmarshal(doc, v); err != nil {
		return fmt.Errorf("nvq: decoding %.200s: %w", doc, err)
	}
	return nil
}

func (n NVQ) path() (string, error) {
	if n.Path == "" {
		return Binary()
	}
	return n.Path, nil
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
	bin, err := n.path()
	if err != nil {
		return nil, 0, err
	}
	cmd := exec.CommandContext(ctx, bin, args...)
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
