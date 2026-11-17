// DS / CSIB time model (csib.h): initial DS, periodic and monitor versions, SRD-BCM vote, link and satellite failures,
// per-satellite application (a version is used only after it has arrived), dissemination over the live intra-SRD ISLs.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <queue>
#include <set>
#include <unordered_map>

#include "mcsr/csib.h"
#include "mcsr/router.h"
#include "mcsr/walker.h"

using namespace mcsr;

static int g_fail = 0, g_checks = 0;
#define CHECK(c, ...) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("FAIL %s:%d  %s  ", __FILE__, __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
static bool near(double a, double b, double tol = 1e-9) { return std::fabs(a - b) <= tol; }
static constexpr double kInfT = CsibModel::kNever;

static std::uint64_t mix(std::uint64_t z) {
    z += 0x9e3779b97f4a7c15ULL;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

// Independent reference: shortest delay [ms] src -> every satellite of SRD k over live intra-SRD ISLs, each hop costs D + extraMs.
static std::unordered_map<int, double> refDist(const SrdPlan& plan, const Graph& g, int k, int src, const FailureSet& f, double extraMs) {
    std::unordered_map<int, double> d;
    for (int m : plan.members[k]) d[m] = kInfT;
    if (f.blocksNode(src)) return d;
    d[src] = 0;
    std::set<std::pair<double, int>> q{{0.0, src}};
    while (!q.empty()) {
        auto [du, u] = *q.begin();
        q.erase(q.begin());
        for (const Edge& e : g.adj[u]) {
            if (plan.srdOf[e.to] != k || f.blocksNode(e.to) || f.blocksLink(u, e.to)) continue;
            const double nd = du + e.t.D + extraMs;
            if (nd < d[e.to]) {
                q.erase({d[e.to], e.to});
                d[e.to] = nd;
                q.insert({nd, e.to});
            }
        }
    }
    return d;
}

struct Fixture {
    Walker w{{12, 10, 550.0, 53.0, 1}};
    BuiltTopology bt;
    SrdPlan plan;
    std::unique_ptr<Router> R;
    Fixture() {
        TopologyOptions o;
        o.state = [](int u, int v, double) {
            std::uint64_t h = mix(linkKey(u, v) ^ mix(3));
            double a = (h & 0xffff) / 65535.0, b = ((h >> 16) & 0xffff) / 65535.0;
            return std::make_pair(0.1 + 0.9 * a, 0.5 + 20.0 * b * b);
        };
        bt = w.buildTopology(o);
        plan = makePlan(12, 10, 4, 5);
        R.reset(new Router(w.gridNeighbors(), bt.g, plan, 256));
    }
};

int main() {
    Fixture fx;
    Router& R = *fx.R;
    const SrdPlan& plan = fx.plan;
    const Graph& g = fx.bt.g;
    const FailureSet none;
    std::printf("SRDs=%d members/SRD=%zu\n", plan.r, plan.members[0].size());

    // ---- initial installation ---------------------------------------------------------------------------------------------
    {
        CsibModel m(plan, CsibParams{});
        m.initialize(R, g);
        for (int k = 0; k < plan.r; ++k) {
            CHECK(m.ds(k) == R.dsCandidates(k).front(), "initial DS = upper-right corner, srd %d", k);
            CHECK(R.isBoundary(m.ds(k)), "DS is a boundary satellite");
            CHECK(m.versions(k).size() == 1 && m.versions(k)[0].cause == CsibCause::Initial && m.versions(k)[0].id == 0, "one initial version");
            for (int sat : plan.members[k]) {
                const CsibVersion* v = m.viewAt(sat, 0.0);
                CHECK(v && v->id == 0 && v->srd == k, "every satellite has the initial CSIB at t=0");
            }
        }
        CHECK(m.stats().versions == 0, "initial versions are not counted as generated");
    }

    // ---- periodic update without vote: timing against an independent shortest-path reference -------------------------------
    {
        CsibParams p;
        p.computeMs = 2.0;
        p.reportProcMs = 0.25;
        p.hopProcMs = 0.5;
        p.useVote = false;
        CsibModel m(plan, p);
        m.initialize(R, g);
        const double t0 = 180.0;
        m.periodic(t0, R, g, none);
        for (int k = 0; k < plan.r; ++k) {
            const CsibVersion& v = m.versions(k).back();
            CHECK(v.id == 1 && v.cause == CsibCause::Periodic && v.ds == m.versions(k)[0].ds && !v.voted, "periodic version, DS kept");
            CHECK(near(v.triggerTime, t0) && near(v.dataTime, t0) && near(v.detectS, 0), "periodic: data sampled at the trigger");
            auto toDs = refDist(plan, g, k, v.ds, none, p.reportProcMs);
            double col = 0;
            for (int b : R.boundarySatellites(k)) if (b != v.ds) col = std::max(col, toDs[b]);
            CHECK(near(v.collectS, col / 1000.0), "collect phase = farthest boundary report: %g vs %g", v.collectS, col / 1000.0);
            CHECK(near(v.computeS, 0.002) && near(v.computeDone, t0 + v.collectS + 0.002), "compute phase");
            auto dis = refDist(plan, g, k, v.ds, none, p.hopProcMs);
            double mx = 0;
            for (size_t i = 0; i < v.members.size(); ++i) {
                const double expect = v.computeDone + dis[v.members[i]] / 1000.0;
                CHECK(near(v.arrival[i], expect), "arrival at %d: %g vs %g", v.members[i], v.arrival[i], expect);
                mx = std::max(mx, dis[v.members[i]]);
            }
            CHECK(near(v.disseminateS, mx / 1000.0), "dissemination = farthest member");
            CHECK(near(v.totalS, v.detectS + v.electS + v.collectS + v.computeS + v.disseminateS), "phases add up to the total");
            CHECK(near(v.arrival[std::find(v.members.begin(), v.members.end(), v.ds) - v.members.begin()], v.computeDone), "DS has it when computed");
            CHECK(v.unreachable == 0 && v.lostReports == 0, "nothing lost in the nominal network");
            // application: the old version until the new one has arrived, never the new one earlier
            for (size_t i = 0; i < v.members.size(); ++i) {
                const int sat = v.members[i];
                CHECK(m.viewAt(sat, t0)->id == 0, "at the trigger the satellite still uses the old CSIB");
                CHECK(m.viewAt(sat, v.arrival[i] - 1e-9)->id == 0, "just before arrival: old CSIB (no future information)");
                CHECK(m.viewAt(sat, v.arrival[i])->id == 1, "at arrival: new CSIB");
                CHECK(near(m.ageAt(sat, v.arrival[i] + 5.0), v.arrival[i] + 5.0 - t0), "age counts from the data time");
            }
        }
        CHECK(m.stats().versions == plan.r && m.stats().byCause[int(CsibCause::Periodic)] == plan.r, "stats count the periodic versions");
        CHECK(m.stats().maxDisseminateS > 0 && m.stats().maxTotalS >= m.stats().maxDisseminateS, "stats keep phase maxima");
    }

    // ---- serialisation: the message size adds per hop, the DS itself is unaffected --------------------------------------------
    {
        CsibParams a, b;
        a.useVote = b.useVote = false;
        b.messageBytes = 1e6;  // 8 ms per hop at 1 Gbit/s
        CsibModel ma(plan, a), mb(plan, b);
        ma.initialize(R, g);
        mb.initialize(R, g);
        ma.periodic(10, R, g, none);
        mb.periodic(10, R, g, none);
        const CsibVersion &va = ma.versions(0).back(), &vb = mb.versions(0).back();
        auto dis = refDist(plan, g, 0, va.ds, none, 8.0);
        for (size_t i = 0; i < va.members.size(); ++i) {
            CHECK(near(vb.arrival[i], vb.computeDone + dis[va.members[i]] / 1000.0), "serialisation arrival");
            if (va.members[i] == va.ds) CHECK(near(vb.arrival[i], va.arrival[i]), "DS unaffected by serialisation");
            else CHECK(vb.arrival[i] >= va.arrival[i] + 0.008 - 1e-12, "at least one extra hop of 8 ms");
        }
    }

    // ---- SRD-BCM vote -------------------------------------------------------------------------------------------------------
    {
        CsibParams p;
        p.reportProcMs = 0.3;
        CsibModel m(plan, p);
        m.initialize(R, g);
        m.periodic(180, R, g, none);
        for (int k = 0; k < plan.r; ++k) {
            const CsibVersion& v = m.versions(k).back();
            CHECK(v.voted && v.ds == R.voteDs(k, &none), "DS of the periodic version = SRD-BCM vote (srd %d)", k);
            double mx = 0;
            for (int b : R.boundarySatellites(k)) {
                auto d = refDist(plan, g, k, b, none, p.reportProcMs);
                for (int c : R.boundarySatellites(k)) if (c != b) mx = std::max(mx, d[c]);
            }
            CHECK(near(v.electS, mx / 1000.0), "vote duration = farthest boundary pair");
            CHECK(near(v.electDone, v.dataTime + v.electS) && near(v.totalS, v.detectS + v.electS + v.collectS + v.computeS + v.disseminateS), "vote phase is part of the pipeline");
            CHECK(R.isBoundary(v.ds), "voted DS is a boundary satellite");
        }
        CHECK(m.stats().votes == plan.r, "one vote per SRD");
        // a monitor refresh does not vote and keeps the DS
        m.monitor(185, R, g, none);
        for (int k = 0; k < plan.r; ++k) {
            const CsibVersion& v = m.versions(k).back();
            CHECK(v.cause == CsibCause::Monitor && !v.voted && v.ds == v.prevDs && near(v.electS, 0), "monitor: no vote, DS kept");
        }
    }

    // ---- DS failure: backup DS, regenerated CSIB, the failed DS keeps its old version --------------------------------------------
    {
        CsibParams p;
        p.computeMs = 1;
        p.useVote = false;
        CsibModel m(plan, p);
        m.initialize(R, g);
        const int k = 3;
        const int ds0 = m.ds(k);
        CHECK(R.classifyNodeFailure(ds0) == NodeFailureAction::SwitchToBackupDs, "DS failure is scenario 3");
        FailureSet f;
        f.nodes.insert(ds0);
        auto srds = m.onNodeFailure(100.0, ds0, R, g, f);
        CHECK(srds == std::vector<int>{k}, "only the DS's SRD regenerates");
        const CsibVersion& v = m.versions(k).back();
        CHECK(v.cause == CsibCause::DsSwitch && v.ds == R.electDs(k, f) && v.ds != ds0 && v.prevDs == ds0, "backup DS takes over (corner order)");
        CHECK(near(v.triggerTime, 100.0) && near(v.dataTime, 100.6) && near(v.detectS, 0.6), "detection delay 3 x 200 ms");
        CHECK(m.ds(k) == v.ds && m.stats().dsSwitches == 1, "switch recorded");
        auto dis = refDist(plan, g, k, v.ds, f, 0.0);
        for (size_t i = 0; i < v.members.size(); ++i) {
            if (v.members[i] == ds0) { CHECK(v.arrival[i] == kInfT, "the failed satellite never receives it"); continue; }
            CHECK(near(v.arrival[i], v.computeDone + dis[v.members[i]] / 1000.0), "dissemination from the backup DS avoids the failed satellite");
        }
        CHECK(m.viewAt(ds0, 1000.0)->id == 0, "the failed satellite keeps the old CSIB");
        const int other = plan.members[k][plan.members[k][0] == ds0 ? 1 : 0];
        CHECK(m.viewAt(other, v.dataTime)->id == 0 && m.viewAt(other, 1000.0)->id == 1, "other members switch after the new CSIB arrives");
        // other SRDs are untouched
        CHECK(m.versions(0).size() == 1, "other SRDs unchanged");
    }

    // ---- boundary / interior satellite failures --------------------------------------------------------------------------------
    {
        CsibParams p;
        p.useVote = false;
        CsibModel m(plan, p);
        m.initialize(R, g);
        const int k = 5;
        int bnd = -1, inner = -1;
        for (int s : plan.members[k]) {
            if (R.isBoundary(s) && s != m.ds(k) && bnd < 0) bnd = s;
            if (!R.isBoundary(s) && inner < 0) inner = s;
        }
        CHECK(bnd >= 0 && inner >= 0, "SRD has boundary and interior satellites");
        CHECK(R.classifyNodeFailure(inner) == NodeFailureAction::RecalcIntraSrd, "interior failure = scenario 1");
        FailureSet fi;
        fi.nodes.insert(inner);
        CHECK(m.onNodeFailure(50, inner, R, g, fi).empty() && m.versions(k).size() == 1 && m.stats().localOnly == 1, "interior failure: no CSIB regeneration");
        CHECK(R.classifyNodeFailure(bnd) == NodeFailureAction::AnnounceAndDsRecalcCsib, "boundary failure = scenario 2");
        FailureSet fb;
        fb.nodes.insert(bnd);
        auto srds = m.onNodeFailure(60, bnd, R, g, fb);
        CHECK(std::find(srds.begin(), srds.end(), k) != srds.end(), "own SRD regenerates");
        std::set<int> expectSrds{k};
        for (const Edge& e : g.adj[bnd]) expectSrds.insert(plan.srdOf[e.to]);
        CHECK(std::set<int>(srds.begin(), srds.end()) == expectSrds, "SRDs that lost an inter-SRD ISL regenerate too");
        for (int s : srds) {
            const CsibVersion& v = m.versions(s).back();
            CHECK(v.cause == CsibCause::NodeFailure && near(v.dataTime, 60.6), "node-failure version after detection");
            if (s == k) CHECK(v.ds == v.prevDs && v.lostReports == 0, "DS kept; the failed satellite sends no report");
        }
    }

    // ---- link failures: the four cases of Sec. V-C-2 --------------------------------------------------------------------------
    {
        CsibParams p;
        p.useVote = false;
        int c1u = -1, c1v = -1, c2u = -1, c2v = -1, c3u = -1, c3v = -1, c4u = -1, c4v = -1;
        for (int u = 0; u < g.size(); ++u)
            for (const Edge& e : g.adj[u]) {
                const int v = e.to;
                const bool same = plan.srdOf[u] == plan.srdOf[v];
                if (!same && c4u < 0) { c4u = u; c4v = v; }
                if (same && !R.isBoundary(u) && !R.isBoundary(v) && c1u < 0) { c1u = u; c1v = v; }
                if (same && R.isBoundary(u) != R.isBoundary(v) && c2u < 0) { c2u = u; c2v = v; }
                if (same && R.isBoundary(u) && R.isBoundary(v) && c3u < 0) { c3u = u; c3v = v; }
            }
        CHECK(c1u >= 0 && c2u >= 0 && c3u >= 0 && c4u >= 0, "found a link of every case");
        CsibModel m(plan, p);
        m.initialize(R, g);
        FailureSet f;
        f.links.insert(linkKey(c1u, c1v));
        CHECK(m.onLinkFailure(10, c1u, c1v, R, g, f).empty() && m.stats().localOnly == 1, "case 1: intra-SRD only, no CSIB");
        f.links.insert(linkKey(c2u, c2v));
        auto s2 = m.onLinkFailure(20, c2u, c2v, R, g, f);
        CHECK(s2 == std::vector<int>{plan.srdOf[c2u]}, "case 2: the SRD regenerates");
        f.links.insert(linkKey(c3u, c3v));
        auto s3 = m.onLinkFailure(30, c3u, c3v, R, g, f);
        CHECK(s3 == std::vector<int>{plan.srdOf[c3u]}, "case 3: the SRD regenerates (boundary only)");
        f.links.insert(linkKey(c4u, c4v));
        auto s4 = m.onLinkFailure(40, c4u, c4v, R, g, f);
        CHECK(s4.size() == 2 && s4[0] == plan.srdOf[c4u] && s4[1] == plan.srdOf[c4v], "case 4: both SRDs regenerate");
        const CsibVersion& v = m.versions(plan.srdOf[c4u]).back();
        CHECK(v.cause == CsibCause::LinkFailure && near(v.dataTime, 40.6) && near(v.detectS, 0.6), "link-failure version after detection");
        // dissemination avoids every failed link of the SRD
        auto dis = refDist(plan, g, v.srd, v.ds, f, 0.0);
        for (size_t i = 0; i < v.members.size(); ++i)
            CHECK(near(v.arrival[i], v.computeDone + dis[v.members[i]] / 1000.0), "arrival over the surviving ISLs");
        CHECK(m.stats().byCause[int(CsibCause::LinkFailure)] == 4, "four link-failure versions (1 + 1 + 2)");
    }

    // ---- a DS cut off from its SRD: nobody gets the new version -----------------------------------------------------------------
    {
        CsibParams p;
        p.useVote = false;
        CsibModel m(plan, p);
        m.initialize(R, g);
        const int k = 4, ds = m.ds(k);
        FailureSet f;
        for (const Edge& e : g.adj[ds]) if (plan.srdOf[e.to] == k) f.links.insert(linkKey(ds, e.to));
        m.periodic(100, R, g, f);
        const CsibVersion& v = m.versions(k).back();
        CHECK(v.unreachable == int(plan.members[k].size()) - 1, "all other members unreachable");
        CHECK(v.lostReports == int(R.boundarySatellites(k).size()) - 1, "every other boundary report is lost");
        for (int sat : plan.members[k])
            CHECK(m.viewAt(sat, 1e6)->id == (sat == ds ? 1 : 0), "only the DS applies a version nobody else can receive");
        CHECK(m.stats().unreachableMembers >= int(plan.members[k].size()) - 1, "stats count the unreachable members");
    }

    // ---- versions that arrive out of order: newer data wins, an older version that arrives later is not applied ---------------------
    {
        CsibParams p;
        p.useVote = false;
        p.hopProcMs = 300.0;  // slow hops make the dissemination time dominate
        CsibModel m(plan, p);
        m.initialize(R, g);
        const int k = 3, ds0 = m.ds(k);
        m.periodic(100.0, R, g, none);  // version 1, data time 100.0, DS = ds0, slow to reach far members
        FailureSet f;
        f.nodes.insert(ds0);
        m.onNodeFailure(100.0, ds0, R, g, f);  // version 2, data time 100.6, backup DS, reaches ITS neighbours first
        const CsibVersion &v1 = m.versions(k)[1], &v2 = m.versions(k)[2];
        bool found = false;
        for (size_t i = 0; i < v1.members.size(); ++i) {
            if (!(v2.arrival[i] < v1.arrival[i])) continue;
            found = true;
            const int sat = v1.members[i];
            CHECK(v2.dataTime > v1.dataTime, "version 2 carries newer data");
            CHECK(m.viewAt(sat, v2.arrival[i])->id == 2, "the newer version is applied as soon as it arrives");
            CHECK(m.viewAt(sat, v1.arrival[i] + 1.0)->id == 2, "the older version arriving later is not applied");
        }
        CHECK(found, "at least one satellite receives version 2 before version 1");
        // pruning: after everything has arrived only the newest version of each SRD is needed
        m.prune(1000.0);
        // the failed DS never receives version 2 and keeps working with version 1, so both stay; version 0 is superseded everywhere
        CHECK(m.versions(k).size() == 2 && m.versions(k)[0].id == 1 && m.versions(k)[1].id == 2, "prune keeps what some member still uses");
        CHECK(m.viewAt(plan.members[k][1], 1000.0)->id == 2 && m.viewAt(ds0, 1000.0)->id == 1, "views unchanged by prune");
        CHECK(m.versions(0).size() == 1 && m.versions(0)[0].id == 1, "an SRD whose members all use version 1 keeps only version 1");
    }

    // ---- regression: findings of the external review (version ids, post-failure graph, directed ISLs) ----
    {
        // 1. version ids stay unique after prune(): a new version must not reuse the id of a pruned one
        CsibParams p;
        p.useVote = false;
        CsibModel m(plan, p);
        m.initialize(R, g);
        m.periodic(10.0, R, g, none);
        m.periodic(20.0, R, g, none);
        m.prune(1000.0);   // everything has arrived: only the newest version of every SRD is kept
        CHECK(m.versions(0).size() == 1 && m.versions(0)[0].id == 2, "prune kept version 2");
        m.periodic(30.0, R, g, none);
        CHECK(m.versions(0).back().id == 3, "id after prune continues the sequence (not reused)");
        CHECK(m.viewAt(plan.members[0][0], 1000.0)->id == 3, "the new version is the view, not shadowed by a reused id");
    }
    {
        // 2. satellite failure: the SRDs that lost an inter-SRD ISL are found from the static grid, also when the graph handed in
        //    is the post-failure snapshot (edges of the failed satellite already gone)
        CsibParams p;
        p.useVote = false;
        int bnd = -1;
        for (int k = 0; k < plan.r && bnd < 0; ++k)
            for (int b : R.boundarySatellites(k))
                if (R.classifyNodeFailure(b) == NodeFailureAction::AnnounceAndDsRecalcCsib) { bnd = b; break; }
        if (bnd >= 0) {
            Graph g2 = g;
            g2.adj[bnd].clear();
            for (auto& row : g2.adj)
                row.erase(std::remove_if(row.begin(), row.end(), [&](const Edge& e) { return e.to == bnd; }), row.end());
            FailureSet fb;
            fb.nodes.insert(bnd);
            CsibModel ma(plan, p), mb(plan, p);
            ma.initialize(R, g);
            mb.initialize(R, g);
            auto full = ma.onNodeFailure(60, bnd, R, g, fb);
            auto cut = mb.onNodeFailure(60, bnd, R, g2, fb);
            std::sort(full.begin(), full.end());
            std::sort(cut.begin(), cut.end());
            CHECK(!full.empty() && full == cut, "affected SRDs do not depend on the edges left in the post-failure graph");
        } else
            CHECK(false, "fixture has a boundary satellite that is not the DS");
    }
    {
        // 3. directed ISLs: the report goes boundary -> DS, the CSIB goes DS -> member
        CsibParams p;
        p.useVote = false;
        const int k = 3;
        CsibModel base(plan, p);
        base.initialize(R, g);
        const int ds = base.ds(k);
        Graph ga = g;   // ISLs INTO the DS become 10x slower, the ones leaving it stay
        for (auto& row : ga.adj)
            for (Edge& e : row)
                if (e.to == ds) e.t.D *= 10.0;
        CsibModel m0(plan, p), m1(plan, p);
        m0.initialize(R, g);
        m1.initialize(R, ga);
        m0.periodic(100.0, R, g, none);
        m1.periodic(100.0, R, ga, none);
        const CsibVersion &a = m0.versions(k).back(), &b = m1.versions(k).back();
        CHECK(b.collectS > a.collectS * 2.0, "slow links into the DS slow the reports");
        bool sameDissemination = near(a.disseminateS, b.disseminateS) || b.disseminateS <= a.disseminateS * 1.0000001 + 1e-12;
        for (size_t i = 0; i < a.members.size(); ++i)
            if (a.members[i] != ds && std::isfinite(a.arrival[i]) && std::isfinite(b.arrival[i]) && b.arrival[i] - b.computeDone > a.arrival[i] - a.computeDone + 1e-9)
                sameDissemination = false;
        CHECK(sameDissemination, "dissemination follows DS -> member and is not slowed by links into the DS");
    }

    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
