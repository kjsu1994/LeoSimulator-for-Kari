// MCSR core types: path tuple {D, A, J}, extension (Eq. 7), multi-attribute consistency (Def. 1).
#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>

namespace mcsr {

constexpr double kInf = std::numeric_limits<double>::infinity();

// Path tuple {D: propagation delay [ms], A: available buffer ratio [0,1], J: jitter [ms]}.
// Default-constructed value is the identity element of extend().
struct PathTuple {
    double D = 0.0;
    double A = kInf;
    double J = 0.0;
};

// Eq. (7): E(Ti,Tj) = {Di+Dj, min(Ai,Aj), max(Ji,Jj)}
inline PathTuple extend(const PathTuple& a, const PathTuple& b) {
    return {a.D + b.D, std::min(a.A, b.A), std::max(a.J, b.J)};
}

// Delays that differ by less than this [ms] are treated as equal. Grid symmetry produces many
// distinct paths whose D sums agree only up to floating-point noise; without a tolerance such
// ties leak dominated tuples into the dominant set. A and J are exact copies (min/max), so no
// tolerance is needed for them.
constexpr double kDelayEps = 1e-9;

// a is at least as good as b on every attribute (lower D, higher A, lower J).
inline bool weaklyDominates(const PathTuple& a, const PathTuple& b) {
    return a.D <= b.D + kDelayEps && a.A >= b.A && a.J <= b.J;
}

// Definition 1 (multi-attribute consistency, a ≺ b): a is better-or-equal on all
// attributes and strictly better on at least one.
inline bool dominates(const PathTuple& a, const PathTuple& b) {
    return weaklyDominates(a, b) && (a.D < b.D - kDelayEps || a.A > b.A || a.J < b.J);
}

// ---- operation counters (algorithm cost, independent of the host CPU) --------------------------
// Counted per operation kind so that Dijkstra and MCSR can be compared with an explicit cost weight per
// kind (see the sensitivity analysis) instead of one conversion factor. Counting is off unless an
// OpScope installs a sink, so the hot paths only pay a null check.
struct OpCounts {
    std::uint64_t
        // generic graph search (Dijkstra and the Pareto search)
        edgeChecks = 0, pqPush = 0, pqPop = 0, relaxations = 0, graphEdges = 0,
        // Pareto label search (MCSR)
        searches = 0, labelExpand = 0, dominanceCmp = 0, labelInsert = 0, labelDelete = 0, capEvict = 0, capReject = 0,
        // MCSR router structure
        tableHit = 0, tableMiss = 0, localBuilt = 0, vedgesBuilt = 0, boundaryGraphs = 0, boundaryEdges = 0,
        pathTraces = 0, pathNodes = 0, linkLookups = 0, finalizeCmp = 0,
        // selection and reserve-path handling
        candCompare = 0, selectChecks = 0, reserveChecks = 0, reserveSwitches = 0, recomputes = 0,
        // module level: nominal plan cache of the simulator module, tree/plan computations
        planHit = 0, planMiss = 0, computations = 0,
        // costs that are easy to forget: segmentation and copying the router cache for a failure recomputation
        segmentCandidates = 0, segmentEdgeScan = 0, cacheCopyEntries = 0,
        // why stored paths were not usable as a reserve at the satellite that noticed the failure
        rejNotThrough = 0, rejSuffixFailed = 0, rejNoFirstHop = 0, rejExit = 0, rejSkipExit = 0,
        // paper-literal wavefront search (Alg. 2/3): rounds, and nodes taken back out of R because a new dominant path arrived
        waveRounds = 0, waveReexpand = 0;
    OpCounts& operator+=(const OpCounts& o) {
        edgeChecks += o.edgeChecks; pqPush += o.pqPush; pqPop += o.pqPop; relaxations += o.relaxations; graphEdges += o.graphEdges;
        searches += o.searches; labelExpand += o.labelExpand; dominanceCmp += o.dominanceCmp; labelInsert += o.labelInsert;
        labelDelete += o.labelDelete; capEvict += o.capEvict; capReject += o.capReject; tableHit += o.tableHit; tableMiss += o.tableMiss;
        localBuilt += o.localBuilt; vedgesBuilt += o.vedgesBuilt; boundaryGraphs += o.boundaryGraphs;
        boundaryEdges += o.boundaryEdges; pathTraces += o.pathTraces; pathNodes += o.pathNodes; linkLookups += o.linkLookups;
        finalizeCmp += o.finalizeCmp; candCompare += o.candCompare; selectChecks += o.selectChecks;
        reserveChecks += o.reserveChecks; reserveSwitches += o.reserveSwitches; recomputes += o.recomputes;
        planHit += o.planHit; planMiss += o.planMiss; computations += o.computations;
        segmentCandidates += o.segmentCandidates; segmentEdgeScan += o.segmentEdgeScan; cacheCopyEntries += o.cacheCopyEntries;
        rejNotThrough += o.rejNotThrough; rejSuffixFailed += o.rejSuffixFailed; rejNoFirstHop += o.rejNoFirstHop; rejExit += o.rejExit; rejSkipExit += o.rejSkipExit;
        waveRounds += o.waveRounds; waveReexpand += o.waveReexpand;
        return *this;
    }
    // name/value pairs for result recording and printing
    template <class F> void each(F&& f) const {
        f("edgeChecks", edgeChecks); f("pqPush", pqPush); f("pqPop", pqPop); f("relaxations", relaxations); f("graphEdges", graphEdges);
        f("searches", searches); f("labelExpand", labelExpand); f("dominanceCmp", dominanceCmp); f("labelInsert", labelInsert);
        f("labelDelete", labelDelete); f("capEvict", capEvict); f("capReject", capReject); f("tableHit", tableHit); f("tableMiss", tableMiss);
        f("localBuilt", localBuilt); f("vedgesBuilt", vedgesBuilt); f("boundaryGraphs", boundaryGraphs);
        f("boundaryEdges", boundaryEdges); f("pathTraces", pathTraces); f("pathNodes", pathNodes); f("linkLookups", linkLookups);
        f("finalizeCmp", finalizeCmp); f("candCompare", candCompare); f("selectChecks", selectChecks);
        f("reserveChecks", reserveChecks); f("reserveSwitches", reserveSwitches); f("recomputes", recomputes);
        f("planHit", planHit); f("planMiss", planMiss); f("computations", computations);
        f("segmentCandidates", segmentCandidates); f("segmentEdgeScan", segmentEdgeScan); f("cacheCopyEntries", cacheCopyEntries);
        f("rejNotThrough", rejNotThrough); f("rejSuffixFailed", rejSuffixFailed); f("rejNoFirstHop", rejNoFirstHop); f("rejExit", rejExit); f("rejSkipExit", rejSkipExit);
        f("waveRounds", waveRounds); f("waveReexpand", waveReexpand);
    }
};
inline thread_local OpCounts* g_ops = nullptr;   // active sink, nullptr = not counting
#define MCSR_OP(field, n) do { if (::mcsr::g_ops) ::mcsr::g_ops->field += static_cast<std::uint64_t>(n); } while (0)
struct OpScope {   // routes all counted operations of the enclosed code into `sink`
    OpCounts* prev;
    explicit OpScope(OpCounts* sink) : prev(g_ops) { g_ops = sink; }
    ~OpScope() { g_ops = prev; }
    OpScope(const OpScope&) = delete;
    OpScope& operator=(const OpScope&) = delete;
};

enum class Service { LS, TS, HR };  // latency-/throughput-sensitive, high-reliability

using LinkKey = std::uint64_t;
inline LinkKey linkKey(int u, int v) {
    if (u > v) std::swap(u, v);
    return (static_cast<std::uint64_t>(u) << 32) | static_cast<std::uint32_t>(v);
}

}  // namespace mcsr
