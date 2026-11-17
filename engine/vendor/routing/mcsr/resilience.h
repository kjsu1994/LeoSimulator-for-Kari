// Event/trigger helpers of the resilient rerouting strategy (Sec. V-C).
#pragma once
#include <unordered_map>

#include "types.h"

namespace mcsr {

// SLS packet contents (Sec. V-C-1).
struct SlsPacket {
    int srcSat;
    int dsId;
    int srdId;
    PathTuple isl;
    double timestamp;
};

// Condition trigger: failure declared after `threshold` consecutive missing SLS packets (default 3).
class LinkMonitor {
public:
    explicit LinkMonitor(int threshold = 3) : th_(threshold) {}
    // Returns true exactly once, when the miss counter reaches the threshold.
    bool report(LinkKey k, bool received) {
        if (received) { miss_[k] = 0; return false; }
        return ++miss_[k] == th_;
    }
private:
    int th_;
    std::unordered_map<LinkKey, int> miss_;
};

// Periodic trigger: all satellites recompute every `interval` seconds (default 3 min).
class PeriodicTrigger {
public:
    explicit PeriodicTrigger(double intervalSec = 180.0) : iv_(intervalSec) {}
    bool due(double t) {
        if (!started_ || t - last_ >= iv_) { started_ = true; last_ = t; return true; }
        return false;
    }
private:
    double iv_, last_ = 0.0;
    bool started_ = false;
};

// Scope of a link-failure update (Sec. V-C-2, cases 1-4).
enum class UpdateScope { IntraSrdLocal, WholeSrdWithBoundary, SrdBoundaryOnly, InterSrdCsib };
// Reaction to a satellite failure (scenarios 1-3).
enum class NodeFailureAction { RecalcIntraSrd, AnnounceAndDsRecalcCsib, SwitchToBackupDs };

}  // namespace mcsr
