"""nvq runs the nvq binary and hands back what it says about NVIDIA cards. The linux x86_64 and aarch64
wheels carry the binary (bin/nvq, built in manylinux2014: glibc 2.17 and newer), so `pip install nvq` is
the whole install, and a version of this package runs exactly the binary released with it. The wheel for
any other platform carries none: NVQ(path=...) names one there.

    n = nvq.NVQ()                       # the package's own binary; NVQ(path=...) runs another
    cards = n.list()                    # every card, lost ones included; no CUDA context opened
    probes = n.probe_all()              # does each card run a kernel, and its capability sheet
    for ev in n.watch(every=1.0): ...   # Xid events, lost cards, samples

Documents are the parsed JSON itself, keys exactly as the contract names them (card["tempC"]). Their
types (documents.py) are TypedDicts generated from the JSON Schema the binary prints with `nvq schema`
(`make types`), so a type checker knows every key and a value the card cannot report is a key that is
absent, never 0. `nvq` on the command line is the binary itself."""

# NVQ.list shadows the builtin inside the class body: annotations there must not be evaluated.
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from collections.abc import Generator
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from . import documents
from .documents import (
    Card,
    EventEccDouble,
    EventLost,
    EventSample,
    EventStart,
    EventWaitError,
    EventXid,
    ListOk,
    ProbeFailed,
    ProbeOk,
)

__all__ = [
    "BINARY",
    "NVQ",
    "SCHEMA",
    "Card",
    "Error",
    "EventEccDouble",
    "EventLost",
    "EventSample",
    "EventStart",
    "EventWaitError",
    "EventXid",
    "ListOk",
    "ProbeFailed",
    "ProbeOk",
    "WatchEvent",
    "documents",
]

type WatchEvent = EventStart | EventXid | EventEccDouble | EventLost | EventWaitError | EventSample

SCHEMA = 1
"""The document version this package reads."""

BINARY = Path(__file__).parent / "bin" / "nvq"
"""The binary this wheel carries; absent from the wheel for platforms nvq does not run on."""


class Error(Exception):
    """A failure nvq reported as a document: `code` from the schema, nvq's `exit` code. When NVML is
    unusable, `list` holds the kernel's roster all the same, every card in state "unknown"."""

    def __init__(self, code: str, detail: str, exit: int, list: ListOk | None = None):
        super().__init__(f"nvq: {code}: {detail} (exit {exit})")
        self.code = code
        self.detail = detail
        self.exit = exit
        self.list = list


@dataclass(frozen=True, kw_only=True)
class NVQ:
    """The binary to run: `path`, or when None the one this wheel carries.

    `deadline` (seconds) bounds one list or probe inside nvq itself (--deadline-ms); None keeps nvq's
    default (10 s list, 60 s probe). Each call's `timeout` bounds the whole process from outside: nvq is
    killed and subprocess.TimeoutExpired raised."""

    path: Path | str | None = None
    deadline: float | None = None

    def list(self, timeout: float | None = None) -> ListOk:
        """Every card the kernel bound. A lost card is a card with state "lost", not an error."""
        doc, code = self._run(["list"], timeout)
        if "cards" not in doc:
            raise _error(doc, code)
        if "error" in doc:
            raise Error(doc["error"]["code"], doc["error"]["detail"], code, doc)
        return doc

    def probe(self, uuid: str, timeout: float | None = None) -> ProbeOk | ProbeFailed:
        """Runs a kernel on one card, with only that card visible to CUDA. It opens a CUDA context: do
        not aim it at a card mid-work you care about. A failed step is a ProbeFailed, not an error."""
        doc, code = self._run(["probe", uuid], timeout)
        if "ok" not in doc:
            raise _error(doc, code)
        return doc

    def probe_all(self, timeout: float | None = None) -> list[ProbeOk | ProbeFailed]:
        """Probes every card at once, each in its own process: a hung card cannot stall the others.
        Each entry is a ProbeOk or a ProbeFailed naming its card."""
        doc, code = self._run(["probe", "all"], timeout)
        if "probes" not in doc:
            raise _error(doc, code)
        return doc["probes"]

    def watch(self, every: float | None = None) -> Generator[WatchEvent]:
        """Events until nvq exits or the iterator is closed (which kills nvq). `every` (seconds) adds a
        sample per card at that interval. nvq ending on its own raises: watch runs until killed."""
        args = ["watch", *(["--every-ms", str(round(every * 1000))] if every else [])]
        with (
            tempfile.TemporaryFile() as stderr,
            subprocess.Popen([str(self._binary()), *args], stdout=subprocess.PIPE, stderr=stderr) as p,
        ):
            assert p.stdout is not None
            try:
                for line in p.stdout:
                    if not line.strip():
                        continue
                    doc = _document(line, args)
                    if "event" not in doc:
                        raise _error(doc, p.wait())
                    yield doc
                code = p.wait()
                stderr.seek(0)
                raise RuntimeError(f"nvq watch ended: exit {code}: {stderr.read().decode().strip()}")
            finally:
                p.kill()  # a no-op once nvq exited; leaving the with block reaps it

    def _binary(self) -> Path:
        return Path(self.path) if self.path is not None else _own_binary()

    def _run(self, args: list[str], timeout: float | None) -> tuple[Any, int]:
        """The document nvq printed and its exit code. A process that printed none (not found, killed, a
        usage error) raises here; any other exit is the document's to explain."""
        deadline = ["--deadline-ms", str(round(self.deadline * 1000))] if self.deadline is not None else []
        p = subprocess.run(
            [str(self._binary()), *deadline, *args], capture_output=True, timeout=timeout, check=False
        )
        if not p.stdout.strip():
            stderr = p.stderr.decode(errors="replace").strip()
            raise RuntimeError(f"nvq {' '.join(args)}: exit {p.returncode}, no document: {stderr}")
        return _document(p.stdout, args), p.returncode


def _own_binary() -> Path:
    if not BINARY.exists():
        raise FileNotFoundError(
            f"nvq: this wheel carries no binary for {sys.platform} (only the linux x86_64 and aarch64 "
            f"wheels do); pass NVQ(path=...)"
        )
    return BINARY


def _document(raw: bytes, args: list[str]) -> Any:
    """A parsed document, typed by the caller: the schema, not this function, vouches for its shape."""
    doc = json.loads(raw)
    if not isinstance(doc, dict) or doc.get("nvq") != SCHEMA:
        raise ValueError(f"nvq {' '.join(args)}: not a version {SCHEMA} document: {raw[:200]!r}")
    return doc


def _error(doc: Any, code: int) -> Error:
    if not isinstance(doc.get("error"), dict):
        raise TypeError(f"nvq: neither the expected document nor an error: {json.dumps(doc)[:200]}")
    return Error(doc["error"]["code"], doc["error"]["detail"], code)


def main() -> None:
    """`nvq` on PATH: becomes the carried binary, arguments and exit code untouched."""
    binary = _own_binary()
    os.execv(binary, [str(binary), *sys.argv[1:]])
