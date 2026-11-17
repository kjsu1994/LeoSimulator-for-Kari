// SPDX-License-Identifier: GPL-3.0-only
// Modified/adapted 2026-11-16; see THIRD_PARTY_NOTICES.txt.
const assert = require("node:assert/strict");
const fs = require("node:fs");
const { parseRoutesCsv } = require("./csv.js");
const raw = fs.readFileSync(`${__dirname}/testdata/bundle/routes.csv`, "utf8");
const rows = parseRoutesCsv(raw);
assert.equal(rows.length, raw.trim().split(/\r?\n/).length - 1);
assert(rows.every(r => r.length === 12 && r[4] === r[6].length - 1));
const h = raw.split(/\r?\n/)[0] + "\n";
for (const bad of ["bad", h + "NaN,1,GS0,2,3,4,1,2,3,5,3-4", h + "1,1,GS0,2,3,4,1,2,3,5,3-5"]) assert.throws(() => parseRoutesCsv(bad));
assert.deepEqual(parseRoutesCsv(h), []);
console.log(`CSV validation passed: ${rows.length} rows, all 11 original columns retained`);
