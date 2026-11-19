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

ENTRY = "simulations/Network/omnetpp.ini"
LEGACY_ENTRY = "simulations/Dijkstra132/omnetpp.ini"
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


def make_bundle(inputs, csv_bytes, config_name, model_hash, *, run_id=None, command=None, provenance="captured-before-run", entry=None):
    entry=entry or (ENTRY if ENTRY in inputs else LEGACY_ENTRY)
    settings = ini_dependencies(inputs,entry)
    files = {"routes.csv": csv_bytes, **{"config/" + name: data for name, data in settings.items()}}
    identifier = run_id or uuid.uuid4().hex
    manifest = {
        "format": FORMAT, "runId": identifier, "createdAt": datetime.now(timezone.utc).isoformat(),
        "configName": config_name, "runNumber": 0, "entryIni": "config/" + entry,
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


def make_native_bundle(inputs, directory, config_name, model_hash, config, run_id, entry=None):
    """Versioned replay binds the exact normalized inputs and recorded metrics to replay."""
    directory=Path(directory)
    csv=directory/'routes.csv';inline=csv.stat().st_size<=4*1024*1024
    bundle,files=make_bundle(inputs,csv.read_bytes() if inline else b'',config_name,model_hash,run_id=run_id,provenance='native-simulation',entry=entry)
    if not inline:files.pop('routes.csv')
    config_bytes=json.dumps(config,ensure_ascii=False,allow_nan=False,sort_keys=True).encode('utf-8')
    files['normalized.json']=config_bytes
    for name in ['summary.json','timeseries.csv','network-events.jsonl','failures.csv','physical-events.json','common-srd.json','control-series.csv']:
        files[name]=(directory/name).read_bytes()

    if config.get('scenario'):files['scenario.json']=json.dumps(config['scenario'],ensure_ascii=False,allow_nan=False).encode('utf-8')
    m=bundle['manifest'];m.update(format='leo-native-replay-v4',profile='leo-native-delta-v4',orbitModelHash=model_hash,
                                normalizedPath='normalized.json',engine={'id':'leo-native','version':json.loads((directory/'summary.json').read_text(encoding='utf8')).get('version','4.0.0')})
    if config.get('packetTrace'):
        h=hashlib.sha256()
        with (directory/'events.csv').open('rb') as stream:
            for chunk in iter(lambda:stream.read(1048576),b''):h.update(chunk)
        m['attachments']={'events.csv':{'sha256':h.hexdigest(),'bytes':(directory/'events.csv').stat().st_size}}
    from comparison import fingerprint
    m['environmentHash']=fingerprint(config)
    for key,item in config.get('externalInputs',{}).items():
        files['inputs/'+key+('.json' if key=='scenarioFile' else '.csv')]=item['text'].encode('utf-8')
    m['commonSrdHash']=config.get('commonSrdHash');m['physicalEventsHash']=config.get('physicalEventsHash');m['metricVersion']=4;m['experimentId']=config.get('experimentId')
    from native_engine import FIELDS,BOOL_FIELDS,ENUM_FIELDS
    m['algorithmSettings']={key:config.get(key) for key in ['algorithm']+[f[0] for f in FIELDS if '.net.routing.' in f[2]]+list(BOOL_FIELDS)+list(ENUM_FIELDS)}
    m['algorithmSettings'].update({k:config.get(k) for k in ('algorithm','labelCap','reservedPaths','csibEnabled','csibPeriod','csibVote','processingMode','reserveDelay','repairDelay','spfDelay')})
    m['inputHash']=hashlib.sha256(config_bytes).hexdigest()
    m['files']={name:hashlib.sha256(data).hexdigest() for name,data in files.items()}
    if not inline:
        digest=hashlib.sha256()
        with csv.open('rb') as stream:
            for chunk in iter(lambda:stream.read(1024*1024),b''):digest.update(chunk)
        m['files']['routes.csv']=digest.hexdigest()
        bundle['fileUrls']={'routes.csv':'/data/runs/'+run_id+'/routes.csv'}
    bundle['files']={name:data.decode('utf-8') for name,data in files.items()}
    return bundle,files
