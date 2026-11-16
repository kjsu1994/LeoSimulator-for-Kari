# SPDX-License-Identifier: GPL-3.0-only
# Modified/adapted 2026-11-16; see THIRD_PARTY_NOTICES.txt.
import csv
import io
import math
import uuid
from datetime import datetime, timezone
from pathlib import Path
import http.server
import json
import os
import re
import subprocess
import sys
import threading
import urllib.parse
import webbrowser
from result_bundle import make_bundle, write_bundle

BASE_DIR = os.path.dirname(sys.executable) if getattr(sys, "frozen", False) else os.path.dirname(os.path.abspath(__file__))
RUN_LOCK = threading.Lock()
NO_WIN = getattr(subprocess, "CREATE_NO_WINDOW", 0)


LEO_DIR = os.path.join(BASE_DIR, "settings")
INI_PATH = os.path.join(LEO_DIR, "simulations", "Dijkstra132", "omnetpp.ini")


def _strip_comment(raw):
    in_q = False
    out = []
    for ch in raw:
        if ch == '"':
            in_q = not in_q
        elif ch == "#" and not in_q:
            break
        out.append(ch)
    return "".join(out).strip()


def _unquote(val):
    v = val.strip()
    return v[1:-1] if len(v) >= 2 and v[0] == '"' and v[-1] == '"' else v


def _num(val, default=0.0):
    m = re.match(r"[-+]?[0-9]*\.?[0-9]+", str(val).strip())
    return float(m.group(0)) if m else default


def _time_sec(val, default=0.0):
    m = re.match(r"^\s*([-+]?[0-9]*\.?[0-9]+)\s*(ms|s|min|h|d)?", str(val).strip())
    if not m:
        return default
    scale = {None: 1.0, "ms": 0.001, "s": 1.0, "min": 60.0, "h": 3600.0, "d": 86400.0}.get(m.group(2), 1.0)
    return float(m.group(1)) * scale


def _parse_ini_tree(entry_path):
    sections = {"General": []}
    extends = {}
    configs = []
    visited = set()

    def walk(path, cur_sec, is_root):
        norm = os.path.normpath(path)
        if norm in visited:
            return cur_sec
        if not os.path.isfile(norm):
            raise ValueError(f"Missing INI include: {norm}")
        visited.add(norm)
        for raw_line in Path(norm).read_text(encoding="utf-8").splitlines():
            line = _strip_comment(raw_line)
            if not line:
                continue
            m_sec = re.match(r"^\[(?:Config\s+)?([^\]]+)\]$", line)
            if m_sec:
                name = m_sec.group(1).strip()
                cur_sec = "General" if name == "General" else name
                sections.setdefault(cur_sec, [])
                if is_root and cur_sec != "General" and not any(c["name"] == cur_sec for c in configs):
                    configs.append({"name": cur_sec, "description": ""})
                continue
            m_inc = re.match(r"^include\s+(.+)$", line)
            if m_inc:
                rel = _unquote(m_inc.group(1))
                cand = os.path.join(os.path.dirname(norm), rel)
                cur_sec = walk(cand, cur_sec, False)
                continue
            if "=" not in line:
                continue
            k, v = [p.strip() for p in line.split("=", 1)]
            if k == "extends":
                extends[cur_sec] = [x.strip() for x in v.split(",") if x.strip()]
            elif k == "description" and is_root:
                cfg_obj = next((c for c in configs if c["name"] == cur_sec), None)
                if cfg_obj is not None:
                    cfg_obj["description"] = _unquote(v)
            else:
                sections.setdefault(cur_sec, []).append((k, v))
        return cur_sec

    walk(entry_path, "General", True)
    return configs, sections, extends


def _resolve_chain(cfg_name, extends):
    def order(name, visiting):
        if name in visiting:
            raise ValueError("Cyclic INI inheritance")
        parents = extends.get(name, [])
        sequences = [order(p, visiting | {name}) for p in parents] + [parents[:]]
        result = [name]
        while any(sequences):
            head = next((s[0] for s in sequences if s and not any(s[0] in other[1:] for other in sequences)), None)
            if head is None:
                raise ValueError("Inconsistent INI inheritance")
            result.append(head)
            for sequence in sequences:
                if sequence and sequence[0] == head:
                    sequence.pop(0)
        return result
    return ["General"] + list(reversed([name for name in order(cfg_name, set()) if name != "General"]))


def _parse_stations(items):
    st_map = {}
    for k, v in items:
        m = re.match(r"^\*\.station\[(\d+)(?:\.\.(\d+))?\]\.(.+)$", k)
        if not m:
            continue
        lo = int(m.group(1))
        hi = int(m.group(2)) if m.group(2) else lo
        prop = m.group(3)
        val = _unquote(v)
        for idx in range(lo, hi + 1):
            st = st_map.setdefault(idx, {"id": idx, "name": f"Station {idx}", "lat": 0.0, "lon": 0.0, "alt": 0.0, "mobile": False})
            if prop == "label":
                st["name"] = val
            elif prop == "nodeType":
                st["mobile"] = (val == "mobile")
            elif prop == "mobility.latitude":
                st["lat"] = float(val)
            elif prop == "mobility.longitude":
                st["lon"] = float(val)
            elif prop == "mobility.altitudeKm":
                st["alt"] = _num(val)
            elif prop == "mobility.speed":
                st["speed"] = _num(val)
            elif prop == "mobility.routeStart":
                st["start"] = _num(val)
            elif prop == "mobility.route":
                pts = []
                for pair in val.split():
                    if "," in pair:
                        a, b = pair.split(",", 1)
                        pts.append([float(a), float(b)])
                if pts:
                    st["route"] = pts
                    st["lat"], st["lon"] = pts[0][0], pts[0][1]
    return [st_map[i] for i in sorted(st_map)]


def _parse_terminals(items, limit):
    if limit <= 0:
        return []
    t_map = {}
    for k, v in items:
        m = re.match(r"^\*\.terminal\[(\d+)\]\.mob\.(centerLatitude|centerLongitude)$", k)
        if not m:
            continue
        idx = int(m.group(1))
        if idx >= limit:
            continue
        t = t_map.setdefault(idx, {"id": idx, "name": f"T{idx}", "lat": 0.0, "lon": 0.0, "alt": 0.0})
        if m.group(2) == "centerLatitude":
            t["lat"] = _num(v)
        else:
            t["lon"] = _num(v)
    return [t_map[i] for i in sorted(t_map)]


def read_ini_config(config_name=None, entry_path=None):
    configs, sections, extends = _parse_ini_tree(entry_path or INI_PATH)
    names = [c["name"] for c in configs]
    if config_name is not None and config_name not in names:
        raise ValueError("Unknown scenario")
    active = config_name or (names[0] if names else "Dijkstra132")
    chain = _resolve_chain(active, extends)

    flat = {}
    items = []
    resolved = {}
    for sec in chain:
        for k, v in reversed(sections.get(sec, [])):
            flat[k] = _unquote(v)
            items.append((k, v))
            if k in resolved:
                del resolved[k]
            resolved[k] = {"k": k, "v": v, "sec": sec}

    def get_num(keys, default, conv=_num):
        for k in keys:
            if k in flat:
                return conv(flat[k], default)
        return default

    def get_str(keys, default):
        for k in keys:
            if k in flat:
                return flat[k]
        return default

    num_terminals = int(get_num(["**.numTerminals", "*.numTerminals"], 0))
    desc = next((c["description"] for c in configs if c["name"] == active), "")

    return {
        "iniPath": entry_path or INI_PATH,
        "leoFound": bool(LEO_DIR),
        "leoDir": LEO_DIR,
        "activeConfig": active,
        "description": desc,
        "extends": extends.get(active, []),
        "chain": chain,
        "configs": [{**c, "extends": extends.get(c["name"], [])} for c in configs if c["name"] != "Dijkstra132Terminals"],
        "ownEntries": [{"k": k, "v": v} for k, v in sections.get(active, [])],
        "effectiveEntries": list(resolved.values()),
        "simTimeLimit": get_num(["sim-time-limit"], 600.0, _time_sec),
        "minimumElevation": get_num(["*.topologyControl.minimumElevation"], 10.0),
        "metric": get_str(["*.satellite[*].net.routing.metric"], "delay"),
        "routingType": get_str(["*.satellite[*].net.routingType"], "Sat_Net_DijkstraRouting"),
        "altitude": get_num(["*.constellationCreator.altitude"], 550.0),
        "inclination": get_num(["*.constellationCreator.inclination"], 53.0),
        "numSatellites": int(get_num(["*.numSatellites"], 132)),
        "planes": int(get_num(["*.**.planeCount", "*.constellationCreator.planeCount"], 12)),
        "phasing": int(get_num(["*.**.interPlaneSpacing", "*.constellationCreator.interPlaneSpacing"], 1)),
        "raanSpread": get_num(["*.constellationCreator.raanSpread"], 360.0),
        "baseYear": int(get_num(["*.constellationCreator.baseYear"], 20)),
        "baseDay": get_num(["*.constellationCreator.baseDay"], 1.5),
        "maxRoutes": int(get_num(["*.mapDataSource.maxRoutes"], 8)),
        "routeLifetime": get_num(["*.mapDataSource.routeLifetime"], 30.0, _time_sec),
        "numTerminals": num_terminals,
        "stations": _parse_stations(items),
        "terminals": _parse_terminals(items, num_terminals),
    }


def parse_routes_csv(csv_text):
    reader = csv.DictReader(io.StringIO(csv_text))
    required = "time route source station firstSat exitSat satHops uplinkMs satToStationMs totalMs path".split()
    if reader.fieldnames != required:
        raise ValueError("Unsupported routes.csv header")
    routes = []
    previous = -1.0
    for line, row in enumerate(reader, 2):
        try:
            if None in row or any(v is None for v in row.values()):
                raise ValueError("Wrong column count")
            if not re.fullmatch(r"(?:GS|T)\d+", row["source"]):
                raise ValueError("Invalid source")
            path = [int(x) for x in row["path"].split("-")]
            t, ms = float(row["time"]), float(row["totalMs"])
            first, last = int(row["firstSat"]), int(row["exitSat"])
            hops = int(row["satHops"])
            up, down = float(row["uplinkMs"]), float(row["satToStationMs"])
            if not all(math.isfinite(x) and x >= 0 for x in (t, ms, up, down)) or t < previous:
                raise ValueError("Invalid or unsorted numeric data")
            if not path or min(path) < 0 or path[0] != first or path[-1] != last or hops != len(path) - 1:
                raise ValueError("Inconsistent path")
            entry = [t, int(row["route"]), int(re.sub(r"^[A-Z]+", "", row["source"])),
                     int(row["station"]), hops, ms, path, int(row["source"].startswith("T")),
                     first, last, up, down]
            if min(entry[1:5]) < 0:
                raise ValueError("Negative identifier")
            routes.append(entry)
            previous = t
        except (ValueError, TypeError) as exc:
            raise ValueError(f"Invalid CSV row {line}: {exc}") from exc
    return routes


def export_offline_result(result):
    target = Path(BASE_DIR, "data/latest_result.js")
    tmp = target.with_suffix(".tmp")
    tmp.write_text("window.SIM_RESULT=" + json.dumps(result, ensure_ascii=True, allow_nan=False) + ";\n", encoding="utf-8")
    os.replace(tmp, target)


def run_simulation(config_name="Dijkstra132"):
    if not RUN_LOCK.acquire(blocking=False):
        raise RuntimeError("Another simulation is running")
    try:
        from native_engine import configuration
        files = {p.relative_to(LEO_DIR).as_posix(): p.read_bytes() for p in Path(LEO_DIR).rglob("*.ini")}
        cfg = configuration(read_ini_config(config_name))
        run_id = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ-") + uuid.uuid4().hex
        run_dir = Path(BASE_DIR, "data", "runs", run_id)
        run_dir.mkdir(parents=True)
        input_path = run_dir / "engine-input.json"
        input_path.write_text(json.dumps(cfg, ensure_ascii=False, allow_nan=False), encoding="utf-8")
        executable = Path(BASE_DIR, "engine", "engine.exe")
        if not executable.is_file():
            raise ValueError("Local engine is missing")
        proc = subprocess.run([str(executable), str(input_path), str(run_dir)], capture_output=True, timeout=600, creationflags=NO_WIN)
        (run_dir / "run.log").write_bytes(proc.stdout + proc.stderr)
        if proc.returncode:
            raise RuntimeError(proc.stderr.decode("utf-8", errors="replace"))
        raw = (run_dir / "routes.csv").read_bytes()
        parse_routes_csv(raw.decode("utf-8-sig"))
        profile = json.loads(Path(BASE_DIR, "replay-model.json").read_text(encoding="utf-8"))
        bundle, payload = make_bundle(files, raw, config_name, profile["modelHash"], run_id=run_id,
                                     command=["engine.exe", "engine-input.json", "."])
        bundle["manifest"]["engine"] = {"id": "leo-native", "version": "1.0.0"}
        bundle["manifest"]["provenance"] = "native-simulation"
        write_bundle(run_dir, bundle, payload)
        result = {"ok": True, "bundle": bundle, "runId": run_id}
        (run_dir / "result.json").write_text(json.dumps(result, ensure_ascii=False, allow_nan=False), encoding="utf-8")
        export_offline_result(result)
        pointer = Path(BASE_DIR, "data", "latest.json")
        tmp = pointer.with_suffix(".tmp")
        tmp.write_text(json.dumps({"runId": run_id}), encoding="utf-8")
        os.replace(tmp, pointer)
        return result
    finally:
        RUN_LOCK.release()


class SimHandler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=BASE_DIR, **kwargs)

    def log_message(self, format, *args):
        pass

    def send_json(self, payload, status=200):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path == "/api/config":
            qs = urllib.parse.parse_qs(parsed.query)
            cfg_name = qs.get("config", [None])[0]
            try:
                return self.send_json(read_ini_config(cfg_name))
            except (ValueError, OSError) as exc:
                return self.send_json({"error": str(exc)}, 400)
        if parsed.path == "/api/result":
            pointer = Path(BASE_DIR, "data/latest.json")
            if not pointer.exists():
                return self.send_json(None)
            run_id = json.loads(pointer.read_text())["runId"]
            if not re.fullmatch(r"[A-Za-z0-9-]+", run_id):
                return self.send_json({"error": "Invalid result pointer"}, 500)
            return self.send_json(json.loads(Path(BASE_DIR, "data/runs", run_id, "result.json").read_text(encoding="utf-8")))
        return super().do_GET()

    def do_POST(self):
        if self.headers.get("Origin") not in (None, f"http://127.0.0.1:{self.server.server_port}", f"http://localhost:{self.server.server_port}"):
            return self.send_json({"error": "Origin rejected"}, 403)
        try:
            length = int(self.headers.get("Content-Length", 0))
            if not 0 <= length <= 4096:
                raise ValueError("Request too large")
            body = json.loads(self.rfile.read(length).decode("utf-8")) if length else {}
            if not isinstance(body, dict):
                raise ValueError("Expected JSON object")
        except (ValueError, UnicodeError) as exc:
            return self.send_json({"error": str(exc)}, 400)

        if self.path == "/api/open-ini":
            try:
                if not LEO_DIR or not os.path.isfile(INI_PATH):
                    raise ValueError("로컬 INI 설정을 찾을 수 없습니다.")
                subprocess.Popen(["notepad.exe", INI_PATH], creationflags=NO_WIN)
                return self.send_json({"ok": True, "path": INI_PATH})
            except (ValueError, OSError) as exc:
                return self.send_json({"error": str(exc)}, 400)

        if self.path == "/api/run":
            try:
                cfg_name = body.get("configName") or read_ini_config().get("activeConfig", "Dijkstra132")
                return self.send_json(run_simulation(cfg_name))
            except Exception as e:
                return self.send_json({"ok": False, "error": str(e)}, status=500)

        self.send_error(404)


if __name__ == "__main__":
    port = 8090
    url = f"http://127.0.0.1:{port}"
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", port), SimHandler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()

    try:
        import webview
        webview.create_window("LEO Network Simulator", url, width=1520, height=900)
        webview.start()
    except ImportError:
        webbrowser.open(url)
        input(f"Running at {url} (Press Enter to quit)...\n")
    finally:
        srv.shutdown()
