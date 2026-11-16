// The engine emits unquoted numeric fields and GS/T identifiers.
function parseRoutesCsv(text) {
  const lines = text.replace(/^\uFEFF/, "").trim().split(/\r?\n/);
  if (lines.shift() !== "time,route,source,station,firstSat,exitSat,satHops,uplinkMs,satToStationMs,totalMs,path") throw new Error("Unsupported CSV header");
  let previous = -1;
  return lines.filter(line => line.trim()).map((line, index) => {
    const r = line.split(",");
    if (r.length !== 11 || !/^(GS|T)\d+$/.test(r[2])) throw new Error(`Invalid row ${index + 2}`);
    const numeric = [0,1,3,4,5,6,7,8,9];
    if (numeric.some(i => !r[i].trim() || !Number.isFinite(Number(r[i])) || Number(r[i]) < 0)) throw new Error(`Invalid number at row ${index + 2}`);
    if ([1,3,4,5,6].some(i => !Number.isInteger(Number(r[i])))) throw new Error("Invalid integer or path");
    if (!/^\d+(?:-\d+)*$/.test(r[10])) throw new Error("Invalid integer or path");
    const path = r[10].split("-").map(Number), t = Number(r[0]);
    if (t < previous || path[0] !== Number(r[4]) || path[path.length-1] !== Number(r[5]) || path.length-1 !== Number(r[6])) throw new Error("Unsorted time or inconsistent path");
    previous = t;
    return [t, Number(r[1]), Number(r[2].replace(/^[A-Z]+/, "")), Number(r[3]), Number(r[6]), Number(r[9]), path, r[2].startsWith("T") ? 1 : 0, Number(r[4]), Number(r[5]), Number(r[7]), Number(r[8])];
  });
}
if (typeof module !== "undefined") module.exports = { parseRoutesCsv };
