# SPDX-License-Identifier: GPL-3.0-only
import copy,csv,io,json,subprocess,tempfile,unittest
from pathlib import Path
import server,run_manager
from comparison import fingerprint

class ResearchTests(unittest.TestCase):
    def config(self):
        c=server.read_ini_config('Dijkstra132');c.update(numSatellites=12,planes=4,simTimeLimit=1,minimumElevation=-90,maxGroundLinks=500,routeRecording='full',packetTrace=True,controlRefresh=.25)
        for s in c['stations']:s['enabled']=False
        c['stations'][0].update(enabled=True,dstStation=10,startTime=0,sendInterval=.01,endTime=1)
        return c
    def engine(self,c,prepare=False):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp);(p/'input.json').write_text(json.dumps(c,allow_nan=False))
            args=[str(Path(server.BASE_DIR)/'engine/engine.exe'),str(p/'input.json'),str(p)]
            if prepare:args.append('--prepare')
            result=subprocess.run(args,capture_output=True,text=True,encoding='utf-8',timeout=60)
            self.assertEqual(result.returncode,0,result.stderr)
            s=json.loads((p/'summary.json').read_text());self.assertEqual(s['attempted'],s['received']+s['dropped']+s['inFlight'])
            if 'control' in s:
                k=s['control'];self.assertGreaterEqual(k['inFlight'],0);self.assertEqual(k['hopAccepted'],k['arrived']+k['dropped']+k['inFlight'])
            return s,{f.name:f.read_text() for f in p.iterdir() if f.suffix in ('.csv','.json','.jsonl')}
    def test_no_access_is_failure_and_in_denominator(self):
        c=self.config();c['minimumElevation']=90
        s,_=self.engine(c);self.assertGreater(s['attempted'],0);self.assertEqual(s['deliveryRate'],0);self.assertEqual(s['dropped'],s['attempted']);self.assertGreater(s['dropReasons']['no-access'],0)
    def test_wire_control_and_neutral_processing(self):
        s,_=self.engine(self.config());self.assertGreater(s['control']['wireBytes'],0);self.assertGreater(s['control']['arrived'],0)
        self.assertTrue(all(v==0 for v in s['appliedProcessingDelays'].values()));self.assertGreater(s['operations']['edgeChecks'],0)
    def test_control_loss_delays_knowledge_and_recovery(self):
        c=self.config();c.update(simTimeLimit=2,failureSchedule='0-1@0.3+0.7',slsInterval=.1,slsMissCount=2)
        s,files=self.engine(c);physical=json.loads(files['physical-events.json']);self.assertEqual([e['time'] for e in physical],[.3,1.])
        events=[json.loads(line) for line in files['network-events.jsonl'].splitlines()]
        detected=[e for e in events if e['event']=='link-detected' and not e['up'] and set((e['a'],e['b']))=={0,1}]
        self.assertTrue(detected);self.assertTrue(all(e['time']>.3 for e in detected));self.assertGreater(s['control']['dropped'],0)
        self.assertTrue(s['recovery'][0]['convergenceSeconds'] is None or s['recovery'][0]['convergenceSeconds']>0)
    def test_packet_jitter_changes_actual_received_delay(self):
        a=self.config();a.update(simTimeLimit=.15,controlMode='time');a['stations'][0].update(sendInterval=.00001,endTime=.15)
        _,files=self.engine(a);route=next(csv.DictReader(io.StringIO(files['routes.csv'])))
        # Inject on every outgoing ISL to exercise whichever path the router selected.
        a.update(congestionMode='environment');a['scenario']={'entries':[{'target':{'kind':'region','params':{'shape':'rect','lat_min':-90,'lat_max':90,'lon_min':-180,'lon_max':180}},'start_s':0,'rho':.5,'penalty':{'jitter_ms':2}}]}
        a['minimumElevation']=10;a['numSatellites']=132;a['planes']=12;a['simTimeLimit']=3;a['stations'][0].update(dstStation=1,sendInterval=.01,endTime=3)
        a['jitterModel']='cost';baseline,_=self.engine(a);a['jitterModel']='packet'
        s,files=self.engine(a)
        self.assertEqual(s['trafficHash'],baseline['trafficHash'])
        self.assertGreater(s['meanDelayMs'],baseline['meanDelayMs']+1)
        self.assertGreater(s['flows'][0]['delayStdMs'],baseline['flows'][0]['delayStdMs']+.2)
        receives=[r for r in csv.DictReader(io.StringIO(files['events.csv'])) if r['event']=='receive']
        self.assertTrue(receives);self.assertGreater(s['flows'][0]['delayStdMs'],0)
    def test_fixed_event_replay_and_hash_sensitivity(self):
        c=self.config();c['failureSchedule']='0-1@0.1+0.2'
        _,files=self.engine(c,True);events=json.loads(files['physical-events.json']);c.update(physicalPrepared=True,physicalEvents=events)
        actual=[]
        for algorithm in ('dijkstra-delay','dijkstra-hop','mcsr-ls','mcsr-ts','mcsr-hr'):
            v=copy.deepcopy(c);v['algorithm']=algorithm;s,out=self.engine(v);actual.append((s['trafficHash'],out['physical-events.json']))
        self.assertEqual(len(set(actual)),1)
        d=copy.deepcopy(c);d['maxHops']+=1;self.assertNotEqual(fingerprint(c),fingerprint(d))
    def test_route_reference_uses_fixed_destination_when_only_source_is_configured(self):
        c=server.read_ini_config('PaperS1DijkstraCutFlood');c['simTimeLimit']=2;c['failureSchedule']='route@1+0.5'
        self.assertEqual(c['failureRouteSrc'],18);c['failureRouteDst']=-1
        _,files=self.engine(c,True);events=json.loads(files['physical-events.json']);self.assertEqual(len(events),2)
    def test_csib_reports_do_not_force_rebuild_before_refresh(self):
        c=server.read_ini_config('Mcsr132');c.update(simTimeLimit=6,minimumElevation=10,csibEnabled=True,csibPeriod=1,csibVote=False,controlTimeout=.3,refreshInterval=5,snapshotOnDelayUpdate=False)
        for station in c['stations']:station['enabled']=False
        c['stations'][0].update(enabled=True,dstStation=1,startTime=0,sendInterval=.01,endTime=6)
        result,_=self.engine(c)
        self.assertGreater(result['control']['generatedByKind'].get('csib',0),0)
        self.assertLess(result['computations'].get('mcsr-snapshot',0),100)
    def test_exported_failure_csv_is_accepted(self):
        c=self.config();c['failureSchedule']='0-1@0.1+0.2';_,files=self.engine(c)
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp)/'failures.csv';p.write_text(files['failures.csv']);c['failureReplay']=str(p)
            self.assertEqual(len(run_manager.load_external_inputs(c,server.INI_PATH)['failureReplayEvents']),2)
    def test_off_grid_schedule_is_rejected(self):
        c=self.config();c['islMode']='scheduled';c['scenario']={'entries':[],'scheduled_events':[{'time':.1,'event':'failure','a':0,'b':8}]}
        with self.assertRaises(ValueError):run_manager.load_external_inputs(c,server.INI_PATH)


class ExperimentTests(unittest.TestCase):
    def test_group_freezes_common_events_and_preserves_result_on_cancel(self):
        import shutil,time
        from comparison import compare_bundles
        with tempfile.TemporaryDirectory() as tmp:
            base=Path(tmp);settings=base/'settings';ini=settings/'simulations/Dijkstra132/omnetpp.ini';ini.parent.mkdir(parents=True)
            ini.write_text('[General]\n*.numSatellites=12\n*.**.planeCount=4\n**.nrOfGS=2\n*.topologyControl.minimumElevation=-90\n[Config A]\nsim-time-limit=1s\n*.station[0].traffic.enabled=true\n*.station[0].traffic.dstStation=1\n*.station[0].traffic.startTime=0s\n*.islFailures.schedule="0-1@0.2+0.3"\n')
            (base/'engine').mkdir();shutil.copy2(Path(server.BASE_DIR)/'engine/engine.exe',base/'engine/engine.exe');shutil.copy2(Path(server.BASE_DIR)/'replay-model.json',base/'replay-model.json')
            reader=lambda name,variables=None:server.read_ini_config(name,entry_path=str(ini),variables=variables)
            job=run_manager.start(base,settings,ini,'A',reader,algorithms=['dijkstra-delay','mcsr-hr']);deadline=time.monotonic()+30
            while run_manager.state(job['runId'])['status'] in ('queued','running'):
                self.assertLess(time.monotonic(),deadline);time.sleep(.05)
            state=run_manager.state(job['runId']);self.assertEqual(state['status'],'completed',state.get('error'));self.assertEqual(len(state['completedRuns']),2)
            bundles=[json.loads((base/'data/runs'/row['runId']/'result.json').read_text(encoding='utf-8'))['bundle'] for row in state['completedRuns']]
            self.assertEqual(bundles[0]['manifest']['physicalEventsHash'],bundles[1]['manifest']['physicalEventsHash']);self.assertTrue(compare_bundles(*bundles)['comparable'])
            self.assertEqual(bundles[0]['files']['physical-events.json'],bundles[1]['files']['physical-events.json'])
            self.assertTrue((base/'data/experiments'/job['runId']/'manifest.json').is_file())
            pointer=(base/'data/latest.json').read_bytes()
            def long_reader(name,variables=None):
                c=reader(name,variables);c.update(simTimeLimit=1000000,physicalPrepared=True,physicalEvents=[])
                c['stations'][0]['sendInterval']=.00001
                return c
            job=run_manager.start(base,settings,ini,'A',long_reader,algorithms=['dijkstra-delay','mcsr-ls'])
            deadline=time.monotonic()+5
            while not run_manager.state(job['runId']).get('currentRunId'):
                self.assertLess(time.monotonic(),deadline);time.sleep(.01)
            time.sleep(.1);start=time.monotonic();run_manager.cancel(job['runId'])
            while run_manager.state(job['runId'])['status'] in ('queued','running','stopping'):
                self.assertLess(time.monotonic()-start,2);time.sleep(.02)
            self.assertEqual(run_manager.state(job['runId'])['status'],'cancelled');self.assertEqual((base/'data/latest.json').read_bytes(),pointer)


if __name__=='__main__':unittest.main()
