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
let resultLoadRequest = 0;
function stateAt(t) {
  if (resultMeta?.replay) return replaySnapshot(t);
  let lo = 0, hi = states.length;
  while (lo < hi) { const mid = (lo + hi) >> 1; if (states[mid].t <= t) lo = mid + 1; else hi = mid; }
  return lo ? states[lo - 1] : null;
}
function clearResult() {
  routes = []; states = []; resultMeta = null; selectedRouteId = null; simTime = 0; csvMapTime = null;
  document.getElementById("csvSource").textContent = "";
  playing = false; document.getElementById("btnPlay").textContent = "Play";
  document.getElementById("resultFiles").hidden = true;
}
function installResult(result) {
  clearResult();
  routes = result.routes; states = result.states; resultMeta = result;
  simTime = Math.min(1210, result.replay ? result.config.simTimeLimit : (states.length ? states[states.length - 1].t : 0));
  applyLoadedConfig(result.config, { result: true });
  syncTimeBounds(result.replay ? result.config.simTimeLimit : (states.length ? states[states.length - 1].t : 0));
  setStatus(`Run ${result.runId} | engine snapshots | input ${result.inputHash.slice(0, 12)} | ${result.createdAt}`);
  const download = document.getElementById("resultFiles");
  download.hidden = false;
  download.href = `data/runs/${result.runId}/${result.replay ? "routes.csv" : "results/routes.csv"}`;
  download.textContent = `Original CSV | run ${result.runId} | SHA-256 ${result.csvHash.slice(0, 16)}`;
  render();
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
  const gslLinks = [], gslSatSet = new Set();
  let islCount = 0;
  if (snapshot) for (const [a, b, kind] of snapshot.links) {
    const from = snapshot.nodes[a], to = snapshot.nodes[b];
    if (!from || !to) continue;
    if (kind < 2) {
      islCount++;
      if (document.getElementById("chkIsl").checked) drawWrappedLine(from[1], from[0], to[1], to[0], kind === 0 ? "#4f8fd6" : "#9aa4b1", 1, 0.7);
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
  for (let i = 0; i < endIdx; i++) { sumHops += routes[i][4]; sumMs += routes[i][5]; }
  const avgMs = endIdx ? (sumMs / endIdx).toFixed(1) : "0.0";
  const avgHops = endIdx ? (sumHops / endIdx).toFixed(1) : "0.0";

  document.getElementById("mapTime").textContent = `${mapTime.toFixed(1)} s`;
  document.getElementById("mapSatellites").textContent = snapshot ? `${sats.length.toLocaleString("ko-KR")}기` : "—";
  document.getElementById("mapIsl").textContent = snapshot ? `${islCount.toLocaleString("ko-KR")}개` : "—";
  document.getElementById("mapGroundLinks").textContent = snapshot ? `${gslLinks.length.toLocaleString("ko-KR")}개` : "—";
  document.getElementById("mapRouting").textContent = `${resultMeta?.config.activeConfig || "CSV"} | ${ORBIT_CFG.routingType} | ${ORBIT_CFG.metric}`;
  document.getElementById("mapSample").textContent = snapshot ? (snapshot.reconstructed && csvMapTime === null ? "설정 기반 재현 · CSV 경로 · 실측 수신 통계 없음" : csvMapTime === null ? `Engine sample: ${snapshot.t.toFixed(3)} s | no interpolation` : `참고 지도 · ${resultMeta.config.activeConfig} · ${snapshot.t.toFixed(3)} s · 선택 CSV와 별도`) : "No engine state for this data";

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
  const reader = new FileReader();
  reader.onload = ev => {
    if (request !== resultLoadRequest) return;
    try {
      const parsed = parseRoutesCsv(ev.target.result);
      const matchesMap = !!resultMeta && JSON.stringify(parsed) === JSON.stringify(resultMeta.routes);
      // CSV has no coordinates: retain unmatched engine state as a labelled reference, never attach foreign paths.
      csvMapTime = matchesMap ? null : (csvMapTime ?? simTime);
      routes = parsed;
      selectedRouteId = null;
      playing = false;
      document.getElementById("btnPlay").textContent = "Play";
      simTime = routes.length ? Math.min(Math.max(simTime, routes[0][0]), routes[routes.length - 1][0]) : 0;
      syncTimeBounds(matchesMap ? (resultMeta.replay ? resultMeta.config.simTimeLimit : states[states.length - 1].t) : (routes.length ? routes[routes.length - 1][0] : 0));
      document.getElementById("csvSource").textContent = `선택 CSV: ${file.name} · ${routes.length.toLocaleString("ko-KR")}개 계획 경로`;
      setStatus(matchesMap ? "선택한 CSV가 저장된 실행 결과와 일치합니다. 지도와 경로를 함께 재생합니다." : "선택한 CSV의 경로 표와 통계를 재생합니다. 위치 정보가 없어 지도는 기존 실행의 참고 화면으로 유지합니다.");
    } catch (err) { setStatus(`CSV error: ${err.message}`); }
    render();
  };
  reader.onerror = () => setStatus("CSV 파일을 읽지 못했습니다. 기존 화면을 유지합니다.");
  reader.readAsText(file);
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
let currentEffectiveEntries = [];

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
    return !q || e.k.toLowerCase().includes(q) || String(e.v).toLowerCase().includes(q) || e.sec.toLowerCase().includes(q);
  });
  document.querySelector("#iniTable tbody").innerHTML = rows.map(e => {
    const ov = e.sec !== "General" ? " class=\"ini-override\"" : "";
    return `<tr${ov}><td class="ini-k" title="${escHtml(e.k)}">${escHtml(e.k)}</td><td class="ini-v" title="${escHtml(e.v)}">${escHtml(e.v)}</td><td class="ini-s">[${escHtml(e.sec)}]</td></tr>`;
  }).join("");
  document.getElementById("iniTable").title = "INI preview only; OMNeT++ resolves wildcard precedence and expressions. Engine snapshots are authoritative.";
  document.getElementById("iniCount").textContent = `표시 ${rows.length} / 전체 ${currentEffectiveEntries.length}개 INI 키 (파란 강조 = 상속/시나리오 설정)`;
}

iniFilter.oninput = renderIniTable;
chkShowNodes.onchange = renderIniTable;

function updateProfileBox(cfg, highlight) {
  document.getElementById("savedConfigName").textContent = cfg.activeConfig || "결과를 선택하세요";
  const entries = cfg.effectiveEntries || [];
  const value = key => entries.find(e => e.k === key)?.v;
  document.getElementById("scenarioConstellation").textContent = cfg.numSatellites == null ? "—" : `${cfg.numSatellites}기 · ${cfg.planes}개 궤도면`;
  document.getElementById("scenarioOrbit").textContent = cfg.altitude == null ? "—" : `${cfg.altitude} km · ${cfg.inclination}°`;
  document.getElementById("scenarioDuration").textContent = cfg.simTimeLimit == null ? "—" : `${cfg.simTimeLimit.toLocaleString("ko-KR")}초`;
  const routing = { delay: "지연 최소", hop: "홉 수 최소" };
  document.getElementById("scenarioRouting").textContent = routing[cfg.metric] || cfg.metric || "—";
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
  if (Array.isArray(cfg.configs) && cfg.configs.length) {
    cfgNameSel.innerHTML = "";
    cfg.configs.forEach(c => {
      const opt = new Option(c.name, c.name);
      if (c.description) opt.title = c.description;
      cfgNameSel.add(opt);
    });
    if (cfg.activeConfig) cfgNameSel.value = cfg.activeConfig;
    cfgNameSel.dataset.loaded = "1";
  }

  const configKey = JSON.stringify([cfg.activeConfig, cfg.effectiveEntries]);
  const resultKey = resultMeta ? JSON.stringify([resultMeta.config.activeConfig, resultMeta.config.effectiveEntries]) : "";
  const showingPreviousRun = !!resultMeta && !opts.result && resultKey !== configKey;
  if (showingPreviousRun) {
    setStatus(`지도에는 [${resultMeta.config.activeConfig}]의 마지막 실행 결과가 표시됩니다. 선택한 [${cfg.activeConfig}] 설정을 반영하려면 실행하세요.`);
  }
  loadedConfigKey = configKey;
  if (opts.result || !resultMeta) {
  updateOrbitConfig({ ...cfg, minEl: cfg.minimumElevation });
  if (Array.isArray(cfg.stations) || Array.isArray(cfg.terminals)) populateFilters();
  if (cfg.simTimeLimit && csvMapTime === null) syncTimeBounds(cfg.simTimeLimit);
  }

  const active = resultMeta?.config.activeConfig || cfg.activeConfig || cfgNameSel.value;
  const rShort = (ORBIT_CFG.routingType || "Routing").replace(/^Sat_Net_|Routing$/g, "");
  document.title = `FLoRaSat LEO Simulator (${active})`;
  document.getElementById("topBadge").textContent =
    `${active} · Walker-Delta ${ORBIT_CFG.numSatellites}/${ORBIT_CFG.planes}/${ORBIT_CFG.phasing} · ${ORBIT_CFG.altitude}km · ${ORBIT_CFG.inclination}° · ${rShort} (${ORBIT_CFG.metric})`;

  currentEffectiveEntries = Array.isArray(cfg.effectiveEntries) ? cfg.effectiveEntries : [];
  updateProfileBox(cfg, !!opts.announce);
  renderIniTable();

  if (opts.announce && !showingPreviousRun) {
    setStatus(`📌 [${active}] .ini 로드됨 (직접 설정 ${(cfg.ownEntries || []).length}줄 · preview keys ${currentEffectiveEntries.length}개)`);
  }
  render();
}

updateProfileBox({}, false);
renderIniTable();

if (location.protocol.startsWith("http")) {
  let configRequest = 0;
  const fetchConfig = (cfg, announce = false) => {
    const request = ++configRequest;
    return fetch(cfg ? `/api/config?config=${encodeURIComponent(cfg)}` : "/api/config", { cache: "no-store" })
      .then(r => { if (!r.ok) throw new Error("설정을 불러오지 못했습니다."); return r.json(); })
      .then(d => { if (request === configRequest) applyLoadedConfig(d, { announce }); })
      .catch(err => { if (request === configRequest) setStatus(err.message); });
  };
  if (window.SIM_RESULT) {
    if (window.SIM_RESULT.bundle) openResultBundle(window.SIM_RESULT.bundle).catch(err => setStatus(err.message));
    else installResult(window.SIM_RESULT);
  }
  fetch("/api/result", { cache: "no-store" }).then(r => { if (!r.ok) throw new Error("저장 결과를 불러오지 못했습니다."); return r.json(); })
    .then(async result => { if (configRequest !== 0) return; if (result && result.runId !== resultMeta?.runId) { if (result.bundle) await openResultBundle(result.bundle); else installResult(result); } if (!resultMeta?.replay) return fetchConfig(cfgNameSel.dataset.loaded ? cfgNameSel.value : ""); })
    .catch(err => { setStatus(resultMeta ? "저장된 실행 결과를 표시합니다. 서버 연결을 확인해 주세요." : err.message); });
  cfgNameSel.onchange = () => fetchConfig(cfgNameSel.value, true);
  document.getElementById("btnRefreshIni").onclick = () => fetchConfig(cfgNameSel.value, true);
  window.addEventListener("focus", () => fetchConfig(cfgNameSel.dataset.loaded ? cfgNameSel.value : "", false));
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

document.getElementById("btnRunSim").onclick = async e => {
  const btn = e.target;
  const cfgName = cfgNameSel.value;
  if (!location.protocol.startsWith("http")) {
    return setStatus("⚠️ 시뮬레이션 실행은 sim-ui.exe에서 동작합니다.");
  }
  btn.disabled = true;
  setStatus(`Running [${cfgName}] from captured inputs (original model unchanged)...`);
  try {
    const t0 = performance.now();
    const res = await fetch("/api/run", { method: "POST", body: JSON.stringify({ configName: cfgName }) }).then(r => r.json());
    if (!res.ok) throw new Error(res.error || "Simulation failed");
    if (res.bundle) await openResultBundle(res.bundle); else installResult(res);
    const elapsed = ((performance.now() - t0) / 1000).toFixed(1);
    setStatus(`Completed [${cfgName}] | run ${res.runId} | ${elapsed}s | ${routes.length} planned routes | CSV + INI bundle saved`);
  } catch (err) {
    setStatus(`❌ 실행 오류: ${err.message}`);
  } finally {
    btn.disabled = false;
  }
};

render();

if (location.protocol === "file:") {
  if (window.SIM_RESULT) {
    if (window.SIM_RESULT.bundle) openResultBundle(window.SIM_RESULT.bundle).catch(err => setStatus(err.message));
    else installResult(window.SIM_RESULT);
  }
  else setStatus("No saved engine result. Run a simulation using start_ui.bat or sim-ui.exe first.");
  cfgNameSel.disabled = true;
  cfgNameSel.hidden = true;
  document.getElementById("savedConfigName").hidden = false;
  document.getElementById("cfgNameLabel").textContent = "결과의 실행 구성";
  document.getElementById("scenarioHeading").textContent = "실행 결과";
  document.getElementById("btnRefreshIni").hidden = true;
  document.getElementById("runnerActions").hidden = true;
  document.getElementById("runModeHint").hidden = false;
}

async function openResultBundle(bundle, request = ++resultLoadRequest) {
  const prepared = await prepareBundle(bundle);
  if (request !== resultLoadRequest) return;
  const previous = { ...ORBIT_CFG, stations: STATIONS, terminals: TERMINALS };
  try {
    updateOrbitConfig({ ...prepared.config, minEl: prepared.config.minimumElevation });
    const trial = replaySnapshot(0);
    if (trial.nodes.some(p => !p.every(Number.isFinite))) throw new Error("설정으로 위치를 계산할 수 없습니다.");
  } catch (err) { updateOrbitConfig(previous); throw err; }
  installResult(prepared);
  document.getElementById("csvSource").textContent = `실행 ID: ${prepared.runId} · CSV ${prepared.routes.length.toLocaleString("ko-KR")}개 · 설정 포함`;
  setStatus(bundle.manifest.provenance === "manual-import-unverified" ? "가져온 CSV·INI로 지도를 재현합니다. 수동 등록 자료이므로 실행 당시 설정인지 확인해 주세요." : "실행 당시 보관한 CSV·INI로 지도를 재현합니다.");
}
document.getElementById("resultFolder").onchange = async e => {
  const selection = [...e.target.files]; e.target.value = "";
  if (!selection.length) return;
  const request = ++resultLoadRequest;
  try {
    const fileMap = new Map();
    for (const file of selection) {
      const relative = file.webkitRelativePath.split("/").slice(1).join("/");
      if (relative === "manifest.json" || relative.endsWith(".ini") || relative.endsWith(".csv")) {
        const text = new TextDecoder("utf-8", { fatal: true, ignoreBOM: true }).decode(await file.arrayBuffer());
        fileMap.set(relative, text);
      }
    }
    if (!fileMap.has("manifest.json")) throw new Error("manifest.json이 있는 실행 결과 폴더를 선택하세요.");
    const manifest = JSON.parse(fileMap.get("manifest.json").replace(/^\uFEFF/, ""));
    await openResultBundle({ manifest, files: Object.fromEntries(fileMap) }, request);
  } catch (err) { if (request === resultLoadRequest) setStatus(`결과를 불러오지 못했습니다: ${err.message} 기존 화면을 유지합니다.`); }
};
