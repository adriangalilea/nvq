#!/usr/bin/env bash
# Every failure nvq promises to name, staged with scripted driver libraries, plus the real card.
# Needs Linux with the nvidia kernel module loaded (the roster is the kernel's), cc, jq.
# Usage: test/run.sh            fakes only
#        test/run.sh --real     also the real cards: list and watch (read-only; probe opens a context,
#                               run it by hand on an idle card)
# NVQ_FIXTURES=<dir> keeps every output there, one file per case, for the schema test (go test).
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
CFLAGS="-std=c11 -O2 -Wall -Wextra -Werror"
make -s nvq
cp nvq "$tmp/nvq"
mkdir -p "$tmp/lib" "$tmp/old"
cc $CFLAGS -shared -fPIC -o "$tmp/lib/libnvidia-ml.so.1" test/fake_nvml.c
cc $CFLAGS -shared -fPIC -o "$tmp/lib/libcuda.so.1" test/fake_cuda.c
cc $CFLAGS -DOLD -shared -fPIC -o "$tmp/old/libnvidia-ml.so.1" test/fake_nvml.c
uuid=$(jq -r '.cards[0].uuid' < <(NVQ_FAKE=ok LD_LIBRARY_PATH="$tmp/lib" "$tmp/nvq" list))

fails=0
# keep <output> <file>: a fixture, with every card UUID masked: fixtures are published, a UUID names
# a physical card.
keep() { printf '%s\n' "$1" | sed -E 's/GPU-[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}/GPU-00000000-1111-2222-3333-444444444444/g' > "$NVQ_FIXTURES/$2"; }
# expect <name> <exit> <jq predicate> <fake mode> <lib dir> -- <nvq args...>
expect() {
  local name=$1 want=$2 pred=$3 mode=$4 lib=$5; shift 6
  local out code=0
  out=$(NVQ_FAKE=$mode LD_LIBRARY_PATH=$lib timeout 20 "$tmp/nvq" "$@") || code=$?
  if [[ -n ${NVQ_FIXTURES:-} && -n $out ]]; then
    local def args=" $* "
    case $args in
      *" probe all "*) def=probeAll ;; *" probe "*) def=probe ;; *" list "*) def=list ;;
      *" watch "*) def=event ;; *" version "*) def=version ;; *) def="" ;;
    esac
    [[ -n $def ]] && keep "$out" "$def.$(tr 'A-Z' 'a-z' <<<"$name" | tr -c 'a-z0-9\n' '_' | cut -c1-60).json"
  fi
  if [[ $code -ne $want ]] || ! jq -e "$pred" >/dev/null <<<"$out"; then
    echo "FAIL $name: exit $code (want $want)"; echo "  $out" | head -c 2000; echo
    fails=$((fails + 1))
  else
    echo "ok   $name"
  fi
}

L=$tmp/lib
expect "list: every field, fan unsupported, riser link, hidden process memory" 0 \
  '.cards[0] | .state=="ok" and .unsupported==["fanPct"] and (has("fanPct")|not)
   and .pcie=={"gen":1,"width":1,"maxGen":4,"maxWidth":16} and .limits==["sw_power_cap","sw_thermal"]
   and .processes==[{"pid":4242,"usedMiB":3072},{"pid":4343}] and .powerW=={"limit":300,"draw":250.5}' \
  ok "$L" -- list
expect "list: driver and library disagree after an upgrade" 3 \
  '.error.code=="lib_rm_version_mismatch" and .cards[0].state=="unknown" and (.cards[0].uuid|startswith("GPU-"))' \
  mismatch "$L" -- list
expect "list: a card fell off the bus" 1 '.cards[0] | .state=="lost" and .error=="gpu_is_lost"' lost "$L" -- list
expect "list: a driver call that does not return ends at the deadline" 5 '.error.code=="deadline"' \
  hang "$L" -- --deadline-ms 300 list
expect "list: a driver older than nvq" 3 \
  '.error.code=="function_not_found" and (.error.detail|contains("nvmlDeviceGetComputeRunningProcesses_v3"))' \
  ok "$tmp/old" -- list
expect "watch: Xid 79 arrives named" 124 'select(.event=="xid") | .xid==79 and .meaning=="gpu has fallen off the bus"' \
  ok "$L" -- watch
expect "probe: kernel runs and verifies, unknown attributes listed" 0 \
  '.ok and .arch=={"compute":"8.9","sm":"sm_89","family":"ada"} and .layout.sms==128 and .layout.maxWarpsPerSM==48
   and .memory.peakGBs==1008.096 and (.unsupported|index("maxBlocksPerSM")) and (.ms|has("jit"))' \
  ok "$L" -- probe "$uuid"
expect "probe: JIT failure names its step" 4 '.ok==false and .step=="jit" and .error=="CUDA_ERROR_INVALID_PTX"' \
  jit "$L" -- probe "$uuid"
expect "probe: an interruptible hang ends at the deadline" 5 '.error.code=="deadline"' \
  hang "$L" -- --deadline-ms 300 probe "$uuid"
expect "probe all: a child deaf to signals is killed from outside" 4 \
  '.probes[0] | .ok==false and .step=="hang" and .error=="killed_at_deadline"' \
  stuck "$L" -- --deadline-ms 300 probe all
expect "usage" 2 'true' ok "$L" -- probe nonsense

if [[ ${1:-} == --real ]]; then
  expect "real: list" 0 '.cards | length > 0 and all(.state=="ok")' "" "" -- list
  out=$(timeout 3 "$tmp/nvq" watch --every-ms 1000 || true)
  [[ -n ${NVQ_FIXTURES:-} ]] && keep "$out" event.real__watch.jsonl
  jq -se 'map(select(.event=="start"))[0].cards | all(.state=="ok")' >/dev/null <<<"$out" && echo "ok   real: watch" || { echo "FAIL real: watch: $out"; fails=$((fails + 1)); }
fi

[[ $fails -eq 0 ]] && echo "all passed" || { echo "$fails failed"; exit 1; }
