"""The client against nvq's real output: ../../test/fixtures, captured by test/run.sh (NVQ_FIXTURES) from
the scripted failure libraries and real cards, named <schema def>.<case>.json, or .jsonl for watch. The
types need no test of their own: documents.py is generated from the schema, and CI regenerates it."""

import json
import subprocess
from pathlib import Path

import jsonschema
import pytest

import nvq

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "test" / "fixtures"
SCHEMA = json.loads((ROOT / "schema" / "nvq.schema.json").read_text())


def docs(path: Path) -> list[dict]:
    return [json.loads(line) for line in path.read_text().splitlines() if line.strip()]


def test_every_fixture_matches_the_schema():
    paths = sorted(FIXTURES.glob("*.json*"))
    assert paths, f"no fixtures in {FIXTURES}"
    for path in paths:
        definition = path.name.split(".")[0]
        validator = jsonschema.Draft202012Validator({**SCHEMA, "$ref": f"#/$defs/{definition}"})
        for doc in docs(path):
            validator.validate(doc)


def fake(tmp_path: Path, fixture: str, code: int) -> nvq.NVQ:
    """An nvq that replays a fixture and exits with code."""
    binary = tmp_path / "nvq"
    binary.write_text(f"#!/bin/sh\ncat '{FIXTURES / fixture}'\nexit {code}\n")
    binary.chmod(0o755)
    return nvq.NVQ(path=binary)


def test_list_outcomes(tmp_path):
    cards = fake(tmp_path, "list.list__a_card_fell_off_the_bus.json", 1).list()
    assert cards["cards"][0]["state"] == "lost" and cards["cards"][0]["error"] == "gpu_is_lost"

    with pytest.raises(nvq.Error) as e:
        fake(tmp_path, "list.list__driver_and_library_disagree_after_an_upgrade.json", 3).list()
    assert e.value.code == "lib_rm_version_mismatch" and e.value.exit == 3
    assert e.value.list is not None and e.value.list["cards"][0]["state"] == "unknown"

    with pytest.raises(nvq.Error) as e:
        fake(tmp_path, "list.list__a_driver_call_that_does_not_return_ends_at_the_deadlin.json", 5).list()
    assert e.value.code == "deadline" and e.value.exit == 5 and e.value.list is None

    card = fake(tmp_path, "list.list__every_field__fan_unsupported__riser_link__hidden_proce.json", 0).list()[
        "cards"
    ][0]
    assert "fanPct" not in card and card["pcie"]["width"] == 1
    assert "usedMiB" not in card["processes"][1] and card["processes"][0]["usedMiB"] == 3072


def test_probe_outcomes(tmp_path):
    p = fake(tmp_path, "probe.probe__jit_failure_names_its_step.json", 4).probe("GPU-x")
    assert p["ok"] is False and p["step"] == "jit" and p["error"] == "CUDA_ERROR_INVALID_PTX"

    p = fake(tmp_path, "probe.probe__kernel_runs_and_verifies__unknown_attributes_listed.json", 0).probe(
        "GPU-x"
    )
    assert p["ok"] is True and p["layout"]["sms"] == 128 and "maxBlocksPerSM" not in p["layout"]
    assert p["arch"]["family"] == "ada"

    probes = fake(
        tmp_path, "probeAll.probe_all__a_child_deaf_to_signals_is_killed_from_outside.json", 4
    ).probe_all()
    assert len(probes) == 1 and probes[0]["step"] == "hang" and probes[0]["error"] == "killed_at_deadline"

    probes = fake(tmp_path, "probeAll.probe_all__an_interruptible_hang_names_its_card.json", 4).probe_all()
    assert probes[0]["uuid"].startswith("GPU-") and probes[0]["error"] == "deadline"


def test_watch(tmp_path):
    kinds = []
    with pytest.raises(RuntimeError, match="watch ended"):
        for ev in fake(tmp_path, "event.real__watch.jsonl", 0).watch():
            kinds.append(ev["event"])
    assert kinds[:2] == ["start", "sample"], kinds


def test_watch_closed_early_kills_nvq(tmp_path):
    binary = tmp_path / "nvq"
    binary.write_text(
        '#!/bin/sh\nwhile :; do echo \'{"nvq":1,"event":"wait_error","at":1,"error":"timeout"}\'; sleep 0.01; done\n'
    )
    binary.chmod(0o755)
    events = nvq.NVQ(path=binary).watch()
    assert next(events)["event"] == "wait_error"
    events.close()  # the finally kills nvq; a leak would hang the with block's wait


def test_a_document_of_another_version_is_refused(tmp_path):
    binary = tmp_path / "nvq"
    binary.write_text('#!/bin/sh\necho \'{"nvq":2,"driver":"","cards":[]}\'\n')
    binary.chmod(0o755)
    with pytest.raises(ValueError, match="not a version 1 document"):
        nvq.NVQ(path=binary).list()


def test_no_binary(tmp_path):
    with pytest.raises(FileNotFoundError):
        nvq.NVQ(path=tmp_path / "absent").list()


@pytest.mark.skipif(
    not nvq.BINARY.exists(), reason="no binary carried: the release workflow puts one in bin/"
)
def test_carried_binary_speaks_this_schema():
    """The binary a wheel carries prints exactly the schema documents.py was generated from."""
    out = subprocess.run([nvq.BINARY, "schema"], capture_output=True, check=True).stdout
    assert out == (ROOT / "schema" / "nvq.schema.json").read_bytes(), "bin/nvq is older than the schema"
    # On a box without a driver it says so; on one with a driver it lists the cards.
    if Path("/proc/driver/nvidia/gpus").exists():
        nvq.NVQ().list()
    else:
        with pytest.raises(nvq.Error) as e:
            nvq.NVQ().list()
        assert e.value.code == "driver_not_loaded"


@pytest.mark.skipif(nvq.BINARY.exists(), reason="this install carries a binary")
def test_without_a_carried_binary_the_package_says_so():
    with pytest.raises(FileNotFoundError, match=r"pass NVQ\(path=...\)"):
        nvq.NVQ().list()
