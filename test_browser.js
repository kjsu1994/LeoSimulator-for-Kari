// SPDX-License-Identifier: GPL-3.0-only
// Modified/adapted 2026-11-16; see THIRD_PARTY_NOTICES.txt.
const assert = require("node:assert/strict");
const fs = require("node:fs"), os = require("node:os"), path = require("node:path");
const { spawn } = require("node:child_process");
const delay = ms => new Promise(r => setTimeout(r, ms));
async function retry(fn) { for (let i=0;i<60;i++) { try { return await fn(); } catch { await delay(250); } } throw new Error("Startup timeout"); }
(async () => {
  const port=20000+Math.floor(Math.random()*10000)*2;
  const profile = fs.mkdtempSync(path.join(os.tmpdir(), "florasat-browser-"));
  const server = spawn("python", ["-c", `import server,http.server; http.server.ThreadingHTTPServer(('127.0.0.1',${port}),server.SimHandler).serve_forever()`], {cwd: __dirname, windowsHide:true, stdio:"ignore"});
  const browser = spawn("C:/Program Files/Google/Chrome/Application/chrome.exe", ["--headless=new", "--disable-extensions", "--disable-background-networking", "--no-first-run", `--remote-debugging-port=${port+1}`, `--user-data-dir=${profile}`, "about:blank"], {windowsHide:true, stdio:"ignore"});
  let ws;
  try {
    await retry(async () => { const r=await fetch(`http://127.0.0.1:${port}/api/config`); assert(r.ok); });
    const pages = await retry(async () => (await fetch(`http://127.0.0.1:${port+1}/json`)).json());
    ws = new WebSocket(pages.find(p=>p.type==="page"&&p.url==="about:blank").webSocketDebuggerUrl);
    await new Promise((resolve,reject)=> {ws.onopen=resolve;ws.onerror=reject;});
    let id=0; const pending=new Map(), errors=[];
    ws.onmessage = e => {const m=JSON.parse(e.data); if(m.id){const p=pending.get(m.id);if(!p)return;pending.delete(m.id); m.error?p.reject(m.error):p.resolve(m.result);} else if(m.method==="Runtime.exceptionThrown") errors.push(m.params.exceptionDetails);};
    const send=(method,params={})=>new Promise((resolve,reject)=>{const key=++id;const timer=setTimeout(()=>{pending.delete(key);reject(Error(`CDP timeout: ${method}`));},40000);pending.set(key,{resolve:r=>{clearTimeout(timer);resolve(r);},reject:e=>{clearTimeout(timer);reject(e);}});ws.send(JSON.stringify({id:key,method,params}));});
    const evaluate=async expression=> {const r=await send("Runtime.evaluate",{expression,awaitPromise:true,returnByValue:true});if(r.exceptionDetails)throw Error(JSON.stringify(r.exceptionDetails));return r.result.value;};
    await send("Runtime.enable");
    await send("Page.navigate",{url:`http://127.0.0.1:${port}`});
    await retry(async()=> assert(await evaluate("typeof resultMeta !== 'undefined' && resultMeta?.replay === true")));
    assert.equal(await evaluate("document.getElementById('scenarioDetails').open"),false);
    assert.equal(await evaluate("document.getElementById('iniTable').checkVisibility()"),false);
    assert.equal(await evaluate("document.getElementById('scenarioConstellation').textContent"),"132기 · 12개 궤도면");
    assert.equal(await evaluate("document.getElementById('scenarioDuration').textContent"),"600초");
    assert.equal(await evaluate("document.getElementById('scenarioRouting').textContent"),"지연 최소");
    assert.equal(await evaluate("document.getElementById('scenarioTraffic').textContent"),"2s · 1000B");
    await evaluate("document.querySelector('#scenarioDetails summary').click();iniFilter.value='routing.metric';iniFilter.oninput()");
    assert.equal(await evaluate("document.getElementById('scenarioDetails').open"),true);
    assert.equal(await evaluate("document.querySelector('#iniTable tbody').children.length"),1);
    await evaluate("document.querySelector('#scenarioDetails summary').click();iniFilter.value='';iniFilter.oninput()");
    console.log("Scenario UI passed: concise conditions, details collapsed by default, disclosure and search work");
    assert.equal(await evaluate("document.getElementById('cfgName').disabled"),false);
    assert(await evaluate("document.getElementById('runnerActions').checkVisibility() && document.getElementById('btnOpenIni').checkVisibility()"));
    await evaluate("simTime=300.5;render()");
    assert.equal(await evaluate("stateAt(simTime).t"),300.5);
    assert.equal(await evaluate("document.getElementById('kpiSat').textContent"),"132 / 264");
    assert.equal(await evaluate("routes.length"),2671);
    assert(await evaluate("document.querySelector('#routeTable tbody').children.length > 0"));
    await evaluate("cfgNameSel.value='Dijkstra132Hop';cfgNameSel.onchange()");
    assert.equal(await evaluate("document.getElementById('scenarioRouting').textContent"),"홉 수 최소");
    assert.equal(await evaluate("routes.length"),2671);
    assert.equal(await evaluate("resultMeta.replay"),true);
    assert(await evaluate("Number(document.getElementById('kpiGsl').textContent) > 0"));
    assert.equal(await evaluate("resultMeta.config.activeConfig"),"Dijkstra132");
    assert.equal(await evaluate("ORBIT_CFG.metric"),"delay");
    assert(await evaluate("document.getElementById('runStatus').textContent.includes('Dijkstra132Hop')"));
    await evaluate("window.dispatchEvent(new Event('focus'))");
    await new Promise(r=>setTimeout(r,300));
    assert.equal(await evaluate("lastSatStates.length"),132);
    await evaluate("(async()=>{await openResultBundle((await fetch('/api/result').then(r=>r.json())).bundle);simTime=599.5;render()})()");
    assert.equal(await evaluate("stateAt(simTime).t"),599.5);
    await evaluate("clearResult();routes=parseRoutesCsv("+JSON.stringify(fs.readFileSync(path.join(__dirname,"testdata/bundle/routes.csv"),"utf8"))+");simTime=30;render()");
    assert.equal(await evaluate("states.length"),0);
    assert.equal(await evaluate("routes.length"),2671);
    assert.equal(errors.length,0,JSON.stringify(errors));
    console.log("Chrome passed: engine replay, fractional-time sampling, table rendering, scenario selection preserves recorded map, result reload, external CSV without invented geometry");

    await evaluate("(async()=>{openSettings(await fetch('/api/config?config=PaperS1Mcsr').then(r=>r.json()))})()");
    assert.equal(await evaluate("document.querySelectorAll('.sender-check').length"),100);
    assert.equal(await evaluate("value('edit-algorithm')"),"mcsr-ls");
    await evaluate("byId('edit-bitrate').value=60;byId('edit-bitrate').oninput()");
    assert(Math.abs(await evaluate("Number(value('edit-sendInterval'))")-.000184)<1e-12);
    await evaluate("byId('entry-kind').value='circle';byId('entry-kind').onchange();byId('entry-city').value='서울';byId('entry-city').onchange()");
    assert.equal(await evaluate("Number(value('target-center_lat'))"),37.5665);
    for(const width of [1100,700]){await send('Emulation.setDeviceMetricsOverride',{width,height:900,deviceScaleFactor:1,mobile:false});assert(await evaluate("byId('settingsDialog').scrollWidth<=byId('settingsDialog').clientWidth"));}
    await evaluate("byId('edit-algorithm').value='dijkstra-delay';updateMcsrControls()");
    assert(await evaluate("byId('edit-csibPeriod').disabled"));
    await evaluate("byId('settingsDialog').close()");

    await evaluate("(async()=>{const cfg=await fetch('/api/config?config=PaperS1Load').then(r=>r.json());openSettings(cfg);window.savedRequest=null;window.realFetch=window.fetch;window.fetch=async(url,options)=>{if(url==='/api/config'&&options?.method==='POST'){window.savedRequest=JSON.parse(options.body);return new Response(JSON.stringify(cfg),{status:200});}return realFetch(url,options);};byId('settingsForm').requestSubmit(byId('settingsForm').querySelector('[type=submit]'));})()");
    await retry(async()=>assert(await evaluate("window.savedRequest!==null && !byId('settingsDialog').open")));
    assert(await evaluate("!Object.hasOwn(savedRequest.changes,'destination') && !Object.hasOwn(savedRequest.changes,'sendInterval')"));
    await evaluate("window.fetch=window.realFetch");

    const nativeFolder=path.join(profile,'native');fs.mkdirSync(nativeFolder);
    const script="import sys,pathlib,shutil,json; import server,run_manager; b=pathlib.Path(sys.argv[1]); (b/'engine').mkdir();shutil.copy2(pathlib.Path(server.BASE_DIR)/'engine/engine.exe',b/'engine/engine.exe');shutil.copy2(pathlib.Path(server.BASE_DIR)/'replay-model.json',b/'replay-model.json');c=server.read_ini_config('Mcsr132');c['simTimeLimit']=2;[s.update(startTime=0) for s in c['stations']];inputs={p.relative_to(server.LEO_DIR).as_posix():p.read_bytes() for p in pathlib.Path(server.LEO_DIR).rglob('*.ini')};r=run_manager.execute(b,server.LEO_DIR,server.INI_PATH,'Mcsr132',server.read_ini_config,frozen=(c,inputs));(b/'browser-result.json').write_text(json.dumps(r),encoding='utf-8')";
    const run=require('node:child_process').spawnSync('python',['-c',script,nativeFolder],{cwd:__dirname,windowsHide:true,encoding:'utf8',timeout:30000});assert.equal(run.status,0,run.stderr);
    const nativeResult=JSON.parse(fs.readFileSync(path.join(nativeFolder,'browser-result.json'),'utf8'));
    await evaluate(`openResultBundle(${JSON.stringify(nativeResult.bundle)})`);
    assert(await evaluate("!byId('nativeMetrics').hidden && byId('metricValues').children.length===8 && resultMeta.summary.received>0"));
    await evaluate("showComparison(resultMeta,resultMeta)");
    assert(await evaluate("byId('comparisonStatus').textContent.includes('동일한 환경')"));
    assert.equal(await evaluate("lastSatStates.length"),132);
    await evaluate("(async()=>{await openResultBundle((await fetch('/api/result').then(r=>r.json())).bundle)})()");
    assert(await evaluate("byId('nativeMetrics').hidden === !resultMeta.summary"));
    assert.equal(errors.length,0,JSON.stringify(errors));
    console.log('Native UI passed: 100-node settings, rate/interval coupling, region editor, responsive dialog, MCSR controls, v2 replay, measured metrics and comparison');

    const offline = path.join(profile, "offline");
    fs.mkdirSync(path.join(offline, "data"), {recursive:true});
    for (const file of ["index.html", "app.js","editor.js", "orbit.js", "csv.js", "bundle.js", "style.css", "data/latest_result.js", "data/earth.png"])
      fs.copyFileSync(path.join(__dirname,file),path.join(offline,file));
    await send("Page.navigate", {url: require("node:url").pathToFileURL(path.join(offline, "index.html")).href});
    await retry(async()=> assert(await evaluate("location.protocol === 'file:' && typeof resultMeta !== 'undefined' && resultMeta?.replay === true")));
    assert.equal(await evaluate("document.getElementById('cfgName').checkVisibility()"),false);
    assert.equal(await evaluate("document.getElementById('runnerActions').checkVisibility()"),false);
    assert.equal(await evaluate("document.getElementById('savedConfigName').textContent"),"Dijkstra132");
    assert.equal(await evaluate("document.getElementById('btnOpenIni').checkVisibility()"),true);
    await evaluate("document.getElementById('btnOpenIni').click()");
    assert(await evaluate("document.getElementById('runStatus').textContent.includes('start_ui.bat')"));
    assert.equal(await evaluate("lastSatStates.length"), 132);
    assert(await evaluate("document.querySelector('#routeTable tbody').children.length > 0"));
    assert(await evaluate("stateAt(simTime).links.length > 0"));
    const validBundle = path.join(profile,"result-package");
    fs.cpSync(path.join(__dirname,"testdata/bundle"),validBundle,{recursive:true});
    const selectFolder = async folder => {
      const {root}=await send("DOM.getDocument");
      const {nodeId}=await send("DOM.querySelector",{nodeId:root.nodeId,selector:"#resultFolder"});
      await send("DOM.setFileInputFiles",{nodeId,files:[folder]});
    };
    await selectFolder(validBundle);
    await retry(async()=>assert(await evaluate("resultMeta?.runId==='reference-dijkstra'")));
    assert.equal(await evaluate("lastSatStates.length"),132);
    assert.equal(await evaluate("document.getElementById('mapTraffic') || document.getElementById('mapMeasured')"),null);
    assert.equal(await evaluate("document.getElementById('mapSatellites').textContent"),"132기");
    assert.equal(await evaluate("document.getElementById('mapIsl').textContent"),"264개");
    assert(await evaluate("document.getElementById('mapGroundLinks').textContent.endsWith('개')"));
    const broken=path.join(profile,"broken-package");fs.cpSync(validBundle,broken,{recursive:true});
    fs.appendFileSync(path.join(broken,"routes.csv"),"modified");
    await selectFolder(broken);
    await retry(async()=>assert(await evaluate("document.getElementById('runStatus').textContent.includes('기존 화면을 유지')")));
    assert.equal(await evaluate("resultMeta.runId"),"reference-dijkstra");
    assert.equal(await evaluate("lastSatStates.length"),132);
    fs.copyFileSync(path.join(validBundle,"routes.csv"),path.join(broken,"routes.csv"));
    fs.unlinkSync(path.join(broken,"config/simulations/Dijkstra132/stations/EsaDsn-Mobiles.ini"));
    await selectFolder(broken);
    await retry(async()=>assert(await evaluate("document.getElementById('runStatus').textContent.includes('Missing/invalid result file')")));
    assert.equal(await evaluate("resultMeta.runId"),"reference-dijkstra");
    console.log("Result folder passed: real directory input, CSV/INI replay, checksums and missing-file rejection retain previous map");
    const importCsv = async file => {
      const {root} = await send("DOM.getDocument");
      const {nodeId} = await send("DOM.querySelector", {nodeId:root.nodeId,selector:"#csvInput"});
      await send("DOM.setFileInputFiles", {nodeId,files:[file]});
    };
    const matching = path.join(offline,"data/matching.csv");
    const canonical = fs.readFileSync(path.join(__dirname,"testdata/bundle/routes.csv"),"utf8");
    fs.writeFileSync(matching,"\uFEFF"+canonical.replace(/\r?\n/g,"\r\n"));
    await evaluate("simTime=40;render()");
    await importCsv(matching);
    await retry(async()=> assert(await evaluate("document.getElementById('csvSource').textContent.includes('matching.csv')")));
    assert.equal(await evaluate("resultMeta.replay"),true);
    assert.equal(await evaluate("csvMapTime"),null);
    assert.equal(await evaluate("lastSatStates.length"),132);
    assert(await evaluate("document.querySelector('#routeTable tbody').children.length > 0"));
    await evaluate("simTime=300;render()");
    assert.equal(await evaluate("stateAt(simTime).t"),300);
    const bad = path.join(offline,"data/invalid.csv");fs.writeFileSync(bad,"invalid");
    await importCsv(bad);
    await retry(async()=> assert(await evaluate("document.getElementById('runStatus').textContent.startsWith('CSV error:')")));
    assert.equal(await evaluate("routes.length"),2671);
    assert.equal(await evaluate("lastSatStates.length"),132);
    const foreign = path.join(offline,"data/foreign.csv");
    const [header, first] = canonical.trim().split(/\r?\n/);
    const r1=first.split(","),r2=first.split(",");r1[0]="7000";r1[9]="444";r2[0]="7010";r2[1]="9000";r2[9]="555";
    fs.writeFileSync(foreign,header+"\n"+r1.join(",")+"\n"+r2.join(",")+"\n");
    await importCsv(foreign);
    await retry(async()=> assert(await evaluate("routes.length===2 && simTime===7000")));
    assert.equal(await evaluate("csvMapTime"),300);
    assert.equal(await evaluate("resultMeta.replay"),true);
    assert.equal(await evaluate("lastSatStates.length"),132);
    assert.equal(await evaluate("document.getElementById('mapTime').textContent"),"300.0 s");
    assert(await evaluate("document.getElementById('kpiDelay').textContent.startsWith('444.0 ms')"));
    assert(await evaluate("document.querySelector('#routeTable tbody').textContent.includes('7000.0s')"));
    await evaluate("document.getElementById('btnPlay').click()");
    await new Promise(r=>setTimeout(r,250));
    assert(await evaluate("simTime>7000 && csvMapTime===300 && lastSatStates.length===132"));
    await evaluate("document.getElementById('btnPlay').click()");
    await importCsv(matching);
    await retry(async()=> assert(await evaluate("routes.length===2671 && csvMapTime===null")));
    assert.equal(await evaluate("lastSatStates.length"),132);
    assert(await evaluate("byId('nativeMetrics').hidden === !resultMeta.summary"));
    console.log("CSV file input passed: matching CSV sync, malformed file preserves view, foreign CSV timeline/statistics with preserved reference map, matching reimport restores sync");
    const point = await evaluate("(()=>{const st=lastStationStates[0],r=canvas.getBoundingClientRect(),scale=Math.min(r.width/W,r.height/H);return {x:r.left+(r.width-W*scale)/2+lonToX(st.lon)*scale,y:r.top+(r.height-H*scale)/2+latToY(st.lat)*scale};})()");
    await send("Input.dispatchMouseEvent", {type:"mouseMoved", x:point.x, y:point.y});
    assert(await evaluate("!document.getElementById('tooltip').hidden && document.getElementById('tooltip').textContent.includes(lastStationStates[0].name)"));
    for (const width of [1520, 1100, 700]) {
      await send("Emulation.setDeviceMetricsOverride",{width,height:900,deviceScaleFactor:1,mobile:false});
      assert(await evaluate("(()=>{const a=document.querySelector('.map-summary').getBoundingClientRect(),b=canvas.getBoundingClientRect();return a.bottom<=b.top+1 && document.documentElement.scrollWidth<=innerWidth;})()"));
    }
    assert.equal(errors.length,0,JSON.stringify(errors));
    console.log("Layout passed: status outside map, no horizontal overflow at 1520, 1100 and 700px");
    console.log("Offline Chrome passed: direct HTML loads actual satellites, links, recent routes and mouse hover details");
    await send("Browser.close").catch(()=>{});
  } finally {
    if(ws){try{ws.send(JSON.stringify({id:999999,method:"Browser.close"}));}catch{}ws.close();}server.kill();browser.kill();
    // shortcut: Chrome may retain profile files briefly; keep temporary profile if Windows holds a lock.
    try {fs.rmSync(profile,{recursive:true,force:true});} catch {}
  }
})().catch(e=>{console.error(e);process.exitCode=1;});
