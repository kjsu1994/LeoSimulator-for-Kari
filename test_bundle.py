# SPDX-License-Identifier: GPL-3.0-only
# Modified/adapted 2026-11-16; see THIRD_PARTY_NOTICES.txt.
import hashlib
import json
import tempfile
import unittest
from pathlib import Path
from result_bundle import ini_dependencies, make_bundle, write_bundle, ENTRY

class BundleTests(unittest.TestCase):
    def test_recursive_ini_preserves_bytes_and_relative_tree(self):
        entry=b'[General]\ninclude stations/nodes.ini\ninclude ../shared.ini\n'
        files={ENTRY:entry,'simulations/Network/stations/nodes.ini':b'# stations\r\n','simulations/shared.ini':b'# shared\n'}
        bundle,payload=make_bundle(files,b'csv','Dijkstra132','model',run_id='test-run-id')
        with tempfile.TemporaryDirectory() as tmp:
            write_bundle(tmp,bundle,payload)
            for name,data in payload.items():
                self.assertEqual(Path(tmp,name).read_bytes(),data)
                self.assertEqual(bundle['manifest']['files'][name],hashlib.sha256(data).hexdigest())
            self.assertEqual(Path(tmp,'config',ENTRY).read_bytes(),entry)
            self.assertNotIn('src', [p.name for p in Path(tmp).iterdir()])

    def test_missing_cycles_and_external_includes_fail(self):
        for source in [b'include missing.ini',b'include omnetpp.ini',b'include /absolute.ini',b'include ../../../../outside.ini']:
            with self.subTest(source=source),self.assertRaises(ValueError):
                ini_dependencies({ENTRY:source})

    def test_each_run_gets_unique_id(self):
        files={ENTRY:b'[General]'}
        a,_=make_bundle(files,b'csv','Dijkstra132','model')
        b,_=make_bundle(files,b'csv','Dijkstra132','model')
        self.assertNotEqual(a['manifest']['runId'],b['manifest']['runId'])
        self.assertEqual(a['manifest']['files'],b['manifest']['files'])

    def test_large_native_csv_is_referenced_without_overwriting_source(self):
        from result_bundle import make_native_bundle
        with tempfile.TemporaryDirectory() as tmp:
            folder=Path(tmp);raw=b'time,route,source,station,firstSat,exitSat,satHops,uplinkMs,satToStationMs,totalMs,path\n'+b'1,1,GS0,2,3,4,1,2.5,3.5,6,3-4\n'*180000
            self.assertGreater(len(raw),4*1024*1024);(folder/'routes.csv').write_bytes(raw)
            for name in ('summary.json','physical-events.json','common-srd.json'):(folder/name).write_text('{}')
            for name in ('timeseries.csv','network-events.jsonl','failures.csv','control-series.csv'):(folder/name).write_text('')
            bundle,files=make_native_bundle({ENTRY:b'[General]'},folder,'Example','model',{},'large-result-run')
            self.assertNotIn('routes.csv',files);self.assertNotIn('routes.csv',bundle['files'])
            self.assertEqual(bundle['manifest']['files']['routes.csv'],hashlib.sha256(raw).hexdigest())
            self.assertTrue(bundle['fileUrls']['routes.csv'].endswith('/routes.csv'))
            self.assertLess(len(json.dumps(bundle)),100000)
            write_bundle(folder,bundle,files);self.assertEqual((folder/'routes.csv').read_bytes(),raw)

if __name__=='__main__': unittest.main()
