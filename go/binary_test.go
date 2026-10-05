package nvq

import (
	"bytes"
	"context"
	"crypto/sha256"
	"os"
	"os/exec"
	"runtime"
	"strings"
	"testing"
)

func carries() bool {
	return runtime.GOOS == "linux" && (runtime.GOARCH == "amd64" || runtime.GOARCH == "arm64")
}

func TestBinaryIsExtractedOnceAndChecked(t *testing.T) {
	t.Setenv("XDG_CACHE_HOME", t.TempDir())
	if !carries() {
		if _, err := extract(); err == nil || !strings.Contains(err.Error(), "set NVQ.Path") {
			t.Fatalf("off linux the module carries no binary and must say so: %v", err)
		}
		return
	}
	a, err := extract()
	if err != nil {
		t.Fatal(err)
	}
	b, err := extract()
	if err != nil || a != b {
		t.Fatalf("second extraction %q %v, first %q", b, err, a)
	}
	if have, _ := os.ReadFile(a); sha256.Sum256(have) != sha256.Sum256(embedded) {
		t.Fatal("extracted bytes differ from the module's")
	}
	// A damaged file under the right name is replaced, never run.
	if err := os.WriteFile(a, []byte("#!/bin/sh\necho hijacked\n"), 0o755); err != nil {
		t.Fatal(err)
	}
	if c, err := extract(); err != nil || c != a {
		t.Fatal(err)
	}
	if have, _ := os.ReadFile(a); sha256.Sum256(have) != sha256.Sum256(embedded) {
		t.Fatal("a damaged binary survived")
	}
}

// The binary this module carries speaks exactly the schema its Go types are tested against: a schema
// change ships only with binaries rebuilt from it (the release workflow does both in one commit).
func TestEmbeddedBinarySpeaksThisSchema(t *testing.T) {
	if !carries() {
		t.Skip("no binary embedded for " + runtime.GOOS + "/" + runtime.GOARCH)
	}
	t.Setenv("XDG_CACHE_HOME", t.TempDir())
	bin, err := extract()
	if err != nil {
		t.Fatal(err)
	}
	out, err := exec.Command(bin, "schema").Output()
	if err != nil {
		t.Fatal(err)
	}
	want, _ := os.ReadFile("../schema/nvq.schema.json")
	if !bytes.Equal(out, want) {
		t.Fatal("bin/ is older than schema/nvq.schema.json: release to rebuild it")
	}
	// The zero NVQ runs the module's own binary: on a box without a driver it says so, on one with a
	// driver it lists the cards.
	_, err = NVQ{}.List(context.Background())
	if _, driver := os.Stat("/proc/driver/nvidia/gpus"); driver != nil {
		if e, ok := err.(*Error); !ok || e.Code != "driver_not_loaded" {
			t.Fatalf("want driver_not_loaded from the embedded binary, got %v", err)
		}
	} else if err != nil {
		t.Fatal(err)
	}
}
