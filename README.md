# FLoRaSat simulation UI

This UI runs the supplied sibling `../leo-main` project. The supported entry point is
`simulations/Dijkstra132/omnetpp.ini`, not every FLoRaSat scenario.

## Start

Run `start_ui.bat` (Python 3; optional `pywebview`) or the rebuilt `sim-ui.exe`.
Keep the HTML, JS, CSS, Dockerfile and data directory next to the executable.
Simulation execution requires WSL with Docker. The server listens on localhost:8090.

## What the screen means

- CSV rows are **planned paths recorded at the first satellite**, not delivery receipts.
- CSV delays are predicted propagation delays, not measured end-to-end latency.
- All eleven CSV columns are retained in the parsed data. Hover the table delay for
  segment details, or download the unchanged CSV using the run link.
- Satellite, station and terminal positions, ISLs and ground links come from the
  engine's existing map interface. The duplicate JavaScript orbit/mobility model
  has been removed.
- Replay displays the latest engine sample at or before the requested time, without
  interpolation. Sampling is 1 simulation second plus a final sample. The displayed
  snapshot time makes the sampling interval explicit.
- Actual sent/received counts and mean measured end-to-end delay on the map come
  from Station traffic modules, including ship/aircraft traffic. They are sampled
  cumulative counters, not inferred from CSV. They do not represent IoT statistics.
- Coverage circles are geometric illustrations, not an RF success prediction.
- External/legacy CSV has no trustworthy matching settings or coordinates: only
  its planned route table is displayed, without invented positions or links.
- The INI table is a preview. OMNeT++ itself resolves wildcard priority, expressions
  and full configuration semantics; the table is not a second simulation engine.

## Reproducible runs

Every execution captures source/model/config inputs before building. A content hash
selects the Docker image, so editing C++ or included INIs cannot silently reuse an
older image. Docker's layer cache reuses installed OMNeT++/INET dependencies.

`data/runs/<run-id>/` contains:
- `inputs/`: exact build input tree and Dockerfile
- `results/`: original CSV, actual state JSONL, engine log and scalar/vector files
- `result.json`: selected config, input hash, CSV hash, Docker image, time and output overrides

Only output settings are overridden: routes CSV path, optional state JSONL path and
1-second state sampling. Routing, traffic, random seeds and simulation duration
come from the captured input configuration. The supplied project's existing result
files and legacy UI CSV are never overwritten. Failed runs retain available logs;
the last successful result pointer is updated only after parsing succeeds.

Changing scenarios or relevant INI settings preserves the last recorded map.
The selected configuration and the displayed run are identified separately, with
a notice when they differ. Only a successful new run replaces the displayed map.
Successful runs reload from `data/latest.json` when the UI restarts. Saved run files
are local artifacts, not a hosted service. No automatic retention/deletion is applied.

## Changes to leo-main

`FloraMapDataSource.{h,cc,ned}` adds an optional `stateLogFile` output, disabled by
default. It samples the existing node/link interface and traffic getters; it does
not replace routing or mobility. `Dijkstra132Hop` now actually sets `metric="hop"`.
The other original simulation algorithms and the existing CSV format are retained.

## Checks and packaging

```
python -m unittest discover -s . -p test_server.py -v
node test_csv.js
node test_browser.js
python -m PyInstaller --noconfirm --onefile --windowed --name sim-ui --distpath . server.py
```

The browser regression check uses installed Chrome and Node's built-in WebSocket,
a temporary browser profile, and localhost ports 8091/9227. It expects the verified
600-second Dijkstra132 result to be available as the latest successful run.

Verified in this workspace: real Docker C++ build and 600-second Dijkstra132 run;
2,671 planned rows; CSV SHA-256 unchanged from the original result; 602 engine
snapshots; final Station sent/received counts both 2,671, matching scalar output.
Python CSV/input tests and headless Chrome replay/invalidation/import checks pass.

## Open HTML directly

Opening index.html with file:// replays the last successful engine result from
`data/latest_result.js`, including positions, links, routes and hover information.
This file is regenerated after every successful run. Keep it with the HTML assets.
Offline playback starts at min(1210 seconds, run end) so recent routes are visible.
Scenario refresh and execution require the local server; offline playback uses the
configuration captured with the saved run.

The map status information is rendered in a separate dark header, outside the canvas.
The layout uses four metrics on wide screens, two columns on medium screens and a
stacked map/sidebar on small screens. Map hover coordinates account for letterboxing.
The offline browser regression uses a temporary local copy because Chrome cannot
read the mapped O: drive in this automated execution context.
