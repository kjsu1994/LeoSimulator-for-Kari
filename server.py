# SPDX-License-Identifier: GPL-3.0-only
# Modified/adapted 2026-11-16; see THIRD_PARTY_NOTICES.txt.
import csv
import io
import math
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
import logging

BASE_DIR = os.path.dirname(sys.executable) if getattr(sys, "frozen", False) else os.path.dirname(os.path.abspath(__file__))
DATA_ROOT = None
LOCATION_PROFILE = None
WORKSPACE_NOTICE = ""
RESULTS_WRITABLE = True
logger = logging.getLogger('leo')
RUN_LOCK = threading.Lock()
NO_WIN = getattr(subprocess, "CREATE_NO_WINDOW", 0)


LEO_DIR = os.path.join(BASE_DIR, "settings")
from result_bundle import ENTRY
INI_PATH = os.path.join(LEO_DIR, *ENTRY.split("/"))


def results_options():
    return {'results':DATA_ROOT} if DATA_ROOT is not None else {}


def results_directory():
    return DATA_ROOT if DATA_ROOT is not None else Path(BASE_DIR)/'data'


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


def read_ini_config(config_name=None, entry_path=None, variables=None):
    configs, sections, extends = _parse_ini_tree(entry_path or INI_PATH)
    names = [c["name"] for c in configs]
    if config_name is not None and config_name not in names:
        raise ValueError("Unknown scenario")
    active = config_name or (names[0] if names else "Dijkstra132")
    chain = _resolve_chain(active, extends)

    resolved = {}
    for sec in chain:
        for k,v in reversed(sections.get(sec,[])):
            if k in resolved:del resolved[k]
            resolved[k]={"k":k,"v":v,"sec":sec}
    desc=next((c['description'] for c in configs if c['name']==active),'')
    selectable=[c for c in configs if c['name']!='Dijkstra132Terminals' and dict(sections.get(c['name'],[])).get('abstract')!='true']
    cfg = {
        "iniPath": entry_path or INI_PATH,
        "leoFound": bool(LEO_DIR),
        "leoDir": LEO_DIR,
        "activeConfig": active,
        "description": desc,
        "extends": extends.get(active, []),
        "chain": chain,
        "configs": [{**c, "extends": extends.get(c["name"], [])} for c in selectable],
        "ownEntries": [{"k": k, "v": v} for k, v in sections.get(active, [])],
        "effectiveEntries": list(resolved.values()),
        "terminals": [],
    }
    cfg["variables"] = variables or {}
    from native_engine import configuration
    return configuration(cfg)


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


def run_simulation(config_name="Dijkstra132"):
    if not RUN_LOCK.acquire(blocking=False):
        raise RuntimeError("Another simulation is running")
    try:
        from run_manager import execute
        return execute(BASE_DIR, LEO_DIR, INI_PATH, config_name, read_ini_config, **results_options())
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
        parsed=urllib.parse.urlparse(self.path)
        if parsed.path=='/api/experiments':
            rows=[]
            for path in sorted((results_directory()/'experiments').glob('*/manifest.json'),reverse=True):
                try:
                    m=json.loads(path.read_text(encoding='utf8'))
                    if m.get('statistics') and m.get('status')=='completed':rows.append(m)
                except (OSError,ValueError):continue
            return self.send_json(rows)
        if parsed.path.startswith('/api/experiments/'):
            try:
                from comparison import folder
                run_id=parsed.path.rsplit('/',1)[-1];folder(BASE_DIR,run_id,**results_options())
                manifest=json.loads((results_directory()/'experiments'/run_id/'manifest.json').read_text(encoding='utf8'))
                if manifest.get('status')!='completed':raise ValueError('Incomplete experiment')
                return self.send_json(json.loads((results_directory()/'experiments'/run_id/'statistics.json').read_text(encoding='utf8')))
            except (ValueError,OSError):return self.send_json({'error':'반복 실험 결과가 없습니다.'},404)

        parsed = urllib.parse.urlparse(self.path)
        if parsed.path == "/api/runs":
            from comparison import runs, run_metadata
            query = urllib.parse.parse_qs(parsed.query)
            return self.send_json(run_metadata(BASE_DIR,**results_options()) if query.get("metadata") == ["1"] else runs(BASE_DIR,**results_options()))
        if parsed.path.startswith("/api/run/"):
            from run_manager import state
            try:
                return self.send_json(state(parsed.path.rsplit("/",1)[-1]))
            except ValueError as exc:
                return self.send_json({"error":str(exc)},404)
        if parsed.path == "/api/compare":
            from comparison import compare
            qs=urllib.parse.parse_qs(parsed.query)
            try:
                return self.send_json(compare(BASE_DIR,qs.get("baseline",[""])[0],qs.get("run",[""])[0],**results_options()))
            except (ValueError,OSError,KeyError) as exc:
                return self.send_json({"error":str(exc)},400)
        if parsed.path == "/api/config":
            qs = urllib.parse.parse_qs(parsed.query)
            cfg_name = qs.get("config", [None])[0]
            try:
                from run_manager import EXECUTION_MODES, load_external_inputs
                if qs.get("defaults")==["1"]:
                    from config_editor import preview_defaults
                    from run_manager import CONFIG_LOCK
                    with CONFIG_LOCK:
                        return self.send_json(preview_defaults(INI_PATH,cfg_name))
                cfg = load_external_inputs(read_ini_config(cfg_name), INI_PATH)
                cfg['capabilities']['comparisonExecutionModes'] = EXECUTION_MODES
                cfg['capabilities']['userWorkspace'] = DATA_ROOT is not None
                if DATA_ROOT is not None:
                    cfg['workspacePath']=str(DATA_ROOT)
                    cfg['workspaceNotice']=WORKSPACE_NOTICE
                return self.send_json(cfg)
            except (ValueError, OSError) as exc:
                return self.send_json({"error": str(exc)}, 400)
        if parsed.path == "/api/result":
            return self.get_result(urllib.parse.parse_qs(parsed.query))
        return super().do_GET()

    def result_folder(self, run_id):
        from comparison import completed_manifest, folder
        directory=folder(BASE_DIR,run_id,**results_options())
        if DATA_ROOT is not None and not directory.exists():
            sample=Path(BASE_DIR)/'data/latest.json'
            if json.loads(sample.read_bytes())['runId']==run_id:
                directory=folder(BASE_DIR,run_id)
        return directory,completed_manifest(directory)

    def get_result(self, query):
        run_id=query.get('runId',[None])[0]
        if run_id is None:
            for root in dict.fromkeys((results_directory(),Path(BASE_DIR)/'data')):
                try:
                    pointer=json.loads((root/'latest.json').read_bytes())
                    run_id=pointer['runId']
                    self.result_folder(run_id)
                    break
                except (OSError,ValueError,KeyError,TypeError,AttributeError):
                    run_id=None
            if run_id is None:return self.send_json(None)
        try:
            directory,manifest=self.result_folder(run_id)
            result=directory/'result.json'
            if query.get('metadata')==['1'] or not result.is_file() or result.stat().st_size>8*1024*1024:
                urls={name:'/data/runs/'+run_id+'/'+urllib.parse.quote(name,safe='/') for name in manifest['files']}
                return self.send_json({'ok':True,'runId':run_id,'bundle':{'manifest':manifest,'fileUrls':urls}})
            payload=json.loads(result.read_bytes())
            if payload['bundle']['manifest']!=manifest:raise ValueError('Result manifest mismatch')
            return self.send_json(payload)
        except (OSError,ValueError,KeyError,TypeError,AttributeError):
            return self.send_json({'error':'보관된 실행 결과가 없습니다.'},404)

    def send_head(self):
        path=urllib.parse.unquote(urllib.parse.urlsplit(self.path).path)
        if not path.startswith('/data/runs/'):
            return super().send_head()
        try:
            run_id,name=path[len('/data/runs/'):].split('/',1)
            directory,manifest=self.result_folder(run_id)
            if name not in manifest['files']:raise ValueError('Invalid result attachment')
            file=(directory/name).open('rb')
        except (OSError,ValueError,KeyError,TypeError,AttributeError):
            self.send_error(404)
            return None
        try:
            self.send_response(200)
            self.send_header('Content-Type',self.guess_type(name))
            self.send_header('Content-Length',str(os.fstat(file.fileno()).st_size))
            self.end_headers()
            return file
        except BaseException:
            file.close()
            raise

    def do_POST(self):
        if self.headers.get("Origin") not in (None, f"http://127.0.0.1:{self.server.server_port}", f"http://localhost:{self.server.server_port}"):
            return self.send_json({"error": "Origin rejected"}, 403)
        try:
            length = int(self.headers.get("Content-Length", 0))
            if not 0 <= length <= 1048576:
                raise ValueError("Request too large")
            body = json.loads(self.rfile.read(length).decode("utf-8")) if length else {}
            if not isinstance(body, dict):
                raise ValueError("Expected JSON object")
        except (ValueError, UnicodeError) as exc:
            return self.send_json({"error": str(exc)}, 400)

        if DATA_ROOT is not None and self.path in ('/api/open-user-data','/api/workspace'):
            try:
                if self.path=='/api/open-user-data':
                    os.startfile(str(DATA_ROOT))
                    return self.send_json({'ok':True})
                from user_workspace import save_result_choice
                from run_manager import idle_workspace
                target=body.get('path')
                if 'path' not in body or (target is not None and
                    (not isinstance(target,str) or not Path(target).is_absolute())):
                    raise ValueError('저장 폴더를 선택하세요.')
                with idle_workspace():
                    save_result_choice(LOCATION_PROFILE,Path(BASE_DIR),DATA_ROOT,None if target is None else Path(target))
                return self.send_json({'ok':True})
            except (OSError,ValueError,RuntimeError) as exc:
                logger.warning('Workspace operation failed: %s',exc)
                return self.send_json({'error':str(exc)},400)

        if self.path == "/api/open-ini":
            try:
                if not LEO_DIR or not os.path.isfile(INI_PATH):
                    raise ValueError("로컬 INI 설정을 찾을 수 없습니다.")
                subprocess.Popen(["notepad.exe", INI_PATH], creationflags=NO_WIN)
                return self.send_json({"ok": True, "path": INI_PATH})
            except (ValueError, OSError) as exc:
                return self.send_json({"error": str(exc)}, 400)

        if self.path == "/api/config":
            try:
                from config_editor import edit
                from run_manager import CONFIG_LOCK
                with CONFIG_LOCK:
                    result=edit(INI_PATH,body.get("configName"),body.get("changes",{}),read_ini_config,body.get("scenario"),reset=body.get("resetDefaults") is True)
                return self.send_json(result)
            except (ValueError,OSError,KeyError) as exc:
                return self.send_json({"error":str(exc)},400)
        if self.path in ("/api/run","/api/experiments"):
            try:
                if self.path=="/api/experiments" and body.get("algorithms") is None:raise ValueError("Select algorithms")
                from run_manager import CONFIG_LOCK, start
                with CONFIG_LOCK:
                    if getattr(self.server,'closing',False):raise ValueError('프로그램을 종료하고 있습니다.')
                    if not RESULTS_WRITABLE:raise ValueError('결과를 저장할 수 없습니다. INI 상세에서 결과 폴더를 선택한 뒤 앱을 다시 여세요.')
                    name=body.get("configName") or read_ini_config()["activeConfig"]
                    return self.send_json(start(BASE_DIR,LEO_DIR,INI_PATH,name,read_ini_config,body.get("variables"),body.get("algorithms") if self.path=="/api/experiments" else None,body.get("research"),**results_options()),202)
            except (ValueError,OSError,KeyError) as exc:
                return self.send_json({"ok":False,"error":str(exc)},400)
        if self.path.startswith("/api/run/") and self.path.endswith("/cancel"):
            try:
                from run_manager import cancel
                return self.send_json(cancel(self.path.split("/")[-2]))
            except ValueError as exc:
                return self.send_json({"error":str(exc)},404)

        self.send_error(404)


def desktop():
    from run_manager import shutdown

    class Server(http.server.ThreadingHTTPServer):
        def handle_error(self, request, client_address):
            logger.exception('HTTP request failed')

    with Server(('127.0.0.1',0),SimHandler) as srv:
        url=f'http://127.0.0.1:{srv.server_port}'
        logger.info('Server ready at %s',url)
        serving=threading.Thread(target=srv.serve_forever,daemon=True)
        serving.start()
        try:
            try:
                import webview
            except ImportError:
                if DATA_ROOT is not None:raise
                webbrowser.open(url)
                input(f'Running at {url} (Press Enter to quit)...')
                return

            class WorkspaceDialog:
                def choose_folder(self):
                    picked=window.create_file_dialog(webview.FOLDER_DIALOG)
                    return picked[0] if picked else None

            window=webview.create_window('LEO Network Simulator',url,width=1520,height=900,
                                         js_api=WorkspaceDialog() if DATA_ROOT is not None else None)
            webview.start()
        finally:
            srv.closing=True
            srv.shutdown()
            shutdown()


def main():
    if not getattr(sys,'frozen',False):
        desktop()
        return
    import ctypes
    from user_workspace import initialize_results, location_profile,select_results,setup_logging
    from windows_runtime import workspace_lock
    from contextlib import ExitStack
    global DATA_ROOT, LOCATION_PROFILE, WORKSPACE_NOTICE, RESULTS_WRITABLE
    root=None
    try:
        assets=Path(BASE_DIR).resolve()
        LOCATION_PROFILE=location_profile(assets)
        with ExitStack() as stack:
            stack.enter_context(workspace_lock(LOCATION_PROFILE))
            DATA_ROOT,WORKSPACE_NOTICE=select_results(assets,LOCATION_PROFILE)
            root=assets
            try:
                DATA_ROOT.mkdir(parents=True,exist_ok=True)
                stack.enter_context(workspace_lock(DATA_ROOT))
                initialize_results(DATA_ROOT)
            except OSError:
                RESULTS_WRITABLE=False
                WORKSPACE_NOTICE='현재 결과 폴더에 쓸 수 없습니다. 결과 폴더를 선택한 뒤 앱을 다시 여세요.'
            try:
                handler=setup_logging(assets)
            except OSError:
                handler=None
            try:
                logger.info('Starting LEO Network Simulator 1.0.0 from %s',BASE_DIR)
                desktop()
                logger.info('Application closed')
            except Exception:
                logger.exception('Application startup or shutdown failed')
                raise
            finally:
                if handler is not None:
                    logger.removeHandler(handler)
                    handler.close()
    except Exception as exc:
        message=str(exc)
        if root is not None and (root/'logs'/'app.log').is_file():
            message+=f'\n\n로그: {root/"logs"/"app.log"}'
        ctypes.windll.user32.MessageBoxW(None,message,'LEO Network Simulator',0x10)


if __name__=='__main__':
    main()
