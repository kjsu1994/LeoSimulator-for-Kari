const assert = require("node:assert/strict");
const fs = require("node:fs"), os = require("node:os"), path = require("node:path");
const { spawn } = require("node:child_process");
const delay = ms => new Promise(r => setTimeout(r, ms));
async function retry(fn) { for (let i=0;i<60;i++) { try { return await fn(); } catch { await delay(250); } } throw new Error("Startup timeout"); }
(async () => {
  const profile = fs.mkdtempSync(path.join(os.tmpdir(), "florasat-browser-"));
  const server = spawn("python", ["-c", "import server,http.server; http.server.ThreadingHTTPServer(('127.0.0.1',8091),server.SimHandler).serve_forever()"], {cwd: __dirname, windowsHide:true, stdio:"ignore"});
  const browser = spawn("C:/Program Files/Google/Chrome/Application/chrome.exe", ["--headless=new", "--no-first-run", "--remote-debugging-port=9227", `--user-data-dir=${profile}`, "about:blank"], {windowsHide:true, stdio:"ignore"});
  let ws;
  try {
    await retry(async () => { const r=await fetch("http://127.0.0.1:8091/api/config"); assert(r.ok); });
    const pages = await retry(async () => (await fetch("http://127.0.0.1:9227/json")).json());
    ws = new WebSocket(pages.find(p=>p.type==="page").webSocketDebuggerUrl);
    await new Promise((resolve,reject)=> {ws.onopen=resolve;ws.onerror=reject;});
    let id=0; const pending=new Map(), errors=[];
    ws.onmessage = e => {const m=JSON.parse(e.data); if(m.id){const p=pending.get(m.id);pending.delete(m.id); m.error?p.reject(m.error):p.resolve(m.result);} else if(m.method==="Runtime.exceptionThrown") errors.push(m.params.exceptionDetails);};
    const send=(method,params={})=>new Promise((resolve,reject)=>{const key=++id;const timer=setTimeout(()=>{pending.delete(key);reject(Error(`CDP timeout: ${method}`));},10000);pending.set(key,{resolve:r=>{clearTimeout(timer);resolve(r);},reject:e=>{clearTimeout(timer);reject(e);}});ws.send(JSON.stringify({id:key,method,params}));});
    const evaluate=async expression=> {const r=await send("Runtime.evaluate",{expression,awaitPromise:true,returnByValue:true});if(r.exceptionDetails)throw Error(JSON.stringify(r.exceptionDetails));return r.result.value;};
    await send("Runtime.enable");
    await send("Page.navigate",{url:"http://127.0.0.1:8091"});
    await retry(async()=> assert(await evaluate("typeof states !== 'undefined' && states.length > 0")));
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
    await evaluate("simTime=300.5;render()");
    assert.equal(await evaluate("stateAt(simTime).t"),300);
    assert.equal(await evaluate("document.getElementById('kpiSat').textContent"),"132 / 264");
    assert.equal(await evaluate("routes.length"),2671);
    assert(await evaluate("document.querySelector('#routeTable tbody').children.length > 0"));
    await evaluate("cfgNameSel.value='Dijkstra132Hop';cfgNameSel.onchange()");
    assert.equal(await evaluate("document.getElementById('scenarioRouting').textContent"),"홉 수 최소");
    assert.equal(await evaluate("routes.length"),2671);
    assert.equal(await evaluate("states.length"),602);
    assert(await evaluate("Number(document.getElementById('kpiGsl').textContent) > 0"));
    assert.equal(await evaluate("resultMeta.config.activeConfig"),"Dijkstra132");
    assert.equal(await evaluate("ORBIT_CFG.metric"),"delay");
    assert(await evaluate("document.getElementById('runStatus').textContent.includes('Dijkstra132Hop')"));
    await evaluate("window.dispatchEvent(new Event('focus'))");
    await new Promise(r=>setTimeout(r,300));
    assert.equal(await evaluate("lastSatStates.length"),132);
    await evaluate("(async()=>{installResult(await fetch('/api/result').then(r=>r.json()));simTime=599.5;render()})()");
    assert.equal(await evaluate("stateAt(simTime).t"),599);
    await evaluate("clearResult();routes=parseRoutesCsv("+JSON.stringify(fs.readFileSync(path.join(__dirname,"data/routes.csv"),"utf8"))+");simTime=30;render()");
    assert.equal(await evaluate("states.length"),0);
    assert.equal(await evaluate("routes.length"),2671);
    assert.equal(errors.length,0,JSON.stringify(errors));
    console.log("Chrome passed: engine replay, fractional-time sampling, table rendering, scenario selection preserves recorded map, result reload, external CSV without invented geometry");
    const offline = path.join(profile, "offline");
    fs.mkdirSync(path.join(offline, "data"), {recursive:true});
    for (const file of ["index.html", "app.js", "orbit.js", "csv.js", "style.css", "data/latest_result.js", "data/earth.png"])
      fs.copyFileSync(path.join(__dirname,file),path.join(offline,file));
    await send("Page.navigate", {url: require("node:url").pathToFileURL(path.join(offline, "index.html")).href});
    await retry(async()=> assert(await evaluate("location.protocol === 'file:' && typeof states !== 'undefined' && states.length > 0")));
    assert.equal(await evaluate("lastSatStates.length"), 132);
    assert(await evaluate("document.querySelector('#routeTable tbody').children.length > 0"));
    assert(await evaluate("stateAt(simTime).links.length > 0"));
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
    if(ws)ws.close();server.kill();browser.kill();
    // shortcut: Chrome may retain profile files briefly; keep temporary profile if Windows holds a lock.
    try {fs.rmSync(profile,{recursive:true,force:true});} catch {}
  }
})().catch(e=>{console.error(e);process.exitCode=1;});
