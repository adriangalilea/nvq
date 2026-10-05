package nvq

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/santhosh-tekuri/jsonschema/v6"
)

// Fixtures are real nvq output, captured by ../test/run.sh (NVQ_FIXTURES) on a box with a card, from the
// scripted failure libraries and the real card. Named <schema def>.<case>.json, or .jsonl for watch.
func fixtures(t *testing.T) map[string][]string {
	t.Helper()
	paths, err := filepath.Glob("../test/fixtures/*.json*")
	if err != nil || len(paths) == 0 {
		t.Fatalf("no fixtures: %v", err)
	}
	byDef := map[string][]string{}
	for _, p := range paths {
		def, _, _ := strings.Cut(filepath.Base(p), ".")
		byDef[def] = append(byDef[def], p)
	}
	return byDef
}

// docs returns a fixture's documents: one, or one per line for watch.
func docs(t *testing.T, path string) [][]byte {
	t.Helper()
	raw, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	var out [][]byte
	sc := bufio.NewScanner(bytes.NewReader(raw))
	sc.Buffer(nil, 1<<20)
	for sc.Scan() {
		if len(bytes.TrimSpace(sc.Bytes())) > 0 {
			out = append(out, bytes.Clone(sc.Bytes()))
		}
	}
	return out
}

func TestEveryFixtureMatchesTheSchema(t *testing.T) {
	c := jsonschema.NewCompiler()
	f, err := os.Open("../schema/nvq.schema.json")
	if err != nil {
		t.Fatal(err)
	}
	doc, err := jsonschema.UnmarshalJSON(f)
	if err != nil {
		t.Fatal(err)
	}
	if err := c.AddResource("nvq.schema.json", doc); err != nil {
		t.Fatal(err)
	}
	seen := 0
	for def, paths := range fixtures(t) {
		sch, err := c.Compile("nvq.schema.json#/$defs/" + def)
		if err != nil {
			t.Fatalf("schema def %s: %v", def, err)
		}
		for _, p := range paths {
			for _, d := range docs(t, p) {
				v, err := jsonschema.UnmarshalJSON(bytes.NewReader(d))
				if err != nil {
					t.Fatal(err)
				}
				if err := sch.Validate(v); err != nil {
					t.Errorf("%s does not match #/$defs/%s:\n%s\n%v", p, def, d, err)
				}
				seen++
			}
		}
	}
	t.Logf("%d documents valid", seen)
}

// Every fixture goes through the same reading a call does, the generated types' checks (required
// fields, constants, enums, patterns) included. documents.go is generated from the schema the fixtures
// were just validated against, so this proves the reading, not the types.
func TestEveryFixtureDecodes(t *testing.T) {
	for def, paths := range fixtures(t) {
		for _, p := range paths {
			for _, d := range docs(t, p) {
				h, err := peek(d)
				if err != nil {
					t.Fatal(err)
				}
				var nvqErr *Error
				switch {
				case h.Cards == nil && h.Probes == nil && h.OK == nil && h.Event == "":
					err = failure(d, 0)
					if errors.As(err, &nvqErr) {
						err = nil
					}
				case def == "list":
					_, err = one[ListOK](d)
				case def == "probe":
					_, err = probe(d, 0)
				case def == "probeAll":
					var all struct{ Probes []json.RawMessage }
					err = json.Unmarshal(d, &all)
					for _, e := range all.Probes {
						if _, err = entry(e, 0); err != nil {
							break
						}
					}
				case def == "event":
					kind, ok := eventKinds[h.Event]
					if !ok {
						t.Fatalf("%s: event %q has no type", p, h.Event)
					}
					_, err = kind(d)
				default:
					t.Fatalf("no reading for def %s", def)
				}
				if err != nil {
					t.Errorf("%s: %v\n%s", p, err, d)
				}
			}
		}
	}
}

// eventKinds is the one hand-kept list in this package: it must name exactly the schema's event kinds.
func TestEventKindsAreTheSchemas(t *testing.T) {
	raw, err := os.ReadFile("../schema/nvq.schema.json")
	if err != nil {
		t.Fatal(err)
	}
	var s struct {
		Defs map[string]struct {
			OneOf []struct {
				Ref string `json:"$ref"`
			} `json:"oneOf"`
			Properties struct {
				Event struct {
					Const string `json:"const"`
				} `json:"event"`
			} `json:"properties"`
		} `json:"$defs"`
	}
	if err := json.Unmarshal(raw, &s); err != nil {
		t.Fatal(err)
	}
	want := map[string]bool{}
	for _, ref := range s.Defs["event"].OneOf {
		if c := s.Defs[strings.TrimPrefix(ref.Ref, "#/$defs/")].Properties.Event.Const; c != "" {
			want[c] = true
		}
	}
	if len(want) == 0 {
		t.Fatal("no event kinds found in the schema")
	}
	for k := range want {
		if eventKinds[k] == nil {
			t.Errorf("schema event %q has no entry in eventKinds", k)
		}
	}
	for k := range eventKinds {
		if !want[k] {
			t.Errorf("eventKinds has %q, which the schema does not", k)
		}
	}
}

// fake is an nvq that replays a fixture and exits with code.
func fake(t *testing.T, fixture string, code int) NVQ {
	t.Helper()
	abs, err := filepath.Abs(filepath.Join("../test/fixtures", fixture))
	if err != nil {
		t.Fatal(err)
	}
	bin := filepath.Join(t.TempDir(), "nvq")
	script := "#!/bin/sh\ncat '" + abs + "'\nexit " + string(rune('0'+code)) + "\n"
	if err := os.WriteFile(bin, []byte(script), 0o755); err != nil {
		t.Fatal(err)
	}
	return NVQ{Path: bin}
}

func TestListOutcomes(t *testing.T) {
	ctx := context.Background()

	l, err := fake(t, "list.list__a_card_fell_off_the_bus.json", 1).List(ctx)
	if err != nil || l.Cards[0].State != StateLost || *l.Cards[0].Error != CodeGPUIsLost {
		t.Fatalf("a lost card is a fact in the list, not an error: %v %+v", err, l)
	}

	l, err = fake(t, "list.list__driver_and_library_disagree_after_an_upgrade.json", 3).List(ctx)
	var e *Error
	if !errors.As(err, &e) || e.Code != "lib_rm_version_mismatch" || e.Exit != 3 || len(l.Cards) == 0 || l.Cards[0].State != "unknown" {
		t.Fatalf("NVML unusable returns the kernel's roster and the error: %v %+v", err, l)
	}

	_, err = fake(t, "list.list__a_driver_call_that_does_not_return_ends_at_the_deadlin.json", 5).List(ctx)
	if !errors.As(err, &e) || e.Code != "deadline" || e.Exit != 5 {
		t.Fatalf("deadline: %v", err)
	}

	l, err = fake(t, "list.list__every_field__fan_unsupported__riser_link__hidden_proce.json", 0).List(ctx)
	if err != nil {
		t.Fatal(err)
	}
	c := l.Cards[0]
	if c.FanPct != nil || c.PCIe.Width != 1 || c.Processes[1].UsedMiB != nil || *c.Processes[0].UsedMiB != 3072 {
		t.Fatalf("absent stays absent: %+v", c)
	}
}

func TestProbeOutcomes(t *testing.T) {
	ctx := context.Background()
	p, err := fake(t, "probe.probe__jit_failure_names_its_step.json", 4).Probe(ctx, "GPU-x")
	f, ok := p.(ProbeFailed)
	if err != nil || !ok || f.Step != "jit" || f.Error != "CUDA_ERROR_INVALID_PTX" {
		t.Fatalf("a failed step is an answer: %v %+v", err, p)
	}
	p, err = fake(t, "probe.probe__kernel_runs_and_verifies__unknown_attributes_listed.json", 0).Probe(ctx, "GPU-x")
	k, ok := p.(ProbeOK)
	if err != nil || !ok || *k.Layout.SMs != 128 || k.Layout.MaxBlocksPerSM != nil || k.Arch.Family != FamilyAda {
		t.Fatalf("probe ok: %v %+v", err, p)
	}
	all, err := fake(t, "probeAll.probe_all__a_child_deaf_to_signals_is_killed_from_outside.json", 4).ProbeAll(ctx)
	if err != nil || len(all) != 1 {
		t.Fatalf("probe all: %v %+v", err, all)
	}
	if f, ok := all[0].(ProbeFailed); !ok || f.Step != "hang" || f.Error != "killed_at_deadline" {
		t.Fatalf("probe all entry: %+v", all[0])
	}
}

func TestWatch(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	var kinds []string
	var end error
	for ev, err := range fake(t, "event.real__watch.jsonl", 0).Watch(ctx, 0) {
		if err != nil {
			end = err
			break
		}
		switch e := ev.(type) {
		case EventStart:
			kinds = append(kinds, e.Event)
		case EventSample:
			if e.TempC == nil {
				t.Fatalf("a real sample has a temperature: %+v", e)
			}
			kinds = append(kinds, e.Event)
		default:
			t.Fatalf("unexpected event %T", ev)
		}
	}
	if len(kinds) < 2 || kinds[0] != "start" || kinds[1] != "sample" {
		t.Fatalf("events %v", kinds)
	}
	if end == nil || !strings.Contains(end.Error(), "watch ended") {
		t.Fatalf("a watch that ends on its own is an error: %v", end)
	}
}

func TestNoBinary(t *testing.T) {
	_, err := NVQ{Path: filepath.Join(t.TempDir(), "absent")}.List(context.Background())
	if err == nil {
		t.Fatal("a missing binary must fail")
	}
}
