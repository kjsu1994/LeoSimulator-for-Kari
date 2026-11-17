// V10 / V11-a verification (core level, no packets):
//   V10   reserve (stored dominant) paths after an ISL cut: existence rate vs. feasible suffix switch at the detecting satellite,
//         whether a recomputation happened, and correctness of the recomputed set against exhaustive enumeration (24 satellites)
//   V11-a failure -> detection -> CSIB collect/compute/disseminate -> per-satellite application, for EVERY link and EVERY
//         satellite of a Starlink-S1-sized constellation (test_csib checks one representative link per case)
// V11-b (packet delivery recovery) is NOT covered: it needs packets. The detection time (600 ms) is an assumption, not a paper value.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <random>
#include <set>

#include "mcsr/csib.h"
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
static bool sameTupleSets(const std::vector<PathTuple>& a, const std::vector<PathTuple>& b) {
    auto same = [](const PathTuple& x, const PathTuple& y) {
        return std::fabs(x.D - y.D) <= 1e-9 && std::fabs(x.A - y.A) <= 1e-12 && std::fabs(x.J - y.J) <= 1e-12;
    };
    auto covers = [&](const std::vector<PathTuple>& u, const std::vector<PathTuple>& v) {
        for (const auto& x : u) { bool f = false; for (const auto& y : v) if (same(x, y)) { f = true; break; } if (!f) return false; }
        return true;
    };
    return covers(a, b) && covers(b, a);
}
static std::vector<PathTuple> tuplesOf(const std::vector<DomPath>& v) { std::vector<PathTuple> t; for (auto& p : v) t.push_back(p.t); return t; }

static Graph withoutLink(const Graph& g, int u, int v) {
    Graph h(g.size());
    for (int a = 0; a < g.size(); ++a)
        for (const Edge& e : g.adj[a]) if (!((a == u && e.to == v) || (a == v && e.to == u))) h.adj[a].push_back(e);
    return h;
}
// Tuple of a path from first principles (plain loops over the geometry and the link-state function).
static bool indepTuple(const Walker& w, const TopologyOptions& o, const std::vector<int>& p, PathTuple& out) {
    auto grid = w.gridNeighbors();
    double D = 0, A = 1e300, J = -1e300;
    for (size_t i = 1; i < p.size(); ++i) {
        const auto& nb = grid[p[i - 1]];
        if (std::find(nb.begin(), nb.end(), p[i]) == nb.end()) return false;
        D += w.distanceKm(p[i - 1], p[i], o.t) / Walker::kC * 1000.0;
        auto aj = o.state(std::min(p[i - 1], p[i]), std::max(p[i - 1], p[i]), o.t);
        A = std::min(A, aj.first); J = std::max(J, aj.second);
    }
    out = {D, A, J};
    return true;
}

// exhaustive Pareto set over all simple paths of g (restricted to `allow` when given), streaming
static std::vector<PathTuple> exhaustiveFront(const Graph& g, int s, int d, const std::vector<char>* allow) {
    std::vector<PathTuple> front;
    std::vector<char> vis(g.size(), 0);
    std::function<void(int, PathTuple)> dfs = [&](int u, PathTuple t) {
        if (u == d) {
            for (const PathTuple& f : front) if (weaklyDominates(f, t)) return;
            front.erase(std::remove_if(front.begin(), front.end(), [&](const PathTuple& f) { return dominates(t, f); }), front.end());
            front.push_back(t);
            return;
        }
        vis[u] = 1;
        for (const Edge& e : g.adj[u]) if (!vis[e.to] && (!allow || (*allow)[e.to])) dfs(e.to, PathTuple{t.D + e.t.D, std::min(t.A, e.t.A), std::max(t.J, e.t.J)});
        vis[u] = 0;
    };
    dfs(s, PathTuple{0.0, kInf, 0.0});
    return front;
}

// ---- V10 part 1: reserve paths, 120 satellites, simulator label cap (32) and uncapped ------------------------------
static void testReserve() {
    Walker w({12, 10, 550, 53, 1});
    TopologyOptions o; o.state = randomState(77);
    BuiltTopology bt = w.buildTopology(o);
    SrdPlan plan = makePlan(12, 10, 4, 5);
    struct Acc { long cases = 0, exist = 0, suffix = 0, suffixNotExist = 0, existNotSuffix = 0, loops = 0, rec = 0, recSearches = 0, resSearches = 0, sw = 0;
                 double excess = 0; long excessN = 0; };
    const size_t caps[2] = {32, 100000};
    const Service svs[3] = {Service::LS, Service::TS, Service::HR};
    const char* sn[3] = {"LS", "TS", "HR"};
    Acc acc[2][3];
    for (int ci = 0; ci < 2; ++ci) {
        Router R(w.gridNeighbors(), bt.g, plan, caps[ci]);
        std::mt19937 rng(2024);
        for (int it = 0; it < 70; ++it) {
            int s = rng() % w.N(), d = rng() % w.N();
            if (s == d) continue;
            for (int k = 0; k < 3; ++k) {
                RouteResult prev = R.route(s, d, svs[k]);
                if (!prev.ok || prev.best.sats.size() < 3) continue;
                const auto& act = prev.best.sats;
                for (size_t i = 1; i < act.size(); ++i) {          // cut every link of the active path, one at a time
                    FailureSet f; f.links.insert(linkKey(act[i - 1], act[i]));
                    const int det = act[i - 1];                       // satellite in front of the dead link
                    Acc& A = acc[ci][k];
                    ++A.cases;
                    // existence: some stored path avoids the cut entirely (what reroute() can use from the source)
                    bool exist = false;
                    for (const DomPath& p : prev.dominant) exist |= !f.blocksPath(p.sats);
                    // feasible switch at the detecting satellite: a stored path through det whose SUFFIX avoids the cut
                    bool suffix = false, loop = false;
                    for (const DomPath& p : prev.dominant) {
                        auto pos = std::find(p.sats.begin(), p.sats.end(), det);
                        if (pos == p.sats.end()) continue;
                        std::vector<int> suf(pos, p.sats.end());
                        if (f.blocksPath(suf)) continue;
                        suffix = true;
                        std::set<int> pre(act.begin(), act.begin() + i);
                        for (size_t j = 1; j < suf.size(); ++j) if (pre.count(suf[j])) loop = true;
                    }
                    A.exist += exist; A.suffix += suffix; A.suffixNotExist += suffix && !exist; A.existNotSuffix += exist && !suffix; A.loops += suffix && loop;
                    // what reroute() does
                    OpCounts n; RouteResult r;
                    { OpScope sc(&n); r = R.reroute(prev, s, d, svs[k], f); }
                    CHECK(r.recomputed == !exist, "V10 reroute recomputes exactly when no stored path survives (%s cap=%zu)", sn[k], caps[ci]);
                    if (!r.recomputed) { ++A.sw; A.resSearches += (long)n.searches; CHECK(n.recomputes == 0 && n.searches == 0 && n.reserveSwitches == 1, "V10 reserve switch performs no search"); }
                    else { ++A.rec; A.recSearches += (long)n.searches; CHECK(n.recomputes == 1 && n.searches > 0, "V10 recomputation performs searches"); }
                    CHECK(r.ok, "V10 a route exists after the cut");
                    if (!r.ok) continue;
                    CHECK(!f.blocksPath(r.best.sats), "V10 chosen path avoids the failed link");
                    PathTuple t; CHECK(indepTuple(w, o, r.best.sats, t) && std::fabs(t.D - r.best.t.D) < 1e-9 && t.A == r.best.t.A && t.J == r.best.t.J, "V10 chosen path tuple recomputed independently");
                    if (!r.recomputed) {   // the reserve must be one of the stored paths and the best of the surviving ones
                        bool member = false; for (auto& p : prev.dominant) member |= p.sats == r.best.sats;
                        CHECK(member, "V10 reserve path is a stored path");
                        std::vector<DomPath> surv; for (auto& p : prev.dominant) if (!f.blocksPath(p.sats)) surv.push_back(p);
                        const DomPath* b = selectPath(surv, svs[k]);
                        CHECK(b && b->sats == r.best.sats, "V10 reserve = service-best of the surviving stored paths");
                    }
                    if (k == 0) {   // LS: a reserve can never beat the recomputed optimum (cap=1e5 optimum is exact)
                        RouteResult fresh = R.reroute(prev, s, d, Service::LS, f, SelectionParams{}, true);
                        CHECK(r.best.t.D >= fresh.best.t.D - 1e-9, "V10 reserve D >= recomputed optimum D");
                        if (caps[ci] == 100000) { A.excess += (r.best.t.D - fresh.best.t.D) / fresh.best.t.D; ++A.excessN; }
                    }
                }
            }
        }
    }
    std::printf("  V10  120 satellites, SRD 4x5; every link of the active path cut once; metrics are shares of those cuts\n");
    std::printf("       cap   svc  cuts  stored-path-survives(E)  valid-suffix-at-detector(F)  F&!E  E&!F  F with prefix loop  recomputed  searches/recompute  LS excess delay (reserve)\n");
    for (int ci = 0; ci < 2; ++ci)
        for (int k = 0; k < 3; ++k) {
            const Acc& A = acc[ci][k];
            std::printf("       %-6zu %-3s %5ld  %6.1f%%                  %6.1f%%                    %4ld  %4ld  %5ld              %5.1f%%      %8.0f            %s\n", caps[ci], sn[k], A.cases,
                        100.0 * A.exist / A.cases, 100.0 * A.suffix / A.cases, A.suffixNotExist, A.existNotSuffix, A.loops, 100.0 * A.rec / A.cases,
                        A.rec ? double(A.recSearches) / A.rec : 0.0, A.excessN ? (std::to_string(100.0 * A.excess / A.excessN).substr(0, 5) + " %").c_str() : "-");
        }
}

// ---- V10 part 2: recomputed set after a cut == exhaustive set on the cut graph (24 satellites) -------------------------
static void testRecomputeExact() {
    Walker w({4, 6, 550, 53, 1});
    TopologyOptions o; o.state = randomState(5);
    BuiltTopology bt = w.buildTopology(o);
    long cases = 0, inter = 0, intra = 0, capHits = 0;
    for (auto [K, L] : std::vector<std::pair<int, int>>{{2, 2}, {2, 3}}) {
        SrdPlan plan = makePlan(4, 6, K, L);
        for (SearchAlgorithm a : {SearchAlgorithm::ParetoLabel, SearchAlgorithm::PaperWave}) {
            Router R(w.gridNeighbors(), bt.g, plan, 100000);
            R.setAlgorithm(a);
            for (int d = 1; d < w.N(); ++d) {
                const int s = 0;
                RouteResult prev = R.route(s, d, Service::LS);
                if (!prev.ok || prev.best.sats.size() < 2) continue;
                const auto& act = prev.best.sats;
                std::set<size_t> pick{1, act.size() / 2 < 1 ? 1 : act.size() / 2, act.size() - 1};
                for (size_t i : pick) {
                    FailureSet f; f.links.insert(linkKey(act[i - 1], act[i]));
                    Graph cut = withoutLink(bt.g, act[i - 1], act[i]);
                    std::vector<char> allow(w.N(), 1);
                    const bool same = plan.srdOf[s] == plan.srdOf[d];
                    if (same) { std::fill(allow.begin(), allow.end(), 0); for (int m : plan.members[plan.srdOf[s]]) allow[m] = 1; }
                    OpCounts n; std::vector<DomPath> P;
                    { OpScope sc(&n); P = R.dominantPaths(s, d, &f); }
                    capHits += (long)(n.capEvict + n.capReject);
                    auto ref = exhaustiveFront(cut, s, d, same ? &allow : nullptr);
                    ++cases; (same ? intra : inter)++;
                    CHECK(sameTupleSets(tuplesOf(P), ref), "V10 recomputed set == exhaustive set on the cut graph (algo=%d K=%d L=%d d=%d cut=%zu |P|=%zu |ref|=%zu)", int(a), K, L, d, i, P.size(), ref.size());
                    for (auto& p : P) CHECK(!f.blocksPath(p.sats), "V10 recomputed path avoids the cut");
                }
            }
        }
    }
    CHECK(capHits == 0, "V10 label cap not reached in the exact comparison");
    std::printf("  V10  recomputation after a cut vs exhaustive enumeration on the cut graph (24 satellites): %ld cases (%ld inter-SRD, %ld intra-SRD), label cap hits %ld\n", cases, inter, intra, capHits);
}

// ---- V11-a: timeline of the CSIB update after every link / satellite failure of a Starlink-S1-sized constellation --------
struct Phase { std::vector<double> apply; long n = 0; double det = 0, col = 0, colMax = 0, dis = 0, disMax = 0, tot = 0, totMax = 0, first = 0, firstMax = 0; long unreach = 0, lost = 0; };
static void addPhase(Phase& p, const CsibVersion& v) {
    ++p.n; p.det += v.detectS; p.col += v.collectS; p.colMax = std::max(p.colMax, v.collectS);
    p.dis += v.disseminateS; p.disMax = std::max(p.disMax, v.disseminateS); p.tot += v.totalS; p.totMax = std::max(p.totMax, v.totalS);
    double fa = CsibModel::kNever;                      // earliest arrival at any member other than the DS
    for (size_t i = 0; i < v.members.size(); ++i) if (v.members[i] != v.ds && v.arrival[i] < CsibModel::kNever) fa = std::min(fa, v.arrival[i] - v.triggerTime);
    if (fa < CsibModel::kNever) { p.first += fa; p.firstMax = std::max(p.firstMax, fa); }
    for (size_t i = 0; i < v.members.size(); ++i) if (v.arrival[i] < CsibModel::kNever) p.apply.push_back((v.arrival[i] - v.triggerTime) * 1000.0);   // ms after the failure
    p.unreach += v.unreachable; p.lost += v.lostReports;
}
static double pct(std::vector<double> v, double q) { if (v.empty()) return 0; std::sort(v.begin(), v.end()); return v[std::min(v.size() - 1, size_t(q * (v.size() - 1) + 0.5))]; }
static FILE* g_csv = nullptr;
static void csvVersion(FILE* f, const char* scen, const CsibVersion& v, const SrdPlan& plan, const std::vector<char>& bnd) {
    for (size_t i = 0; i < v.members.size(); ++i)
        std::fprintf(f, "%s,%d,%d,%d,%d,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n", scen, v.srd, v.members[i], int(bnd[v.members[i]]), int(v.members[i] == v.ds), v.triggerTime, v.dataTime, v.collectDone, v.computeDone,
                     v.arrival[i] < CsibModel::kNever ? v.arrival[i] : -1.0, v.arrival[i] < CsibModel::kNever ? (v.arrival[i] - v.triggerTime) * 1000.0 : -1.0);
    (void)plan;
}
// Independent per-member delay DS -> member over the post-failure graph inside one SRD (Bellman-Ford, directed edges).
static std::vector<double> bellmanFord(const SrdPlan& plan, const Graph& g, const FailureSet& f, int srd, int src) {
    std::vector<double> d(g.size(), CsibModel::kNever);
    if (f.blocksNode(src)) return d;
    d[src] = 0;
    for (bool ch = true; ch;) {
        ch = false;
        for (int u : plan.members[srd]) {
            if (d[u] >= CsibModel::kNever || f.blocksNode(u)) continue;
            for (const Edge& e : g.adj[u])
                if (plan.srdOf[e.to] == srd && !f.blocksNode(e.to) && !f.blocksLink(u, e.to) && d[u] + e.t.D < d[e.to]) { d[e.to] = d[u] + e.t.D; ch = true; }
        }
    }
    return d;
}
static void checkVersion(const CsibModel& m, const SrdPlan& plan, const Graph& g, const FailureSet& f, const CsibVersion& v, double tFail, double detectS, const char* tag) {
    CHECK(v.ok, "%s version generated", tag);
    if (!v.ok) return;
    // ordering of the pipeline timestamps
    CHECK(std::fabs(v.triggerTime - tFail) < 1e-12 && std::fabs(v.dataTime - (tFail + detectS)) < 1e-12, "%s: failure -> detection = %g s", tag, detectS);
    CHECK(v.dataTime <= v.electDone && v.electDone <= v.collectDone && v.collectDone <= v.computeDone, "%s: detect <= elect <= collect <= compute", tag);
    CHECK(std::fabs(v.totalS - (v.detectS + v.electS + v.collectS + v.computeS + v.disseminateS)) < 1e-9, "%s: total = sum of the phases", tag);
    // per-satellite application: arrival = computeDone + independent delay DS -> member, never before computeDone
    auto bf = bellmanFord(plan, g, f, v.srd, v.ds);
    int unreach = 0;
    for (size_t i = 0; i < v.members.size(); ++i) {
        const int sat = v.members[i];
        const bool dead = f.blocksNode(sat) || bf[sat] >= CsibModel::kNever;
        if (dead) { ++unreach; CHECK(v.arrival[i] >= CsibModel::kNever, "%s: unreachable member never receives", tag); continue; }
        CHECK(std::fabs(v.arrival[i] - (v.computeDone + bf[sat] / 1000.0)) < 1e-9, "%s: arrival at %d = computeDone + independent DS->member delay", tag, sat);
        CHECK(v.arrival[i] >= v.computeDone - 1e-12, "%s: no member applies before the CSIB exists", tag);
        // gating by viewAt: just before arrival the satellite does NOT use this version, at arrival it uses this one or a newer one
        const CsibVersion* before = m.viewAt(sat, v.arrival[i] - 1e-9);
        CHECK(!before || before->id != v.id, "%s: %d does not use the version before it arrives", tag, sat);
        const CsibVersion* at = m.viewAt(sat, v.arrival[i]);
        CHECK(at && at->dataTime >= v.dataTime, "%s: %d uses the version (or newer) when it arrives", tag, sat);
    }
    CHECK(unreach == v.unreachable, "%s: unreachable count (%d vs %d)", tag, unreach, v.unreachable);
}

static void testTimeline() {
    Walker w({72, 22, 550, 53, 1});
    TopologyOptions o;
    BuiltTopology bt0 = w.buildTopology(o);
    SrdPlan plan = segment(72, 22, bt0.avgInterD, bt0.avgIntraD);          // Starlink S1: K=4, L=9 (paper-literal flood model)
    CsibParams cp;                                                         // defaults: computeMs=0, no processing, detect 600 ms (assumption)
    std::printf("  V11-a Starlink S1 (N=%d, %d SRDs, SRD %dx%d), compute %.0f ms, detection %.0f ms (assumption), hop/report processing 0, serialisation ignored\n",
                w.N(), plan.r, plan.K, plan.L, cp.computeMs, cp.detectMs);
    // independent boundary / corner definitions from the static grid and the SRD rectangles
    auto grid = w.gridNeighbors();
    std::vector<char> bnd(w.N(), 0);
    for (int u = 0; u < w.N(); ++u) for (int v : grid[u]) if (plan.srdOf[v] != plan.srdOf[u]) bnd[u] = 1;
    auto upperRight = [&](int srd) { return plan.NP * plan.rect[srd].qhi + plan.rect[srd].xhi; };
    Phase ph[4]; long linksSeen[4] = {0, 0, 0, 0}, regen[4] = {0, 0, 0, 0};
    std::set<LinkKey> done;
    long versions = 0;
    for (int u = 0; u < w.N(); ++u)
        for (const Edge& e : bt0.g.adj[u]) {
            const int v = e.to;
            if (!done.insert(linkKey(u, v)).second) continue;
            const int cas = plan.srdOf[u] != plan.srdOf[v] ? 3 : (!bnd[u] && !bnd[v]) ? 0 : (bnd[u] && bnd[v]) ? 2 : 1;   // paper cases 1..4
            Graph g = withoutLink(bt0.g, u, v);
            FailureSet f; f.links.insert(linkKey(u, v));
            Router R(grid, g, plan, 32);
            CsibModel m(plan, cp);
            m.initialize(R, g, FailureSet{});
            const double tFail = 10.0 + (u % 7) * 0.1;
            auto srds = m.onLinkFailure(tFail, u, v, R, g, f);
            ++linksSeen[cas];
            std::vector<int> expect;
            if (cas == 1 || cas == 2) expect = {plan.srdOf[u]};
            if (cas == 3) expect = {plan.srdOf[u], plan.srdOf[v]};
            CHECK(srds == expect, "V09 link case %d regenerates exactly the SRDs of the paper's case (u=%d v=%d)", cas + 1, u, v);
            for (int k : srds) {
                const CsibVersion& ver = m.versions(k).back();
                CHECK(ver.cause == CsibCause::LinkFailure && ver.id == 1, "link failure version");
                if (versions++ % 7 == 0 || cas == 3) checkVersion(m, plan, g, f, ver, tFail, cp.detectMs / 1000.0, "link");
                else { CHECK(ver.ok && ver.computeDone >= ver.dataTime && ver.totalS >= ver.detectS, "link: ordering (sampled)"); }
                if (g_csv && ph[cas].n == 0) { static const char* nm[4] = {"link_case1", "link_case2", "link_case3", "link_case4"}; csvVersion(g_csv, nm[cas], ver, plan, bnd); }
                addPhase(ph[cas], ver); ++regen[cas];
            }
        }
    const char* cn[4] = {"case 1 interior-interior", "case 2 interior-boundary", "case 3 boundary-boundary (same SRD)", "case 4 inter-SRD"};
    std::printf("       link failures (every one of the %zu links cut once):\n", done.size());
    std::printf("       %-38s links  CSIB made  detect  collect mean/max  disseminate mean/max  failure->last member mean/max [ms]  first other member mean [ms]  unreachable  lost reports\n", "scenario");
    for (int c = 0; c < 4; ++c) {
        const Phase& p = ph[c];
        if (!p.n) { std::printf("       %-38s %5ld  %9ld  (none: update stays inside the SRD, no CSIB)\n", cn[c], linksSeen[c], 0L); continue; }
        std::printf("       %-38s %5ld  %9ld  %5.0f   %7.2f / %7.2f   %8.2f / %8.2f      %9.2f / %9.2f                %9.2f            %5ld        %5ld\n", cn[c], linksSeen[c], p.n, 1000 * p.det / p.n,
                    1000 * p.col / p.n, 1000 * p.colMax, 1000 * p.dis / p.n, 1000 * p.disMax, 1000 * p.tot / p.n, 1000 * p.totMax, 1000 * p.first / p.n, p.unreach, p.lost);
    }
    std::printf("       per-satellite application delay [ms after the failure], over every (version, member) that receives it:\n");
    for (int c = 0; c < 4; ++c) if (ph[c].n) std::printf("         %-38s n=%zu  min %.2f  median %.2f  p95 %.2f  max %.2f\n", cn[c], ph[c].apply.size(), pct(ph[c].apply, 0), pct(ph[c].apply, .5), pct(ph[c].apply, .95), pct(ph[c].apply, 1));
    // satellite failures: every satellite once
    Phase np[3]; long nSeen[3] = {0, 0, 0};
    long nv = 0;
    for (int v = 0; v < w.N(); ++v) {
        Graph g(w.N());                      // post-failure snapshot: the satellite's edges are gone
        for (int a = 0; a < w.N(); ++a) { if (a == v) continue; for (const Edge& e : bt0.g.adj[a]) if (e.to != v) g.adj[a].push_back(e); }
        FailureSet f; f.nodes.insert(v);
        Router R(grid, g, plan, 32);
        CsibModel m(plan, cp);
        m.initialize(R, bt0.g, FailureSet{});
        const int k = plan.srdOf[v];
        const int scen = v == upperRight(k) ? 2 : bnd[v] ? 1 : 0;           // paper scenarios 1..3
        const double tFail = 20.0;
        auto srds = m.onNodeFailure(tFail, v, R, g, f);
        ++nSeen[scen];
        std::vector<int> expect;
        if (scen == 2) expect = {k};
        if (scen == 1) { expect = {k}; for (int vn : grid[v]) { int o2 = plan.srdOf[vn]; if (o2 != k && std::find(expect.begin(), expect.end(), o2) == expect.end()) expect.push_back(o2); } }
        CHECK(srds == expect, "V09 satellite scenario %d regenerates the paper's SRDs (v=%d)", scen + 1, v);
        for (int s : srds) {
            const CsibVersion& ver = m.versions(s).back();
            CHECK(ver.cause == (scen == 2 ? CsibCause::DsSwitch : CsibCause::NodeFailure), "node failure cause");
            if (scen == 2) {   // backup DS = first live corner in the paper's order (upper-right is the failed DS)
                const SrdRect& r = plan.rect[k];
                int want = -1;
                for (auto [q, x] : {std::pair{r.qhi, r.xhi}, {r.qhi, r.xlo}, {r.qlo, r.xlo}, {r.qlo, r.xhi}}) { int id = plan.NP * q + x; if (id != v) { want = id; break; } }
                CHECK(ver.ds == want && ver.prevDs == v, "V08 backup DS is the next corner (%d vs %d)", ver.ds, want);
            }
            if (nv++ % 5 == 0 || scen == 2) checkVersion(m, plan, g, f, ver, tFail, cp.detectMs / 1000.0, "node");
            if (g_csv && np[scen].n == 0) { static const char* nm[3] = {"node_scen1", "node_scen2", "node_scen3"}; csvVersion(g_csv, nm[scen], ver, plan, bnd); }
            addPhase(np[scen], ver);
        }
    }
    const char* nn[3] = {"scenario 1 interior satellite", "scenario 2 boundary satellite (non-DS)", "scenario 3 DS"};
    std::printf("       satellite failures (every one of the %d satellites failed once):\n", w.N());
    for (int c = 0; c < 3; ++c) {
        const Phase& p = np[c];
        if (!p.n) { std::printf("       %-38s %5ld  %9d  (none: intra-SRD recomputation only, no CSIB)\n", nn[c], nSeen[c], 0); continue; }
        std::printf("       %-38s %5ld  %9ld  %5.0f   %7.2f / %7.2f   %8.2f / %8.2f      %9.2f / %9.2f                %9.2f            %5ld        %5ld\n", nn[c], nSeen[c], p.n, 1000 * p.det / p.n,
                    1000 * p.col / p.n, 1000 * p.colMax, 1000 * p.dis / p.n, 1000 * p.disMax, 1000 * p.tot / p.n, 1000 * p.totMax, 1000 * p.first / p.n, p.unreach, p.lost);
    }
    std::printf("       per-satellite application delay [ms after the failure]:\n");
    for (int c = 0; c < 3; ++c) if (np[c].n) std::printf("         %-38s n=%zu  min %.2f  median %.2f  p95 %.2f  max %.2f\n", nn[c], np[c].apply.size(), pct(np[c].apply, 0), pct(np[c].apply, .5), pct(np[c].apply, .95), pct(np[c].apply, 1));
}

int main(int argc, char** argv) {
    if (argc > 1) { g_csv = std::fopen(argv[1], "w"); if (g_csv) std::fprintf(g_csv, "scenario,srd,sat,isBoundary,isDs,tFail,tDetected,collectDone,computeDone,appliedAt,appliedAfterFailureMs\n"); }
    auto t0 = std::chrono::steady_clock::now();
    std::printf("[V10 reserve paths]\n"); testReserve();
    std::printf("[V10 recomputation exactness]\n"); testRecomputeExact();
    std::printf("[V11-a failure -> detection -> CSIB -> application]\n"); testTimeline();
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (g_csv) std::fclose(g_csv);
    std::printf("\n%d checks, %d failed (%.1fs)\n", g_checks, g_fail, sec);
    return g_fail ? 1 : 0;
}
