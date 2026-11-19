# SPDX-License-Identifier: GPL-3.0-only
import copy,json,tempfile,time,unittest,shutil
from pathlib import Path
from research import paired_interval,aggregate,evidence,source_hashes,file_hash,measurements
from verify_workloads import cached_result
import server,run_manager

class ValidationTests(unittest.TestCase):
    def records(self):
        return [{'seed':seed,'algorithm':a,'config':{'seed':seed,'processingMode':'neutral'},'summary':{'metricVersion':4,'processingMode':'neutral','trafficHash':str(seed),'attempted':10,'received':10,'deliveryRate':80+index*2,'throughputMbps':1+index,'meanDelayMs':4+index,'flows':[]}} for seed in range(3) for index,a in enumerate(['dijkstra-delay','mcsr-ls'])]
    def test_paired_interval_unit_and_reproducibility(self):
        a=paired_interval([2]*10);self.assertEqual(a['ci95'],[2,2]);self.assertEqual(a,paired_interval([2]*10));self.assertIsNone(paired_interval([1])['ci95'])
        report=aggregate(self.records(),['dijkstra-delay','mcsr-ls'],list(range(3)))
        rate=report['comparisons'][0]['metrics'][0];self.assertEqual(rate['statistics']['meanDifference'],2);self.assertEqual(rate['statistics']['pairs'],3);self.assertIsNone(report['winner'])
    def test_incomplete_or_unpaired_experiments_rejected(self):
        rows=self.records()
        with self.assertRaises(ValueError):aggregate(rows[:-1],['dijkstra-delay','mcsr-ls'],list(range(3)))
        rows[-1]['summary']['trafficHash']='different'
        with self.assertRaises(ValueError):aggregate(rows,['dijkstra-delay','mcsr-ls'],list(range(3)))
        rows=self.records();rows[-1]['config']['queueCapacity']=1
        with self.assertRaises(ValueError):aggregate(rows,['dijkstra-delay','mcsr-ls'],list(range(3)))
    def test_missing_receivers_not_discarded_and_cohorts_not_mixed(self):
        rows=self.records();rows[-1]['summary']['received']=0
        report=aggregate(rows,['dijkstra-delay','mcsr-ls'],list(range(3)))
        delay=next(m for m in report['comparisons'][0]['metrics'] if m['name']=='meanDelayMs')
        self.assertIsNone(delay['statistics']);self.assertEqual(delay['missingSeeds'],[2]);self.assertEqual(delay['observedPairs'],2)
        s={'measurement':{'received':1,'jitterMs':None},'flows':[{'received':10,'jitterMs':9}]};self.assertIsNone(measurements(s)['jitterMs'])
    def test_cache_invalidated_by_input_output_or_incomplete_record(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp);files={name:'{}' for name in ['summary.json','physical-events.json','common-srd.json','engine-input.json']}
            for name,text in files.items():(p/name).write_text(text)
            record={'signature':'input-engine-test','outputs':{name:file_hash(p/name) for name in files}};(p/'validated.json').write_text(json.dumps(record))
            self.assertEqual(cached_result(p,'input-engine-test'),record);self.assertIsNone(cached_result(p,'changed'))
            (p/'summary.json').write_text('changed');self.assertIsNone(cached_result(p,'input-engine-test'))
            (p/'validated.json').write_text('{');self.assertIsNone(cached_result(p,'input-engine-test'))
    def test_evidence_is_bound_to_engine_and_sources(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp);(p/'engine').mkdir();(p/'engine/engine.exe').write_bytes(b'test');(p/'input.py').write_text('a=1')
            report=p/'data/validation/verification-test';report.mkdir(parents=True)
            (report/'test.log').write_text('passed');value={'checks':[{'status':'passed','log':'test.log','sha256':file_hash(report/'test.log')}],'status':'passed','createdAt':'test','engineSHA256':file_hash(p/'engine/engine.exe'),'sources':source_hashes(p)}
            (report/'report.json').write_text(json.dumps(value));self.assertEqual(evidence(p)['status'],'passed')
            (p/'input.py').write_text('a=2');self.assertEqual(evidence(p)['status'],'not-recorded')
    def test_statistics_history_and_research_request_api(self):
        import http.server,threading,urllib.request
        from unittest.mock import patch
        with tempfile.TemporaryDirectory() as tmp,patch.object(server,'BASE_DIR',tmp):
            p=Path(tmp)/'data/experiments/verification-1234';p.mkdir(parents=True)
            report={'schema':1,'seeds':[1,2],'comparisons':[]};manifest={'status':'completed','experimentId':'verification-1234','statistics':'statistics.json','seeds':[1,2]}
            (p/'statistics.json').write_text(json.dumps(report));(p/'manifest.json').write_text(json.dumps(manifest))
            httpd=http.server.ThreadingHTTPServer(('127.0.0.1',0),server.SimHandler);thread=threading.Thread(target=httpd.serve_forever,daemon=True);thread.start()
            try:
                url=f'http://127.0.0.1:{httpd.server_port}'
                with urllib.request.urlopen(url+'/api/experiments') as response:self.assertEqual(json.load(response),[manifest])
                with urllib.request.urlopen(url+'/api/experiments/verification-1234') as response:self.assertEqual(json.load(response),report)
                with patch.object(run_manager,'start',return_value={'ok':True}) as start:
                    options={'repetitions':10,'seedStart':7};request=urllib.request.Request(url+'/api/experiments',data=json.dumps({'configName':'A','algorithms':['mcsr-ls'],'research':options}).encode(),headers={'Content-Type':'application/json'})
                    with urllib.request.urlopen(request) as response:self.assertEqual(response.status,202)
                    self.assertEqual(start.call_args.args[-1],options)
                manifest['status']='cancelled';(p/'manifest.json').write_text(json.dumps(manifest))
                with urllib.request.urlopen(url+'/api/experiments') as response:self.assertEqual(json.load(response),[])
                import urllib.error
                with self.assertRaises(urllib.error.HTTPError) as error:urllib.request.urlopen(url+'/api/experiments/verification-1234')
                self.assertEqual(error.exception.code,404)
            finally:httpd.shutdown();httpd.server_close();thread.join()
    def test_unwritable_experiment_directory_is_failed_not_stuck(self):
        from unittest.mock import patch
        with tempfile.TemporaryDirectory() as tmp:
            cfg=server.read_ini_config('Dijkstra132');cfg['scenarioFile']='';cfg['failureReplay']='';cfg['islScheduleFile']=''
            with patch.object(Path,'mkdir',side_effect=PermissionError('read-only')):
                job=run_manager.start(tmp,tmp,Path(tmp)/'omnetpp.ini','A',lambda *args,**kwargs:copy.deepcopy(cfg))
                deadline=time.monotonic()+3
                while run_manager.state(job['runId'])['status'] in ('queued','running'):
                    self.assertLess(time.monotonic(),deadline);time.sleep(.01)
            self.assertEqual(run_manager.state(job['runId'])['status'],'failed')
    def test_cancel_during_statistics_never_publishes_completed_report(self):
        from unittest.mock import patch
        import research
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp);(p/'engine').mkdir();shutil.copy2(Path(server.BASE_DIR)/'engine/engine.exe',p/'engine/engine.exe');shutil.copy2(Path(server.BASE_DIR)/'replay-model.json',p/'replay-model.json')
            settings=p/'settings';settings.mkdir();ini=settings/'omnetpp.ini';ini.write_text('[General]\n*.numSatellites=12\n*.**.planeCount=4\n**.nrOfGS=2\n*.topologyControl.minimumElevation=-90\n[Config A]\nsim-time-limit=.2s\n*.station[0].traffic.enabled=true\n*.station[0].traffic.dstStation=1\n*.station[0].traffic.startTime=0s\n')
            reader=lambda name,variables=None:server.read_ini_config(name,entry_path=str(ini),variables=variables)
            def cancel_when_aggregating(records,algorithms,seeds):
                run_manager.cancel(records[0]['config']['experimentId']);return {'schema':1}
            with patch.object(research,'aggregate',side_effect=cancel_when_aggregating):
                job=run_manager.start(p,settings,ini,'A',reader,algorithms=['dijkstra-delay'],research={'repetitions':2})
                deadline=time.monotonic()+10
                while run_manager.state(job['runId'])['status'] in ('queued','running','stopping'):
                    self.assertLess(time.monotonic(),deadline);time.sleep(.02)
            self.assertEqual(run_manager.state(job['runId'])['status'],'cancelled')
            manifest=json.loads((p/'data/experiments'/job['runId']/'manifest.json').read_text(encoding='utf8'));self.assertEqual(manifest['status'],'cancelled')
    def test_measurement_ini_validation(self):
        from config_editor import edit
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp)/'omnetpp.ini';p.write_text('[General]\n*.numSatellites=12\n*.**.planeCount=4\n**.nrOfGS=2\n[Config A]\nsim-time-limit=10s\n*.station[0].traffic.enabled=true\n*.station[0].traffic.dstStation=1\n')
            reader=lambda name:server.read_ini_config(name,entry_path=str(p))
            result=edit(p,'A',{'warmupSeconds':2,'drainSeconds':3},reader);self.assertEqual(result['warmupSeconds'],2)
            before=p.read_bytes()
            with self.assertRaises(ValueError):edit(p,'A',{'warmupSeconds':8},reader)
            self.assertEqual(p.read_bytes(),before)
    def test_repeated_jobs_freeze_each_seed_and_persist_statistics(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp);settings=p/'settings';settings.mkdir();ini=settings/'omnetpp.ini'
            ini.write_text('[General]\n*.numSatellites=12\n*.**.planeCount=4\n**.nrOfGS=2\n*.topologyControl.minimumElevation=-90\n[Config A]\nsim-time-limit=.3s\n*.station[0].traffic.enabled=true\n*.station[0].traffic.dstStation=1\n*.station[0].traffic.startTime=0s\n*.station[0].traffic.sendInterval=.02s\n')
            (p/'engine').mkdir();shutil.copy2(Path(server.BASE_DIR)/'engine/engine.exe',p/'engine/engine.exe');shutil.copy2(Path(server.BASE_DIR)/'replay-model.json',p/'replay-model.json')
            reader=lambda name,variables=None:server.read_ini_config(name,entry_path=str(ini),variables=variables)
            job=run_manager.start(p,settings,ini,'A',reader,algorithms=['dijkstra-delay','mcsr-ls'],research={'repetitions':3,'warmupSeconds':.05,'drainSeconds':.05})
            deadline=time.monotonic()+45
            while run_manager.state(job['runId'])['status'] in ('queued','running'):
                self.assertLess(time.monotonic(),deadline);time.sleep(.03)
            state=run_manager.state(job['runId']);self.assertEqual(state['status'],'completed',state.get('error'));self.assertEqual(state['statistics']['completedRuns'],6)
            report=json.loads((p/'data/experiments'/job['runId']/'statistics.json').read_text(encoding='utf8'));self.assertEqual(report['seeds'],[23542,23543,23544])
            for row in state['completedRuns']:
                summary=json.loads((p/'data/runs'/row['runId']/'summary.json').read_text(encoding='utf8'));m=summary['measurement']
                self.assertEqual(m['attempted'],m['received']+m['dropped']+m['inFlight']);self.assertLess(m['attempted'],summary['attempted']);self.assertIn('capEvict',summary['operations'])
            with self.assertRaises(ValueError):run_manager.start(p,settings,ini,'A',reader,algorithms=['mcsr-ls'],research={'repetitions':True})

if __name__=='__main__':unittest.main()
