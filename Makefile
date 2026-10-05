# make          ./nvq, with the schema compiled in
# make test     the scripted-failure suite (Linux, nvidia kernel module loaded, jq)
# make layout   nvq's NVML/CUDA declarations against NVIDIA's headers in /usr/local/cuda-*
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

# Every layout, constant and function signature nvq declares, against each CUDA toolkit installed.
# Deprecations print and do not fail: they are next migrations, not mismatches.
TOOLKITS ?= $(wildcard /usr/local/cuda-*/include)
layout:
	@test -n "$(TOOLKITS)" || { echo "no /usr/local/cuda-*/include"; exit 1; }
	@for d in $(TOOLKITS); do \
	  $(CC) -std=c11 -Wall -Werror -Wno-error=deprecated-declarations -I$$d -fsyntax-only test/layout.c || exit 1; \
	  echo "layout ok: $$d"; done

# Always a fresh compile on this machine: never a ./nvq left by another build.
release: build/schema.inc
	mkdir -p dist
	$(CC) $(WARN) $(CFLAGS) -Ibuild -DNVQ_VERSION='"$(VERSION)"' -o dist/nvq-linux-$(ARCH) src/nvq.c -ldl
	cd dist && sha256sum nvq-linux-$(ARCH) > nvq-linux-$(ARCH).sha256

clean:
	rm -rf nvq build dist

.PHONY: test layout release clean
