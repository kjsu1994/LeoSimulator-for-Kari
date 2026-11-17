// Paper-literal dominant-path calculation (Algorithm 3, intra-SRD): round-based wavefront over the sets
//   R = calculated satellites, F = satellites to expand in this round, T = satellites not yet calculated.
// Each round, every satellite Vi in F extends the tuples of ALL its stored dominant paths over the ISLs to its
// adjacent satellites, performs the multi-attribute consistency check (Def. 1) against what the neighbour already
// holds (this is also the "duplicate adjacent satellites in F" check of lines 7-14), then F moves to R and the
// neighbours that received a new path form the next F.
//
// The pseudo-code only lets satellites "not in R" be reached. Taken literally (reexpand = false) a satellite is never
// revisited, so a lower-delay path with more hops that reaches an already calculated satellite later is lost, and
// the result is NOT the dominant path set that Theorem 2 claims. The complexity analysis (Sec. IV-B) says that when
// new paths are added to an already calculated node its neighbours are recalculated (worst case: all r nodes); that is
// the reading used by reexpand = true: a node in R that receives a new non-dominated path goes back to F.
// reexpand = true converges to the complete dominant set (it is a synchronous label-correcting search);
// reexpand = false is kept to measure what the literal pseudo-code would miss.
// No label cap: the paper has none.
#pragma once
#include <functional>
#include <vector>

#include "pareto.h"

namespace mcsr {

struct WaveOptions {
    bool reexpand = true;
    std::function<bool(int, int)> edgeOk;  // optional edge filter (u,v)
    std::function<bool(int)> nodeOk;       // optional node filter
};

inline ParetoResult waveSearch(const Graph& g, int src, const WaveOptions& o = {}) {
    ParetoResult r;
    const int n = g.size();
    r.at.assign(n, {});
    r.pool.push_back({PathTuple{}, src, -1, -1, true});
    r.at[src].push_back(0);
    std::vector<char> inR(n, 0);
    std::vector<std::vector<int>> pending(n);  // labels created but not yet extended, per node
    pending[src].push_back(0);
    std::vector<int> F{src};
    MCSR_OP(searches, 1);
    while (!F.empty()) {
        MCSR_OP(waveRounds, 1);
        std::vector<std::vector<int>> work(F.size());  // snapshot: a round is synchronous
        for (size_t i = 0; i < F.size(); ++i) work[i].swap(pending[F[i]]);
        std::vector<int> next;
        std::vector<char> inNext(n, 0);
        for (size_t i = 0; i < F.size(); ++i) {
            const int vi = F[i];
            for (int li : work[i]) {
                if (!r.pool[li].alive) continue;
                MCSR_OP(labelExpand, 1);
                const Label cur = r.pool[li];
                for (const Edge& e : g.adj[vi]) {
                    MCSR_OP(edgeChecks, 1);
                    if (!o.reexpand && inR[e.to]) continue;  // Alg. 3 line 4/16: only satellites not in R
                    if (o.edgeOk && !o.edgeOk(vi, e.to)) continue;
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
                    const int ni = static_cast<int>(r.pool.size());
                    r.pool.push_back({nt, e.to, li, e.ex, true});
                    fr.push_back(ni);
                    MCSR_OP(labelInsert, 1);
                    pending[e.to].push_back(ni);
                    if (!inNext[e.to]) { inNext[e.to] = 1; next.push_back(e.to); }
                }
            }
        }
        for (int v : F) inR[v] = 1;  // F -> R
        std::vector<int> nf;
        for (int v : next) {
            if (inR[v]) {
                if (!o.reexpand) continue;  // Alg. 3 line 16 adds only satellites not (yet) in R, F was just moved to R
                inR[v] = 0;                 // new dominant path at a calculated node: calculate its neighbours again
                MCSR_OP(waveReexpand, 1);
            }
            nf.push_back(v);
        }
        F.swap(nf);
    }
    return r;
}

}  // namespace mcsr
