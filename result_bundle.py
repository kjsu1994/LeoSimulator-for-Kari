# SPDX-License-Identifier: GPL-3.0-only
# Modified/adapted 2026-11-16; see THIRD_PARTY_NOTICES.txt.
"""Read-only capture of a simulation's CSV and recursive INI dependencies."""
import hashlib
import json
import posixpath
import re
import uuid
from datetime import datetime, timezone
from pathlib import Path

ENTRY = "simulations/Dijkstra132/omnetpp.ini"
FORMAT = "florasat-csv-ini-v1"
PROFILE = "leo-dijkstra-delta-v1"


def content_hash(files):
    h = hashlib.sha256()
    for name, data in sorted(files.items()):
        h.update(name.encode() + b"\0" + hashlib.sha256(data).digest())
    return h.hexdigest()


def ini_dependencies(files, entry=ENTRY):
    found, active = {}, set()
    def visit(name):
        if name in active:
            raise ValueError(f"Cyclic INI include: {name}")
        if name in found:
            return
        if name.startswith("/") or ":" in name or name == ".." or name.startswith("../"):
            raise ValueError(f"INI path leaves project: {name}")
        if name not in files:
            raise ValueError(f"Missing INI: {name}")
        data = files[name]
        active.add(name)
        for line in data.decode("utf-8-sig").splitlines():
            match = re.match(r'^\s*include\s+("[^"]+"|[^#\s]+)\s*(?:#.*)?$', line)
            if match:
                relative = match[1].strip('"').replace("\\", "/")
                if relative.startswith("/") or ":" in relative:
                    raise ValueError("Absolute INI includes cannot be packaged")
                visit(posixpath.normpath(posixpath.join(posixpath.dirname(name), relative)))
        active.remove(name)
        found[name] = data
    visit(entry)
    return found


def make_bundle(inputs, csv_bytes, config_name, model_hash, *, run_id=None, command=None, provenance="captured-before-run"):
    settings = ini_dependencies(inputs)
    files = {"routes.csv": csv_bytes, **{"config/" + name: data for name, data in settings.items()}}
    identifier = run_id or uuid.uuid4().hex
    manifest = {
        "format": FORMAT, "runId": identifier, "createdAt": datetime.now(timezone.utc).isoformat(),
        "configName": config_name, "runNumber": 0, "entryIni": "config/" + ENTRY,
        "csvPath": "routes.csv", "profile": PROFILE, "modelHash": model_hash,
        "provenance": provenance, "command": command or [], "overrides": {},
        "files": {name: hashlib.sha256(data).hexdigest() for name, data in files.items()},
    }
    return {"manifest": manifest, "files": {name: data.decode("utf-8") for name, data in files.items()}}, files


def write_bundle(directory, bundle, files):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    for name, data in files.items():
        path = directory / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    (directory / "manifest.json").write_text(json.dumps(bundle["manifest"], ensure_ascii=False, indent=2), encoding="utf-8")


def main():
    import argparse
    parser = argparse.ArgumentParser(description="Package CSV with its actual run-time INI tree; never changes the source project.")
    parser.add_argument("--csv", required=True)
    parser.add_argument("--source", required=True, help="Project tree used for this run, including src/ and simulations/")
    parser.add_argument("--config", required=True)
    parser.add_argument("--output", default="data/runs")
    args = parser.parse_args()
    root = Path(args.source).resolve()
    inputs = {p.relative_to(root).as_posix(): p.read_bytes() for p in (root / "simulations").rglob("*.ini")}
    source = {p.relative_to(root).as_posix(): p.read_bytes() for p in (root / "src").rglob("*") if p.is_file() and p.suffix in {".cc", ".cpp", ".c", ".h", ".hpp", ".ned", ".msg"}}
    if not source:
        raise ValueError("Source tree is required to identify the replay model")
    from server import parse_routes_csv
    raw = Path(args.csv).read_bytes()
    parse_routes_csv(raw.decode("utf-8-sig"))
    bundle, files = make_bundle(inputs, raw, args.config, content_hash(source), provenance="manual-import-unverified")
    folder = Path(args.output) / bundle["manifest"]["runId"]
    write_bundle(folder, bundle, files)
    print(folder.resolve())
    print("Manual import: confirm that the supplied INI tree was actually used for this CSV.")

if __name__ == "__main__":
    main()
