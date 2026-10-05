# make          ./nvq, with the schema compiled in
# make test     the scripted-failure suite (Linux, nvidia kernel module loaded, jq)
# make release  dist/nvq-linux-<arch> + .sha256, the artifacts a GitHub release carries
VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)
ARCH ?= $(shell uname -m | sed 's/x86_64/amd64/; s/aarch64/arm64/')
CFLAGS ?= -O2
WARN = -std=c11 -Wall -Wextra -Werror

nvq: src/nvq.c src/json.h src/nvml.h src/cuda.h build/schema.inc
	$(CC) $(WARN) $(CFLAGS) -Ibuild -DNVQ_VERSION='"$(VERSION)"' -o $@ src/nvq.c -ldl

# The schema as a C byte list, with od (coreutils): no xxd on minimal build images.
build/schema.inc: schema/nvq.schema.json
	mkdir -p build
	od -An -v -tx1 $< | sed 's/ \([0-9a-f][0-9a-f]\)/0x\1,/g' > $@

test: nvq
	test/run.sh

# Always a fresh compile on this machine: never a ./nvq left by another build.
release: build/schema.inc
	mkdir -p dist
	$(CC) $(WARN) $(CFLAGS) -Ibuild -DNVQ_VERSION='"$(VERSION)"' -o dist/nvq-linux-$(ARCH) src/nvq.c -ldl
	cd dist && sha256sum nvq-linux-$(ARCH) > nvq-linux-$(ARCH).sha256

clean:
	rm -rf nvq build dist

.PHONY: test release clean
