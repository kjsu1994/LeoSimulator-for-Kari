// Dominant-path-set engine: multi-criteria label-setting search over the routing algebra
// (D,+) x (A,min) x (J,max). Because extend() is isotone w.r.t. dominance (Def. 1, property 1),
// discarding dominated labels never loses a dominant path (Theorem 2).
#pragma once
#include <functional>
#include <queue>
#include <vector>

#include "graph.h"

namespace mcsr {

struct Label {
    PathTuple t;
    int node;
    int prev;  // previous label index in pool (-1 for source)
    int ex;    // Edge::ex of the edge used to reach this label
    bool alive;
};

struct ParetoResult {
    std::vector<Label> pool;
    std::vector<std::vector<int>> at;  // alive label indices per node = dominant path set P

    // Reconstructs node sequence (src..node) and the per-hop Edge::ex values (size nodes-1).
    void trace(int labelIdx, std::vector<int>& nodes, std::vector<int>& exs) const {
        nodes.clear();
        exs.clear();
        MCSR_OP(pathTraces, 1);
        for (int i = labelIdx; i >= 0; i = pool[i].prev) {
            nodes.push_back(pool[i].node);
            if (pool[i].prev >= 0) exs.push_back(pool[i].ex);
        }
        MCSR_OP(pathNodes, nodes.size());
        std::reverse(nodes.begin(), nodes.end());
        std::reverse(exs.begin(), exs.end());
    }
};

struct EngineOptions {
    // Safety valve for pathological Pareto-front growth. 256 is effectively unbounded for
    // 3 attributes on grid-like LEO topologies; lower it for on-board-resource experiments.
    std::size_t maxLabelsPerNode = 256;
    std::function<bool(int, int)> edgeOk;  // optional edge filter (u,v)
    std::function<bool(int)> nodeOk;       // optional node filter
};

inline ParetoResult paretoSearch(const Graph& g, int src, const EngineOptions& o = {}) {
    ParetoResult r;
    r.at.assign(g.size(), {});
    r.pool.push_back({PathTuple{}, src, -1, -1, true});
    r.at[src].push_back(0);
    using QE = std::pair<double, int>;
    std::priority_queue<QE, std::vector<QE>, std::greater<QE>> pq;
    pq.push({0.0, 0});
    MCSR_OP(searches, 1);
    MCSR_OP(pqPush, 1);
    while (!pq.empty()) {
        int li = pq.top().second;
        pq.pop();
        MCSR_OP(pqPop, 1);
        if (!r.pool[li].alive) continue;
        MCSR_OP(labelExpand, 1);
        const Label cur = r.pool[li];
        for (const Edge& e : g.adj[cur.node]) {
            MCSR_OP(edgeChecks, 1);
            if (o.edgeOk && !o.edgeOk(cur.node, e.to)) continue;
            if (o.nodeOk && !o.nodeOk(e.to)) continue;
            const PathTuple nt = extend(cur.t, e.t);
            auto& fr = r.at[e.to];
            bool covered = false;
            for (int k : fr) {
                MCSR_OP(dominanceCmp, 1);
                if (weaklyDominates(r.pool[k].t, nt)) { covered = true; break; }
            }
            if (covered) continue;
            fr.erase(std::remove_if(fr.begin(), fr.end(),
                                    [&](int k) {
                                        MCSR_OP(dominanceCmp, 1);
                                        if (dominates(nt, r.pool[k].t)) { r.pool[k].alive = false; MCSR_OP(labelDelete, 1); return true; }
                                        return false;
                                    }),
                     fr.end());
            if (fr.size() >= o.maxLabelsPerNode) {
                // Front is full. Keep the three per-node extremes exact (min D, max A, min J):
                // they are what LS / TS / HR selection and Dijkstra-style optimality need, and the
                // extremes propagate correctly through the isotone extension. A new label may
                // only enter by evicting the largest-D non-extremal label.
                int minD = fr[0], maxA = fr[0], minJ = fr[0];
                for (int k : fr) {
                    if (r.pool[k].t.D < r.pool[minD].t.D) minD = k;
                    if (r.pool[k].t.A > r.pool[maxA].t.A) maxA = k;
                    if (r.pool[k].t.J < r.pool[minJ].t.J) minJ = k;
                }
                const bool extremal = nt.D < r.pool[minD].t.D || nt.A > r.pool[maxA].t.A || nt.J < r.pool[minJ].t.J;
                if (!extremal) { MCSR_OP(capReject, 1); continue; }   // front full, label discarded (counts as a cap hit)
                int victim = -1;
                for (int k : fr)
                    if (k != minD && k != maxA && k != minJ && (victim < 0 || r.pool[k].t.D > r.pool[victim].t.D)) victim = k;
                if (victim < 0) { MCSR_OP(capReject, 1); continue; }
                r.pool[victim].alive = false;
                MCSR_OP(labelDelete, 1);
                MCSR_OP(capEvict, 1);
                fr.erase(std::find(fr.begin(), fr.end(), victim));
            }
            int ni = static_cast<int>(r.pool.size());
            r.pool.push_back({nt, e.to, li, e.ex, true});
            fr.push_back(ni);
            MCSR_OP(labelInsert, 1);
            MCSR_OP(pqPush, 1);
            pq.push({nt.D, ni});
        }
    }
    return r;
}

}  // namespace mcsr
