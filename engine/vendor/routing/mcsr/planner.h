// Planner: the glue between a simulator's link graph and the MCSR router.
// Built from a snapshot graph (satellite ids = plane*satsPerPlane + slot, edge tuples {D ms, A, J ms})
// it segments the constellation (Alg. 1), and plans a source-routed path from one satellite to the
// best of several exit satellites (each with its own ground-link tuple), per service class.
// No simulator dependencies: used by the OMNeT++ module and by the unit tests.
#pragma once
#include <memory>
#include <vector>

#include "router.h"

namespace mcsr {

struct PlannerConfig {
    int planes = 0;           // NO
    int satsPerPlane = 0;     // NP
    int interPlaneSpacing = 1;  // Walker phasing f (seam shift)
    std::size_t labelCap = 32;
    FloodModel flood = FloodModel::PaperLiteral;
    // Fixed SRD size (K orbits x L slots); 0 => minimise T_Cov (Alg. 1). For unit tests on small grids and for the
    // simulator module to REUSE the (K, L) that Algorithm 1 chose at its first snapshot; the module exposes no way
    // to override it (the SRD composition must follow the paper).
    int fixedK = 0, fixedL = 0;
};

class Planner {
public:
    struct Exit { int sat; PathTuple gsl; };  // exit satellite + tuple of its ground link
    struct Plan {
        bool ok = false;
        DomPath path;                // satellite sequence from `from` to the exit satellite; t = total incl. GSL
        std::size_t numDominant = 0; // size of the dominant set the choice was made from
        bool interSrd = false;
        bool recomputed = true;
        // The whole dominant set the choice was made from (shared, immutable): the reserved candidates a
        // satellite can fall back to after a failure without a new search.
        std::shared_ptr<const std::vector<DomPath>> front;
    };

    // Not copyable/movable: Router keeps references into this object.
    Planner(const Planner&) = delete;
    Planner& operator=(const Planner&) = delete;

    static std::unique_ptr<Planner> create(const PlannerConfig& c, Graph g) {
        return std::unique_ptr<Planner>(new Planner(c, std::move(g)));
    }

    const SrdPlan& srdPlan() const { return plan_; }
    const Graph& graph() const { return g_; }
    double avgInterD() const { return tx_; }
    double avgIntraD() const { return ty_; }
    Router& router() { return *router_; }

    // skipExit: exit satellite to ignore (e.g. its ground link just went down while a packet is on it).
    Plan plan(int from, const std::vector<Exit>& exits, Service sv, const FailureSet* f = nullptr,
              const SelectionParams& sp = {}, int skipExit = -1) {
        Plan out;
        std::vector<DomPath> cand;
        for (const Exit& e : exits) {
            if (e.sat == skipExit) continue;
            if (f && f->blocksNode(e.sat)) continue;
            for (DomPath& p : router_->dominantPaths(from, e.sat, f)) {
                p.t = extend(p.t, e.gsl);
                cand.push_back(std::move(p));
            }
        }
        std::vector<DomPath> front;
        for (size_t i = 0; i < cand.size(); ++i) {
            bool dom = false;
            for (size_t j = 0; j < cand.size() && !dom; ++j) { MCSR_OP(candCompare, 1); dom = i != j && dominates(cand[j].t, cand[i].t); }
            if (!dom) front.push_back(cand[i]);
        }
        out.numDominant = front.size();
        out.front = std::make_shared<const std::vector<DomPath>>(front);
        if (const DomPath* b = selectPath(front, sv, sp)) {
            out.ok = true;
            out.path = *b;
            out.interSrd = !b->sats.empty() && srd_[from] != srd_[b->sats.back()];
        }
        return out;
    }

private:
    Planner(const PlannerConfig& c, Graph g) : g_(std::move(g)) {
        for (int u = 0; u < g_.size(); ++u) MCSR_OP(segmentEdgeScan, g_.adj[u].size());
        if (!(c.fixedK > 0 && c.fixedL > 0)) MCSR_OP(segmentCandidates, c.planes * c.satsPerPlane);   // Alg. 1 grid search
        const int NO = c.planes, NP = c.satsPerPlane, N = NO * NP;
        double si = 0, sx = 0;
        long ci = 0, cx = 0;
        for (int u = 0; u < g_.size(); ++u)
            for (const Edge& e : g_.adj[u]) {
                if (e.to < u) continue;
                if (u / NP == e.to / NP) { si += e.t.D; ++ci; }
                else { sx += e.t.D; ++cx; }
            }
        ty_ = ci ? si / ci : 1.0;
        tx_ = cx ? sx / cx : 1.0;
        if (c.fixedK > 0 && c.fixedL > 0) plan_ = makePlan(NO, NP, c.fixedK, c.fixedL);
        else plan_ = segment(NO, NP, tx_, ty_, c.flood);
        srd_ = plan_.srdOf;
        Router::GridNeighbors nb(N);
        for (int u = 0; u < N; ++u) {
            const int q = u / NP, x = u % NP;
            nb[u] = {NP * q + (x + 1) % NP, NP * q + (x + NP - 1) % NP,
                     q + 1 < NO ? NP * (q + 1) + x : (x + c.interPlaneSpacing) % NP,
                     q - 1 >= 0 ? NP * (q - 1) + x : NP * (NO - 1) + ((x - c.interPlaneSpacing) % NP + NP) % NP};
        }
        router_ = std::make_unique<Router>(std::move(nb), g_, plan_, c.labelCap);
    }

    Graph g_;
    SrdPlan plan_;
    std::vector<int> srd_;
    double tx_ = 1.0, ty_ = 1.0;
    std::unique_ptr<Router> router_;
};

}  // namespace mcsr
