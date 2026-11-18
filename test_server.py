# SPDX-License-Identifier: GPL-3.0-only
# Modified/adapted 2026-11-16; see THIRD_PARTY_NOTICES.txt.
import csv
import io
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
import server

HEADER = "time,route,source,station,firstSat,exitSat,satHops,uplinkMs,satToStationMs,totalMs,path\n"
ROW = "1.2,1,GS0,2,3,4,1,2.5,3.5,6,3-4\n"

class DataTests(unittest.TestCase):
    def test_all_existing_csv_fields_survive(self):
        raw = Path(server.BASE_DIR, "testdata/bundle/routes.csv").read_text()
        rows = list(csv.DictReader(io.StringIO(raw)))
        actual = server.parse_routes_csv(raw)
        self.assertEqual(len(actual), len(rows))
        for r, p in zip(rows, actual):
            self.assertEqual(p, [float(r["time"]), int(r["route"]), int(r["source"].lstrip("GST")), int(r["station"]), int(r["satHops"]), float(r["totalMs"]), list(map(int,r["path"].split("-"))), int(r["source"].startswith("T")), int(r["firstSat"]), int(r["exitSat"]), float(r["uplinkMs"]), float(r["satToStationMs"])])

    def test_bad_csv_is_rejected_instead_of_skipped(self):
        for raw in ["wrong\n", HEADER + ROW.replace("GS0", "bad"), HEADER + ROW.replace("1.2", "nan"), HEADER + ROW.replace("3-4", "3-5"), HEADER + ROW + ROW.replace("1.2", "0.1"), HEADER + ROW.replace(",1,2.5", ",2,2.5")]:
            with self.subTest(raw=raw), self.assertRaises(ValueError):
                server.parse_routes_csv(raw)
        self.assertEqual(server.parse_routes_csv(HEADER), [])

    def test_terminal_and_zero_hop(self):
        p = server.parse_routes_csv(HEADER + "0,1,T7,2,3,3,0,0,1,1,3\n")[0]
        self.assertEqual(p[2], 7)
        self.assertEqual(p[7], 1)

    def test_unknown_config_cannot_reach_shell(self):
        with self.assertRaises(ValueError):
            server.read_ini_config("Dijkstra132; touch injected")
        self.assertEqual(server.read_ini_config("Dijkstra132Hop")["metric"], "hop")

    def test_ini_editor_opens_live_settings_without_modifying_files(self):
        import http.server
        import threading
        import urllib.request
        before = Path(server.INI_PATH).read_bytes()
        with patch.object(server.subprocess, "Popen") as launch:
            httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), server.SimHandler)
            thread = threading.Thread(target=httpd.serve_forever, daemon=True)
            thread.start()
            try:
                request = urllib.request.Request(f"http://127.0.0.1:{httpd.server_port}/api/open-ini", method="POST")
                with urllib.request.urlopen(request) as response:
                    data = json.load(response)
                self.assertTrue(data["ok"])
                self.assertEqual(data["path"], server.INI_PATH)
                launch.assert_called_once_with(["notepad.exe", server.INI_PATH], creationflags=server.NO_WIN)
                self.assertEqual(Path(server.INI_PATH).read_bytes(), before)
            finally:
                httpd.shutdown()
                httpd.server_close()
                thread.join()

    def test_missing_include_is_an_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp, "omnetpp.ini")
            path.write_text("include missing.ini\n")
            with self.assertRaises(ValueError):
                server._parse_ini_tree(str(path))

    def test_local_engine_is_reproducible_without_original(self):
        import subprocess
        from native_engine import configuration
        cfg=configuration(server.read_ini_config("Dijkstra132"))
        cfg["simTimeLimit"]=10
        outputs=[]
        with tempfile.TemporaryDirectory() as tmp:
            for i in range(2):
                folder=Path(tmp,str(i));folder.mkdir()
                file=folder/"input.json";file.write_text(json.dumps(cfg))
                subprocess.run([str(Path(server.BASE_DIR,"engine/engine.exe")),str(file),str(folder)],check=True)
                outputs.append((folder/"routes.csv").read_bytes())
                summary=json.loads((folder/"summary.json").read_text())
                self.assertEqual(summary["attempted"],summary["received"]+summary["dropped"]+summary["inFlight"])
                server.parse_routes_csv(outputs[-1].decode())
        self.assertEqual(outputs[0],outputs[1])

    def test_native_queue_and_hop_limit(self):
        import subprocess
        from native_engine import configuration
        base=configuration(server.read_ini_config("Dijkstra132"))
        base["simTimeLimit"]=2
        base["minimumElevation"]=-90
        base["maxGroundLinks"]=500
        base["packetTrace"]=True
        base["groundlinkDatarate"]=1000000
        base["islDatarate"]=1000000
        for st in base["stations"]:
            st["enabled"]=False
        base["stations"][0].update(enabled=True,dstStation=10,startTime=0,sendInterval=.001)
        with tempfile.TemporaryDirectory() as tmp:
            for hops in (64,0):
                cfg=json.loads(json.dumps(base));cfg["maxHops"]=hops
                folder=Path(tmp,str(hops));folder.mkdir()
                file=folder/"input.json";file.write_text(json.dumps(cfg))
                subprocess.run([str(Path(server.BASE_DIR,"engine/engine.exe")),str(file),str(folder)],check=True)
                summary=json.loads((folder/"summary.json").read_text())
                self.assertEqual(summary["attempted"],summary["received"]+summary["dropped"]+summary["inFlight"])
                self.assertGreater(summary["sent"],100)
                if hops==0:
                    self.assertEqual(summary["received"],0)
                    self.assertGreater(summary["dropped"],0)
                else:
                    self.assertGreater(summary["received"],0)
                    self.assertGreater(summary["inFlight"],0)
                    deliveries=list(csv.DictReader((folder/"events.csv").read_text().splitlines()))
                    self.assertTrue(any(r["event"]=="receive" and float(r["detail"])>.1 for r in deliveries))

    def test_native_runs_from_isolated_product_folder(self):
        import shutil
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            shutil.copytree(Path(server.BASE_DIR,"settings"),root/"settings")
            (root/"engine").mkdir()
            shutil.copy2(Path(server.BASE_DIR,"engine/engine.exe"),root/"engine/engine.exe")
            shutil.copy2(Path(server.BASE_DIR,"replay-model.json"),root/"replay-model.json")
            with patch.object(server,"BASE_DIR",tmp),patch.object(server,"LEO_DIR",str(root/"settings")),patch.object(server,"INI_PATH",str(root/"settings/simulations/Dijkstra132/omnetpp.ini")):
                result=server.run_simulation("Dijkstra132")
            self.assertEqual(result["bundle"]["manifest"]["engine"]["id"],"leo-native")
            self.assertTrue((root/"data/runs"/result["runId"]/"routes.csv").is_file())
            self.assertFalse((root/"leo-main").exists())

    def test_hop_preset_selects_hop_algorithm(self):
        from native_engine import configuration
        cfg=configuration(server.read_ini_config("Dijkstra132Hop"))
        self.assertEqual(cfg["metric"],"hop")
        self.assertEqual(cfg["algorithm"],"dijkstra-hop")

    def test_native_rejects_invalid_settings(self):
        from native_engine import configuration
        cfg=server.read_ini_config("Dijkstra132")
        cfg["effectiveEntries"].append({"k":"*.**.planeCount","v":"0","sec":"probe"})
        with self.assertRaises(ValueError): configuration(cfg)

if __name__ == "__main__":
    unittest.main()
