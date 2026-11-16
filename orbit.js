// Display configuration only. Positions and links come exclusively from engine snapshots.
const PI = Math.PI, TWOPI = 2 * PI, DEG = PI / 180, RAD = 180 / PI;
let ORBIT_CFG = { altitude: 550, inclination: 53, minEl: 10, numSatellites: 132, planes: 12,
  phasing: 1, raanSpread: 360, baseYear: 20, baseDay: 1.5,
  routingType: "Sat_Net_DijkstraRouting", metric: "delay", maxRoutes: 8, routeLifetime: 30 };
let STATIONS = [], TERMINALS = [];
function updateOrbitConfig(cfg = {}) {
  for (const key of Object.keys(ORBIT_CFG)) if (cfg[key] !== undefined && cfg[key] !== null) ORBIT_CFG[key] = cfg[key];
  if (Array.isArray(cfg.stations)) STATIONS = cfg.stations;
  if (Array.isArray(cfg.terminals)) TERMINALS = cfg.terminals;
}
