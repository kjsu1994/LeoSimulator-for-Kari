# SPDX-License-Identifier: GPL-3.0-only
import copy,json,tempfile,time,unittest,shutil
from pathlib import Path
from research import paired_interval,aggregate,evidence,source_hashes,file_hash,measurements
from verify_workloads import cached_result
import server,run_manager

class ValidationTests(unittest.TestCase):
    def records(self):
        return [{'seed':seed,'algorithm':a,'config':{'seed':seed,'processingMode':'neutral'},'summary':{'metricVersion':4,'processingMode':'neutral','linkRandomDraws':0,'trafficHash':str(seed),'attempted':10,'received':10,'deliveryRate':80+index*2,'throughputMbps':1+index,'meanDelayMs':4+index,'flows':[]}} for seed in range(3) for index,a in enumerate(['dijkstra-delay','mcsr-ls'])]
    def test_paired_interval_unit_and_reproducibility(self):
        a=paired_interval([2]*10);self.assertEqual(a['ci95'],[2,2]);self.assertEqual(a,paired_interval([2]*10));self.assertIsNone(paired_interval([1])['ci95'])
        report=aggregate(self.records(),['dijkstra-delay','mcsr-ls'],list(range(3)))
        rate=report['comparisons'][0]['metrics'][0];self.assertEqual(rate['statistics']['meanDifference'],2);self.assertEqual(rate['statistics']['pairs'],3);self.assertIsNone(report['winner'])
    def test_fixed_inputs_have_no_interval_but_link_draws_or_faults_can_vary(self):
        rows=self.records()
        for row in rows:row['summary']['trafficHash']='fixed'
        def compare():
            return aggregate(rows,['dijkstra-delay','mcsr-ls'],[0,1,2])['comparisons'][0]
        fixed=compare()
        self.assertEqual(fixed['inputClassification'],'fixed-input')
        self.assertIsNone(fixed['metrics'][0]['statistics']['ci95'])
        for row in rows:row['summary']['linkRandomDraws']=1
        self.assertEqual(compare()['seedsUsingLinkRandomness'],3)
        self.assertEqual(compare()['metrics'][0]['statistics']['ci95'],[2,2])
        for row in rows:
            row['summary']['linkRandomDraws']=0
            row['config']['physicalEvents']=[{'time':row['seed']}]
        self.assertEqual(compare()['uniqueInputs'],3)
        self.assertTrue(compare()['intervalAvailable'])

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
    def test_long_workload_sends_until_end_not_original_200_seconds(self):
        from verify_workloads import workload_config
        from unittest.mock import patch
        base=server.read_ini_config('PaperS1Mcsr');base.update(warmupSeconds=10,drainSeconds=10,packetTrace=True)
        for duration in (10,600):
            for control in (False,True):
                with patch.object(server,'read_ini_config',return_value=copy.deepcopy(base)):
                    cfg=workload_config(60,'mcsr-hr',duration,control,control)
                self.assertEqual(cfg['simTimeLimit'],duration)
                self.assertTrue(all(s['endTime']==duration for s in cfg['stations'] if s['enabled']))
                self.assertEqual(cfg['warmupSeconds']+cfg['drainSeconds'],0);self.assertFalse(cfg['packetTrace'])
                source=cfg['stations'][18];self.assertAlmostEqual(source['packetSize']*8/source['sendInterval']/1e6,60)
    def test_expected_packet_counts_cover_full_tick_rounded_interval(self):
        from verify_workloads import attempt_bounds
        c={'simTimeLimit':600,'stations':[{'enabled':True,'startTime':1,'endTime':600,'sendInterval':.001104}]}
        expected=attempt_bounds(c);self.assertEqual(expected,[542573,542573])
        c['stations'][0]['endTime']=200;short=attempt_bounds(c);self.assertLess(short[1],expected[0]/2)
        c['stations'][0].pop('startTime');c['stations'][0]['startRange']=[1,2]
        low,high=attempt_bounds(c);self.assertLess(low,high);self.assertEqual(high,short[1])
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
            case=report/'case';case.mkdir();output=case/'summary.json';output.write_text('{}')
            validator=case/'validated.json';validator.write_text(json.dumps({'outputs':{'summary.json':file_hash(output)}}))
            workloads={'status':'passed','engineSHA256':value['engineSHA256'],'sources':value['sources'],
                       'scope':'long-packet-control','runs':1,'satellites':132,'simSeconds':600,
                       'verified':[{'path':'case/validated.json','sha256':file_hash(validator)}]}
            (report/'workloads.json').write_text(json.dumps(workloads))
            self.assertEqual(evidence(p)['workloads'][0]['satellites'],132)
            output.write_text('changed')
            self.assertEqual(evidence(p)['workloads'],[])

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
            previous=run_manager.execute(p,settings,ini,'A',reader)
            latest=p/'data/latest.json';offline=p/'data/latest_result.js'
            pointers=(latest.read_bytes(),offline.read_bytes())
            with patch.object(research,'aggregate',side_effect=cancel_when_aggregating):
                job=run_manager.start(p,settings,ini,'A',reader,algorithms=['dijkstra-delay'],research={'repetitions':2})
                deadline=time.monotonic()+10
                while run_manager.state(job['runId'])['status'] in ('queued','running','stopping'):
                    self.assertLess(time.monotonic(),deadline);time.sleep(.02)
            self.assertEqual(run_manager.state(job['runId'])['status'],'cancelled')
            manifest=json.loads((p/'data/experiments'/job['runId']/'manifest.json').read_text(encoding='utf8'));self.assertEqual(manifest['status'],'cancelled')
            self.assertEqual(len(manifest['runs']),2)
            self.assertEqual((latest.read_bytes(),offline.read_bytes()),pointers)
            self.assertEqual(json.loads(latest.read_text())['runId'],previous['runId'])
            self.assertTrue(all((p/'data/runs'/r['runId']/'result.json').is_file() for r in manifest['runs']))

    def test_model_version_mismatch_rejected_by_engine(self):
        import subprocess
        with tempfile.TemporaryDirectory() as tmp:
            folder=Path(tmp)
            cfg=server.read_ini_config('Dijkstra132')
            cfg['modelVersion']='leo-research-4.1'
            input_file=folder/'input.json';input_file.write_text(json.dumps(cfg),encoding='utf8')
            engine=Path(server.BASE_DIR)/'engine/engine.exe'
            result=subprocess.run([str(engine),str(input_file),str(folder)],capture_output=True,timeout=10)
            self.assertNotEqual(result.returncode,0)
            self.assertIn(b'Engine model version mismatch',result.stderr)
            self.assertFalse((folder/'summary.json').exists())

    def test_latest_publish_rolls_back_if_offline_publish_fails(self):
        from unittest.mock import patch
        with tempfile.TemporaryDirectory() as tmp:
            data=Path(tmp)/'data';data.mkdir()
            run_manager.publish_latest(Path(tmp),{'runId':'old'})
            offline=data/'latest_result.js';before=offline.read_bytes()
            replace=run_manager.os.replace
            def fail_offline(source,destination):
                if destination==offline:raise PermissionError('locked')
                replace(source,destination)
            with patch.object(run_manager.os,'replace',side_effect=fail_offline):
                with self.assertRaises(PermissionError):run_manager.publish_latest(Path(tmp),{'runId':'new'})
            self.assertEqual(json.loads((data/'latest.json').read_text())['runId'],'old')
            self.assertEqual(offline.read_bytes(),before)

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
            last=state['completedRuns'][-1]['runId']
            self.assertEqual(json.loads((p/'data/latest.json').read_text())['runId'],last)
            self.assertIn(last,(p/'data/latest_result.js').read_text())
            self.assertEqual(report['comparisons'][0]['inputClassification'],'fixed-input')
            for row in state['completedRuns']:
                summary=json.loads((p/'data/runs'/row['runId']/'summary.json').read_text(encoding='utf8'));m=summary['measurement']
                supplied=json.loads((p/'data/runs'/row['runId']/'engine-input.json').read_text(encoding='utf8'))
                self.assertEqual(summary['version'],'1.0.0')
                self.assertEqual(summary['modelVersion'],supplied['modelVersion'])
                self.assertEqual(m['attempted'],m['received']+m['dropped']+m['inFlight']);self.assertLess(m['attempted'],summary['attempted']);self.assertIn('capEvict',summary['operations'])
            with self.assertRaises(ValueError):run_manager.start(p,settings,ini,'A',reader,algorithms=['mcsr-ls'],research={'repetitions':True})

if __name__=='__main__':unittest.main()
