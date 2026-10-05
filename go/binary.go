package nvq

import (
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"sync"
)

// This module carries the nvq binary for linux/amd64 and linux/arm64 (bin/, committed by the release
// workflow, built in manylinux2014: glibc 2.17 and newer). go.sum pins those bytes, so a program that
// imports a version of this package runs exactly the binary released with it: no download, no
// compiler, nothing to install.

var binary struct {
	once sync.Once
	path string
	err  error
}

// Binary returns the path of this module's own nvq, written once into the user cache directory under
// its sha256 and reused from there by every process: an existing file is checked, never trusted by
// name; a missing or damaged one is replaced atomically. Off linux/amd64 and linux/arm64 there is none.
func Binary() (string, error) {
	binary.once.Do(func() { binary.path, binary.err = extract() })
	return binary.path, binary.err
}

func extract() (string, error) {
	if len(embedded) == 0 {
		return "", fmt.Errorf("nvq: no binary for %s/%s in this module; set NVQ.Path", runtime.GOOS, runtime.GOARCH)
	}
	sum := sha256.Sum256(embedded)
	cache, err := os.UserCacheDir()
	if err != nil {
		return "", fmt.Errorf("nvq: %w; set NVQ.Path", err)
	}
	dir := filepath.Join(cache, "nvq")
	path := filepath.Join(dir, "nvq-"+hex.EncodeToString(sum[:8]))
	if have, err := os.ReadFile(path); err == nil && sha256.Sum256(have) == sum {
		return path, nil
	} else if err != nil && !errors.Is(err, os.ErrNotExist) {
		return "", err
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return "", err
	}
	tmp, err := os.CreateTemp(dir, ".nvq-*")
	if err != nil {
		return "", err
	}
	defer os.Remove(tmp.Name()) // a no-op once renamed
	if _, err := tmp.Write(embedded); err != nil {
		tmp.Close()
		return "", err
	}
	if err := tmp.Chmod(0o755); err != nil {
		tmp.Close()
		return "", err
	}
	if err := tmp.Sync(); err != nil {
		tmp.Close()
		return "", err
	}
	if err := tmp.Close(); err != nil {
		return "", err
	}
	// Two processes extracting at once both rename the same bytes into place: the last one wins and
	// both are right.
	return path, os.Rename(tmp.Name(), path)
}
