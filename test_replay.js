const assert=require("node:assert/strict"),fs=require("node:fs"),path=require("node:path"),vm=require("node:vm");
const {readReplayIni,prepareBundle,bundlePath}=require("./bundle.js");
const {parseRoutesCsv}=require("./csv.js");
global.parseRoutesCsv=parseRoutesCsv;
const dir=path.join(__dirname,"testdata/bundle");
const manifest=JSON.parse(fs.readFileSync(path.join(dir,"manifest.json"),"utf8"));
const files=Object.fromEntries(Object.keys(manifest.files).map(n=>[n,fs.readFileSync(path.join(dir,n),"utf8")]));
(async()=>{
  const prepared=await prepareBundle({manifest,files});
  assert.equal(prepared.config.numSatellites,132);assert.equal(prepared.config.stations.length,13);
  assert.equal(prepared.config.stations[9].mobile,true);assert.equal(prepared.config.stations[9].speed,250);
  assert.equal(prepared.config.metric,"delay");assert.equal(prepared.routes.length,2671);
  await assert.rejects(prepareBundle({manifest,files:{...files,"routes.csv":"changed"}}));
  const missing={...files};delete missing["config/simulations/Dijkstra132/stations/EsaDsn-Mobiles.ini"];
  await assert.rejects(prepareBundle({manifest,files:missing}));
  await assert.rejects(prepareBundle({manifest:{...manifest,modelHash:"unsupported"},files}));
  assert.throws(()=>bundlePath("manifest.json","../outside"));
  assert.throws(()=>readReplayIni(new Map([["a.ini","include b.ini"],["b.ini","include a.ini"]]),"a.ini","Dijkstra132"));
  const ctx=vm.createContext({console});vm.runInContext(fs.readFileSync(path.join(__dirname,"orbit.js"),"utf8"),ctx);
  ctx.cfg=prepared.config;vm.runInContext("updateOrbitConfig({...cfg,minEl:cfg.minimumElevation})",ctx);
  const reference=JSON.parse(fs.readFileSync(path.join(__dirname,"testdata/replay-reference.json"),"utf8"));
  let maxKm=0;
  for(const recorded of reference.states){
    ctx.time=recorded.t;const actual=vm.runInContext("replaySnapshot(time)",ctx);
    assert.equal(actual.nodes.length,recorded.nodes.length);
    for(let i=0;i<actual.nodes.length;i++){
      const a=actual.nodes[i],b=recorded.nodes[i];
      const lat=Math.abs(a[0]-b[0])*111.32,lon=Math.abs(a[1]-b[1])*111.32;
      maxKm=Math.max(maxKm,lat,lon);assert(lat<.00001 && lon<.00001,"Position differs from independent engine reference by >1cm");
    }
    assert.deepEqual(JSON.parse(JSON.stringify([...actual.links].sort())),[...recorded.links].sort());
    assert.equal(actual.sent,undefined);assert.equal(actual.received,undefined);
  }
  assert.equal(vm.runInContext("replaySnapshot(300.5).nodes[0][0] !== replaySnapshot(300).nodes[0][0]",ctx),true);
  console.log(`Bundle/replay passed: original INI tree, C3 inheritance, hashes, unsupported models and independent positions (<${(maxKm*1000).toFixed(4)}m), exact links at ${reference.states.length} reference samples`);
})().catch(e=>{console.error(e);process.exitCode=1});
