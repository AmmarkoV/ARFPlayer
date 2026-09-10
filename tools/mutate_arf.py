#!/usr/bin/env python3
"""Build broken .arfz containers from a good one and check the reader survives them.

Every mutation below corresponds to something a real writer bug, a truncated
download or a format change could produce.  The bar the reader has to clear is
the same for all of them: exit cleanly with a diagnostic naming what is wrong,
never crash, never hang, and never report success on a broken container.

Usage:
    python3 tools/mutate_arf.py samples/summerlove_0.arfz ./build/arfplay
"""
from __future__ import annotations

import json
import os
import struct
import subprocess
import sys
import tempfile
import zipfile


def rebuild(source: str, destination: str, replace: dict, drop=()) -> None:
    """Copy a container, swapping in replacement entries and dropping others."""
    with zipfile.ZipFile(source) as original, zipfile.ZipFile(destination, "w") as mutant:
        for item in original.namelist():
            if item in drop:
                continue
            mutant.writestr(item, replace.get(item, original.read(item)))


def mutants(source: str, directory: str):
    """Yield (name, path, should_load) for each broken container."""
    with zipfile.ZipFile(source) as archive:
        document = json.loads(archive.read("arf.json"))
        stream = archive.read("animations/joints.bin")

    def path(name: str) -> str:
        return os.path.join(directory, name + ".arfz")

    # A download that stopped halfway: the ZIP central directory is gone.
    whole = open(source, "rb").read()
    truncated = path("truncated_zip")
    open(truncated, "wb").write(whole[: len(whole) // 2])
    yield "truncated ZIP", truncated, False

    # arf.json itself is not JSON.
    broken_json = path("broken_json")
    rebuild(source, broken_json, {"arf.json": b'{"preamble": {,,,}'})
    yield "malformed arf.json", broken_json, False

    # A mandatory top level key is gone.
    without_components = dict(document)
    del without_components["components"]
    missing = path("missing_components")
    rebuild(source, missing, {"arf.json": json.dumps(without_components).encode()})
    yield "missing components", missing, False

    # The JSON and the binaries disagree about how big a tensor is.
    wrong_length = json.loads(json.dumps(document))
    wrong_length["data"][0]["byteLength"] += 4
    mismatch = path("bytelength_mismatch")
    rebuild(source, mismatch, {"arf.json": json.dumps(wrong_length).encode()})
    yield "byteLength mismatch", mismatch, False

    # A data item points at an entry that is not in the archive.
    dangling = path("missing_data_entry")
    rebuild(source, dangling, {}, drop=("data/mesh_indices.bin",))
    yield "missing data entry", dangling, False

    # The skeleton's joint order no longer matches the node order, which would
    # otherwise drive every joint with another joint's animation.
    reordered = json.loads(json.dumps(document))
    joints = reordered["components"]["skeletons"][0]["joints"]
    joints[1], joints[2] = joints[2], joints[1]
    swapped = path("skeleton_reordered")
    rebuild(source, swapped, {"arf.json": json.dumps(reordered).encode()})
    yield "skeleton joint order disagrees with nodes", swapped, False

    # The animation stream carries only its config unit.
    config_length = struct.unpack_from("<I", stream, 1)[0]
    empty = path("zero_frames")
    rebuild(source, empty, {"animations/joints.bin": stream[: 5 + config_length]})
    yield "zero frame animation stream", empty, False

    # A frame stops in the middle of a joint matrix.
    chopped = path("truncated_stream")
    rebuild(source, chopped, {"animations/joints.bin": stream[: 5 + config_length + 400]})
    yield "truncated animation stream", chopped, False

    # An AAU type from a future writer, sitting between two real frames.  This
    # one MUST still load: unit_length exists so readers can step over it.
    unknown = struct.pack("<BI", 99, 8) + b"\x00" * 8
    head = 5 + config_length
    forward = path("unknown_aau")
    rebuild(source, forward, {"animations/joints.bin": stream[:head] + unknown + stream[head:]})
    yield "unknown AAU type (must still load)", forward, True


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 1

    source, player = sys.argv[1], sys.argv[2]
    failures = 0

    with tempfile.TemporaryDirectory() as directory:
        for name, mutant, should_load in mutants(source, directory):
            try:
                done = subprocess.run([player, "--info", mutant], capture_output=True, timeout=30)
            except subprocess.TimeoutExpired:
                print(f"HANG  {name}")
                failures += 1
                continue

            loaded = done.returncode == 0
            message = (done.stderr or done.stdout).decode(errors="replace").strip().splitlines()
            summary = message[-1] if message else "(no output)"

            if done.returncode < 0:
                print(f"CRASH {name}: killed by signal {-done.returncode}")
                failures += 1
            elif loaded != should_load:
                print(f"FAIL  {name}: expected {'a clean load' if should_load else 'a rejection'}, "
                      f"got exit {done.returncode}")
                failures += 1
            elif should_load:
                print(f"OK    {name}: loaded, {summary}")
            elif not summary:
                print(f"FAIL  {name}: rejected but said nothing")
                failures += 1
            else:
                print(f"OK    {name}: {summary}")

    print()
    print("all mutations handled" if failures == 0 else f"{failures} mutation(s) mishandled")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
