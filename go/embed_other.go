//go:build !linux || !(amd64 || arm64)

package nvq

// No NVIDIA driver runs here, so no binary is embedded: NVQ.Path must name one.
var embedded []byte
