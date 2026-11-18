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

const vm=require("node:vm");
const eventParser=fs.readFileSync(`${__dirname}/editor.js`,"utf8").match(/function readEventCsv\(text\)\{[^\n]+/)[0];
const eventInput="\uFEFFtime,event,satA,satB\r\n0.1,down,0,1\r\n0.2,up,0,1\r\n";
const events=vm.runInNewContext(eventParser+`;readEventCsv(${JSON.stringify(eventInput)})`);
assert.equal(events.length,2);assert.equal(events[0].event,"failure");assert.equal(events[1].event,"restore");
console.log("Scheduled-link CSV passed: BOM, CRLF and legacy event names");
