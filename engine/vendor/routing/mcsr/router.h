// MCSR router: intra-SRD (Alg. 3) and inter-SRD (Alg. 2) dominant paths, service-aware selection,
// DS/CSIB handling, GS access (gamma factor) and resilient rerouting (Sec. V).
#pragma once
#include <deque>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "pareto.h"
#include "resilience.h"
#include "segmentation.h"
#include "wave.h"

namespace mcsr {

struct DomPath {
    PathTuple t;
    std::vector<int> sats;
};

struct FailureSet {
    std::unordered_set<LinkKey> links;
    std::unordered_set<int> nodes;
    bool empty() const { return links.empty() && nodes.empty(); }
    bool blocksNode(int v) const { return nodes.count(v) > 0; }
    bool blocksLink(int u, int v) const { return links.count(linkKey(u, v)) > 0 || blocksNode(u) || blocksNode(v); }
    bool blocksPath(const std::vector<int>& p) const {
        for (size_t i = 0; i < p.size(); ++i) {
            if (blocksNode(p[i])) return true;
            if (i && blocksLink(p[i - 1], p[i])) return true;
        }
        return false;
    }
};

struct SelectionParams {
    double hrJitterMs = 15.0;      // HR: J < 15 ms [43]
    double tsDelayBoundMs = kInf;  // TS: "delay within acceptable limits"
};

// Optimal path selection from a dominant set (Alg. 2 lines 18-28 / Sec. III-C-3).
inline const DomPath* selectPath(const std::vector<DomPath>& s, Service sv, const SelectionParams& sp = {}) {
    if (s.empty()) return nullptr;
    MCSR_OP(selectChecks, s.size());
    auto better = [&](const DomPath* a, const DomPath* b, auto lex) { return !b || lex(a->t, b->t); };
    const DomPath* best = nullptr;
    if (sv == Service::LS) {  // min D, then max A, then min J
        for (const auto& p : s)
            if (better(&p, best, [](const PathTuple& a, const PathTuple& b) {
                    if (a.D != b.D) return a.D < b.D;
                    if (a.A != b.A) return a.A > b.A;
                    return a.J < b.J; }))
                best = &p;
    } else if (sv == Service::TS) {  // max A subject to D bound, then min D
        for (int pass = 0; pass < 2 && !best; ++pass)
            for (const auto& p : s) {
                if (pass == 0 && p.t.D > sp.tsDelayBoundMs) continue;
                if (better(&p, best, [](const PathTuple& a, const PathTuple& b) {
                        if (a.A != b.A) return a.A > b.A;
                        return a.D < b.D; }))
                    best = &p;
            }
    } else {  // HR: J < threshold, then max A (then min D); if none qualifies, min J
        for (const auto& p : s)
            if (p.t.J < sp.hrJitterMs &&
                better(&p, best, [](const PathTuple& a, const PathTuple& b) {
                    if (a.A != b.A) return a.A > b.A;
                    return a.D < b.D; }))
                best = &p;
        if (!best)
            for (const auto& p : s)
                if (better(&p, best, [](const PathTuple& a, const PathTuple& b) {
                        if (a.J != b.J) return a.J < b.J;
                        return a.A > b.A; }))
                    best = &p;
    }
    return best;
}

struct RouteResult {
    bool ok = false;
    DomPath best;
    std::vector<DomPath> dominant;
    bool recomputed = true;  // false => served from reserved dominant paths (no recalculation)
    bool interSrd = false;
};

struct AccessCandidate { int sat; double slantKm; };

// How the dominant path sets are searched.
//  ParetoLabel      priority-queue label search with an optional per-node label cap (the original implementation)
//  PaperWave        paper Algorithms 2 and 3: R/F/T wavefront, new paths at calculated nodes are re-expanded (complete set)
//  PaperWaveLiteral the same wavefront with the pseudo-code taken literally (calculated nodes never reached again)
enum class SearchAlgorithm { ParetoLabel, PaperWave, PaperWaveLiteral };

// Measurement only (never a module option): which labels the multi-attribute consistency check of the inter-SRD wavefront
// compares with each other.
//  Port      labels that entered an SRD through the same entrance satellite (the implementation's reading; complete set)
//  PortType  labels that crossed from the same neighbour SRD Si into Sj ("ISLs of the same port type", Alg. 2 line 6 / Step 1 read literally)
//  Srd       all labels that entered the same SRD
enum class ConsistencyGroup { Port, PortType, Srd };

class Router {
public:
    // gridNbrs[u] = the four logical +grid neighbours of satellite u (static; independent of which
    // ISLs are currently up). Satellite ids follow V(Qm,Xn) = NP*Qm + Xn.
    using GridNeighbors = std::vector<std::vector<int>>;
    Router(GridNeighbors gridNbrs, const Graph& g, const SrdPlan& plan, std::size_t labelCap = 32)
        : g_(g), plan_(plan), cap_(labelCap) {
        const int N = static_cast<int>(gridNbrs.size());
        isBoundary_.assign(N, 0);
        nbrSrds_.assign(N, {});
        gamma_.assign(N, 0);
        boundaryOf_.resize(plan.r);
        for (int u = 0; u < N; ++u) {
            for (int nb : gridNbrs[u]) {
                if (plan.srdOf[nb] == plan.srdOf[u]) ++gamma_[u];
                else {
                    isBoundary_[u] = 1;
                    auto& v = nbrSrds_[u];   // static: stays valid when ISLs/satellites fail
                    if (std::find(v.begin(), v.end(), plan.srdOf[nb]) == v.end()) v.push_back(plan.srdOf[nb]);
                }
            }
            if (isBoundary_[u]) boundaryOf_[plan.srdOf[u]].push_back(u);
        }
    }

    // Selecting a paper algorithm drops the cached tables. The wavefront has no label cap (the paper has none).
    void setAlgorithm(SearchAlgorithm a) { algo_ = a; base_ = Cache{}; }
    // Measurement only: granularity of the consistency check in the wavefront (default Port = the complete result).
    void setConsistencyGroup(ConsistencyGroup g) { group_ = g; }
    SearchAlgorithm algorithm() const { return algo_; }

    // Tuple of an explicit satellite sequence on the current graph (used to rate a suffix of a stored path).
    PathTuple pathTupleOf(const std::vector<int>& p) const { return pathTuple(p); }
    const SrdPlan& plan() const { return plan_; }
    bool isBoundary(int sat) const { return isBoundary_[sat] != 0; }
    const std::vector<int>& boundarySatellites(int srd) const { return boundaryOf_[srd]; }
    int gamma(int sat) const { return gamma_[sat]; }
    // SRDs (other than its own) that hold a +grid neighbour of `sat`; from the static grid, so it does not depend on live ISLs.
    const std::vector<int>& neighbourSrds(int sat) const { return nbrSrds_[sat]; }

    // ---- Dominant path set P(s -> d) -------------------------------------------------------
    std::vector<DomPath> dominantPaths(int s, int d, const FailureSet* f = nullptr) {
        static const FailureSet none;
        if (!f || f->empty()) return dominant(s, d, base_, none);
        Cache tmp = base_;  // reuse everything not touched by the failure
        MCSR_OP(cacheCopyEntries, base_.local.size() + base_.tables.size() + base_.rtables.size() + base_.vedges.size());
        invalidate(tmp, *f);
        return dominant(s, d, tmp, *f);
    }

    RouteResult route(int s, int d, Service sv, const FailureSet* f = nullptr, const SelectionParams& sp = {}) {
        RouteResult r;
        r.dominant = dominantPaths(s, d, f);
        r.interSrd = plan_.srdOf[s] != plan_.srdOf[d];
        if (const DomPath* b = selectPath(r.dominant, sv, sp)) { r.ok = true; r.best = *b; }
        return r;
    }

    // Resilient rerouting (Sec. V-A): fall back to a reserved dominant path if one survives the
    // failure (no recalculation); otherwise recalculate on the failed topology.
    RouteResult reroute(const RouteResult& prev, int s, int d, Service sv, const FailureSet& f,
                        const SelectionParams& sp = {}, bool forceRecompute = false) {
        if (!forceRecompute) {
            RouteResult r;
            r.interSrd = prev.interSrd;
            r.recomputed = false;
            MCSR_OP(reserveChecks, prev.dominant.size());
            for (const auto& p : prev.dominant)
                if (!f.blocksPath(p.sats)) r.dominant.push_back(p);
            if (const DomPath* b = selectPath(r.dominant, sv, sp)) { r.ok = true; r.best = *b; MCSR_OP(reserveSwitches, 1); return r; }
        }
        MCSR_OP(recomputes, 1);
        RouteResult r = route(s, d, sv, &f, sp);
        r.recomputed = true;
        return r;
    }

    // ---- Failure handling (Sec. V-C-2) ------------------------------------------------------
    UpdateScope classifyLinkFailure(int u, int v) const {
        if (plan_.srdOf[u] != plan_.srdOf[v]) return UpdateScope::InterSrdCsib;  // case 4
        const bool bu = isBoundary_[u], bv = isBoundary_[v];
        if (!bu && !bv) return UpdateScope::IntraSrdLocal;       // case 1
        if (bu && bv) return UpdateScope::SrdBoundaryOnly;       // case 3
        return UpdateScope::WholeSrdWithBoundary;                // case 2
    }
    // Default DS = V(Qmax,Xmax); the other three SRD corners are backups (Sec. II-C).
    std::vector<int> dsCandidates(int srd) const {
        const SrdRect& r = plan_.rect[srd];
        std::vector<int> c;
        for (auto [q, x] : {std::pair{r.qhi, r.xhi}, {r.qhi, r.xlo}, {r.qlo, r.xlo}, {r.qlo, r.xhi}}) {
            int id = plan_.NP * q + x;
            if (std::find(c.begin(), c.end(), id) == c.end()) c.push_back(id);
        }
        return c;
    }
    int electDs(int srd, const FailureSet& f) const {
        for (int c : dsCandidates(srd))
            if (!f.blocksNode(c)) return c;
        for (int b : boundaryOf_[srd])
            if (!f.blocksNode(b)) return b;
        return -1;
    }
    // SRD-BCM vote (Sec. II-C: "each boundary satellite votes to select a DS ... the optimal DS is selected based on
    // the inter-SRD ISL attribute tuples"). The paper gives no formula, so this is an INTERPRETATION:
    //  * voters and candidates are the live boundary satellites of the SRD;
    //  * a voter rates a candidate by the best dominant intra-SRD path voter -> candidate (smallest D, then larger A,
    //    then smaller J) - this is the path along which the voter's inter-SRD ISL information reaches the DS;
    //  * every voter votes for its best OTHER candidate (a satellite would otherwise always vote for itself);
    //  * the plurality wins; ties go to the smallest sum of D over all voters, then to the corner order of
    //    dsCandidates() (the initial upper-right rule), then to the lower id.
    // `votes` (optional) receives the candidates followed by their vote counts.
    int voteDs(int srd, const FailureSet* f = nullptr, std::vector<int>* votes = nullptr) {
        static const FailureSet none;
        const FailureSet& fs = f ? *f : none;
        Cache tmp;
        Cache* c = &base_;
        if (!fs.empty()) { tmp = base_; invalidate(tmp, fs); c = &tmp; }
        std::vector<int> cand;
        for (int b : boundaryOf_[srd]) if (!fs.blocksNode(b)) cand.push_back(b);
        if (cand.empty()) return -1;
        if (cand.size() == 1) return cand[0];
        const int nc = int(cand.size());
        std::vector<int> cnt(nc, 0);
        std::vector<double> sumD(nc, 0.0);
        Local& L = getLocal(srd, *c, fs);
        for (int vi = 0; vi < nc; ++vi) {
            ParetoResult& pr = getTable(cand[vi], *c, fs);
            int best = -1;
            PathTuple bt;
            for (int j = 0; j < nc; ++j) {
                PathTuple t{kInf, 0.0, kInf};
                for (int li : pr.at[L.localOf.at(cand[j])]) {
                    const PathTuple& x = pr.pool[li].t;
                    if (x.D < t.D - 1e-9 || (std::fabs(x.D - t.D) <= 1e-9 && (x.A > t.A || (x.A == t.A && x.J < t.J)))) t = x;
                }
                sumD[j] += (t.D >= kInf ? 1e9 : t.D);
                if (j == vi) continue;
                if (best < 0 || t.D < bt.D - 1e-9 || (std::fabs(t.D - bt.D) <= 1e-9 && (t.A > bt.A || (t.A == bt.A && t.J < bt.J)))) { best = j; bt = t; }
            }
            if (best >= 0) ++cnt[best];
        }
        const std::vector<int> corner = dsCandidates(srd);
        auto cornerRank = [&](int id) { auto it = std::find(corner.begin(), corner.end(), id); return int(it - corner.begin()); };
        int win = 0;
        for (int j = 1; j < nc; ++j) {
            bool better = cnt[j] > cnt[win] ||
                (cnt[j] == cnt[win] && (sumD[j] < sumD[win] - 1e-9 ||
                 (std::fabs(sumD[j] - sumD[win]) <= 1e-9 && (cornerRank(cand[j]) < cornerRank(cand[win]) ||
                  (cornerRank(cand[j]) == cornerRank(cand[win]) && cand[j] < cand[win])))));
            if (better) win = j;
        }
        if (votes) { votes->assign(cand.begin(), cand.end()); votes->insert(votes->end(), cnt.begin(), cnt.end()); }
        return cand[win];
    }
    NodeFailureAction classifyNodeFailure(int v) const {
        const int k = plan_.srdOf[v];
        if (v == electDs(k, FailureSet{})) return NodeFailureAction::SwitchToBackupDs;  // scenario 3
        return isBoundary_[v] ? NodeFailureAction::AnnounceAndDsRecalcCsib              // scenario 2
                              : NodeFailureAction::RecalcIntraSrd;                      // scenario 1
    }

    // ---- GS access strategy (Sec. II-E-2) ---------------------------------------------------
    int selectAccessSatellite(const std::vector<AccessCandidate>& c) const {
        int best = -1;
        for (const auto& a : c) {
            if (best < 0) { best = a.sat; bestRng_ = a.slantKm; continue; }
            const int ga = gamma_[a.sat], gb = gamma_[best];
            const bool aStable = ga == 4, bStable = gb == 4;
            bool take;
            if (aStable != bStable) take = aStable;
            else if (aStable) take = a.slantKm < bestRng_;
            else take = ga > gb || (ga == gb && a.slantKm < bestRng_);
            if (take) { best = a.sat; bestRng_ = a.slantKm; }
        }
        return best;
    }

private:
    struct Local {
        Graph g;
        std::vector<int> globalOf;
        std::unordered_map<int, int> localOf;
    };
    struct VEdge { int u, v; PathTuple t; std::vector<int> path; };
    struct Cache {
        std::unordered_map<int, Local> local;
        std::unordered_map<int, ParetoResult> tables;   // key: global source satellite (forward edges)
        std::unordered_map<int, ParetoResult> rtables;  // key: global destination satellite (reversed edges)
        std::unordered_map<int, std::vector<VEdge>> vedges;  // key: SRD (the "CSIB" boundary part)
    };

    void invalidate(Cache& c, const FailureSet& f) const {
        std::set<int> bad;
        for (int v : f.nodes) bad.insert(plan_.srdOf[v]);
        for (LinkKey k : f.links) {
            bad.insert(plan_.srdOf[int(k >> 32)]);
            bad.insert(plan_.srdOf[int(k & 0xffffffffu)]);
        }
        for (int k : bad) { c.local.erase(k); c.vedges.erase(k); }
        for (auto it = c.tables.begin(); it != c.tables.end();)
            it = bad.count(plan_.srdOf[it->first]) ? c.tables.erase(it) : std::next(it);
        for (auto it = c.rtables.begin(); it != c.rtables.end();)
            it = bad.count(plan_.srdOf[it->first]) ? c.rtables.erase(it) : std::next(it);
    }

    PathTuple linkTuple(int u, int v) const {
        std::size_t scanned = 0;
        for (const Edge& e : g_.adj[u]) { ++scanned; if (e.to == v) { MCSR_OP(linkLookups, scanned); return e.t; } }
        MCSR_OP(linkLookups, scanned);
        return PathTuple{kInf, 0.0, kInf};  // link absent (e.g. closed by handover)
    }
    PathTuple pathTuple(const std::vector<int>& p) const {
        PathTuple t;
        for (size_t i = 1; i < p.size(); ++i) t = extend(t, linkTuple(p[i - 1], p[i]));
        return t;
    }

    Local& getLocal(int k, Cache& c, const FailureSet& f) {
        auto it = c.local.find(k);
        if (it != c.local.end()) return it->second;
        Local L;
        MCSR_OP(localBuilt, 1);
        for (int u : plan_.members[k])
            if (!f.blocksNode(u)) { L.localOf[u] = int(L.globalOf.size()); L.globalOf.push_back(u); }
        L.g = Graph(int(L.globalOf.size()));
        for (int u : L.globalOf)
            for (const Edge& e : g_.adj[u]) {
                auto jt = L.localOf.find(e.to);
                if (jt == L.localOf.end() || f.blocksLink(u, e.to)) continue;
                L.g.addDirected(L.localOf[u], jt->second, e.t);
                MCSR_OP(graphEdges, 1);
            }
        return c.local.emplace(k, std::move(L)).first->second;
    }

    ParetoResult runSearch(const Graph& g, int src) const {
        if (algo_ == SearchAlgorithm::ParetoLabel) {
            EngineOptions o;
            o.maxLabelsPerNode = cap_;
            return paretoSearch(g, src, o);
        }
        WaveOptions w;
        w.reexpand = algo_ == SearchAlgorithm::PaperWave;
        return waveSearch(g, src, w);
    }

    ParetoResult& getTable(int src, Cache& c, const FailureSet& f) {
        auto it = c.tables.find(src);
        if (it != c.tables.end()) { MCSR_OP(tableHit, 1); return it->second; }
        MCSR_OP(tableMiss, 1);
        Local& L = getLocal(plan_.srdOf[src], c, f);
        return c.tables.emplace(src, runSearch(L.g, L.localOf.at(src))).first->second;
    }

    // Dominant paths INTO `dst` inside its SRD: a Pareto search from dst over the reversed edges. Links
    // may be asymmetric (the available buffer A and the jitter J are per direction), so the d-rooted
    // search must not reuse the forward table. For symmetric links it gives the same tuples.
    ParetoResult& getRevTable(int dst, Cache& c, const FailureSet& f) {
        auto it = c.rtables.find(dst);
        if (it != c.rtables.end()) { MCSR_OP(tableHit, 1); return it->second; }
        MCSR_OP(tableMiss, 1);
        Local& L = getLocal(plan_.srdOf[dst], c, f);
        Graph R(L.g.size());
        for (int u = 0; u < L.g.size(); ++u)
            for (const Edge& e : L.g.adj[u]) R.addDirected(e.to, u, e.t);
        return c.rtables.emplace(dst, runSearch(R, L.localOf.at(dst))).first->second;
    }

    std::vector<int> globalPath(const ParetoResult& pr, int label, const Local& L) const {
        std::vector<int> nodes, exs;
        pr.trace(label, nodes, exs);
        for (int& n : nodes) n = L.globalOf[n];
        return nodes;
    }

    // Intra-SRD labels from `src` to `dst` (same SRD) as global paths.
    std::vector<DomPath> intra(int src, int dst, Cache& c, const FailureSet& f) {
        std::vector<DomPath> out;
        if (f.blocksNode(src) || f.blocksNode(dst)) return out;
        ParetoResult& pr = getTable(src, c, f);
        Local& L = getLocal(plan_.srdOf[src], c, f);
        for (int li : pr.at[L.localOf.at(dst)]) out.push_back({pr.pool[li].t, globalPath(pr, li, L)});
        return out;
    }

    const std::vector<VEdge>& getVEdges(int k, Cache& c, const FailureSet& f) {
        auto it = c.vedges.find(k);
        if (it != c.vedges.end()) return it->second;
        std::vector<VEdge> ve;
        for (int b : boundaryOf_[k]) {
            if (f.blocksNode(b)) continue;
            ParetoResult& pr = getTable(b, c, f);
            Local& L = getLocal(k, c, f);
            for (int b2 : boundaryOf_[k]) {
                if (b2 == b || f.blocksNode(b2)) continue;
                for (int li : pr.at[L.localOf.at(b2)]) ve.push_back({b, b2, pr.pool[li].t, globalPath(pr, li, L)});
            }
        }
        MCSR_OP(vedgesBuilt, ve.size());
        return c.vedges.emplace(k, std::move(ve)).first->second;
    }

    std::vector<DomPath> dominant(int s, int d, Cache& c, const FailureSet& f) {
        std::vector<DomPath> out;
        if (f.blocksNode(s) || f.blocksNode(d)) return out;
        if (s == d) return {{PathTuple{}, {s}}};
        const int A = plan_.srdOf[s], B = plan_.srdOf[d];
        if (A == B) return finalize(intra(s, d, c, f));  // Sec. III-D: same SRD -> intra-SRD routing only
        if (algo_ != SearchAlgorithm::ParetoLabel) return dominantWave(s, d, c, f);

        // Boundary graph H: boundary satellites + s + d. Edges: real inter-SRD ISLs, and virtual
        // edges carrying the intra-SRD dominant tuples between boundary satellites (the CSIB).
        std::unordered_map<int, int> id;
        std::vector<int> sat;
        auto nodeOf = [&](int v) {
            auto it = id.find(v);
            if (it != id.end()) return it->second;
            id[v] = int(sat.size());
            sat.push_back(v);
            return id[v];
        };
        for (int k = 0; k < plan_.r; ++k) for (int b : boundaryOf_[k]) nodeOf(b);
        nodeOf(s); nodeOf(d);
        Graph H(int(sat.size()));
        std::vector<const std::vector<int>*> ex;  // virtual-edge expansions (no per-query copies)
        std::deque<std::vector<int>> own;         // storage for s-/d-specific expansions
        for (int k = 0; k < plan_.r; ++k) {
            for (int u : boundaryOf_[k]) {
                if (f.blocksNode(u)) continue;
                for (const Edge& e : g_.adj[u])
                    if (plan_.srdOf[e.to] != k && !f.blocksLink(u, e.to)) H.addDirected(id[u], id[e.to], e.t);
            }
            for (const VEdge& v : getVEdges(k, c, f)) {
                ex.push_back(&v.path);
                H.addDirected(id[v.u], id[v.v], v.t, int(ex.size()) - 1);
            }
        }
        if (!isBoundary_[s]) {  // s -> boundary(A)
            ParetoResult& pr = getTable(s, c, f);
            Local& L = getLocal(A, c, f);
            for (int b : boundaryOf_[A]) {
                if (f.blocksNode(b)) continue;
                for (int li : pr.at[L.localOf.at(b)]) {
                    own.push_back(globalPath(pr, li, L));
                    ex.push_back(&own.back());
                    H.addDirected(id[s], id[b], pr.pool[li].t, int(ex.size()) - 1);
                }
            }
        }
        if (!isBoundary_[d]) {  // boundary(B) -> d: d-rooted search on the reversed graph, paths reversed back
            ParetoResult& pr = getRevTable(d, c, f);
            Local& L = getLocal(B, c, f);
            for (int b : boundaryOf_[B]) {
                if (f.blocksNode(b)) continue;
                for (int li : pr.at[L.localOf.at(b)]) {
                    std::vector<int> p = globalPath(pr, li, L);
                    std::reverse(p.begin(), p.end());
                    own.push_back(std::move(p));
                    ex.push_back(&own.back());
                    H.addDirected(id[b], id[d], pr.pool[li].t, int(ex.size()) - 1);
                }
            }
        }
        MCSR_OP(boundaryGraphs, 1);
        for (int u = 0; u < H.size(); ++u) MCSR_OP(boundaryEdges, H.adj[u].size());
        EngineOptions o;
        o.maxLabelsPerNode = cap_;
        ParetoResult pr = paretoSearch(H, id[s], o);
        for (int li : pr.at[id[d]]) {
            std::vector<int> nodes, exs;
            pr.trace(li, nodes, exs);
            std::vector<int> path{s};
            for (size_t i = 1; i < nodes.size(); ++i) {
                if (exs[i - 1] >= 0) path.insert(path.end(), ex[exs[i - 1]]->begin() + 1, ex[exs[i - 1]]->end());
                else path.push_back(sat[nodes[i]]);
            }
            out.push_back({PathTuple{}, std::move(path)});
        }
        return finalize(std::move(out));
    }


    // Algorithm 2 (inter-SRD routing), paper structure. Nodes are SRDs: R = calculated, F = to expand this round,
    // T = not yet calculated. A path label sits at the ENTRANCE PORT (boundary satellite) through which an SRD was
    // entered; the consistency check (Def. 1) compares labels of the same port only, because the cost of leaving the
    // SRD depends on where it was entered. Expanding a label across SRD Si means: intra-SRD dominant tuple entrance ->
    // exit port (the CSIB part), then the ISL from the exit port into the adjacent SRD Sj, which creates a label at
    // Sj's entrance port. Source side: s -> exit ports via the intra-SRD table of s; destination side: entrance port ->
    // d via the d-rooted table. reexpand follows the complexity analysis (a calculated SRD that receives a new
    // dominant path is calculated again); the literal variant never returns to an SRD in R.
    std::vector<DomPath> dominantWave(int s, int d, Cache& c, const FailureSet& f) {
        const bool reexp = algo_ == SearchAlgorithm::PaperWave;
        const int A = plan_.srdOf[s], B = plan_.srdOf[d];
        struct WL { PathTuple t; int sat; int prev; std::vector<int> seg; bool alive; };
        std::vector<WL> pool;
        std::unordered_map<int, std::vector<int>> at;     // entrance port (or s) -> labels (check `alive`)
        std::unordered_map<long long, std::vector<int>> grp;  // consistency-check group -> alive labels
        std::vector<std::vector<int>> pending(plan_.r);   // labels not yet extended, per SRD
        std::vector<char> inR(plan_.r, 0), seen(plan_.r, 0);
        std::unordered_map<int, std::vector<const VEdge*>> byFrom;  // intra-SRD tuples between boundary satellites
        std::vector<char> veBuilt(plan_.r, 0);
        auto ensureVe = [&](int k) {
            if (veBuilt[k]) return;
            veBuilt[k] = 1;
            for (const VEdge& v : getVEdges(k, c, f)) byFrom[v.u].push_back(&v);
        };
        pool.push_back({PathTuple{}, s, -1, {}, true});
        at[s].push_back(0);
        grp[groupKey(s, -1)].push_back(0);
        pending[A].push_back(0);
        std::vector<int> F{A};
        std::size_t T = plan_.r;  // SRDs never calculated so far
        auto insert = [&](int y, const PathTuple& nt, int prev, std::vector<int>&& seg) {
            auto& fr = grp[groupKey(y, plan_.srdOf[pool[prev].sat])];
            for (int k : fr) {
                MCSR_OP(dominanceCmp, 1);
                if (weaklyDominates(pool[k].t, nt)) return false;
            }
            fr.erase(std::remove_if(fr.begin(), fr.end(), [&](int k) {
                         MCSR_OP(dominanceCmp, 1);
                         if (dominates(nt, pool[k].t)) { pool[k].alive = false; MCSR_OP(labelDelete, 1); return true; }
                         return false; }),
                     fr.end());
            pool.push_back({nt, y, prev, std::move(seg), true});
            fr.push_back(int(pool.size()) - 1);
            at[y].push_back(int(pool.size()) - 1);
            MCSR_OP(labelInsert, 1);
            pending[plan_.srdOf[y]].push_back(int(pool.size()) - 1);
            return true;
        };
        while (!F.empty() && (reexp || T > 0)) {
            MCSR_OP(waveRounds, 1);
            std::vector<std::vector<int>> work(F.size());
            for (size_t i = 0; i < F.size(); ++i) work[i].swap(pending[F[i]]);
            std::set<int> next;
            for (size_t i = 0; i < F.size(); ++i) {
                const int Si = F[i];
                ensureVe(Si);
                for (int li : work[i]) {
                    if (!pool[li].alive) continue;
                    MCSR_OP(labelExpand, 1);
                    const int p = pool[li].sat;
                    const PathTuple lt = pool[li].t;
                    auto leave = [&](int x, const PathTuple& it, const std::vector<int>& ipath) {  // ipath: p ... x
                        for (const Edge& e : g_.adj[x]) {
                            MCSR_OP(edgeChecks, 1);
                            const int Sj = plan_.srdOf[e.to];
                            if (Sj == Si || f.blocksLink(x, e.to)) continue;
                            if (!reexp && inR[Sj]) continue;  // Alg. 2 line 4: adjacent SRDs not in R
                            std::vector<int> seg(ipath.begin() + 1, ipath.end());
                            seg.push_back(e.to);
                            if (insert(e.to, extend(extend(lt, it), e.t), li, std::move(seg))) next.insert(Sj);
                        }
                    };
                    if (pool[li].prev < 0 && !isBoundary_[p]) {  // s inside the SRD: intra-SRD paths to its exit ports
                        ParetoResult& pr = getTable(p, c, f);
                        Local& L = getLocal(Si, c, f);
                        for (int x : boundaryOf_[Si]) {
                            if (f.blocksNode(x)) continue;
                            for (int lj : pr.at[L.localOf.at(x)]) leave(x, pr.pool[lj].t, globalPath(pr, lj, L));
                        }
                    } else {
                        leave(p, PathTuple{}, {p});  // leave through the entrance port itself
                        auto it = byFrom.find(p);
                        if (it != byFrom.end())
                            for (const VEdge* v : it->second) leave(v->v, v->t, v->path);
                    }
                }
            }
            for (int k : F) if (!inR[k]) { inR[k] = 1; if (!seen[k]) { seen[k] = 1; --T; } }  // F -> R, removed from T
            std::vector<int> nf;
            for (int k : next) {
                if (inR[k]) {
                    if (!reexp) continue;  // Alg. 2 line 16: only SRDs not (yet) in R join the next F
                    inR[k] = 0;            // calculated SRD received a new dominant path: calculate it again
                    MCSR_OP(waveReexpand, 1);
                }
                nf.push_back(k);
            }
            F.swap(nf);
        }
        // Destination side: every entrance port y of d's SRD, then the intra-SRD dominant paths y -> d.
        std::vector<DomPath> out;
        auto build = [&](int li, const std::vector<int>& tail) {
            std::vector<int> chain;
            for (int i = li; i >= 0; i = pool[i].prev) chain.push_back(i);
            std::reverse(chain.begin(), chain.end());
            std::vector<int> path{s};
            for (int i : chain) path.insert(path.end(), pool[i].seg.begin(), pool[i].seg.end());
            path.insert(path.end(), tail.begin(), tail.end());
            out.push_back({PathTuple{}, std::move(path)});
        };
        if (isBoundary_[d]) ensureVe(B);
        for (auto& kv : at) {
            const int y = kv.first;
            if (plan_.srdOf[y] != B) continue;
            if (y == d) { for (int li : kv.second) if (pool[li].alive) build(li, {}); continue; }
            if (!isBoundary_[y]) continue;
            if (isBoundary_[d]) {
                auto it = byFrom.find(y);
                if (it == byFrom.end()) continue;
                for (const VEdge* v : it->second)
                    if (v->v == d)
                        for (int li : kv.second) if (pool[li].alive) build(li, std::vector<int>(v->path.begin() + 1, v->path.end()));
            } else {
                ParetoResult& pr = getRevTable(d, c, f);
                Local& L = getLocal(B, c, f);
                if (f.blocksNode(y)) continue;
                for (int lj : pr.at[L.localOf.at(y)]) {
                    std::vector<int> p = globalPath(pr, lj, L);
                    std::reverse(p.begin(), p.end());  // y ... d
                    for (int li : kv.second) if (pool[li].alive) build(li, std::vector<int>(p.begin() + 1, p.end()));
                }
            }
        }
        return finalize(std::move(out));
    }

public:
    // Measurement only: Algorithm 2 read as "inter-SRD paths first, intra-SRD paths afterwards" (Sec. III-C/III-D wording).
    // The wavefront runs over the inter-SRD ISLs only: a label carries the tuple of the ISL chain, the cost of crossing an
    // SRD is NOT part of it, and the consistency check compares labels per `gb`. Every surviving ISL chain is then joined with
    // the intra-SRD dominant paths (entrance -> exit of each SRD on the way, source -> first exit, last entrance -> d) and the
    // true tuples of the joined paths are filtered for dominance. Worklist order instead of R/F/T rounds (same fixed point).
    // prune = false: no dominance among ISL chains at all (every satellite-simple ISL chain is kept, at most `maxLabels`),
    // which separates "intra-SRD cost joined afterwards" from "labels removed early on incomplete cost".
    // *capped (optional) is set when the label limit was hit: the result of such a run is a truncated enumeration, NOT a
    // complete dominant set, and must not be reported as a completeness check.
    std::vector<DomPath> dominantPathsInterFirst(int s, int d, ConsistencyGroup gb, bool prune = true, std::size_t maxLabels = 4000000, bool* capped = nullptr) {
        static const FailureSet none;
        Cache& c = base_;
        const FailureSet& f = none;
        const ConsistencyGroup saved = group_;
        group_ = gb;
        if (capped) *capped = false;
        const int A = plan_.srdOf[s], B = plan_.srdOf[d];
        if (A == B) { group_ = saved; return finalize(intra(s, d, c, f)); }
        struct IL { PathTuple t; int x, w, prev, srd; bool alive; };  // ISL x -> w that entered SRD srd; t = ISL-chain tuple
        std::vector<IL> pool;
        std::unordered_map<long long, std::vector<int>> grp;
        std::deque<int> work;
        pool.push_back({PathTuple{}, -1, s, -1, A, true});
        work.push_back(0);
        while (!work.empty()) {
            const int li = work.front();
            work.pop_front();
            if (!pool[li].alive) continue;
            MCSR_OP(labelExpand, 1);
            const IL cur = pool[li];
            for (int x : boundaryOf_[cur.srd]) {
                for (const Edge& e : g_.adj[x]) {
                    MCSR_OP(edgeChecks, 1);
                    const int Sk = plan_.srdOf[e.to];
                    if (Sk == cur.srd) continue;
                    const PathTuple nt = extend(cur.t, e.t);
                    if (!prune) {  // keep every satellite-simple ISL chain
                        if (pool.size() >= maxLabels) { if (capped) *capped = true; continue; }
                        bool simple = e.to != s && !(x == s && cur.x >= 0);
                        for (int i = li; simple && i > 0; i = pool[i].prev) {
                            if (pool[i].w == e.to) simple = false;
                            if (i != li && pool[i].w == x) simple = false;
                            if (pool[i].x == e.to || (i != li && pool[i].x == x)) simple = false;
                        }
                        if (!simple) continue;
                        pool.push_back({nt, x, e.to, li, Sk, true});
                        work.push_back(int(pool.size()) - 1);
                        MCSR_OP(labelInsert, 1);
                        continue;
                    }
                    auto& fr = grp[groupKey(e.to, cur.srd)];
                    bool covered = false;
                    for (int k : fr) {
                        MCSR_OP(dominanceCmp, 1);
                        if (weaklyDominates(pool[k].t, nt)) { covered = true; break; }
                    }
                    if (covered) continue;
                    fr.erase(std::remove_if(fr.begin(), fr.end(), [&](int k) {
                                 MCSR_OP(dominanceCmp, 1);
                                 if (dominates(nt, pool[k].t)) { pool[k].alive = false; MCSR_OP(labelDelete, 1); return true; }
                                 return false; }),
                             fr.end());
                    pool.push_back({nt, x, e.to, li, Sk, true});
                    fr.push_back(int(pool.size()) - 1);
                    work.push_back(int(pool.size()) - 1);
                    MCSR_OP(labelInsert, 1);
                }
            }
        }
        std::vector<DomPath> out;
        auto dedupe = [](std::vector<DomPath>& v) {  // keep tuple-wise non-dominated joined paths (one of each equal tuple)
            auto same = [](const PathTuple& a, const PathTuple& b) { return std::fabs(a.D - b.D) <= 1e-9 && a.A == b.A && std::fabs(a.J - b.J) <= 1e-9; };
            std::vector<DomPath> r;
            for (size_t i = 0; i < v.size(); ++i) {
                bool dom = false;
                for (size_t j = 0; j < v.size() && !dom; ++j)
                    if (i != j) dom = dominates(v[j].t, v[i].t) || (j < i && same(v[j].t, v[i].t));
                if (!dom) r.push_back(v[i]);
            }
            v.swap(r);
        };
        for (int li = 0; li < int(pool.size()); ++li) {
            if (!pool[li].alive || pool[li].srd != B || li == 0) continue;
            std::vector<int> chain;
            for (int i = li; i > 0; i = pool[i].prev) chain.push_back(i);
            std::reverse(chain.begin(), chain.end());
            std::vector<DomPath> cur{{PathTuple{}, {s}}};
            int pos = s;
            auto join = [&](int to) {  // append the intra-SRD dominant paths pos -> to
                std::vector<DomPath> nxt;
                const std::vector<DomPath> seg = pos == to ? std::vector<DomPath>{{PathTuple{}, {pos}}} : intra(pos, to, c, f);
                for (const DomPath& a : cur)
                    for (const DomPath& b : seg) {
                        DomPath n{extend(a.t, b.t), a.sats};
                        n.sats.insert(n.sats.end(), b.sats.begin() + 1, b.sats.end());
                        nxt.push_back(std::move(n));
                    }
                dedupe(nxt);
                cur.swap(nxt);
                pos = to;
            };
            bool ok = true;
            for (int ci : chain) {
                const IL& h = pool[ci];
                join(h.x);
                if (cur.empty()) { ok = false; break; }
                const PathTuple et = linkTuple(h.x, h.w);
                for (DomPath& a : cur) { a.t = extend(a.t, et); a.sats.push_back(h.w); }
                pos = h.w;
            }
            if (!ok) continue;
            join(d);
            for (DomPath& a : cur) out.push_back(std::move(a));
        }
        group_ = saved;
        return finalize(std::move(out));
    }
private:

    // Key of the consistency-check group of a label entering satellite y from SRD `from` (-1: the source).
    long long groupKey(int y, int from) const {
        switch (group_) {
            case ConsistencyGroup::Port: return y;
            case ConsistencyGroup::PortType: return static_cast<long long>(plan_.srdOf[y]) * 100003LL + (from + 1);
            default: return plan_.srdOf[y];
        }
    }

    // Recompute true tuples from the topology, drop looping/duplicate paths, keep non-dominated.
    std::vector<DomPath> finalize(std::vector<DomPath> in) const {
        std::vector<DomPath> v;
        std::set<std::vector<int>> seen;
        for (auto& p : in) {
            if (std::set<int>(p.sats.begin(), p.sats.end()).size() != p.sats.size()) continue;  // loop
            if (!seen.insert(p.sats).second) continue;
            p.t = pathTuple(p.sats);
            v.push_back(std::move(p));
        }
        std::vector<DomPath> out;
        for (size_t i = 0; i < v.size(); ++i) {
            bool dom = false;
            for (size_t j = 0; j < v.size() && !dom; ++j) { MCSR_OP(finalizeCmp, 1); dom = i != j && dominates(v[j].t, v[i].t); }
            if (!dom) out.push_back(v[i]);
        }
        return out;
    }

    const Graph& g_;
    const SrdPlan& plan_;
    std::size_t cap_;
    SearchAlgorithm algo_ = SearchAlgorithm::ParetoLabel;
    ConsistencyGroup group_ = ConsistencyGroup::Port;
    std::vector<char> isBoundary_;
    std::vector<std::vector<int>> nbrSrds_;
    std::vector<int> gamma_;
    std::vector<std::vector<int>> boundaryOf_;
    Cache base_;
    mutable double bestRng_ = 0.0;
};

}  // namespace mcsr
