// Designated satellite (DS) and constellation segment information base (CSIB) as a TIME model (Sec. II-C, III-C, V-C).
//
// What the paper says: every SRD has a DS, a boundary satellite. The DS collects the ISL tuples of the SRD's boundary
// satellites (they send SLS-based notifications to it), calculates the inter-SRD routes, generates the CSIB and
// periodically updates it and disseminates it to the satellites of its SRD. The DS is initially the upper-right boundary
// satellite; later the best DS is chosen by the SRD-BCM vote (Sec. II-C). The update trigger interval is initially 3 min
// (Sec. V-C-1); failures re-trigger the update according to the cases of Sec. V-C-2.
//
// What this header does: it decides WHEN each satellite of an SRD has which CSIB version. It does not send packets.
// Every version records the timestamps of its pipeline (trigger -> detect -> [vote] -> collect -> compute -> disseminate) and
// the arrival time at every member satellite, computed over the real live intra-SRD ISL graph with its link delays.
// A satellite applies a version only when the version has reached it (`viewAt`); before that it keeps the previous one.
//
// UNITS (fixed): timestamps are simulation seconds (double); every duration parameter is in milliseconds and its name ends in
// `Ms`; link delays are the `PathTuple::D` values of the graph (milliseconds).
//
// ASSUMPTIONS (the paper gives none of these values; each one is a parameter in CsibParams):
//   * computeMs: DS computation time of one CSIB (the paper treats T_Cal as a constant without a value)
//   * reportProcMs / hopProcMs: processing time per hop of a boundary report / of the CSIB message
//   * messageBytes: CSIB message size; serialisation per hop = messageBytes*8 / islBitsPerSecond
//   * detectMs: failure detection time (default = 3 missed SLS packets x 200 ms, the condition trigger of Sec. V-C-1)
//   * the vote runs on the periodic trigger only (the DS is kept between votes unless it failed); the vote rule itself is
//     Router::voteDs, an interpretation (the paper gives no formula)
//   * the vote costs one all-to-all exchange among the boundary satellites (the farthest pair decides when it ends)
//   * ISL delays are directed: the report travels boundary satellite -> DS (reverse search), the CSIB DS -> member
//   * the initial CSIB of every SRD is installed before t = 0 (the network starts converged)
//   * intra-SRD routing information that satellites compute themselves (Alg. 3) is not modelled here; only the CSIB flow is
#pragma once
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

#include "router.h"

namespace mcsr {

enum class CsibCause {
    Initial,      // installed before t = 0
    Periodic,     // periodic update trigger (3 min), SRD-BCM vote
    Monitor,      // attribute refresh standing in for the continuous monitoring of A and J (no period in the paper)
    LinkFailure,  // Sec. V-C-2 cases 2-4 (case 1 changes no CSIB)
    NodeFailure,  // boundary satellite failure (scenario 2)
    DsSwitch      // DS failure, backup DS takes over (scenario 3)
};
inline const char* causeName(CsibCause c) {
    switch (c) {
        case CsibCause::Initial: return "initial";
        case CsibCause::Periodic: return "periodic";
        case CsibCause::Monitor: return "monitor";
        case CsibCause::LinkFailure: return "linkFailure";
        case CsibCause::NodeFailure: return "nodeFailure";
        default: return "dsSwitch";
    }
}

struct CsibParams {
    double periodSec = 180.0;       // paper: initially 3 min
    double computeMs = 0.0;         // assumption (paper: T_Cal, constant, no value)
    double reportProcMs = 0.0;      // assumption
    double hopProcMs = 0.0;         // assumption
    double messageBytes = 0.0;      // assumption, 0 = serialisation ignored
    double islBitsPerSecond = 1e9;  // ISL rate used for serialisation
    double detectMs = 600.0;        // assumption: 3 missed SLS packets x 200 ms
    bool useVote = true;            // SRD-BCM vote on the periodic trigger (interpretation), else the corner order only
};

struct CsibVersion {
    int srd = -1;
    int id = 0;  // sequence number inside the SRD, 0 = initial
    CsibCause cause = CsibCause::Initial;
    int ds = -1, prevDs = -1;
    double triggerTime = 0;    // the trigger fired (failure: the failure instant)
    double dataTime = 0;       // boundary ISL tuples sampled / failure known (= triggerTime + detect for failures)
    double electDone = 0;      // vote finished (= dataTime when there was no vote)
    double collectDone = 0;    // DS holds all reachable boundary reports
    double computeDone = 0;    // CSIB generated
    // phase durations [s]
    double detectS = 0, electS = 0, collectS = 0, computeS = 0, disseminateS = 0, totalS = 0;
    int unreachable = 0;       // members that never receive this version (cut off from the DS)
    int lostReports = 0;       // boundary satellites whose report cannot reach the DS
    bool voted = false;
    std::vector<int> members;
    std::vector<double> arrival;  // arrival time at each member [s], +inf = never
    bool ok = true;               // false: the SRD has no live boundary satellite, no version could be made
};

struct CsibStats {
    long versions = 0, dsSwitches = 0, votes = 0, localOnly = 0, failed = 0;
    long byCause[6] = {0, 0, 0, 0, 0, 0};
    long unreachableMembers = 0, lostReports = 0;
    // per phase: sum and maximum over all generated versions (excluding the initial ones)
    double sumDetectS = 0, sumElectS = 0, sumCollectS = 0, sumComputeS = 0, sumDisseminateS = 0, sumTotalS = 0;
    double maxDetectS = 0, maxElectS = 0, maxCollectS = 0, maxComputeS = 0, maxDisseminateS = 0, maxTotalS = 0;
    long timed = 0;  // versions in the sums
};

class CsibModel {
public:
    static constexpr double kNever = std::numeric_limits<double>::infinity();

    CsibModel(const SrdPlan& plan, CsibParams p) : plan_(plan), p_(p), versions_(plan.r), lastId_(plan.r, 0), ds_(plan.r, -1) {}

    // Installs the initial CSIB of every SRD (cause Initial, available everywhere at t = 0, initial DS = upper-right rule).
    void initialize(Router& r, const Graph& g, const FailureSet& f = FailureSet{}) {
        for (int k = 0; k < plan_.r; ++k) {
            CsibVersion v;
            v.srd = k;
            v.id = 0;
            v.cause = CsibCause::Initial;
            v.ds = v.prevDs = ds_[k] = r.electDs(k, f);
            v.ok = v.ds >= 0;
            v.members = plan_.members[k];
            v.arrival.assign(v.members.size(), 0.0);
            versions_[k].push_back(std::move(v));
        }
    }

    // ---- triggers ---------------------------------------------------------------------------------------------------------
    // Periodic trigger (all SRDs, synchronised) at time t. Runs the SRD-BCM vote when params.useVote.
    void periodic(double t, Router& r, const Graph& g, const FailureSet& f) {
        for (int k = 0; k < plan_.r; ++k) generate(k, CsibCause::Periodic, t, t, r, g, f, p_.useVote);
    }
    // Attribute refresh (stands in for the continuous monitoring of A and J). No vote, the DS is kept.
    void monitor(double t, Router& r, const Graph& g, const FailureSet& f) {
        for (int k = 0; k < plan_.r; ++k) generate(k, CsibCause::Monitor, t, t, r, g, f, false);
    }
    // Link failure at tFail (Sec. V-C-2). `f` holds all failures in effect, including this one. The router/graph are the
    // post-failure snapshot. Returns the SRDs whose CSIB is regenerated (empty for case 1).
    std::vector<int> onLinkFailure(double tFail, int u, int v, Router& r, const Graph& g, const FailureSet& f) {
        std::vector<int> srds;
        const UpdateScope sc = r.classifyLinkFailure(u, v);
        if (sc == UpdateScope::IntraSrdLocal) { ++stats_.localOnly; return srds; }
        srds.push_back(plan_.srdOf[u]);
        if (plan_.srdOf[v] != plan_.srdOf[u]) srds.push_back(plan_.srdOf[v]);
        for (int k : srds) generate(k, CsibCause::LinkFailure, tFail, tFail + p_.detectMs / 1000.0, r, g, f, false);
        return srds;
    }
    // Satellite failure at tFail (scenarios 1-3). `f` is the post-failure set (it contains v); the action is classified against
    // the nominal DS (Router::classifyNodeFailure compares v with the default DS of its SRD).
    std::vector<int> onNodeFailure(double tFail, int v, Router& r, const Graph& g, const FailureSet& f) {
        std::vector<int> srds;
        const int k = plan_.srdOf[v];
        const NodeFailureAction a = r.classifyNodeFailure(v);
        if (a == NodeFailureAction::RecalcIntraSrd) { ++stats_.localOnly; return srds; }
        if (a == NodeFailureAction::SwitchToBackupDs) {
            generate(k, CsibCause::DsSwitch, tFail, tFail + p_.detectMs / 1000.0, r, g, f, false);
            srds.push_back(k);
        } else {  // boundary satellite: its SRD and every SRD that lost an inter-SRD ISL to it regenerate
            srds.push_back(k);
            // static grid neighbours: the graph handed in is the post-failure snapshot and has lost the edges of v
            for (int o : r.neighbourSrds(v))
                if (o != k && std::find(srds.begin(), srds.end(), o) == srds.end()) srds.push_back(o);
            for (int s : srds) generate(s, CsibCause::NodeFailure, tFail, tFail + p_.detectMs / 1000.0, r, g, f, false);
        }
        return srds;
    }

    // ---- what a satellite may use at time t ---------------------------------------------------------------------------------
    // The version of the satellite's SRD that is applied at `sat` at time t: of all versions that have ARRIVED (arrival <= t),
    // the one with the newest data (dataTime, then id). Never a version that is still on its way. nullptr if none (only
    // before initialize()).
    const CsibVersion* viewAt(int sat, double t) const {
        const int k = plan_.srdOf[sat];
        const CsibVersion* best = nullptr;
        for (const CsibVersion& v : versions_[k]) {
            if (!v.ok) continue;
            const int idx = memberIndex(k, sat);
            if (idx < 0 || !(v.arrival[idx] <= t)) continue;
            if (!best || v.dataTime > best->dataTime || (v.dataTime == best->dataTime && v.id > best->id)) best = &v;
        }
        return best;
    }
    // Age of the information the satellite works with at t [s].
    double ageAt(int sat, double t) const { const CsibVersion* v = viewAt(sat, t); return v ? t - v->dataTime : kNever; }

    const std::vector<CsibVersion>& versions(int srd) const { return versions_[srd]; }
    int ds(int srd) const { return ds_[srd]; }
    const CsibStats& stats() const { return stats_; }

    // Drops versions that can no longer become the view of any member at or after time t: the version is not the current view
    // of any member, and either it never reaches the member or the member already works with newer data.
    void prune(double t) {
        for (int k = 0; k < plan_.r; ++k) {
            std::vector<const CsibVersion*> cur;
            for (int sat : plan_.members[k]) cur.push_back(viewAt(sat, t));
            std::vector<CsibVersion> keep;
            for (const CsibVersion& v : versions_[k]) {
                bool needed = false;
                for (size_t i = 0; i < plan_.members[k].size() && !needed; ++i) {
                    if (cur[i] == &v) { needed = true; break; }
                    const int idx = memberIndex(k, plan_.members[k][i]);
                    const bool pending = idx >= 0 && v.arrival[idx] > t && v.arrival[idx] < kNever;
                    if (pending && (!cur[i] || v.dataTime > cur[i]->dataTime)) needed = true;
                }
                if (needed) keep.push_back(v);
            }
            versions_[k].swap(keep);
        }
    }

private:
    int memberIndex(int srd, int sat) const {
        const auto& m = plan_.members[srd];
        // members are stored in a fixed order; build the index lazily once per SRD
        auto it = idx_.find(srd);
        if (it == idx_.end()) {
            std::unordered_map<int, int> mm;
            for (size_t i = 0; i < m.size(); ++i) mm[m[i]] = int(i);
            it = idx_.emplace(srd, std::move(mm)).first;
        }
        auto jt = it->second.find(sat);
        return jt == it->second.end() ? -1 : jt->second;
    }

    // Shortest delay [ms] from `src` to the SRD members over the live intra-SRD ISLs; each hop costs D + extraMs.
    // reverse = true: delay of the paths that END at `src` (directed ISLs followed backwards), i.e. member -> src.
    std::vector<double> distances(int srd, int src, const Graph& g, const FailureSet& f, double extraMs, bool reverse = false) const {
        const auto& mem = plan_.members[srd];
        std::unordered_map<int, int> loc;
        for (size_t i = 0; i < mem.size(); ++i) loc[mem[i]] = int(i);
        std::vector<double> d(mem.size(), kNever);
        if (f.blocksNode(src)) return d;
        using QE = std::pair<double, int>;
        std::priority_queue<QE, std::vector<QE>, std::greater<QE>> pq;
        std::vector<std::vector<std::pair<int, double>>> in;   // reverse adjacency inside the SRD (only when reverse)
        if (reverse) {
            in.resize(mem.size());
            for (size_t i = 0; i < mem.size(); ++i)
                for (const Edge& e : g.adj[mem[i]]) {
                    auto it = loc.find(e.to);
                    if (it != loc.end()) in[it->second].push_back({mem[i], e.t.D});   // mem[i] -> e.to
                }
        }
        d[loc[src]] = 0.0;
        pq.push({0.0, src});
        while (!pq.empty()) {
            auto [du, u] = pq.top();
            pq.pop();
            if (du > d[loc[u]]) continue;
            if (!reverse) {
                for (const Edge& e : g.adj[u]) {
                    auto it = loc.find(e.to);
                    if (it == loc.end() || f.blocksNode(e.to) || f.blocksLink(u, e.to)) continue;
                    const double nd = du + e.t.D + extraMs;
                    if (nd < d[it->second]) { d[it->second] = nd; pq.push({nd, e.to}); }
                }
            } else {
                for (const auto& pr : in[loc[u]]) {   // pr.first -> u
                    const int w = pr.first;
                    if (f.blocksNode(w) || f.blocksLink(w, u)) continue;
                    const double nd = du + pr.second + extraMs;
                    const int wi = loc[w];
                    if (nd < d[wi]) { d[wi] = nd; pq.push({nd, w}); }
                }
            }
        }
        return d;
    }

    void generate(int k, CsibCause cause, double tTrigger, double dataTime, Router& r, const Graph& g, const FailureSet& f, bool vote) {
        CsibVersion v;
        v.srd = k;
        v.id = ++lastId_[k];
        v.cause = cause;
        v.prevDs = ds_[k];
        v.triggerTime = tTrigger;
        v.dataTime = dataTime;
        v.members = plan_.members[k];
        v.arrival.assign(v.members.size(), kNever);
        v.detectS = dataTime - tTrigger;
        // --- DS: kept, unless it failed (backup corner order) or the vote runs
        int ds = ds_[k];
        if (ds < 0 || f.blocksNode(ds)) ds = r.electDs(k, f);
        const double extraReport = p_.reportProcMs;
        if (vote && ds >= 0) {
            const int w = r.voteDs(k, &f);
            if (w >= 0) {
                v.voted = true;
                // one all-to-all exchange among the live boundary satellites: ends when the farthest pair has heard
                double mx = 0;
                for (int b : r.boundarySatellites(k)) {
                    if (f.blocksNode(b)) continue;
                    const auto dist = distances(k, b, g, f, extraReport);
                    for (int c : r.boundarySatellites(k)) {
                        if (c == b || f.blocksNode(c)) continue;
                        const int ci = memberIndex(k, c);
                        if (ci >= 0 && dist[ci] < kNever) mx = std::max(mx, dist[ci]);
                    }
                }
                v.electS = mx / 1000.0;
                ds = w;
            }
        }
        if (ds < 0) {  // no live boundary satellite: nothing can be generated
            v.ok = false;
            v.ds = -1;
            ++stats_.failed;
            versions_[k].push_back(std::move(v));
            return;
        }
        v.ds = ds;
        ds_[k] = ds;
        if (ds != v.prevDs) ++stats_.dsSwitches;
        v.electDone = dataTime + v.electS;
        // --- collect: every live boundary satellite reports to the DS
        const auto toDs = distances(k, ds, g, f, extraReport, true);   // report direction: boundary satellite -> DS
        double col = 0;
        for (int b : r.boundarySatellites(k)) {
            if (f.blocksNode(b) || b == ds) continue;
            const int bi = memberIndex(k, b);
            if (bi < 0 || toDs[bi] >= kNever) { ++v.lostReports; continue; }
            col = std::max(col, toDs[bi]);
        }
        v.collectS = col / 1000.0;
        v.collectDone = v.electDone + v.collectS;
        // --- compute
        v.computeS = p_.computeMs / 1000.0;
        v.computeDone = v.collectDone + v.computeS;
        // --- disseminate over the live intra-SRD ISLs (per hop: delay + processing + serialisation)
        const double serMs = p_.messageBytes > 0 ? p_.messageBytes * 8.0 / p_.islBitsPerSecond * 1000.0 : 0.0;
        const auto dist = distances(k, ds, g, f, p_.hopProcMs + serMs);
        double last = v.computeDone;
        for (size_t i = 0; i < v.members.size(); ++i) {
            if (f.blocksNode(v.members[i]) || dist[i] >= kNever) { ++v.unreachable; continue; }
            v.arrival[i] = v.computeDone + dist[i] / 1000.0;
            last = std::max(last, v.arrival[i]);
        }
        v.disseminateS = last - v.computeDone;
        v.totalS = last - tTrigger;
        // --- statistics
        ++stats_.versions;
        ++stats_.byCause[int(cause)];
        if (v.voted) ++stats_.votes;
        stats_.unreachableMembers += v.unreachable;
        stats_.lostReports += v.lostReports;
        stats_.sumDetectS += v.detectS; stats_.maxDetectS = std::max(stats_.maxDetectS, v.detectS);
        stats_.sumElectS += v.electS; stats_.maxElectS = std::max(stats_.maxElectS, v.electS);
        stats_.sumCollectS += v.collectS; stats_.maxCollectS = std::max(stats_.maxCollectS, v.collectS);
        stats_.sumComputeS += v.computeS; stats_.maxComputeS = std::max(stats_.maxComputeS, v.computeS);
        stats_.sumDisseminateS += v.disseminateS; stats_.maxDisseminateS = std::max(stats_.maxDisseminateS, v.disseminateS);
        stats_.sumTotalS += v.totalS; stats_.maxTotalS = std::max(stats_.maxTotalS, v.totalS);
        ++stats_.timed;
        versions_[k].push_back(std::move(v));
    }

    const SrdPlan& plan_;
    CsibParams p_;
    std::vector<std::vector<CsibVersion>> versions_;
    std::vector<int> lastId_;   // last id handed out per SRD: monotonic, never reused after prune()
    std::vector<int> ds_;
    CsibStats stats_;
    mutable std::unordered_map<int, std::unordered_map<int, int>> idx_;
};

}  // namespace mcsr
