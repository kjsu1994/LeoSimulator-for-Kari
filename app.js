// SPDX-License-Identifier: GPL-3.0-only
// Modified/adapted 2026-11-16; see THIRD_PARTY_NOTICES.txt.
// Interactive 2D Map, Route Replay & Simulator Runner Controller
const canvas = document.getElementById("mapCanvas");
const ctx = canvas.getContext("2d");
const W = 2160, H = 1080;
const ROUTE_COLORS = ["#ff6b6b", "#40c4ff", "#69f0ae", "#ffe066", "#b388ff", "#ff9e40", "#ea80fc", "#ff5c8a"];

const earthImg = new Image();
earthImg.src = "data/earth.png";
earthImg.onload = () => render();

let routes = []; // Legacy files have no matching configuration/state evidence.
let states = [], resultMeta = null, loadedConfigKey = "", csvMapTime = null;
let resultLoadRequest = 0, previewConfig = null;
function stateAt(t) {
  if (resultMeta?.replay) {const state=replaySnapshot(t);return resultMeta.networkEvents?.length?applyNetworkTimeline(state,resultMeta.networkEvents,resultMeta.runId,t):state;}
  if(!resultMeta&&previewConfig)return replaySnapshot(t);
  let lo = 0, hi = states.length;
  while (lo < hi) { const mid = (lo + hi) >> 1; if (states[mid].t <= t) lo = mid + 1; else hi = mid; }
  return lo ? states[lo - 1] : null;
}
function clearResult() {
  window.dispatchEvent(new Event("sim-clear"));
  routes = []; states = []; resultMeta = null; selectedRouteId = null; simTime = 0; csvMapTime = null;
  document.getElementById("csvSource").textContent = "";
  playing = false; document.getElementById("btnPlay").textContent = "Play";
  document.getElementById("resultFiles").hidden = true;
}
function installResult(result) {
  clearResult();
  previewConfig=null;routes = result.routes; states = result.states; resultMeta = result;
  simTime = Math.min(1210, result.replay ? result.config.simTimeLimit : (states.length ? states[states.length - 1].t : 0));
  applyLoadedConfig(result.config, { result: true });
  syncTimeBounds(result.replay ? result.config.simTimeLimit : (states.length ? states[states.length - 1].t : 0));
  setStatus(`${scenarioName(result.config.activeConfig)} · ${algorithmName(result.config)} 실행 결과를 불러왔습니다.`);
  const download = document.getElementById("resultFiles");
  download.hidden = false;
  download.href = `data/runs/${result.runId}/${result.replay ? "routes.csv" : "results/routes.csv"}`;
  download.textContent = `CSV | run ${result.runId} | SHA-256 ${result.csvHash.slice(0, 16)}`;
  render();
  window.dispatchEvent(new CustomEvent("sim-result",{detail:result}));
}
let simTime = routes.length ? Math.min(1210.0, routes[routes.length - 1][0]) : 0.0;
let maxSimTime = routes.length ? Math.max(10, Math.ceil(routes[routes.length - 1][0])) : 600.0;
let playing = false, speed = 10, selectedRouteId = null, lastFrame = 0;
let lastSatStates = [], lastStationStates = [];

const cfgNameSel = document.getElementById("cfgName");
const srcFilter = document.getElementById("srcFilter");
const dstFilter = document.getElementById("dstFilter");

function populateFilters() {
  const prevSrc = srcFilter.value || "-1";
  const prevDst = dstFilter.value || "-1";
  srcFilter.innerHTML = '<option value="-1">All Sources</option>';
  dstFilter.innerHTML = '<option value="-1">All Destinations</option>';

  const fixed = STATIONS.filter(s => !s.mobile), mobile = STATIONS.filter(s => s.mobile);
  fixed.forEach(s => srcFilter.add(new Option(`GS${s.id}: ${s.name}`, `GS:${s.id}`)));
  mobile.forEach(s => srcFilter.add(new Option(`Mobile ${s.id}: ${s.name}`, `GS:${s.id}`)));
  TERMINALS.forEach(t => srcFilter.add(new Option(`T${t.id}: Terminal ${t.id}`, `T:${t.id}`)));

  mobile.forEach(s => dstFilter.add(new Option(`Mobile ${s.id}: ${s.name}`, `${s.id}`)));
  fixed.forEach(s => dstFilter.add(new Option(`GS${s.id}: ${s.name}`, `${s.id}`)));

  srcFilter.value = [...srcFilter.options].some(o => o.value === prevSrc) ? prevSrc : "-1";
  dstFilter.value = [...dstFilter.options].some(o => o.value === prevDst) ? prevDst : "-1";
}
populateFilters();

function syncTimeBounds(limitSec) {
  const dataMax = routes.length ? Math.ceil(routes[routes.length - 1][0]) : 0;
  maxSimTime = Math.max(10, dataMax || 0, limitSec || 0);
  document.getElementById("timeSlider").max = maxSimTime;
  if (simTime > maxSimTime) simTime = dataMax ? Math.min(maxSimTime, routes[routes.length - 1][0]) : 0;
}
syncTimeBounds(maxSimTime);

const lonToX = lon => (lon + 180) / 360 * W;
const latToY = lat => (90 - lat) / 180 * H;

function drawWrappedLine(lon1, lat1, lon2, lat2, color, width, alpha = 1) {
  ctx.save();
  ctx.strokeStyle = color;
  ctx.lineWidth = width;
  ctx.globalAlpha = alpha;
  ctx.beginPath();
  if (Math.abs(lon1 - lon2) <= 180) {
    ctx.moveTo(lonToX(lon1), latToY(lat1));
    ctx.lineTo(lonToX(lon2), latToY(lat2));
  } else {
    const lon2Adj = lon2 < lon1 ? lon2 + 360 : lon2 - 360;
    const boundaryLon = lon2 < lon1 ? 180 : -180;
    const t = (boundaryLon - lon1) / (lon2Adj - lon1);
    const latMid = lat1 + t * (lat2 - lat1);
    ctx.moveTo(lonToX(lon1), latToY(lat1));
    ctx.lineTo(lonToX(boundaryLon), latToY(latMid));
    ctx.moveTo(lonToX(-boundaryLon), latToY(latMid));
    ctx.lineTo(lonToX(lon2), latToY(lat2));
  }
  ctx.stroke();
  ctx.restore();
}

function drawFootprint(latDeg, lonDeg, altKm) {
  const Re = 6371, el = ORBIT_CFG.minEl * DEG;
  const psi = Math.acos(Math.min(1, Re / (Re + altKm) * Math.cos(el))) - el;
  const lat0 = latDeg * DEG, lon0 = lonDeg * DEG;
  ctx.save();
  ctx.strokeStyle = "rgba(240,196,25,0.65)";
  ctx.fillStyle = "rgba(240,196,25,0.12)";
  ctx.lineWidth = 1;
  ctx.beginPath();
  let prevLon = null, wrapped = false;
  const pts = [];
  for (let i = 0; i <= 48; i++) {
    const brg = (i / 48) * TWOPI;
    const sLat = Math.sin(lat0) * Math.cos(psi) + Math.cos(lat0) * Math.sin(psi) * Math.cos(brg);
    const lat = Math.asin(Math.max(-1, Math.min(1, sLat)));
    const y = Math.sin(brg) * Math.sin(psi) * Math.cos(lat0);
    const x = Math.cos(psi) - Math.sin(lat0) * sLat;
    let lon = (lon0 + Math.atan2(y, x)) * RAD;
    while (lon > 180) lon -= 360;
    while (lon < -180) lon += 360;
    if (prevLon !== null && Math.abs(lon - prevLon) > 180) wrapped = true;
    prevLon = lon;
    pts.push([lonToX(lon), latToY(lat * RAD)]);
  }
  if (!wrapped) {
    pts.forEach(([x, y], i) => i ? ctx.lineTo(x, y) : ctx.moveTo(x, y));
    ctx.closePath();
    ctx.fill();
    ctx.stroke();
  }
  ctx.restore();
}

function upperBoundTime(t, data = routes) {
  if(data.upperBound)return data.upperBound(t);
  let lo = 0, hi = data.length;
  while (lo < hi) {
    const mid = (lo + hi) >> 1;
    if (data[mid][0] <= t) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

function recentRoutes(data, time, limit) {
  const src = srcFilter.value, dst = parseInt(dstFilter.value, 10);
  if(data.recent)return data.recent(time,src,dst,limit,ORBIT_CFG.routeLifetime);
  const found = [];
  for (let i = upperBoundTime(time, data) - 1; i >= 0 && found.length < limit; i--) {
    const r = data[i];
    if (time - r[0] > ORBIT_CFG.routeLifetime) break;
    const key = (r[7] ? "T:" : "GS:") + r[2];
    if ((src === "-1" || key === src) && (dst === -1 || r[3] === dst)) found.push(r);
  }
  return found;
}

function render() {
  ctx.fillStyle = "#0d2240";
  ctx.fillRect(0, 0, W, H);
  if (earthImg.complete && earthImg.naturalWidth) ctx.drawImage(earthImg, 0, 0, W, H);
  ctx.strokeStyle = "rgba(255,255,255,0.15)";
  ctx.lineWidth = 1;
  for (let lon = -180; lon <= 180; lon += 30) {
    const x = lonToX(lon);
    ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, H); ctx.stroke();
  }
  for (let lat = -90; lat <= 90; lat += 30) {
    const y = latToY(lat);
    ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(W, y); ctx.stroke();
  }

  const mapTime = csvMapTime ?? simTime;
  const snapshot = stateAt(mapTime);
  const sats = snapshot ? snapshot.nodes.slice(0, snapshot.numSats).map((p, idx) => ({ idx, lat: p[0], lon: p[1], alt: p[2] })) : [];
  const stStates = snapshot ? snapshot.nodes.slice(snapshot.numSats, snapshot.numSats + snapshot.numStations).map((p, id) => {
    const st = STATIONS.find(s => s.id === id) || { id, name: `GS${id}`, mobile: false };
    return { ...st, lat: p[0], lon: p[1], alt: p[2] };
  }) : [];
  const terminalStates = snapshot ? snapshot.nodes.slice(snapshot.numSats + snapshot.numStations).map((p, id) => ({ id, lat: p[0], lon: p[1], alt: p[2] })) : [];
  lastSatStates = sats; lastStationStates = stStates;
  if(typeof drawScenarioOverlay === "function")drawScenarioOverlay(snapshot,mapTime);
  const gslLinks = [], gslSatSet = new Set();
  let islCount = 0;
  if (snapshot) for (const [a, b, kind] of snapshot.links) {
    const from = snapshot.nodes[a], to = snapshot.nodes[b];
    if (!from || !to) continue;
    if (kind < 2) {
      islCount++;
      if (document.getElementById("chkIsl").checked) drawWrappedLine(from[1], from[0], to[1], to[0], (snapshot.linkEffects?.get(`${a}:${b}`)?.rho>0||snapshot.linkEffects?.get(`${b}:${a}`)?.rho>0)?"#ff9d47":kind === 0 ? "#4f8fd6" : "#9aa4b1", 1, 0.7);
    } else {
      gslLinks.push([a, b]); gslSatSet.add(b);
      if (document.getElementById("chkGsl").checked) drawWrappedLine(from[1], from[0], to[1], to[0], kind === 3 ? "#00d0ff" : "#22b14c", 2, 0.9);
    }
  }
  if (document.getElementById("chkFoot").checked) for (const idx of gslSatSet) {
    const sat = sats[idx]; if (sat) drawFootprint(sat.lat, sat.lon, sat.alt);
  }

  const routeLifetime = ORBIT_CFG.routeLifetime;
  const maxRoutes = Math.max(0, ORBIT_CFG.maxRoutes);
  const endIdx = upperBoundTime(simTime);
  const windowRoutes = recentRoutes(routes, simTime, 40);
  const drawnRoutes = (csvMapTime === null ? windowRoutes.slice(0, maxRoutes) : recentRoutes(resultMeta?.routes || [], mapTime, maxRoutes)).reverse();
  const activeSatSet = new Set();

  if (snapshot && document.getElementById("chkRoutes").checked) {
    drawnRoutes.forEach((r, idx) => {
      const [t, rId, srcId, dstId, hops, ms, path, srcIsTerm] = r;
      const srcSt = srcIsTerm ? terminalStates.find(s => s.id === srcId) : stStates.find(s => s.id === srcId);
      const dstSt = stStates.find(s => s.id === dstId);
      if (!srcSt || !dstSt || path.some(id => !sats[id])) return;
      const age = Math.max(0, mapTime - t);
      const alpha = csvMapTime === null && selectedRouteId === rId ? 1.0 : Math.max(0.15, 1.0 - age / routeLifetime);
      const color = ROUTE_COLORS[idx % ROUTE_COLORS.length];
      const width = csvMapTime === null && selectedRouteId === rId ? 6 : 4;
      const pts = [[srcSt.lon, srcSt.lat]];
      path.forEach(satIdx => {
        if (sats[satIdx]) {
          activeSatSet.add(satIdx);
          pts.push([sats[satIdx].lon, sats[satIdx].lat]);
        }
      });
      pts.push([dstSt.lon, dstSt.lat]);
      for (let i = 1; i < pts.length; i++) {
        drawWrappedLine(pts[i - 1][0], pts[i - 1][1], pts[i][0], pts[i][1], color, width, alpha);
      }
      if (document.getElementById("chkLabels").checked) {
        ctx.save();
        ctx.font = "bold 20px sans-serif";
        ctx.fillStyle = color;
        ctx.globalAlpha = alpha;
        ctx.fillText(`${srcSt.name} -> ${dstSt.name}  ${hops} ISL hops  ${ms.toFixed(1)} ms`, lonToX(pts[0][0]) + 12, latToY(pts[0][1]) - 14);
        ctx.restore();
      }
    });
  }

  for (const s of sats) {
    ctx.beginPath();
    ctx.arc(lonToX(s.lon), latToY(s.lat), 4, 0, TWOPI);
    ctx.fillStyle = "#ffffff";
    ctx.fill();
    if (activeSatSet.has(s.idx)) {
      ctx.strokeStyle = "orange";
      ctx.lineWidth = 2;
      ctx.stroke();
    }
  }

  for (const tm of terminalStates) {
    ctx.beginPath();
    ctx.arc(lonToX(tm.lon), latToY(tm.lat), 3.5, 0, TWOPI);
    ctx.fillStyle = "#ffe066";
    ctx.fill();
  }

  const showLabels = document.getElementById("chkLabels").checked;
  for (const st of stStates) {
    const x = lonToX(st.lon), y = latToY(st.lat);
    ctx.save();
    if (!st.mobile) {
      ctx.fillStyle = "#ff4040";
      ctx.strokeStyle = "#ffffff";
      ctx.lineWidth = 1;
      ctx.fillRect(x - 6, y - 6, 12, 12);
      ctx.strokeRect(x - 6, y - 6, 12, 12);
    } else {
      ctx.fillStyle = "#00d0ff";
      ctx.strokeStyle = "#ffffff";
      ctx.lineWidth = 1;
      ctx.beginPath();
      ctx.moveTo(x, y - 8);
      ctx.lineTo(x - 7, y + 6);
      ctx.lineTo(x + 7, y + 6);
      ctx.closePath();
      ctx.fill();
      ctx.stroke();
    }
    if (showLabels) {
      ctx.font = "bold 20px sans-serif";
      ctx.fillStyle = st.mobile ? "#7fe8ff" : "#ffffff";
      ctx.fillText(st.name, x + 16, y + 18);
    }
    ctx.restore();
  }

  let sumMs = 0, sumHops = 0;
  if(routes.sumsAt)[sumHops,sumMs]=routes.sumsAt(endIdx);
  else for (let i = 0; i < endIdx; i++) { sumHops += routes[i][4]; sumMs += routes[i][5]; }
  const avgMs = endIdx ? (sumMs / endIdx).toFixed(1) : "0.0";
  const avgHops = endIdx ? (sumHops / endIdx).toFixed(1) : "0.0";

  document.getElementById("mapTime").textContent = `${mapTime.toFixed(1)} s`;
  document.getElementById("mapSatellites").textContent = snapshot ? `${sats.length.toLocaleString("ko-KR")}기` : "—";
  document.getElementById("mapIsl").textContent = snapshot ? `${islCount.toLocaleString("ko-KR")}개` : "—";
  document.getElementById("mapGroundLinks").textContent = snapshot ? `${gslLinks.length.toLocaleString("ko-KR")}개` : "—";
  document.getElementById("mapRouting").textContent = `${scenarioName(resultMeta?.config.activeConfig || previewConfig?.activeConfig || "CSV")} · ${algorithmName(resultMeta?.config||previewConfig)}${resultMeta?.summary?.experimental?" · 환경/실험 설정":""}`;
  document.getElementById("mapSample").textContent = snapshot && !resultMeta ? "설정 미리보기 · 실행 결과와 별도" : snapshot ? (snapshot.reconstructed && csvMapTime === null ? "설정 기반 재현 · 기록된 경로" : csvMapTime === null ? `Engine sample: ${snapshot.t.toFixed(3)} s | no interpolation` : `참고 지도 · ${scenarioName(resultMeta.config.activeConfig)} · ${snapshot.t.toFixed(3)} s · 선택 CSV와 별도`) : "No engine state for this data";

  document.getElementById("timeLabel").textContent = `t = ${simTime.toFixed(1)} s`;
  document.getElementById("timeSlider").value = simTime;
  document.getElementById("kpiSat").textContent = snapshot ? `${sats.length} / ${islCount}` : "N/A";
  document.getElementById("kpiGslLabel").textContent = "설정 기반 연결";
  document.getElementById("kpiGsl").textContent = snapshot ? gslLinks.length : "N/A";
  document.getElementById("kpiPkts").textContent = `${endIdx} / ${routes.length}`;
  document.getElementById("kpiDelay").textContent = `${avgMs} ms / ${avgHops}`;
  document.getElementById("activeCount").textContent = `${windowRoutes.length} in ${routeLifetime}s`;

  const tbody = document.querySelector("#routeTable tbody");
  tbody.innerHTML = windowRoutes.slice(0, 25).map(r => {
    const [t, rId, srcId, dstId, hops, ms, path, srcIsTerm] = r;
    const dstSt = stStates.find(s => s.id === dstId) || STATIONS.find(s => s.id === dstId);
    const srcTag = srcIsTerm ? `T${srcId}` : `GS${srcId}`;
    const dstTag = (dstSt && dstSt.mobile) ? `M${dstId}` : `GS${dstId}`;
    const sel = selectedRouteId === rId ? " class='selected'" : "";
    return `<tr data-id="${rId}" data-t="${t}"${sel}>
      <td>${t.toFixed(1)}s</td>
      <td>${srcTag}→${dstTag}</td>
      <td>${hops}</td>
      <td title="First satellite: ${r[8]}; exit satellite: ${r[9]}; uplink: ${r[10]} ms; satellite-to-station: ${r[11]} ms; total: ${ms} ms">${ms.toFixed(1)}ms</td>
      <td class="path-cell" title="${path.join("-")}">${path.join("-")}</td>
    </tr>`;
  }).join("");
}

function loop(ts) {
  if (playing) {
    const dt = lastFrame ? (ts - lastFrame) / 1000 : 0;
    simTime = Math.min(maxSimTime, simTime + dt * speed);
    if (simTime >= maxSimTime) { playing = false; document.getElementById("btnPlay").textContent = "▶ Play"; }
    render();
  }
  lastFrame = ts;
  requestAnimationFrame(loop);
}
requestAnimationFrame(loop);

document.getElementById("btnPlay").onclick = e => {
  playing = !playing;
  e.target.textContent = playing ? "⏸ Pause" : "▶ Play";
};
document.getElementById("btnStepBack").onclick = () => { simTime = Math.max(0, simTime - 1); render(); };
document.getElementById("btnStepFwd").onclick = () => { simTime = Math.min(maxSimTime, simTime + 1); render(); };
document.getElementById("btnSnap").onclick = () => { simTime = Math.min(maxSimTime, 1210); selectedRouteId = null; render(); };
document.getElementById("timeSlider").oninput = e => { simTime = parseFloat(e.target.value); render(); };
document.getElementById("speedSel").onchange = e => { speed = parseFloat(e.target.value); };
["chkIsl", "chkGsl", "chkFoot", "chkRoutes", "chkLabels", "srcFilter", "dstFilter"].forEach(id => {
  document.getElementById(id).onchange = () => render();
});

document.querySelector("#routeTable tbody").onclick = e => {
  const tr = e.target.closest("tr");
  if (!tr) return;
  selectedRouteId = parseInt(tr.dataset.id, 10);
  simTime = parseFloat(tr.dataset.t);
  render();
};

// Hover Tooltip on Map (accounts for object-fit: contain letterboxing)
const tooltip = document.getElementById("tooltip");
canvas.onmousemove = e => {
  const rect = canvas.getBoundingClientRect();
  const scale = Math.min(rect.width / W, rect.height / H);
  if (!scale) return (tooltip.hidden = true);
  const offX = (rect.width - W * scale) / 2, offY = (rect.height - H * scale) / 2;
  const mx = (e.clientX - rect.left - offX) / scale, my = (e.clientY - rect.top - offY) / scale;
  if (mx < 0 || mx > W || my < 0 || my > H) return (tooltip.hidden = true);
  let hit = null;
  for (const st of lastStationStates) {
    if (Math.hypot(lonToX(st.lon) - mx, latToY(st.lat) - my) < 14) {
      hit = `${st.mobile ? "Mobile Terminal" : "Ground Station"} #${st.id}\n${st.name}\nLat: ${st.lat.toFixed(2)}°, Lon: ${st.lon.toFixed(2)}°`;
      break;
    }
  }
  if (!hit) {
    for (const s of lastSatStates) {
      if (Math.hypot(lonToX(s.lon) - mx, latToY(s.lat) - my) < 10) {
        hit = `Satellite sat[${s.idx}] (engine snapshot)\nLat: ${s.lat.toFixed(2)}°, Lon: ${s.lon.toFixed(2)}°, Alt: ${s.alt.toFixed(0)}km`;
        break;
      }
    }
  }
  if (hit && resultMeta?.networkEvents?.length) {const state=stateAt(csvMapTime??simTime);for(const s of lastSatStates)if(Math.hypot(lonToX(s.lon)-mx,latToY(s.lat)-my)<10){const info=state.knowledge?.get(s.idx);if(info)hit+=`\nCSIB v${info.version} · 정보 시각 ${info.dataTime.toFixed(3)}s`;break;}}
  if (hit) {
    const wrap = canvas.parentElement.getBoundingClientRect();
    tooltip.hidden = false;
    tooltip.textContent = hit;
    tooltip.style.left = `${Math.min(wrap.width - 210, e.clientX - wrap.left + 12)}px`;
    tooltip.style.top = `${Math.min(wrap.height - 70, e.clientY - wrap.top + 12)}px`;
  } else {
    tooltip.hidden = true;
  }
};
canvas.onmouseleave = () => { tooltip.hidden = true; };

// Custom CSV Loader
document.getElementById("csvInput").onchange = e => {
  const file = e.target.files[0];
  if (!file) return;
  e.target.value = "";
  const request = ++resultLoadRequest;
  (async()=>{
    try {
      let parsed;
      if(resultMeta&&file.size>4*1024*1024){setStatus("CSV 확인 중…");const digest=await crypto.subtle.digest("SHA-256",await file.arrayBuffer());const hash=[...new Uint8Array(digest)].map(n=>n.toString(16).padStart(2,"0")).join("");if(hash===resultMeta.csvHash)parsed=resultMeta.routes;}
      parsed=parsed||await parseRoutesBlob(file,()=>{},p=>{if(request!==resultLoadRequest)throw Error("결과 선택이 변경되었습니다.");setStatus(`CSV 읽는 중 ${Math.round(p*100)}%`);});
      if(request!==resultLoadRequest)return;

      const matchesMap = !!resultMeta && sameRoutes(parsed,resultMeta.routes);
      // CSV has no coordinates: retain unmatched engine state as a labelled reference, never attach foreign paths.
      csvMapTime = matchesMap ? null : (csvMapTime ?? simTime);
      routes = parsed;
      selectedRouteId = null;
      playing = false;
      document.getElementById("btnPlay").textContent = "Play";
      simTime = routes.length ? Math.min(Math.max(simTime, routes[0][0]), routes[routes.length - 1][0]) : 0;
      syncTimeBounds(matchesMap ? (resultMeta.replay ? resultMeta.config.simTimeLimit : states[states.length - 1].t) : (routes.length ? routes[routes.length - 1][0] : 0));
      document.getElementById("csvSource").textContent = `선택 CSV: ${file.name} · ${routes.length.toLocaleString("ko-KR")}개 계획 경로`;
      window.dispatchEvent(new CustomEvent("sim-csv",{detail:{matchesMap}}));
      setStatus(matchesMap ? "선택한 CSV가 저장된 실행 결과와 일치합니다. 지도와 경로를 함께 재생합니다." : "선택한 CSV의 경로 표와 통계를 재생합니다. 위치 정보가 없어 지도는 기존 실행의 참고 화면으로 유지합니다.");
    } catch(err){if(request===resultLoadRequest)setStatus(`CSV error: ${err.message}`);}
    if(request===resultLoadRequest)render();
  })();
};

// ---- Local Backend API Integration (server.py) ----
const runStatus = document.getElementById("runStatus");
function setStatus(msg) {
  runStatus.hidden = !msg;
  runStatus.textContent = msg;
}

const escHtml = s => String(s).replace(/[&<>"']/g, ch => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[ch]));
const iniFilter = document.getElementById("iniFilter");
const chkShowNodes = document.getElementById("chkShowNodes");
let currentEffectiveEntries = [],iniLabels={};
const INI_NAMES={sendInterval:"송신 간격",packetSize:"패킷 크기",startTime:"송신 시작",endTime:"송신 종료",enabled:"사용 여부",dstType:"목적지 종류",dstStation:"목적지 노드",metric:"경로 선택 기준",service:"MCSR 서비스",routingType:"라우팅 모듈",algorithm:"알고리즘",numSatellites:"위성 수",numTerminals:"단말 수",nrOfGS:"지상 노드 수","sim-time-limit":"실행 시간","seed-0-mt":"난수 시드"};
function iniLabel(key){return iniLabels[key]||INI_NAMES[key.split(".").at(-1)]||"";}

function flashEl(el) {
  if (!el) return;
  el.classList.remove("flash-change");
  void el.offsetWidth;
  el.classList.add("flash-change");
  clearTimeout(el._flashTid);
  el._flashTid = setTimeout(() => el.classList.remove("flash-change"), 900);
}

function renderIniTable() {
  const q = (iniFilter.value || "").trim().toLowerCase();
  const showNodes = chkShowNodes.checked;
  const rows = currentEffectiveEntries.filter(e => {
    if (!showNodes && !q && /^\*\.(station|terminal)\[\d+\]\.(mob|mobility|label|nodeType)/.test(e.k)) return false;
    return !q || iniLabel(e.k).toLowerCase().includes(q) || e.k.toLowerCase().includes(q) || String(e.v).toLowerCase().includes(q) || e.sec.toLowerCase().includes(q);
  });
  document.querySelector("#iniTable tbody").innerHTML = rows.map(e => {
    const ov = e.sec !== "General" ? " class=\"ini-override\"" : "";
    return `<tr${ov}><td class="ini-k" title="${escHtml(e.k)}">${iniLabel(e.k)?`<strong class="ini-label">${escHtml(iniLabel(e.k))}</strong>`:""}${escHtml(e.k)}</td><td class="ini-v" title="${escHtml(e.v)}">${escHtml(e.v)}</td><td class="ini-s">[${escHtml(e.sec)}]</td></tr>`;
  }).join("");
  document.getElementById("iniCount").textContent = `표시 ${rows.length} / 전체 ${currentEffectiveEntries.length}개 INI 키 (파란 강조 = 상속/시나리오 설정)`;
}

iniFilter.oninput = renderIniTable;
chkShowNodes.onchange = renderIniTable;

const SCENARIO_NAMES={
  Dijkstra132:"기본 통신",Dijkstra132Pair:"단일 구간 통신",Dijkstra132StationsOnly:"지상국 간 통신",Dijkstra132PairFail:"반복 링크 장애",Dijkstra132RandomFail:"무작위 링크 장애",
  PaperS1:"대규모 단일 구간",PaperS1Load:"대규모 부하",PaperS1DijkstraCutFlood:"대규모 장애·복구"
};
const ADDITIONAL_CONFIGS=new Set("Dijkstra132Hop Mcsr132 Mcsr132TS Mcsr132HR Mcsr132Pair Mcsr132PairFail Mcsr132RandomFail PaperS1Mcsr PaperS1McsrTS PaperS1McsrHR PaperS1Dijkstra PaperS1McsrFail PaperS1DijkstraFail PaperS1McsrCut PaperS1DijkstraCut PaperS1McsrCongestion PaperS1DijkstraCongestion PaperS1DijkstraCongestionAware PaperS1McsrCutFlood PaperS1McsrCutFloodRes PaperS1DijkstraCutFloodLoad PaperS1McsrCutFloodLoad PaperS1McsrCutFloodResLoad PaperS1McsrCsib".split(" "));
const ALGORITHM_NAMES={"dijkstra-delay":"Dijkstra · 지연 최소","dijkstra-hop":"Dijkstra · 홉 최소","mcsr-ls":"MCSR · 지연 우선 (LS)","mcsr-ts":"MCSR · 처리량 우선 (TS)","mcsr-hr":"MCSR · 신뢰성 우선 (HR)"};
function scenarioName(name){
  if(SCENARIO_NAMES[name])return SCENARIO_NAMES[name];
  if(!ADDITIONAL_CONFIGS.has(name))return name||"결과를 선택하세요";
  const base=name.startsWith("PaperS1")?"대규모 ":"";
  const kind=name.includes("CongestionAware")?"혼잡 · 지연 반영":name.includes("Congestion")?"혼잡":name.includes("CutFloodResLoad")?"장애·복구 · 예비 경로·배경 부하":name.includes("CutFloodLoad")?"장애·복구 · 배경 부하":name.includes("CutFloodRes")?"장애·복구 · 예비 경로":name.includes("CutFlood")?"장애·복구":name.includes("PairFail")||name.includes("Cut")?"반복 링크 장애":name.includes("Fail")?"무작위 링크 장애":name.includes("Csib")?"DS·CSIB":name.includes("Pair")?"단일 구간 통신":"기본 통신";
  return base+kind;
}
function algorithmName(cfg){return ALGORITHM_NAMES[cfg?.algorithm]||cfg?.algorithm||ALGORITHM_NAMES["dijkstra-"+(cfg?.metric||"delay")];}
let selectedConfig=null,configSaving=false,configLoading=false;
function populateConfigChoices(cfg){
  const all=document.getElementById("showAllConfigs").checked;
  cfgNameSel.replaceChildren();
  for(const c of cfg.configs||[]){
    const basic=Object.hasOwn(SCENARIO_NAMES,c.name),custom=!ADDITIONAL_CONFIGS.has(c.name);
    if(!all&&!basic&&!custom&&c.name!==cfg.activeConfig)continue;
    const opt=new Option(scenarioName(c.name)+(basic||custom?"":` (${c.name})`),c.name);opt.title=c.name;cfgNameSel.add(opt);
  }
  cfgNameSel.value=cfg.activeConfig;
}
document.getElementById("showAllConfigs").onchange=()=>{if(selectedConfig)populateConfigChoices(selectedConfig);};
// This changes only the algorithm in the selected INI section, never the recorded result.
document.getElementById("algorithmChoice").onchange=async event=>{
  if(configSaving||configLoading||!selectedConfig)return;
  const cfg=selectedConfig,algorithm=event.target.value;
  const controls=[cfgNameSel,event.target,...["btnRunSim","btnExperiment","btnSettings","btnRefreshIni","btnOpenIni","showAllConfigs"].map(id=>document.getElementById(id))];
  const states=controls.map(e=>e.disabled);configSaving=true;controls.forEach(e=>e.disabled=true);
  try{
    const response=await fetch("/api/config",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({configName:cfg.activeConfig,changes:{algorithm}})});
    const result=await response.json();if(!response.ok)throw Error(result.error||"알고리즘을 저장하지 못했습니다.");
    applyLoadedConfig(result,{announce:true});
  }catch(error){event.target.value=cfg.algorithm;setStatus(error.message);}
  finally{configSaving=false;controls.forEach((e,i)=>e.disabled=states[i]);}
};

function updateProfileBox(cfg, highlight) {
  document.getElementById("savedConfigName").textContent = scenarioName(cfg.activeConfig);
  const entries = cfg.effectiveEntries || [];
  const value = key => entries.find(e => e.k === key)?.v;
  document.getElementById("scenarioConstellation").textContent = cfg.numSatellites == null ? "—" : `${cfg.numSatellites}기 · ${cfg.planes}개 궤도면`;
  document.getElementById("scenarioOrbit").textContent = cfg.altitude == null ? "—" : `${cfg.altitude} km · ${cfg.inclination}°`;
  document.getElementById("scenarioDuration").textContent = cfg.simTimeLimit == null ? "—" : `${cfg.simTimeLimit.toLocaleString("ko-KR")}초`;
  const routing = { delay: "지연 최소", hop: "홉 수 최소" };
  document.getElementById("scenarioRouting").textContent = ({"mcsr-ls":"MCSR · 지연 우선 (LS)","mcsr-ts":"MCSR · 처리량 우선 (TS)","mcsr-hr":"MCSR · 신뢰성 우선 (HR)"})[cfg.algorithm] || routing[cfg.metric] || cfg.metric || "—";
  const interval = value("*.station[*].traffic.sendInterval");
  const size = value("*.station[*].traffic.packetSize");
  document.getElementById("scenarioTraffic").textContent = interval && size ? `${interval} · ${size}` : (cfg.activeConfig ? "노드별 설정" : "—");
  document.getElementById("scenarioTerminals").textContent = cfg.numTerminals == null ? "—" : (cfg.numTerminals === 0 ? "사용 안 함" : `${cfg.numTerminals}개`);
  const iniSrc = document.getElementById("cfgIniSrc");
  const descEl = document.getElementById("cfgDesc");
  const extEl = document.getElementById("cfgExtends");
  const ownEl = document.getElementById("cfgOwnIni");

  if (cfg.iniPath) {
    iniSrc.textContent = cfg.iniPath.startsWith("config/") ? `보관된 설정: ${cfg.iniPath}` : cfg.leoFound ? `📂 ${cfg.iniPath}` : `⚠️ 내장 백업: ${cfg.iniPath}`;
    iniSrc.title = cfg.iniPath;
    iniSrc.style.color = cfg.leoFound ? "#69f0ae" : "#ffe066";
  } else if (!location.protocol.startsWith("http")) {
    iniSrc.textContent = "ℹ️ 로컬 HTML 모드";
    iniSrc.style.color = "#8da2c0";
  }

  const desc = cfg.description || "";
  descEl.textContent = desc;
  descEl.hidden = !desc;

  const chainTxt = Array.isArray(cfg.extends) && cfg.extends.length
    ? `extends = ${cfg.extends.join(", ")}`
    : (Array.isArray(cfg.chain) && cfg.chain.length > 1 ? `상속: ${cfg.chain.join(" → ")}` : "");
  extEl.textContent = chainTxt;
  extEl.hidden = !chainTxt;

  const active = cfg.activeConfig || cfgNameSel.value || "Dijkstra132";
  const ownLines = (cfg.ownEntries || []).map(e => `${e.k} = ${e.v}`);
  ownEl.textContent = `[Config ${active}]\n` + (ownLines.length ? ownLines.join("\n") : "; (직접 오버라이드 없음 — extends 설정 그대로 사용)");

  if (highlight) flashEl(document.getElementById("cfgProfile"));
}

function applyLoadedConfig(cfg, opts = {}) {
  if (!cfg) return;
  selectedConfig=cfg;
  const algorithmChoice=document.getElementById("algorithmChoice");
  algorithmChoice.replaceChildren();
  for(const [id,label]of Object.entries(cfg.capabilities?.algorithms||ALGORITHM_NAMES))algorithmChoice.add(new Option(ALGORITHM_NAMES[id]||label,id));
  algorithmChoice.value=cfg.algorithm||"dijkstra-"+(cfg.metric||"delay");
  if (Array.isArray(cfg.configs) && cfg.configs.length) {
    populateConfigChoices(cfg);
    cfgNameSel.dataset.loaded = "1";
  }

  const configKey = JSON.stringify([cfg.activeConfig, cfg.effectiveEntries]);
  const resultKey = resultMeta ? JSON.stringify([resultMeta.config.activeConfig, resultMeta.config.effectiveEntries]) : "";
  const showingPreviousRun = !!resultMeta && !opts.result && resultKey !== configKey;
  if (showingPreviousRun) {
    setStatus(`지도에는 [${scenarioName(resultMeta.config.activeConfig)} · ${algorithmName(resultMeta.config)}]의 마지막 실행 결과가 표시됩니다. 선택한 [${scenarioName(cfg.activeConfig)} · ${algorithmName(cfg)}] 설정을 반영하려면 실행하세요.`);
  }
  loadedConfigKey = configKey;
  if (opts.result || !resultMeta) {
  if(!resultMeta)previewConfig=cfg;
  updateOrbitConfig({ ...cfg, minEl: cfg.minimumElevation });
  if (Array.isArray(cfg.stations) || Array.isArray(cfg.terminals)) populateFilters();
  if (cfg.simTimeLimit && csvMapTime === null) syncTimeBounds(cfg.simTimeLimit);
  }

  const active = resultMeta?.config.activeConfig || cfg.activeConfig || cfgNameSel.value;
  const rShort = (ORBIT_CFG.routingType || "Routing").replace(/^Sat_Net_|Routing$/g, "");
  document.title = `LEO Network Simulator (${scenarioName(active)})`;
  document.getElementById("topBadge").textContent =
    `${scenarioName(active)} · Walker-Delta ${ORBIT_CFG.numSatellites}/${ORBIT_CFG.planes}/${ORBIT_CFG.phasing} · ${ORBIT_CFG.altitude}km · ${ORBIT_CFG.inclination}° · ${algorithmName(resultMeta?.config||cfg)}`;

  window.dispatchEvent(new CustomEvent("sim-config",{detail:cfg}));
  iniLabels=Object.fromEntries((cfg.capabilities?.fields||[]).map(f=>[f.ini,f.label]));
  currentEffectiveEntries = Array.isArray(cfg.effectiveEntries) ? cfg.effectiveEntries : [];
  updateProfileBox(cfg, !!opts.announce);
  renderIniTable();

  if (opts.announce && !showingPreviousRun) {
    setStatus(`${scenarioName(cfg.activeConfig)} · ${algorithmName(cfg)} 설정을 불러왔습니다.`);
  }
  render();
}

updateProfileBox({}, false);
renderIniTable();

if (location.protocol.startsWith("http")) {
  let configRequest = 0;
  const fetchConfig = (cfg, announce = false) => {
    const request = ++configRequest;
    configLoading=true;document.getElementById("algorithmChoice").disabled=true;
    return fetch(cfg ? `/api/config?config=${encodeURIComponent(cfg)}` : "/api/config", { cache: "no-store" })
      .then(r => { if (!r.ok) throw new Error("설정을 불러오지 못했습니다."); return r.json(); })
      .then(d => { if (request === configRequest) applyLoadedConfig(d, { announce }); })
      .catch(err => { if (request === configRequest) setStatus(err.message); })
      .finally(()=>{if(request===configRequest){configLoading=false;document.getElementById("algorithmChoice").disabled=configSaving||document.getElementById("btnRunSim").disabled;}});
  };
  const startup=++resultLoadRequest;
  fetchConfig().then(()=>fetch("/api/result?metadata=1",{cache:"no-store"})).then(r=>{if(!r.ok)throw Error("저장 결과를 불러오지 못했습니다.");return r.json();})
    .then(async result=>{if(startup!==resultLoadRequest)return;if(result?.bundle)await openResultBundle(result.bundle,startup);else if(result)installResult(result);})
    .catch(err=>{if(startup===resultLoadRequest)setStatus(`${err.message} 설정 미리보기를 유지합니다.`);});
  cfgNameSel.onchange = () => {++resultLoadRequest;return fetchConfig(cfgNameSel.value, true);};
  document.getElementById("btnRefreshIni").onclick = () => fetchConfig(cfgNameSel.value, true);
  window.addEventListener("focus", () => !configSaving&&fetchConfig(cfgNameSel.dataset.loaded ? cfgNameSel.value : "", false));
}

document.getElementById("btnOpenIni").onclick = async () => {
  if (location.protocol === "file:") {
    setStatus("INI 편집은 sim-ui.exe 또는 start_ui.bat에서 사용할 수 있습니다. HTML에 표시된 INI는 저장된 실행의 사본입니다.");
    return;
  }
  try {
    const res = await fetch("/api/open-ini", { method: "POST" });
    const data = await res.json();
    if (!res.ok) throw new Error(data.error || "설정 파일을 열지 못했습니다.");
    setStatus(`새 실행 설정 편집: ${data.path} · 저장 후 설정 새로고침을 누르세요. 보관된 실행 결과는 변경되지 않습니다.`);
  } catch (err) { setStatus(err.message); }
};

let activeJob=null;
async function runNative(algorithms=null,research=null) {
  if(configSaving||configLoading)return setStatus("설정을 반영하고 있습니다. 잠시 후 실행하세요.");
  if(!location.protocol.startsWith("http"))return setStatus("새 계산은 sim-ui.exe에서 실행하세요.");
  const btn=document.getElementById("btnRunSim"),compare=document.getElementById("btnExperiment"),stop=document.getElementById("btnStopSim");btn.disabled=true;compare.disabled=true;stop.hidden=false;
  const settingsControls=["cfgName","algorithmChoice","btnSettings","btnOpenIni","btnRefreshIni","showAllConfigs"].map(id=>document.getElementById(id)),settingsStates=settingsControls.map(e=>e.disabled);settingsControls.forEach(e=>e.disabled=true);
  try {
    let response=await fetch(algorithms?"/api/experiments":"/api/run",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({configName:cfgNameSel.value,...(algorithms?{algorithms,research}:{})})});
    let data=await response.json();if(!response.ok)throw Error(data.error||"실행하지 못했습니다.");
    activeJob=data.runId;
    for(;;){
      await new Promise(resolve=>setTimeout(resolve,500));
      const job=await fetch(`/api/run/${activeJob}`,{cache:"no-store"}).then(r=>r.json());
      if(job.error&&job.status!=="cancelled")throw Error(job.error);
      if(job.status==="completed"){if(algorithms){const latest=job.completedRuns.at(-1);const result=await fetch(`/api/result?runId=${encodeURIComponent(latest.runId)}&metadata=1`).then(r=>r.json());await openResultBundle(result.bundle);if(job.statistics)showExperimentStatistics(job.statistics);else document.getElementById("btnCompare").click();setStatus(`비교 실행 완료 · ${job.completedRuns.length}개`);}else{const result=await fetch(`/api/result?runId=${encodeURIComponent(job.runId)}&metadata=1`).then(r=>r.json());await openResultBundle(result.bundle);setStatus(`계산 완료 · ${job.runId}`);}break;}
      if(job.status==="cancelled"){setStatus("계산을 중지했습니다. 이전 결과를 유지합니다.");break;}
      if(job.status==="failed")throw Error(job.error||"계산 실패");
      const p=job.progress;setStatus(p?`계산 중 ${job.completedRuns?.length||0} / ${job.totalRuns||1} · ${Math.round(p.progress*100)}% · ${p.time.toFixed(1)} / ${p.limit}s · 수신 ${p.received.toLocaleString()}`:"계산 준비 중…");
      const progress=document.getElementById("runProgress");progress.hidden=false;progress.value=p?((job.completedRuns?.length||0)+p.progress)/(job.totalRuns||1):0;
    }
  }catch(error){setStatus(`실행 오류: ${error.message}`);}
  finally {settingsControls.forEach((e,i)=>e.disabled=settingsStates[i]);activeJob=null;btn.disabled=false;compare.disabled=false;stop.hidden=true;document.getElementById("runProgress").hidden=true;}
}
document.getElementById("btnRunSim").onclick=()=>runNative();
document.getElementById("btnStopSim").onclick=async()=>{if(activeJob){await fetch(`/api/run/${activeJob}/cancel`,{method:"POST"});setStatus("계산을 중지하는 중…");}};

render();

if (location.protocol === "file:") {
  const saved=()=>{const result=window.SIM_RESULT;if(!result)return;
    if(result.bundle?.fileUrls){const text=result.bundle.files?.[result.bundle.manifest.normalizedPath];if(text)applyLoadedConfig(JSON.parse(text));setStatus("대용량 결과입니다. 실행 결과 폴더를 선택하면 전체 경로를 읽습니다.");}
    else if(result.bundle)openResultBundle(result.bundle).catch(err=>setStatus(err.message));else installResult(result);
  };
  if(window.SIM_RESULT)saved();else{const script=document.createElement("script");script.src="data/latest_result.js?v=map-load-1";script.onload=saved;script.onerror=()=>setStatus("저장된 결과를 자동으로 읽지 못했습니다. 실행 결과 폴더를 선택하세요.");document.head.append(script);}
  document.getElementById("algorithmChoice").disabled=true;
  document.getElementById("showAllConfigs").closest("label").hidden=true;
  cfgNameSel.disabled = true;
  cfgNameSel.hidden = true;
  document.getElementById("savedConfigName").hidden = false;
  document.getElementById("cfgNameLabel").textContent = "시험 조건";
  document.getElementById("scenarioHeading").textContent = "실행 결과";
  document.getElementById("btnRefreshIni").hidden = true;
  document.getElementById("runnerActions").hidden = true;
  document.getElementById("runModeHint").hidden = false;
}

async function openResultBundle(bundle, request = ++resultLoadRequest) {
  const prepared = await prepareBundle(bundle,msg=>{if(request!==resultLoadRequest)throw Error("결과 선택이 변경되었습니다.");setStatus(msg);});
  if (request !== resultLoadRequest) return;
  const previous = { ...ORBIT_CFG, stations: STATIONS, terminals: TERMINALS };
  try {
    updateOrbitConfig({ ...prepared.config, minEl: prepared.config.minimumElevation });
    const trial = replaySnapshot(0);
    if (trial.nodes.some(p => !p.every(Number.isFinite))) throw new Error("설정으로 위치를 계산할 수 없습니다.");
  } catch (err) { updateOrbitConfig(previous); throw err; }
  installResult(prepared);
  document.getElementById("csvSource").textContent = `실행 ID: ${prepared.runId} · CSV ${prepared.routes.length.toLocaleString("ko-KR")}개 · 설정 포함`;
  setStatus(prepared.notice || (bundle.manifest.provenance === "manual-import-unverified" ? "가져온 CSV·INI로 지도를 재현합니다. 수동 등록 자료이므로 실행 당시 설정인지 확인해 주세요." : "실행 당시 보관한 CSV·INI로 지도를 재현합니다."));
}
document.getElementById("resultFolder").onchange = async e => {
  const selection = [...e.target.files]; e.target.value = "";
  if (!selection.length) return;
  const request = ++resultLoadRequest;
  try {
    const bundle=await bundleFromFiles(selection);
    await openResultBundle(bundle,request);
  } catch (err) { if (request === resultLoadRequest) setStatus(`결과를 불러오지 못했습니다: ${err.message} 기존 화면을 유지합니다.`); }
};
