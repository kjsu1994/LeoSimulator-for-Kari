#include <cassert>
#include <chrono>
#include <cstdio>
#include <functional>
#include <queue>
#include <random>

#include "mcsr/planner.h"
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
static LinkStateFn randomState(std::uint64_t seed) {
    return [seed](int u, int v, double) {
        std::uint64_t h = mix(linkKey(u, v) ^ mix(seed));
        double a = (h & 0xffff) / 65535.0, b = ((h >> 16) & 0xffff) / 65535.0;
        return std::make_pair(0.1 + 0.9 * a, 0.5 + 20.0 * b * b);
    };
}

// ---- independent references --------------------------------------------------------------------
static std::vector<double> dijkstraD(const Graph& g, int s, const FailureSet& f) {
    std::vector<double> d(g.size(), kInf);
    using Q = std::pair<double, int>;
    std::priority_queue<Q, std::vector<Q>, std::greater<Q>> pq;
    d[s] = 0; pq.push({0, s});
    while (!pq.empty()) {
        auto [du, u] = pq.top(); pq.pop();
        if (du > d[u]) continue;
        for (const Edge& e : g.adj[u]) {
            if (f.blocksLink(u, e.to)) continue;
            if (du + e.t.D < d[e.to]) { d[e.to] = du + e.t.D; pq.push({d[e.to], e.to}); }
        }
    }
    return d;
}
static std::vector<double> widest(const Graph& g, int s, const FailureSet& f) {
    std::vector<double> a(g.size(), -1.0);
    using Q = std::pair<double, int>;
    std::priority_queue<Q> pq;
    a[s] = kInf; pq.push({kInf, s});
    while (!pq.empty()) {
        auto [au, u] = pq.top(); pq.pop();
        if (au < a[u]) continue;
        for (const Edge& e : g.adj[u]) {
            if (f.blocksLink(u, e.to)) continue;
            double na = std::min(au, e.t.A);
            if (na > a[e.to]) { a[e.to] = na; pq.push({na, e.to}); }
        }
    }
    return a;
}
static std::vector<PathTuple> bruteForce(const Graph& g, int s, int d) {
    std::vector<PathTuple> all;
    std::vector<char> vis(g.size(), 0);
    std::function<void(int, PathTuple)> dfs = [&](int u, PathTuple t) {
        if (u == d) { all.push_back(t); return; }
        vis[u] = 1;
        for (const Edge& e : g.adj[u]) if (!vis[e.to]) dfs(e.to, extend(t, e.t));
        vis[u] = 0;
    };
    dfs(s, PathTuple{});
    std::vector<PathTuple> out;
    for (size_t i = 0; i < all.size(); ++i) {
        bool dom = false;
        for (size_t j = 0; j < all.size() && !dom; ++j) dom = dominates(all[j], all[i]);
        bool dup = false;
        for (auto& o : out) if (weaklyDominates(o, all[i]) && weaklyDominates(all[i], o)) dup = true;
        if (!dom && !dup) out.push_back(all[i]);
    }
    return out;
}
static bool sameTupleSets(const std::vector<PathTuple>& a, const std::vector<PathTuple>& b) {
    // Compared as SETS of distinct tuples, with tolerance. (1) D sums of different hop orders differ by floating-point
    // noise, so sorting by exact D would pair up tuples with equal D but different A/J in an order-dependent way.
    // (2) Two different paths can have the same tuple (grid symmetry); neither dominates the other, so which of them a
    // search keeps depends on its order of discovery. Only the tuples are an order-independent property.
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
static std::vector<PathTuple> engineTuples(const ParetoResult& r, int node) {
    std::vector<PathTuple> t; for (int li : r.at[node]) t.push_back(r.pool[li].t); return t;
}

// ---- tests ---------------------------------------------------------------------------------------
static void testAlgebra() {
    std::mt19937 rng(1);
    std::uniform_real_distribution<double> u(0, 1);
    for (int i = 0; i < 20000; ++i) {
        PathTuple a{u(rng), u(rng), u(rng)}, b{u(rng), u(rng), u(rng)}, c{u(rng), u(rng), u(rng)};
        CHECK(!dominates(a, a), "irreflexive");
        if (dominates(a, b)) {
            CHECK(weaklyDominates(extend(a, c), extend(b, c)), "isotone (Def.1 property 1)");
            if (dominates(b, c)) CHECK(dominates(a, c), "transitive");
        }
        PathTuple l = extend(extend(a, b), c), r = extend(a, extend(b, c));
        CHECK(std::fabs(l.D - r.D) < 1e-12 && l.A == r.A && l.J == r.J, "associative");
    }
    PathTuple fig3_1{9, 0.2, 5}, fig3_2{4, 0.8, 2};  // Fig. 3 style: Tuple2 ≺ Tuple1
    CHECK(dominates(fig3_2, fig3_1), "fig3");
}

static void testSegmentation() {
    // Case 1 (divisible), 2, 3, 4 (non-divisible): full cover, balanced to within 1.
    for (auto [NO, NP, K, L] : std::vector<std::array<int, 4>>{{72, 22, 12, 11}, {72, 22, 12, 7}, {72, 22, 10, 11}, {72, 22, 10, 7}, {84, 60, 9, 13}, {5, 5, 5, 5}, {5, 5, 1, 1}}) {
        SrdPlan p = makePlan(NO, NP, K, L);
        size_t tot = 0;
        for (auto& m : p.members) tot += m.size();
        CHECK(int(tot) == NO * NP, "cover %d", NO * NP);
        for (int s : p.srdOf) CHECK(s >= 0 && s < p.r, "assigned");
        int minW = 1 << 30, maxW = 0, minH = 1 << 30, maxH = 0;
        for (auto& r : p.rect) {
            minH = std::min(minH, r.qhi - r.qlo + 1); maxH = std::max(maxH, r.qhi - r.qlo + 1);
            minW = std::min(minW, r.xhi - r.xlo + 1); maxW = std::max(maxW, r.xhi - r.xlo + 1);
        }
        CHECK(maxH - minH <= 1 && maxW - minW <= 1, "balanced %d %d %d %d", NO, NP, K, L);
        if (NO % K == 0 && NP % L == 0) CHECK(p.r == NO * NP / (K * L) && minH == K && minW == L, "case1 r");
    }
    // Choice minimises T_Cov over the full (K,L) grid.
    SizeChoice c = chooseSrdSize(72, 22, 3.5, 3.9);
    for (int i = 1; i <= 72; ++i)
        for (int j = 1; j <= 22; ++j) CHECK(tcov(72, 22, i, j, 3.5, 3.9, FloodModel::PaperLiteral) >= c.tcov - 1e-12, "argmin");
}

static void testEngineVsBruteForce() {
    Walker w({3, 4, 550, 53, 1});  // 12 satellites, wrap-around, small enough for exhaustive DFS
    for (int seed = 0; seed < 5; ++seed) {
        TopologyOptions o; o.state = randomState(seed);
        BuiltTopology bt = w.buildTopology(o);
        for (int s : {0, 5}) {
            ParetoResult r = paretoSearch(bt.g, s);
            for (int d = 0; d < w.N(); ++d) {
                if (d == s) continue;
                CHECK(sameTupleSets(engineTuples(r, d), bruteForce(bt.g, s, d)), "pareto==brute seed=%d s=%d d=%d", seed, s, d);
            }
        }
    }
}

// Segmented routing against EXHAUSTIVE simple-path enumeration (not another search algorithm) on a 12-satellite wrap-around grid,
// plus a soundness guard for the measurement-only "inter-SRD first" reading: it returns real paths, so it can lose dominant
// paths but never beat the true dominant set.
static void testSegmentedVsBruteForce() {
    Walker w({3, 4, 550, 53, 1});
    for (auto [K, L] : std::vector<std::pair<int, int>>{{1, 2}, {2, 2}, {3, 2}, {1, 4}}) {
        SrdPlan plan = makePlan(3, 4, K, L);
        CHECK(plan.r >= 2, "at least two SRDs (K=%d L=%d)", K, L);
        for (int seed = 0; seed < 3; ++seed) {
            TopologyOptions o; o.state = randomState(seed);
            BuiltTopology bt = w.buildTopology(o);
            for (SearchAlgorithm algo : {SearchAlgorithm::ParetoLabel, SearchAlgorithm::PaperWave}) {
                Router R(w.gridNeighbors(), bt.g, plan, 256);
                R.setAlgorithm(algo);
                Router Rinter(w.gridNeighbors(), bt.g, plan, 256);
                Rinter.setAlgorithm(SearchAlgorithm::PaperWave);
                for (int s : {0, 5, 9}) {
                    for (int d = 0; d < w.N(); ++d) {
                        if (d == s || plan.srdOf[s] == plan.srdOf[d]) continue;
                        std::vector<PathTuple> ref = bruteForce(bt.g, s, d);
                        CHECK(sameTupleSets(tuplesOf(R.dominantPaths(s, d)), ref), "segmented==brute algo=%d K=%d L=%d seed=%d s=%d d=%d", int(algo), K, L, seed, s, d);
                        if (algo != SearchAlgorithm::PaperWave) continue;
                        bool capHit = false;
                        auto noPrune = Rinter.dominantPathsInterFirst(s, d, ConsistencyGroup::Port, false, 4000000, &capHit);
                        CHECK(!capHit, "label limit not reached on the 12-satellite grid");
                        CHECK(sameTupleSets(tuplesOf(noPrune), ref),
                              "inter-first WITHOUT early pruning == brute K=%d L=%d seed=%d s=%d d=%d", K, L, seed, s, d);
                        for (ConsistencyGroup g : {ConsistencyGroup::Port, ConsistencyGroup::PortType, ConsistencyGroup::Srd})
                            for (const DomPath& p : Rinter.dominantPathsInterFirst(s, d, g)) {
                                bool covered = false;
                                for (const PathTuple& r : ref) covered |= weaklyDominates(r, p.t);
                                CHECK(covered, "inter-first path is never better than the true dominant set (K=%d L=%d seed=%d s=%d d=%d)", K, L, seed, s, d);
                            }
                    }
                }
            }
        }
    }
}

static void testRouterAgainstGlobal() {
    Walker w({12, 10, 550, 53, 1});
    TopologyOptions o; o.state = randomState(7);
    BuiltTopology bt = w.buildTopology(o);
    for (auto [K, L] : std::vector<std::pair<int, int>>{{4, 5}, {5, 4}, {3, 3}}) {  // incl. non-divisible
        SrdPlan plan = makePlan(12, 10, K, L);
        Router R(w.gridNeighbors(), bt.g, plan, 256);
        std::mt19937 rng(K * 31 + L);
        int inter = 0;
        for (int it = 0; it < 40; ++it) {
            int s = rng() % w.N(), d = rng() % w.N();
            if (s == d || plan.srdOf[s] == plan.srdOf[d]) continue;
            ++inter;
            FailureSet none;
            auto P = R.dominantPaths(s, d);
            ParetoResult ref = paretoSearch(bt.g, s);
            CHECK(!P.empty(), "nonempty");
            CHECK(sameTupleSets(tuplesOf(P), engineTuples(ref, d)), "inter-SRD dominant set == global Pareto set (K=%d L=%d s=%d d=%d)", K, L, s, d);
            for (auto& p : P) {
                CHECK(p.sats.front() == s && p.sats.back() == d, "endpoints");
                for (size_t i = 1; i < p.sats.size(); ++i) {
                    bool adj = false;
                    for (auto& e : bt.g.adj[p.sats[i - 1]]) adj |= e.to == p.sats[i];
                    CHECK(adj, "hop is a real ISL");
                }
            }
            // Selection rules
            RouteResult ls = R.route(s, d, Service::LS), ts = R.route(s, d, Service::TS), hr = R.route(s, d, Service::HR);
            CHECK(std::fabs(ls.best.t.D - dijkstraD(bt.g, s, none)[d]) < 1e-9, "LS == global shortest delay");
            CHECK(std::fabs(ts.best.t.A - widest(bt.g, s, none)[d]) < 1e-12, "TS == global widest bottleneck");
            bool anyOk = false;
            for (auto& p : P) anyOk |= p.t.J < 15.0;
            if (anyOk) {
                CHECK(hr.best.t.J < 15.0, "HR respects J<15ms");
                for (auto& p : P) if (p.t.J < 15.0) CHECK(p.t.A <= hr.best.t.A + 1e-12, "HR picks max A among J<15");
            }
        }
        CHECK(inter > 10, "enough inter-SRD samples");
    }
}

// Links are directed in the simulator (available buffer A and jitter J are measured per direction), so
// the hierarchical router must also be exact when u->v and v->u carry different tuples. This
// exercises the d-rooted (reversed-graph) intra-SRD tables.
static void testAsymmetricLinks() {
    Walker w({12, 10, 550, 53, 1});
    TopologyOptions o; o.state = randomState(11);
    BuiltTopology bt = w.buildTopology(o);
    Graph g = bt.g;
    for (int u = 0; u < g.size(); ++u)
        for (Edge& e : g.adj[u]) {   // directed hash: (u,v) != (v,u)
            std::uint64_t h = mix((std::uint64_t(u) << 32) ^ std::uint64_t(e.to) ^ mix(99));
            e.t.A = 0.05 + 0.95 * ((h & 0xffff) / 65535.0);
            e.t.J = 0.5 + 20.0 * (((h >> 16) & 0xffff) / 65535.0);
        }
    int asym = 0;
    for (int u = 0; u < g.size(); ++u)
        for (const Edge& e : g.adj[u])
            for (const Edge& r : g.adj[e.to]) if (r.to == u && r.t.A != e.t.A) ++asym;
    CHECK(asym > 100, "graph really is asymmetric (%d)", asym);

    for (auto [K, L] : std::vector<std::pair<int, int>>{{4, 5}, {5, 4}, {3, 3}}) {
        SrdPlan plan = makePlan(12, 10, K, L);
        Router R(w.gridNeighbors(), g, plan, 256);
        std::mt19937 rng(K * 17 + L);
        int inter = 0, intra = 0;
        for (int it = 0; it < 60; ++it) {
            int s = rng() % w.N(), d = rng() % w.N();
            if (s == d) continue;
            (plan.srdOf[s] == plan.srdOf[d] ? intra : inter)++;
            auto P = R.dominantPaths(s, d);
            ParetoResult ref = paretoSearch(g, s);
            CHECK(!P.empty(), "nonempty");
            CHECK(sameTupleSets(tuplesOf(P), engineTuples(ref, d)) || plan.srdOf[s] == plan.srdOf[d],
                  "asymmetric: inter-SRD dominant set == global directed Pareto set (K=%d L=%d s=%d d=%d)", K, L, s, d);
            for (auto& p : P) {   // reported tuple must equal the tuple of the walked directed edges
                PathTuple t;
                bool ok = true;
                for (size_t i = 1; i < p.sats.size(); ++i) {
                    bool f = false;
                    for (auto& e : g.adj[p.sats[i - 1]]) if (e.to == p.sats[i]) { t = extend(t, e.t); f = true; break; }
                    ok &= f;
                }
                CHECK(ok && std::fabs(t.D - p.t.D) < 1e-9 && t.A == p.t.A && t.J == p.t.J, "tuple matches directed edges");
            }
        }
        CHECK(inter > 15, "enough inter-SRD samples (%d)", inter);
    }
}

// With a tight label cap the three extremes (min D, max A, min J) must stay exact.
static void testCappedExtremes() {
    Walker w({12, 10, 550, 53, 1});
    TopologyOptions o; o.state = randomState(21);
    BuiltTopology bt = w.buildTopology(o);
    SrdPlan plan = makePlan(12, 10, 4, 5);
    Router R(w.gridNeighbors(), bt.g, plan, 6);
    std::mt19937 rng(9);
    FailureSet none;
    int n = 0;
    for (int it = 0; it < 60; ++it) {
        int s = rng() % w.N(), d = rng() % w.N();
        if (s == d || plan.srdOf[s] == plan.srdOf[d]) continue;
        auto P = R.dominantPaths(s, d);
        CHECK(!P.empty(), "capped nonempty");
        double minD = kInf, maxA = -1, minJ = kInf;
        for (auto& p : P) { minD = std::min(minD, p.t.D); maxA = std::max(maxA, p.t.A); minJ = std::min(minJ, p.t.J); }
        ParetoResult ref = paretoSearch(bt.g, s);
        double rD = kInf, rA = -1, rJ = kInf;
        for (int li : ref.at[d]) { auto& t = ref.pool[li].t; rD = std::min(rD, t.D); rA = std::max(rA, t.A); rJ = std::min(rJ, t.J); }
        CHECK(std::fabs(minD - rD) < 1e-9, "capped min D exact");
        CHECK(maxA == rA, "capped max A exact");
        CHECK(minJ == rJ, "capped min J exact");
        ++n;
    }
    CHECK(n > 15, "capped samples");
}

static void testIntraSrd() {
    Walker w({12, 10, 550, 53, 1});
    TopologyOptions o; o.state = randomState(11);
    BuiltTopology bt = w.buildTopology(o);
    SrdPlan plan = makePlan(12, 10, 4, 5);
    Router R(w.gridNeighbors(), bt.g, plan, 256);
    FailureSet none;
    int n = 0;
    for (int s = 0; s < w.N(); ++s)
        for (int d = 0; d < w.N(); ++d) {
            if (s == d || plan.srdOf[s] != plan.srdOf[d]) continue;
            auto P = R.dominantPaths(s, d);
            // reference: Pareto search restricted to the SRD
            EngineOptions eo;
            int k = plan.srdOf[s];
            eo.nodeOk = [&](int v) { return plan.srdOf[v] == k; };
            ParetoResult ref = paretoSearch(bt.g, s, eo);
            CHECK(sameTupleSets(tuplesOf(P), engineTuples(ref, d)), "intra dominant set s=%d d=%d", s, d);
            for (auto& p : P) for (int v : p.sats) CHECK(plan.srdOf[v] == k, "stays inside SRD");
            ++n;
        }
    CHECK(n > 100, "intra samples");
}

static void testResilience() {
    Walker w({12, 10, 550, 53, 1});
    TopologyOptions o; o.state = randomState(3);
    BuiltTopology bt = w.buildTopology(o);
    SrdPlan plan = makePlan(12, 10, 4, 5);
    Router R(w.gridNeighbors(), bt.g, plan, 256);
    std::mt19937 rng(5);
    int reserved = 0, recomputed = 0, trials = 0;
    for (int it = 0; it < 60; ++it) {
        int s = rng() % w.N(), d = rng() % w.N();
        if (s == d || plan.srdOf[s] == plan.srdOf[d]) continue;
        RouteResult r0 = R.route(s, d, Service::LS);
        if (!r0.ok || r0.best.sats.size() < 3) continue;
        size_t h = 1 + rng() % (r0.best.sats.size() - 1);
        FailureSet f; f.links.insert(linkKey(r0.best.sats[h - 1], r0.best.sats[h]));
        ++trials;
        RouteResult r1 = R.reroute(r0, s, d, Service::LS, f);
        CHECK(r1.ok, "reroute exists");
        CHECK(!f.blocksPath(r1.best.sats), "avoids failed link");
        (r1.recomputed ? recomputed : reserved)++;
        // Forced recompute must equal global Pareto/Dijkstra on the failed topology.
        RouteResult r2 = R.reroute(r0, s, d, Service::LS, f, {}, true);
        EngineOptions eo; eo.edgeOk = [&](int u, int v) { return !f.blocksLink(u, v); };
        ParetoResult ref = paretoSearch(bt.g, s, eo);
        CHECK(sameTupleSets(tuplesOf(r2.dominant), engineTuples(ref, d)), "recomputed set == global Pareto under failure");
        CHECK(std::fabs(r2.best.t.D - dijkstraD(bt.g, s, f)[d]) < 1e-9, "LS after failure == Dijkstra");
        // Reserved path never beats recomputation, and is a valid dominant-set member of the old set.
        CHECK(r1.best.t.D >= r2.best.t.D - 1e-9, "reserved path no better than optimum");
        // Node failure of an intermediate satellite.
        FailureSet fn; fn.nodes.insert(r0.best.sats[h]);
        if (r0.best.sats[h] != d) {
            RouteResult r3 = R.reroute(r0, s, d, Service::LS, fn, {}, true);
            CHECK(r3.ok && !fn.blocksPath(r3.best.sats), "node failure reroute");
            CHECK(std::fabs(r3.best.t.D - dijkstraD(bt.g, s, fn)[d]) < 1e-9, "node failure LS == Dijkstra");
        }
    }
    CHECK(trials > 15, "enough resilience trials");

    // No reserved path can survive when every ISL next to the destination fails on one side:
    // cut all but one link of d; the surviving dominant paths must all use that link, so failing
    // it too leaves nothing and the reroute must report "no route" after a recomputation.
    {
        int s = 0, d = w.index(8, 7);
        RouteResult r0 = R.route(s, d, Service::LS);
        FailureSet cut;
        for (auto& nb : w.neighbors(d)) cut.links.insert(linkKey(d, nb.id));
        RouteResult r1 = R.reroute(r0, s, d, Service::LS, cut);
        CHECK(!r1.ok && r1.recomputed, "isolated destination -> recomputed, no route");
        FailureSet part;  // leave exactly one ISL of d alive -> a reserved path (or recompute) must use it
        auto nbs = w.neighbors(d);
        for (size_t i = 1; i < nbs.size(); ++i) part.links.insert(linkKey(d, nbs[i].id));
        RouteResult r2 = R.reroute(r0, s, d, Service::LS, part);
        CHECK(r2.ok && r2.best.sats[r2.best.sats.size() - 2] == nbs[0].id, "uses the only live ISL");
        CHECK(std::fabs(r2.best.t.D - dijkstraD(bt.g, s, part)[d]) < 1e-9 || !r2.recomputed, "consistent");
        RouteResult r3 = R.reroute(r0, s, d, Service::LS, part, {}, true);
        CHECK(std::fabs(r3.best.t.D - dijkstraD(bt.g, s, part)[d]) < 1e-9, "forced recompute optimal");
    }
    std::printf("  resilience: %d trials, %d served from reserved dominant paths, %d recomputed\n", trials, reserved, recomputed);

    // Failure scopes & DS election.
    int in1 = -1, bd = -1, bd2 = -1;
    for (int v : plan.members[0]) { if (!R.isBoundary(v) && in1 < 0) in1 = v; }
    CHECK(R.classifyLinkFailure(0, 1) != UpdateScope::InterSrdCsib || plan.srdOf[0] != plan.srdOf[1], "scope sanity");
    for (int v = 0; v < w.N(); ++v)
        for (auto& nb : w.neighbors(v))
            if (plan.srdOf[nb.id] != plan.srdOf[v]) { bd = v; bd2 = nb.id; }
    CHECK(R.classifyLinkFailure(bd, bd2) == UpdateScope::InterSrdCsib, "case 4");
    if (in1 >= 0) {
        for (auto& nb : w.neighbors(in1))
            if (plan.srdOf[nb.id] == 0 && !R.isBoundary(nb.id)) { CHECK(R.classifyLinkFailure(in1, nb.id) == UpdateScope::IntraSrdLocal, "case 1"); break; }
    }
    int ds = R.electDs(0, FailureSet{});
    CHECK(ds == R.dsCandidates(0).front() && ds == plan.NP * plan.rect[0].qhi + plan.rect[0].xhi, "default DS = V(Qmax,Xmax)");
    FailureSet fd; fd.nodes.insert(ds);
    CHECK(R.electDs(0, fd) == R.dsCandidates(0)[1], "backup DS");
    CHECK(R.classifyNodeFailure(ds) == NodeFailureAction::SwitchToBackupDs, "DS failure");

    {   // SRD-BCM vote (an interpretation, see Router::voteDs): deterministic, a boundary satellite, ignores dead candidates
        std::vector<int> votes;
        const int v0 = R.voteDs(0, nullptr, &votes);
        CHECK(v0 >= 0 && R.isBoundary(v0) && plan.srdOf[v0] == 0, "vote picks a live boundary satellite of the SRD");
        CHECK(R.voteDs(0) == v0, "vote deterministic");
        long total = 0;
        const size_t nc = votes.size() / 2;
        for (size_t i = 0; i < nc; ++i) total += votes[nc + i];
        CHECK(total == (long)nc, "every voter casts exactly one vote");
        FailureSet fv; fv.nodes.insert(v0);
        const int v1 = R.voteDs(0, &fv);
        CHECK(v1 >= 0 && v1 != v0 && R.isBoundary(v1), "dead winner replaced");
    }
    LinkMonitor m(3);
    CHECK(!m.report(7, false) && !m.report(7, false) && m.report(7, false) && !m.report(7, false), "3 consecutive misses trigger once");
    CHECK(!m.report(8, false) && !m.report(8, true) && !m.report(8, false), "reset on reception");
    PeriodicTrigger pt(180);
    CHECK(pt.due(0) && !pt.due(100) && pt.due(180) && !pt.due(200), "periodic 3-min trigger");
}

static void testGeometryAndAccess() {
    Walker w({72, 22, 550, 53, 1});
    // ISL lengths are plausible, handover closes only high-latitude inter-orbit links.
    BuiltTopology full = w.buildTopology();
    TopologyOptions o; o.closeHighLatInterOrbit = true;
    BuiltTopology cut = w.buildTopology(o);
    size_t a = 0, b = 0;
    for (auto& v : full.g.adj) a += v.size();
    for (auto& v : cut.g.adj) b += v.size();
    CHECK(b < a && b > a * 0.8, "handover removes a minority of ISLs (%zu -> %zu)", a, b);
    CHECK(full.avgIntraD > 1.0 && full.avgIntraD < 10.0, "intra ISL ~ %.2f ms", full.avgIntraD);
    for (int i : {0, 100, 777}) CHECK(std::fabs(w.latitudeDeg(i, 0)) <= 53.0 + 1e-9, "|lat|<=inc");
    // GS access prefers access-stable satellites.
    SrdPlan plan = segment(72, 22, full.avgInterD, full.avgIntraD);
    Router R(w.gridNeighbors(), full.g, plan);
    auto vis = w.visible({22.3, 114.2}, 0.0, 25.0);
    CHECK(!vis.empty(), "GS sees satellites");
    std::vector<AccessCandidate> cand;
    for (auto& [s, r] : vis) cand.push_back({s, r});
    int sel = R.selectAccessSatellite(cand);
    int maxG = 0;
    for (auto& c : cand) maxG = std::max(maxG, R.gamma(c.sat));
    CHECK(sel >= 0 && R.gamma(sel) == maxG, "access picks highest gamma");
}

// Planner = what the OMNeT++ module calls: graph snapshot -> best path to one of several exit satellites.
static void testPlanner() {
    Walker w({12, 10, 550, 53, 1});
    TopologyOptions o; o.state = randomState(31);
    BuiltTopology bt = w.buildTopology(o);
    PlannerConfig pc; pc.planes = 12; pc.satsPerPlane = 10; pc.interPlaneSpacing = 1; pc.labelCap = 256;
    pc.fixedK = 4; pc.fixedL = 5;
    auto P = Planner::create(pc, bt.g);
    CHECK(P->srdPlan().r == 6, "planner SRD count");
    // Neighbour table inside the planner must equal the Walker's.
    Router ref(w.gridNeighbors(), bt.g, makePlan(12, 10, 4, 5), 256);
    for (int u = 0; u < w.N(); ++u) CHECK(ref.gamma(u) == P->router().gamma(u) && ref.isBoundary(u) == P->router().isBoundary(u), "same boundary/gamma u=%d", u);
    std::mt19937 rng(77);
    FailureSet none;
    for (int it = 0; it < 40; ++it) {
        int s = rng() % w.N();
        std::vector<Planner::Exit> ex;
        for (int k = 0; k < 3; ++k) ex.push_back({int(rng() % w.N()), PathTuple{1.0 + (rng() % 100) / 50.0, 0.5 + (rng() % 50) / 100.0, 0.0}});
        auto pl = P->plan(s, ex, Service::LS);
        CHECK(pl.ok, "plan ok");
        // LS optimum == min over exits of Dijkstra(s, exit) + GSL delay
        auto dj = dijkstraD(bt.g, s, none);
        double best = kInf;
        for (auto& e : ex) best = std::min(best, dj[e.sat] + e.gsl.D);
        // Paper Sec. III-D: same-SRD endpoints use intra-SRD routing only, so the global optimum is only
        // guaranteed when no exit shares the source's SRD; otherwise the plan can only be >= optimum.
        bool sameSrd = false;
        for (auto& e : ex) sameSrd |= P->srdPlan().srdOf[e.sat] == P->srdPlan().srdOf[s];
        if (!sameSrd) CHECK(std::fabs(pl.path.t.D - best) < 1e-9, "planner LS == min_e (dijkstra + gsl)");
        else CHECK(pl.path.t.D >= best - 1e-9, "planner LS >= global optimum when intra-only applies");
        CHECK(pl.path.sats.front() == s, "starts at source");
        bool isExit = false;
        for (auto& e : ex) isExit |= e.sat == pl.path.sats.back();
        CHECK(isExit, "ends at an exit satellite");
        auto ts = P->plan(s, ex, Service::TS);
        double bestA = -1;
        auto wd = widest(bt.g, s, none);
        for (auto& e : ex) bestA = std::max(bestA, std::min(wd[e.sat], e.gsl.A));
        CHECK(ts.ok, "TS plan ok");
        if (!sameSrd) CHECK(ts.path.t.A == bestA, "planner TS == max bottleneck incl. GSL");
        else CHECK(ts.path.t.A <= bestA, "planner TS <= global optimum when intra-only applies");
        // failure on the planned path -> still a valid, failure-free plan
        if (pl.path.sats.size() >= 3) {
            FailureSet f; f.links.insert(linkKey(pl.path.sats[1], pl.path.sats[2]));
            auto p2 = P->plan(s, ex, Service::LS, &f);
            if (p2.ok) CHECK(!f.blocksPath(p2.path.sats), "plan avoids failed link");
        }
    }
}

// Paper Fig. 1(c): 3x3 satellites in one SRD with the ISL tuples printed in the figure. The figure gives the
// dominant paths from V(1,1) to every satellite (Eq. 8/9 for V(3,3)); this reproduces them exactly.
static void testPaperFig1c() {
    // V(x,y): x = column 1..3 (left->right), y = row 1..3 (bottom->top), node id = (y-1)*3 + (x-1)
    auto id = [](int x, int y) { return (y - 1) * 3 + (x - 1); };
    Graph g(9);
    auto e = [&](int x1, int y1, int x2, int y2, double D, double A, double J) { g.addUndirected(id(x1, y1), id(x2, y2), PathTuple{D, A, J}); };
    e(1, 1, 2, 1, 14, 0.8, 4);   e(2, 1, 3, 1, 6, 0.9, 5);       // bottom row
    e(1, 2, 2, 2, 10, 0.8, 3);   e(2, 2, 3, 2, 14, 0.8, 10);     // middle row
    e(1, 3, 2, 3, 10, 0.8, 3);   e(2, 3, 3, 3, 18, 0.6, 10);     // top row
    e(1, 1, 1, 2, 18, 0.6, 5);   e(1, 2, 1, 3, 18, 0.6, 5);      // left column
    e(2, 1, 2, 2, 38, 0.2, 15);  e(2, 2, 2, 3, 24, 0.55, 10);    // middle column
    e(3, 1, 3, 2, 10, 0.8, 4);   e(3, 2, 3, 3, 20, 0.3, 5);      // right column
    ParetoResult r = paretoSearch(g, id(1, 1));
    struct Exp { int x, y; double D, A, J; int px, py; };         // expected single/first dominant path: tuple + previous hop
    const Exp one[] = {{2, 1, 14, 0.8, 4, 1, 1}, {3, 1, 20, 0.8, 5, 2, 1}, {1, 2, 18, 0.6, 5, 1, 1}, {3, 2, 30, 0.8, 5, 3, 1},
                       {1, 3, 36, 0.6, 5, 1, 2}, {2, 3, 46, 0.6, 5, 1, 3}};
    for (const Exp& x : one) {
        const auto& at = r.at[id(x.x, x.y)];
        // V(1,2): the figure prints only the direct path (18,0.6,5). The definition of the dominant set also keeps the
        // 5-hop detour 1,1 -> 2,1 -> 3,1 -> 3,2 -> 2,2 -> 1,2 with (54,0.8,10): higher A, but higher D and J, so it is not
        // dominated. The paper's round-based expansion never revisits a node it has already calculated, hence omits it.
        const size_t want = (x.x == 1 && x.y == 2) ? 2 : 1;
        CHECK(at.size() == want, "Fig1c V(%d,%d): %zu dominant paths (got %zu)", x.x, x.y, want, at.size());
        bool found = false;
        for (int li : at) {
            const PathTuple& t = r.pool[li].t;
            std::vector<int> nodes, exs;
            r.trace(li, nodes, exs);
            if (std::fabs(t.D - x.D) < 1e-9 && std::fabs(t.A - x.A) < 1e-12 && std::fabs(t.J - x.J) < 1e-12 &&
                nodes[nodes.size() - 2] == id(x.px, x.py))
                found = true;
            else if (want == 2)
                CHECK(std::fabs(t.D - 54) < 1e-9 && t.A == 0.8 && t.J == 10 && nodes.size() == 6, "Fig1c V(1,2) extra path is the (54,0.8,10) detour");
        }
        CHECK(found, "Fig1c V(%d,%d): figure's path (%g,%g,%g) via V(%d,%d) present", x.x, x.y, x.D, x.A, x.J, x.px, x.py);
    }
    // V(2,2) is not printed in the figure; just require a non-empty set that is internally consistent
    CHECK(!r.at[id(2, 2)].empty(), "Fig1c V(2,2) reachable");
    // V(3,3): two dominant paths, Eq. (8) and Eq. (9)
    const auto& at33 = r.at[id(3, 3)];
    CHECK(at33.size() == 2, "Fig1c V(3,3) has two dominant paths (got %zu)", at33.size());
    std::vector<DomPath> P;
    for (int li : at33) {
        DomPath d; d.t = r.pool[li].t;
        std::vector<int> ex;
        r.trace(li, d.sats, ex);
        P.push_back(d);
    }
    const std::vector<int> eq8 = {id(1, 1), id(1, 2), id(1, 3), id(2, 3), id(3, 3)};
    const std::vector<int> eq9 = {id(1, 1), id(2, 1), id(3, 1), id(3, 2), id(3, 3)};
    bool has8 = false, has9 = false;
    for (const auto& d : P) {
        if (d.sats == eq8) { has8 = true; CHECK(std::fabs(d.t.D - 64) < 1e-9 && d.t.A == 0.6 && d.t.J == 10, "Eq.(8) tuple (64,0.6,10)"); }
        if (d.sats == eq9) { has9 = true; CHECK(std::fabs(d.t.D - 50) < 1e-9 && d.t.A == 0.3 && d.t.J == 5, "Eq.(9) tuple (50,0.3,5)"); }
    }
    CHECK(has8 && has9, "Eq.(8) and Eq.(9) paths found");
    // service selection on the figure's two paths: LS -> shorter delay (Eq. 9), TS -> larger buffer (Eq. 8), HR (both J<15) -> larger A (Eq. 8)
    const DomPath* ls = selectPath(P, Service::LS);
    const DomPath* ts = selectPath(P, Service::TS);
    const DomPath* hr = selectPath(P, Service::HR);
    CHECK(ls && ls->sats == eq9, "LS picks Eq.(9)");
    CHECK(ts && ts->sats == eq8, "TS picks Eq.(8)");
    CHECK(hr && hr->sats == eq8, "HR picks Eq.(8)");
}


// Operation counters: invariants that tie the counts to what the search really did, and "counting is free of side effects".
static void testOpCounters() {
    Walker w({12, 10, 550, 53, 1});
    TopologyOptions o; o.state = randomState(41);
    BuiltTopology bt = w.buildTopology(o);
    // 1. Pareto search: every pushed queue entry is popped; every inserted label is alive or was deleted.
    for (int src : {0, 17, 63}) {
        OpCounts n;
        ParetoResult counted;
        { OpScope sc(&n); counted = paretoSearch(bt.g, src); }
        ParetoResult plain = paretoSearch(bt.g, src);   // no sink installed
        CHECK(counted.pool.size() == plain.pool.size(), "counting does not change the search");
        long alive = 0; for (auto& v : counted.at) alive += (long)v.size();
        CHECK(n.searches == 1, "one search");
        CHECK(n.pqPush == n.pqPop, "pq drained: push %llu pop %llu", (unsigned long long)n.pqPush, (unsigned long long)n.pqPop);
        CHECK(n.labelInsert + 1 == counted.pool.size(), "labelInsert == pool-1");
        CHECK(n.labelDelete == counted.pool.size() - (std::uint64_t)alive, "deleted == pool - alive");
        CHECK(n.labelExpand <= n.pqPop && n.labelExpand > 0, "expansions <= pops");
        CHECK(n.edgeChecks > 0 && n.dominanceCmp > 0, "edge/dominance work counted");
    }
    // 2. Router caches: the second identical query is served from the tables (no new Pareto tables), but still
    //    searches the boundary graph.
    SrdPlan plan = makePlan(12, 10, 4, 5);
    Router R(w.gridNeighbors(), bt.g, plan, 64);
    int s = -1, d = -1;   // non-boundary endpoints in different SRDs, so both per-endpoint tables are needed
    for (int i = 0; i < w.N() && s < 0; ++i) if (!R.isBoundary(i)) s = i;
    for (int i = 0; i < w.N() && d < 0; ++i) if (!R.isBoundary(i) && plan.srdOf[i] != plan.srdOf[s]) d = i;
    CHECK(s >= 0 && d >= 0, "found non-boundary pair");
    OpCounts first, second;
    { OpScope sc(&first); R.dominantPaths(s, d); }
    { OpScope sc(&second); R.dominantPaths(s, d); }
    CHECK(first.tableMiss > 0 && first.localBuilt > 0 && first.vedgesBuilt > 0, "first query builds tables/locals/vedges");
    CHECK(second.tableMiss == 0 && second.localBuilt == 0 && second.vedgesBuilt == 0 && second.tableHit > 0, "second query hits the cache");
    CHECK(second.boundaryGraphs == 1 && second.searches == 1, "boundary graph is searched again");
    CHECK(first.searches > second.searches, "cache removes searches");
    // 3. Reserve switch vs recompute.
    RouteResult r0 = R.route(s, d, Service::LS);
    CHECK(r0.ok && r0.best.sats.size() >= 3, "route for reserve test");
    FailureSet far; far.links.insert(linkKey(w.N() - 1, w.N() - 2));   // not on the path (checked below)
    if (!far.blocksPath(r0.best.sats)) {
        OpCounts n; RouteResult r1;
        { OpScope sc(&n); r1 = R.reroute(r0, s, d, Service::LS, far); }
        CHECK(!r1.recomputed && n.reserveSwitches == 1 && n.recomputes == 0 && n.searches == 0, "unaffected path: reserve kept, no search");
        CHECK(n.reserveChecks == r0.dominant.size(), "every stored candidate checked");
    }
    FailureSet all;   // kill every stored candidate -> recompute
    for (auto& p : r0.dominant) all.links.insert(linkKey(p.sats[0], p.sats[1]));
    { OpCounts n; RouteResult r2;
      { OpScope sc(&n); r2 = R.reroute(r0, s, d, Service::LS, all); }
      CHECK(r2.recomputed && n.recomputes == 1 && n.reserveSwitches == 0 && n.searches > 0, "all candidates dead: recompute with searches"); }
    // 4. nested scopes restore the previous sink
    OpCounts outer, inner;
    { OpScope a(&outer); { OpScope b(&inner); paretoSearch(bt.g, 0); } paretoSearch(bt.g, 1); }
    CHECK(inner.searches == 1 && outer.searches == 1 && g_ops == nullptr, "nested scopes");
}

// The planner exposes the whole dominant set (reserved candidates) and the router can rate a suffix of a stored path.
static void testReservedCandidates() {
    Walker w({12, 10, 550, 53, 1});
    TopologyOptions o; o.state = randomState(51);
    BuiltTopology bt = w.buildTopology(o);
    PlannerConfig pc; pc.planes = 12; pc.satsPerPlane = 10; pc.interPlaneSpacing = 1; pc.labelCap = 64; pc.fixedK = 4; pc.fixedL = 5;
    auto P = Planner::create(pc, bt.g);
    std::mt19937 rng(61);
    int checked = 0;
    for (int it = 0; it < 40; ++it) {
        int s = rng() % w.N();
        std::vector<Planner::Exit> ex{{int(rng() % w.N()), PathTuple{1.5, 0.9, 0.0}}, {int(rng() % w.N()), PathTuple{2.5, 0.8, 0.0}}};
        auto pl = P->plan(s, ex, Service::LS);
        if (!pl.ok) continue;
        CHECK(pl.front && pl.front->size() == pl.numDominant, "front holds the dominant set");
        bool found = false;
        for (auto& p : *pl.front) found |= p.sats == pl.path.sats;
        CHECK(found, "chosen path is a member of the stored set");
        // tuple of a suffix == tuple recomputed from its links; the full path equals the stored tuple minus the GSL
        for (auto& p : *pl.front) {
            if (p.sats.size() < 3) continue;
            std::vector<int> suf(p.sats.begin() + 1, p.sats.end());
            PathTuple full = P->router().pathTupleOf(p.sats);
            PathTuple tail = P->router().pathTupleOf(suf);
            CHECK(tail.D <= full.D + 1e-9 && tail.A >= full.A && tail.J <= full.J, "a suffix is never worse than the whole path");
            ++checked;
            break;
        }
    }
    CHECK(checked > 10, "enough suffix checks");
}


// ---- paper Algorithms 2/3 (R/F/T wavefront) against the label search and brute force ---------------------------------
static void testPaperWave() {
    // Algorithm 3 on the 12-satellite wrap-around grid: wavefront (re-expanding) == brute force == label search.
    {
        Walker w({3, 4, 550, 53, 1});
        for (int seed = 0; seed < 5; ++seed) {
            TopologyOptions o; o.state = randomState(seed);
            BuiltTopology bt = w.buildTopology(o);
            for (int s : {0, 5}) {
                ParetoResult wv = waveSearch(bt.g, s);
                for (int d = 0; d < w.N(); ++d) {
                    if (d == s) continue;
                    CHECK(sameTupleSets(engineTuples(wv, d), bruteForce(bt.g, s, d)), "wave==brute seed=%d s=%d d=%d", seed, s, d);
                }
            }
        }
    }
    // Algorithm 3 on a larger grid: wavefront == uncapped label search; literal variant is a subset of real paths.
    {
        Walker w({8, 8, 550, 53, 1});
        long litDiff = 0, litNodes = 0;
        for (int seed = 1; seed <= 3; ++seed) {
            TopologyOptions o; o.state = randomState(seed * 11);
            BuiltTopology bt = w.buildTopology(o);
            for (int s : {0, 13, 40}) {
                EngineOptions eo; eo.maxLabelsPerNode = 100000;
                ParetoResult ref = paretoSearch(bt.g, s, eo);
                ParetoResult wv = waveSearch(bt.g, s);
                WaveOptions lo; lo.reexpand = false;
                ParetoResult lit = waveSearch(bt.g, s, lo);
                for (int d = 0; d < w.N(); ++d) {
                    if (d == s) continue;
                    CHECK(sameTupleSets(engineTuples(wv, d), engineTuples(ref, d)), "wave==label search seed=%d s=%d d=%d", seed, s, d);
                    ++litNodes;
                    litDiff += !sameTupleSets(engineTuples(lit, d), engineTuples(ref, d));
                    CHECK(!engineTuples(lit, d).empty(), "literal reaches every satellite");
                }
            }
        }
        std::printf("  literal (no re-expansion) differs from the dominant set at %ld of %ld (satellite, source) pairs\n", litDiff, litNodes);
    }
    // Algorithm 2 + 3 through the Router: PaperWave == ParetoLabel (uncapped) for intra-SRD and inter-SRD pairs, with and
    // without failures; paths are real, loop-free and the literal variant returns valid but possibly incomplete sets.
    {
        Walker w({12, 10, 550, 53, 1});
        TopologyOptions o; o.state = randomState(7);
        BuiltTopology bt = w.buildTopology(o);
        int inter = 0, intra = 0, failCases = 0;
        long litDiff = 0, litPairs = 0;
        for (auto [K, L] : std::vector<std::pair<int, int>>{{4, 5}, {5, 4}, {3, 3}}) {
            SrdPlan plan = makePlan(12, 10, K, L);
            Router ref(w.gridNeighbors(), bt.g, plan, 100000);
            Router wv(w.gridNeighbors(), bt.g, plan, 100000);
            wv.setAlgorithm(SearchAlgorithm::PaperWave);
            Router lit(w.gridNeighbors(), bt.g, plan, 100000);
            lit.setAlgorithm(SearchAlgorithm::PaperWaveLiteral);
            std::mt19937 rng(K * 131 + L);
            for (int it = 0; it < 60; ++it) {
                int s = rng() % w.N(), d = rng() % w.N();
                if (s == d) continue;
                (plan.srdOf[s] == plan.srdOf[d] ? intra : inter)++;
                auto P = wv.dominantPaths(s, d);
                auto Q = ref.dominantPaths(s, d);
                CHECK(!P.empty(), "wave nonempty");
                CHECK(sameTupleSets(tuplesOf(P), tuplesOf(Q)), "wave router == label router (K=%d L=%d s=%d d=%d)", K, L, s, d);
                for (auto& p : P) {
                    CHECK(p.sats.front() == s && p.sats.back() == d, "wave endpoints");
                    CHECK(std::set<int>(p.sats.begin(), p.sats.end()).size() == p.sats.size(), "wave path is loop-free");
                    for (size_t i = 1; i < p.sats.size(); ++i) {
                        bool adj = false;
                        for (const Edge& e : bt.g.adj[p.sats[i - 1]]) adj |= e.to == p.sats[i];
                        CHECK(adj, "wave hop is a real ISL");
                    }
                }
                auto Lp = lit.dominantPaths(s, d);
                ++litPairs;
                litDiff += !sameTupleSets(tuplesOf(Lp), tuplesOf(Q));
                CHECK(!Lp.empty(), "literal nonempty");
                // failure on the middle link of the LS path
                const DomPath* b = selectPath(Q, Service::LS);
                if (b && b->sats.size() >= 3) {
                    FailureSet f;
                    size_t m = b->sats.size() / 2;
                    f.links.insert(linkKey(b->sats[m - 1], b->sats[m]));
                    auto Pf = wv.dominantPaths(s, d, &f);
                    auto Qf = ref.dominantPaths(s, d, &f);
                    ++failCases;
                    CHECK(sameTupleSets(tuplesOf(Pf), tuplesOf(Qf)), "wave == label under failure (K=%d L=%d s=%d d=%d)", K, L, s, d);
                    for (auto& p : Pf) CHECK(!f.blocksPath(p.sats), "wave path avoids failure");
                }
            }
        }
        CHECK(inter > 40 && intra > 10 && failCases > 40, "enough wave samples %d %d %d", inter, intra, failCases);
        std::printf("  literal pseudo-code differs from the dominant set for %ld of %ld router pairs\n", litDiff, litPairs);
    }
}

int main() {
    auto t0 = std::chrono::steady_clock::now();
    std::printf("[algebra]\n");      testAlgebra();
    std::printf("[segmentation]\n"); testSegmentation();
    std::printf("[paper Fig.1(c) example]\n"); testPaperFig1c();
    std::printf("[engine vs brute force]\n"); testEngineVsBruteForce();
    std::printf("[router vs global reference]\n"); testRouterAgainstGlobal();
    std::printf("[segmented vs exhaustive enumeration]\n"); testSegmentedVsBruteForce();
    std::printf("[asymmetric (directed) links]\n"); testAsymmetricLinks();
    std::printf("[capped label front keeps extremes]\n"); testCappedExtremes();
    std::printf("[intra-SRD]\n");    testIntraSrd();
    std::printf("[resilience]\n");   testResilience();
    std::printf("[planner]\n");      testPlanner();
    std::printf("[geometry/access]\n"); testGeometryAndAccess();
    std::printf("[operation counters]\n"); testOpCounters();
    std::printf("[reserved candidates]\n"); testReservedCandidates();
    std::printf("[paper Algorithms 2/3 wavefront]\n"); testPaperWave();
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("\n%d checks, %d failed (%.1fs)\n", g_checks, g_fail, sec);
    return g_fail ? 1 : 0;
}
