# Replay model provenance and validation

orbit.js restores the dynamic replay implementation from this repository's
commit a4eb353. Corrections follow the supplied leo-main's cOrbitA,
cNoradBaseA, cNoradSGP4A, cEci and ConstellationTopologyControl equations.
The libnorad source attributes Michael F. Henry (2002-2003), Aiden Valentine,
and the underlying Space Track Report No. 3 equations. The supplied leo-main
LICENSE.md is LGPLv3; the leosatellites upstream includes GPLv3 material.
The combined delivered program uses GPLv3; retain the original file terms.
This replay must not be presented as an independently licensed replacement engine. Keep applicable copyright/license notices in
redistribution and review source distribution obligations for delivery.

The model is bound to replay-model.json's source fingerprint. Unknown source
versions, TLE/Walker-Star/uneven-plane/IoT models, runtime parameter expressions,
non-zero argument of perigee and per-satellite BStar variations are rejected.
The supported presets are the four Dijkstra ground-node configurations.

Recorded reference: run 20261116T021809Z-a5affc0d, 132 satellites, 13 ground
nodes, 600 simulation seconds. This pre-existing engine output is preserved in
testdata/replay-reference.json for independent tests. It was captured while the
read-only state export existed; that export has since been removed from leo-main.
Its routing/position model for Dijkstra132 is unchanged by that output removal.
During implementation, all 602 recorded samples had identical topology and
maximum satellite coordinate discrepancy about 4mm. The committed small reference
fixture exercises initial, intermediate and final samples (including a duplicate
final sample), with a 1cm coordinate tolerance. This is validation for the supplied
model and these inputs, not proof of equivalence for arbitrary FLoRaSat models.

Topology is updated at the INI interval, while displayed positions are evaluated
at the selected replay time. Measured deliveries and end-to-end statistics are
not reconstructed from planned-route CSV. They remain unavailable unless actual
recorded evidence is supplied.
