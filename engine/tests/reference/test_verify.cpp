// V01-V05 verification supplement (audit of test_mcsr.cpp found these conditions unchecked):
//   V01  exhaustive simple-path enumeration at 24 satellites (test_mcsr stops at 12), WITH a check that no label cap was reached
//   V02  every returned path is a valid chain of SRD ports (exit/entrance satellites, real grid ISLs, adjacent SRDs)
//   V03  D/A/J of every returned path recomputed independently from the geometry and the link-state function
//        (not from the Graph the router reads and not through extend()/pathTuple())
//   V04  LS/TS/HR choices against optima taken over ALL simple paths, constraints checked on the recomputed tuple
//   V05  labelCap: retention of the uncapped dominant set, selection exactness, constraint violations, cap-hit counts
// Everything here uses the same headers the simulator compiles. Nothing here is a paper number; the pass criteria are ours.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <random>
#include <set>

#include "mcsr/router.h"
#include "mcsr/walker.h"

using namespace mcsr;

static int g_fail = 0, g_checks = 0;
#define CHECK(c, ...) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("FAIL %s:%d  %s  ", __FILE__, __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static std::uint64_t mix(std::uint64_t z) {
    z += 0x9e3779b97f4a7c15ULL;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}
static LinkStateFn randomState(std::uint64_t seed) {   // same distribution as test_mcsr: A in [0.1,1], J in [0.5,20.5] ms
    return [seed](int u, int v, double) {
        std::uint64_t h = mix(linkKey(u, v) ^ mix(seed));
        double a = (h & 0xffff) / 65535.0, b = ((h >> 16) & 0xffff) / 65535.0;
        return std::make_pair(0.1 + 0.9 * a, 0.5 + 20.0 * b * b);
    };
}

static bool sameTupleSets(const std::vector<PathTuple>& a, const std::vector<PathTuple>& b) {
    auto same = [](const PathTuple& x, const PathTuple& y) {
        return std::fabs(x.D - y.D) <= 1e-9 && std::fabs(x.A - y.A) <= 1e-12 && std::fabs(x.J - y.J) <= 1e-12;
    };
    auto covers = [&](const std::vector<PathTuple>& u, const std::vector<PathTuple>& v) {
        for (const auto& x : u) {
            bool f = false;
            for (const auto& y : v) if (same(x, y)) { f = true; break; }
            if (!f) return false;
        }
        return true;
    };
    return covers(a, b) && covers(b, a);
}
static std::vector<PathTuple> tuplesOf(const std::vector<DomPath>& v) {
    std::vector<PathTuple> t; for (auto& p : v) t.push_back(p.t); return t;
}

// ---- independent references ---------------------------------------------------------------------------------------------
// Tuple of a satellite sequence from first principles: D = chord length / c, (A,J) from the link-state function. Plain loops: no
// extend(), no Router::pathTuple(), no Graph. Returns false when a hop is not a +grid ISL.
struct Indep {
    const Walker& w;
    TopologyOptions o;
    std::vector<std::vector<int>> grid;
    Indep(const Walker& w_, const TopologyOptions& o_) : w(w_), o(o_), grid(w_.gridNeighbors()) {}
    bool isGrid(int u, int v) const { return std::find(grid[u].begin(), grid[u].end(), v) != grid[u].end(); }
    bool tuple(const std::vector<int>& p, PathTuple& out) const {
        double D = 0, A = 1e300, J = -1e300;
        for (size_t i = 1; i < p.size(); ++i) {
            int u = p[i - 1], v = p[i];
            if (!isGrid(u, v)) return false;
            D += w.distanceKm(u, v, o.t) / Walker::kC * 1000.0;
            auto aj = o.state(std::min(u, v), std::max(u, v), o.t);
            A = std::min(A, aj.first);
            J = std::max(J, aj.second);
        }
        if (p.size() < 2) { D = 0; A = kInf; J = 0; }
        out = {D, A, J};
        return true;
    }
};

// Exhaustive enumeration of ALL simple paths s->d (a hop-by-hop DFS, nothing shared with the engines). Keeps every tuple.
struct Exhaustive {
    std::vector<PathTuple> all;
    std::vector<PathTuple> front;   // non-dominated, de-duplicated (same dominance relation as the engines)
    double minD = kInf, maxA = -1, minJ = kInf;
};
// `allow` (optional) restricts the enumeration to an induced subgraph, e.g. the members of one SRD (paper Sec. III-D: intra-SRD routing).
static Exhaustive exhaustive(const Graph& g, int s, int d, const std::vector<char>* allow = nullptr) {
    Exhaustive ex;
    std::vector<char> vis(g.size(), 0);
    std::function<void(int, PathTuple)> dfs = [&](int u, PathTuple t) {
        if (u == d) { ex.all.push_back(t); return; }
        vis[u] = 1;
        for (const Edge& e : g.adj[u]) if (!vis[e.to] && (!allow || (*allow)[e.to])) dfs(e.to, PathTuple{t.D + e.t.D, std::min(t.A, e.t.A), std::max(t.J, e.t.J)});
        vis[u] = 0;
    };
    dfs(s, PathTuple{0.0, kInf, 0.0});
    for (const PathTuple& t : ex.all) {
        ex.minD = std::min(ex.minD, t.D); ex.maxA = std::max(ex.maxA, t.A); ex.minJ = std::min(ex.minJ, t.J);
        bool covered = false;
        for (const PathTuple& f : ex.front) if (weaklyDominates(f, t)) { covered = true; break; }
        if (covered) continue;
        ex.front.erase(std::remove_if(ex.front.begin(), ex.front.end(), [&](const PathTuple& f) { return dominates(t, f); }), ex.front.end());
        ex.front.push_back(t);
    }
    return ex;
}

// ---- V02 / V03 validator ----------------------------------------------------------------------------------------------
enum Err { E_ENDS = 1, E_LOOP = 2, E_NOT_ISL = 4, E_FAILED = 8, E_PORT = 16, E_SRD_ADJ = 32, E_TUPLE = 64 };
struct Validation { int mask = 0; int srdRevisits = 0, crossings = 0; };
static Validation validate(const Router& R, const SrdPlan& plan, const Indep& ind, const DomPath& p, int s, int d, const FailureSet* f) {
    Validation v;
    const auto& q = p.sats;
    if (q.empty() || q.front() != s || q.back() != d) v.mask |= E_ENDS;
    if (std::set<int>(q.begin(), q.end()).size() != q.size()) v.mask |= E_LOOP;
    std::set<int> seenSrd;
    int prevSrd = -1;
    for (size_t i = 0; i < q.size(); ++i) {
        int sr = plan.srdOf[q[i]];
        if (sr != prevSrd) { if (!seenSrd.insert(sr).second) ++v.srdRevisits; prevSrd = sr; }
        if (i == 0) continue;
        int x = q[i - 1], w = q[i];
        if (!ind.isGrid(x, w)) v.mask |= E_NOT_ISL;
        if (f && f->blocksLink(x, w)) v.mask |= E_FAILED;
        if (plan.srdOf[x] != plan.srdOf[w]) {   // SRD boundary crossing = exit port x of SRD A, entrance port w of SRD B
            ++v.crossings;
            if (!R.isBoundary(x) || !R.isBoundary(w)) v.mask |= E_PORT;
            const auto& nx = R.neighbourSrds(x); const auto& nw = R.neighbourSrds(w);
            if (std::find(nx.begin(), nx.end(), plan.srdOf[w]) == nx.end() || std::find(nw.begin(), nw.end(), plan.srdOf[x]) == nw.end()) v.mask |= E_SRD_ADJ;
        }
    }
    PathTuple t;
    if (ind.tuple(q, t)) {
        if (std::fabs(t.D - p.t.D) > 1e-9 || std::fabs(t.A - p.t.A) > 1e-12 || std::fabs(t.J - p.t.J) > 1e-12) v.mask |= E_TUPLE;
    } else v.mask |= E_NOT_ISL;
    return v;
}

struct Counters { long paths = 0, crossings = 0, revisits = 0; };
static void validateAll(const Router& R, const SrdPlan& plan, const Indep& ind, const std::vector<DomPath>& P, int s, int d, const FailureSet* f,
                        Counters& c, const char* tag) {
    for (const DomPath& p : P) {
        Validation v = validate(R, plan, ind, p, s, d, f);
        CHECK(v.mask == 0, "%s path invalid mask=%d s=%d d=%d", tag, v.mask, s, d);
        ++c.paths; c.crossings += v.crossings; c.revisits += v.srdRevisits;
    }
}

static long capHits(const OpCounts& n) { return (long)(n.capEvict + n.capReject); }

// ---- V01 + V02 + V03 + V04 on 24 satellites -----------------------------------------------------------------------------
static void testExhaustive24() {
    struct Shape { int NO, NP; std::vector<std::pair<int, int>> KL; };
    const std::vector<Shape> shapes = {{4, 6, {{2, 2}, {2, 3}, {1, 3}, {4, 2}}}, {3, 8, {{1, 2}, {3, 2}, {3, 4}}}};
    long pairs = 0, inter = 0, intra = 0, maxPaths = 0, totalPaths = 0, frontMax = 0;
    long capLabel = 0, capLabel256 = 0;
    Counters cnt;
    long v04Pairs = 0, lsSecondaryDiff = 0, intraGap = 0, intraGapPairs = 0, hrFallback = 0, hrConstrained = 0, tsInfeasible = 0;
    for (const Shape& sh : shapes) {
        Walker w({sh.NO, sh.NP, 550, 53, 1});
        for (int seed = 0; seed < 3; ++seed) {
            TopologyOptions o; o.state = randomState(100 + seed);
            BuiltTopology bt = w.buildTopology(o);
            Indep ind(w, o);
            std::vector<SrdPlan> plans;
            for (auto [K, L] : sh.KL) { plans.push_back(makePlan(sh.NO, sh.NP, K, L)); CHECK(plans.back().r >= 2, "two SRDs at least"); }
            for (int s : {0, 13}) {
                for (int d = 0; d < w.N(); ++d) {
                    if (d == s) continue;
                    Exhaustive ex = exhaustive(bt.g, s, d);
                    ++pairs; maxPaths = std::max<long>(maxPaths, (long)ex.all.size()); totalPaths += (long)ex.all.size();
                    frontMax = std::max<long>(frontMax, (long)ex.front.size());
                    // independent tuple of the extreme paths: exhaustive min/max values are real attribute values
                    for (size_t pi = 0; pi < plans.size(); ++pi) {
                        const SrdPlan& plan = plans[pi];
                        const bool same = plan.srdOf[s] == plan.srdOf[d];
                        (same ? intra : inter)++;
                        // Same-SRD pairs: the paper routes inside the SRD only (Sec. III-D), so the reference is the exhaustive set over the
                        // SRD's induced subgraph. Different-SRD pairs: the reference is the exhaustive set over the whole constellation.
                        Exhaustive exSrd;
                        if (same) {
                            std::vector<char> allow(w.N(), 0);
                            for (int m : plan.members[plan.srdOf[s]]) allow[m] = 1;
                            exSrd = exhaustive(bt.g, s, d, &allow);
                            ++intraGapPairs;
                            if (!sameTupleSets(exSrd.front, ex.front)) ++intraGap;
                        }
                        const Exhaustive& ref = same ? exSrd : ex;
                        struct Cfg { SearchAlgorithm a; size_t cap; const char* name; };
                        for (Cfg cfg : {Cfg{SearchAlgorithm::ParetoLabel, 100000, "label(cap=1e5)"}, Cfg{SearchAlgorithm::PaperWave, 100000, "wave"},
                                        Cfg{SearchAlgorithm::ParetoLabel, 256, "label(cap=256)"}}) {
                            Router R(w.gridNeighbors(), bt.g, plan, cfg.cap);
                            R.setAlgorithm(cfg.a);
                            OpCounts n;
                            std::vector<DomPath> P;
                            { OpScope sc(&n); P = R.dominantPaths(s, d); }
                            if (cfg.cap == 100000 && cfg.a == SearchAlgorithm::ParetoLabel) capLabel += capHits(n);
                            if (cfg.cap == 256) capLabel256 += capHits(n);
                            CHECK(sameTupleSets(tuplesOf(P), ref.front), "V01 %s == exhaustive%s  %dx%d plan=%zu seed=%d s=%d d=%d (|P|=%zu |ref|=%zu)", cfg.name, same ? "(SRD interior)" : "", sh.NO, sh.NP, pi, seed, s, d, P.size(), ref.front.size());
                            validateAll(R, plan, ind, P, s, d, nullptr, cnt, cfg.name);
                            if (cfg.cap != 100000) continue;
                            // V04: selections against optima over ALL simple paths
                            ++v04Pairs;
                            SelectionParams sp;
                            RouteResult ls = R.route(s, d, Service::LS), ts = R.route(s, d, Service::TS), hr = R.route(s, d, Service::HR);
                            PathTuple tl, tt, th;
                            CHECK(ind.tuple(ls.best.sats, tl) && ind.tuple(ts.best.sats, tt) && ind.tuple(hr.best.sats, th), "selected paths are real");
                            CHECK(std::fabs(tl.D - ref.minD) <= 1e-9, "V04 LS D == min over all simple paths (%g vs %g)", tl.D, ref.minD);
                            { // secondary criterion: among paths with D within eps of min D, max A (selectPath compares D exactly)
                                double aAtMin = -1; for (auto& t : ref.all) if (t.D <= ref.minD + 1e-9) aAtMin = std::max(aAtMin, t.A);
                                if (tl.A + 1e-12 < aAtMin) ++lsSecondaryDiff;
                            }
                            CHECK(tt.A == ref.maxA, "V04 TS A == max over all simple paths (%g vs %g)", tt.A, ref.maxA);
                            { double dAtMax = kInf; for (auto& t : ref.all) if (t.A == ref.maxA) dAtMax = std::min(dAtMax, t.D);
                              CHECK(tt.D <= dAtMax + 1e-9, "V04 TS tie-break: min D among max-A paths"); }
                            { double bestA = -1, bestD = kInf; bool any = false;
                              for (auto& t : ref.all) if (t.J < sp.hrJitterMs) { any = true; if (t.A > bestA || (t.A == bestA && t.D < bestD)) { bestA = t.A; bestD = t.D; } }
                              if (any) { ++hrConstrained;
                                  CHECK(th.J < sp.hrJitterMs, "V04 HR constraint J<15 holds on the recomputed tuple (J=%g)", th.J);
                                  CHECK(th.A == bestA, "V04 HR A == max A among J<15 paths (%g vs %g)", th.A, bestA);
                                  CHECK(th.D <= bestD + 1e-9, "V04 HR tie-break min D");
                              } else { ++hrFallback; CHECK(th.J == ref.minJ, "V04 HR fallback: no J<15 path -> min J"); } }
                            // Pareto-optimality of the chosen paths against the exhaustive front
                            for (const PathTuple* c : {&tl, &tt, &th}) { bool dom = false; for (auto& f : ref.front) dom |= dominates(f, *c); CHECK(!dom, "V04 chosen path is not dominated by any simple path"); }
                            // TS with a delay bound (implementation parameter, paper gives none)
                            SelectionParams spb; spb.tsDelayBoundMs = 1.3 * ref.minD;
                            RouteResult tsb = R.route(s, d, Service::TS, nullptr, spb);
                            PathTuple tb; CHECK(ind.tuple(tsb.best.sats, tb), "tsb real");
                            double bA = -1; for (auto& t : ref.all) if (t.D <= spb.tsDelayBoundMs) bA = std::max(bA, t.A);
                            CHECK(tb.D <= spb.tsDelayBoundMs + 1e-9 && tb.A == bA, "V04 TS with delay bound: D<=bound and A == max A within bound (A %g vs %g)", tb.A, bA);
                            // TS with a bound no path can meet: selectPath falls back to the unconstrained choice (implementation choice)
                            SelectionParams spi; spi.tsDelayBoundMs = 0.5 * ref.minD;
                            RouteResult tsi = R.route(s, d, Service::TS, nullptr, spi);
                            PathTuple ti; CHECK(ind.tuple(tsi.best.sats, ti) && ti.A == ref.maxA, "V04 TS with an infeasible bound falls back to max A");
                            ++tsInfeasible;
                        }
                    }
                }
            }
        }
    }
    CHECK(capLabel == 0, "V01 the cap=1e5 label search never reached its label cap (hits=%ld)", capLabel);
    std::printf("  V01  %ld (s,d) pairs, %ld router runs inter-SRD / %ld intra-SRD, %ld simple paths enumerated (max %ld per pair), max exhaustive front %ld\n",
                pairs, inter, intra, totalPaths, maxPaths, frontMax);
    std::printf("       label cap hits: cap=1e5 -> %ld ; cap=256 -> %ld ; PaperWave has no cap\n", capLabel, capLabel256);
    std::printf("  V02/V03  %ld returned paths validated (%ld SRD crossings, %ld SRD revisits inside a path)\n", cnt.paths, cnt.crossings, cnt.revisits);
    std::printf("  intra-SRD scope: for %ld of %ld same-SRD pairs the exhaustive set over the whole constellation differs from the one inside the SRD\n", intraGap, intraGapPairs);
    std::printf("  V04  branches exercised: HR constrained (some J<15 path) %ld, HR fallback (no J<15 path) %ld, TS infeasible-bound fallback %ld\n", hrConstrained, hrFallback, tsInfeasible);
    std::printf("  V04  %ld pairs; LS secondary criterion (max A among near-equal D) differed in %ld pairs (exact-D comparison in selectPath)\n", v04Pairs, lsSecondaryDiff);
}

// ---- V02/V03 on the 120-satellite grid, with failures, plus negative controls -------------------------------------------
static void testPortsLarger() {
    Walker w({12, 10, 550, 53, 1});
    TopologyOptions o; o.state = randomState(7);
    BuiltTopology bt = w.buildTopology(o);
    Indep ind(w, o);
    Counters cnt; long fails = 0, pairs = 0;
    for (auto [K, L] : std::vector<std::pair<int, int>>{{4, 5}, {5, 4}, {3, 3}}) {
        SrdPlan plan = makePlan(12, 10, K, L);
        for (SearchAlgorithm a : {SearchAlgorithm::ParetoLabel, SearchAlgorithm::PaperWave}) {
            Router R(w.gridNeighbors(), bt.g, plan, 256);
            R.setAlgorithm(a);
            std::mt19937 rng(K * 17 + L);
            for (int it = 0; it < 50; ++it) {
                int s = rng() % w.N(), d = rng() % w.N();
                if (s == d) continue;
                ++pairs;
                auto P = R.dominantPaths(s, d);
                validateAll(R, plan, ind, P, s, d, nullptr, cnt, a == SearchAlgorithm::PaperWave ? "wave120" : "label120");
                const DomPath* b = selectPath(P, Service::LS);
                if (b && b->sats.size() >= 3) {
                    FailureSet f; size_t m = b->sats.size() / 2; f.links.insert(linkKey(b->sats[m - 1], b->sats[m]));
                    auto Pf = R.dominantPaths(s, d, &f);
                    ++fails;
                    validateAll(R, plan, ind, Pf, s, d, &f, cnt, "failure120");
                }
            }
        }
    }
    std::printf("  V02/V03  120 satellites: %ld pairs, %ld with a failed link, %ld paths validated (%ld crossings, %ld SRD revisits)\n", pairs, fails, cnt.paths, cnt.crossings, cnt.revisits);

    // Negative controls: the validator must reject corrupted paths, otherwise "0 errors" would mean nothing.
    SrdPlan plan = makePlan(12, 10, 4, 5);
    Router R(w.gridNeighbors(), bt.g, plan, 256);
    int s = -1, d = -1;
    for (int i = 0; i < w.N() && s < 0; ++i) if (!R.isBoundary(i)) s = i;
    for (int i = 0; i < w.N() && d < 0; ++i) if (!R.isBoundary(i) && plan.srdOf[i] != plan.srdOf[s]) d = i;
    auto P = R.dominantPaths(s, d);
    CHECK(!P.empty(), "control base path");
    DomPath good = *std::max_element(P.begin(), P.end(), [](const DomPath& x, const DomPath& y) { return x.sats.size() < y.sats.size(); });
    CHECK(validate(R, plan, ind, good, s, d, nullptr).mask == 0, "control: untouched path is valid");
    DomPath m1 = good; m1.t.D += 0.01;                                         // tuple does not match the links
    CHECK(validate(R, plan, ind, m1, s, d, nullptr).mask & E_TUPLE, "control: wrong tuple is detected");
    DomPath m2 = good; std::swap(m2.sats[1], m2.sats[2]);                      // hop that is not an ISL
    CHECK(validate(R, plan, ind, m2, s, d, nullptr).mask & E_NOT_ISL, "control: non-ISL hop is detected");
    DomPath m3 = good; m3.sats.push_back(m3.sats.back());                      // repeated satellite
    CHECK(validate(R, plan, ind, m3, s, d, nullptr).mask & E_LOOP, "control: repeated satellite is detected");
    FailureSet f; f.links.insert(linkKey(good.sats[0], good.sats[1]));
    CHECK(validate(R, plan, ind, good, s, d, &f).mask & E_FAILED, "control: failed link on the path is detected");
    // a crossing between two satellites that are not both boundary satellites: find an interior satellite with a neighbour in another SRD? by
    // construction impossible, so mutate the plan the validator trusts instead: view the same path under a different (shifted) SRD plan.
    SrdPlan other = makePlan(12, 10, 3, 3);
    Router Ro(w.gridNeighbors(), bt.g, other, 256);
    int bad = 0;
    for (const DomPath& p : P) bad += (validate(R, other, ind, p, s, d, nullptr).mask & (E_PORT | E_SRD_ADJ)) != 0;
    (void)Ro;
    CHECK(bad > 0, "control: a path checked against the wrong SRD plan violates port rules in at least one case");
}

// ---- V05 labelCap -------------------------------------------------------------------------------------------------------
static void testLabelCap() {
    struct Row { long pairs = 0, retained = 0, refTotal = 0, capHit = 0, pairsWithHit = 0;
                 long same[3] = {0, 0, 0}, hrViolation = 0, minDExact = 0; double sizeSum = 0; };
    const size_t caps[] = {8, 16, 32, 64, 128, 256, 100000};
    const int NC = 7;
    Row rows[NC];
    long refMaxFront = 0; double refSizeSum = 0; long refPairs = 0;
    for (int seed = 0; seed < 3; ++seed) {
        Walker w({12, 10, 550, 53, 1});
        TopologyOptions o; o.state = randomState(300 + seed);
        BuiltTopology bt = w.buildTopology(o);
        SrdPlan plan = makePlan(12, 10, 4, 5);
        Router ref(w.gridNeighbors(), bt.g, plan, 100000);
        std::vector<std::unique_ptr<Router>> rs;
        for (size_t c : caps) rs.emplace_back(new Router(w.gridNeighbors(), bt.g, plan, c));
        std::mt19937 rng(900 + seed);
        for (int it = 0; it < 80; ++it) {
            int s = rng() % w.N(), d = rng() % w.N();
            if (s == d) continue;
            OpCounts nref; std::vector<DomPath> F;
            { OpScope sc(&nref); F = ref.dominantPaths(s, d); }
            CHECK(capHits(nref) == 0, "V05 reference (cap=1e5) never reached its cap");
            ++refPairs; refSizeSum += F.size(); refMaxFront = std::max<long>(refMaxFront, (long)F.size());
            const Service sv[3] = {Service::LS, Service::TS, Service::HR};
            for (int ci = 0; ci < NC; ++ci) {
                OpCounts n; std::vector<DomPath> C;
                { OpScope sc(&n); C = rs[ci]->dominantPaths(s, d); }
                Row& r = rows[ci];
                ++r.pairs; r.sizeSum += C.size(); r.refTotal += (long)F.size(); r.capHit += capHits(n); r.pairsWithHit += capHits(n) > 0;
                for (const DomPath& f : F) for (const DomPath& c : C) if (std::fabs(f.t.D - c.t.D) <= 1e-9 && f.t.A == c.t.A && f.t.J == c.t.J) { ++r.retained; break; }
                for (int k = 0; k < 3; ++k) {
                    const DomPath* a = selectPath(F, sv[k]); const DomPath* b = selectPath(C, sv[k]);
                    if (a && b && std::fabs(a->t.D - b->t.D) <= 1e-9 && a->t.A == b->t.A && a->t.J == b->t.J) ++r.same[k];
                }
                { const DomPath* la = selectPath(F, Service::LS); const DomPath* lb = selectPath(C, Service::LS); if (la && lb && std::fabs(la->t.D - lb->t.D) <= 1e-9) ++r.minDExact; }
                const DomPath* a = selectPath(F, Service::HR); const DomPath* b = selectPath(C, Service::HR);
                if (a && b && a->t.J < 15.0 && !(b->t.J < 15.0)) ++r.hrViolation;
                if (caps[ci] >= 100000) CHECK(sameTupleSets(tuplesOf(C), tuplesOf(F)), "V05 cap=1e5 == reference");
            }
        }
    }
    std::printf("  V05  120 satellites, SRD 4x5, 3 seeds, %ld pairs; reference |P| mean %.2f max %ld (independent random A,J)\n", refPairs, refSizeSum / refPairs, refMaxFront);
    std::printf("       cap     retention  LS-same  TS-same  HR-same  HR J<15 lost  cap hits (pairs hit)  mean |P|\n");
    for (int ci = 0; ci < NC; ++ci) {
        const Row& r = rows[ci];
        std::printf("       %-7zu %6.1f%%   %6.1f%%  %6.1f%%  %6.1f%%  %6ld        %6ld (%ld)        %.2f\n", caps[ci], 100.0 * r.retained / r.refTotal,
                    100.0 * r.same[0] / r.pairs, 100.0 * r.same[1] / r.pairs, 100.0 * r.same[2] / r.pairs, r.hrViolation, r.capHit, r.pairsWithHit, r.sizeSum / r.pairs);
    }
    // invariants that hold by construction of the cap rule: the three extremes survive at every cap (already covered at cap 6 by test_mcsr)
    CHECK(rows[NC - 1].retained == rows[NC - 1].refTotal, "V05 uncapped retains everything");
    for (int ci = 0; ci < NC; ++ci) CHECK(rows[ci].minDExact == rows[ci].pairs, "V05 LS min D exact at cap %zu", caps[ci]);
}

int main() {
    auto t0 = std::chrono::steady_clock::now();
    std::printf("[V01 exhaustive 24 satellites + V02/V03 validation + V04 selection]\n"); testExhaustive24();
    std::printf("[V02/V03 larger grid, failures, negative controls]\n"); testPortsLarger();
    std::printf("[V05 labelCap]\n"); testLabelCap();
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("\n%d checks, %d failed (%.1fs)\n", g_checks, g_fail, sec);
    return g_fail ? 1 : 0;
}
