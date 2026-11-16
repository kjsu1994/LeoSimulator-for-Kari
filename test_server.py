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
        raw = Path(server.BASE_DIR, "data/routes.csv").read_text()
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
        self.assertEqual(server.read_ini_config("Dijkstra132Hop")["metric"], "delay")

    def test_offline_export_preserves_result(self):
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "data").mkdir()
            result = {"runId": "test", "routes": [[1, 2]], "states": [{"t": 1}], "config": {"description": "saved"}}
            with patch.object(server, "BASE_DIR", tmp):
                server.export_offline_result(result)
            script = Path(tmp, "data/latest_result.js").read_text()
            self.assertEqual(json.loads(script.removeprefix("window.SIM_RESULT=").strip().removesuffix(";")), result)
            self.assertFalse(Path(tmp, "data/latest_result.tmp").exists())

    def test_missing_include_is_an_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp, "omnetpp.ini")
            path.write_text("include missing.ini\n")
            with self.assertRaises(ValueError):
                server._parse_ini_tree(str(path))

    def test_inputs_ignore_results_but_track_engine_and_includes(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            for folder in ("src", "simulations/Dijkstra132/results"):
                (base/folder).mkdir(parents=True)
            (base/"src/test.cc").write_text("engine")
            ini = base/"simulations/Dijkstra132/omnetpp.ini"
            ini.write_text("[General]")
            with patch.object(server, "LEO_DIR", tmp):
                a, first = server.capture_inputs()
                (base/"simulations/Dijkstra132/results/routes.csv").write_text("old result")
                self.assertEqual(server.capture_inputs()[1], first)
                ini.write_text("[General]\n# changed")
                self.assertNotEqual(server.capture_inputs()[1], first)
                ini.write_text("[General]")
                (base/"src/test.cc").write_text("changed engine")
                self.assertNotEqual(server.capture_inputs()[1], first)
                self.assertNotIn("simulations/Dijkstra132/results/routes.csv", a)

if __name__ == "__main__":
    unittest.main()
