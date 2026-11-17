// Walker-Delta constellation (Sec. II-A): logical grid index V(Qm,Xn), +grid ISLs, ephemeris,
// handover rule, and ground-station visibility (Sec. II-E).
#pragma once
#include <cmath>
#include <functional>
#include <utility>
#include <vector>

#include "mcsr/graph.h"

namespace mcsr {

struct WalkerParams {
    int NO = 72;          // orbits
    int NP = 22;          // satellites per orbit
    double altKm = 550.0; // h
    double incDeg = 53.0; // alpha
    int f = 1;            // phasing factor
};

struct Vec3 { double x, y, z; };
struct GroundStation { double latDeg, lonDeg; };

// Returns {A, J[ms]} for ISL (u,v) at time t.
using LinkStateFn = std::function<std::pair<double, double>(int, int, double)>;

struct TopologyOptions {
    double t = 0.0;
    bool closeHighLatInterOrbit = false;  // ephemeris-driven periodic handover (Sec. V-C-1)
    double handoverMarginDeg = 2.0;       // close inter-orbit ISL when |lat| >= alpha - margin
    LinkStateFn state;                    // defaults: A=1, J=0
};

struct BuiltTopology {
    Graph g;
    double avgIntraD = 0.0;  // Ty
    double avgInterD = 0.0;  // Tx
};

class Walker {
public:
    static constexpr double kRe = 6371.0, kMu = 398600.4418, kC = 299792.458, kOmegaE = 7.2921159e-5;
    explicit Walker(WalkerParams p) : p_(p) {}
    const WalkerParams& params() const { return p_; }
    int N() const { return p_.NO * p_.NP; }
    int index(int q, int x) const { return p_.NP * q + x; }  // Eq. (3)
    int orbitOf(int i) const { return i / p_.NP; }
    int slotOf(int i) const { return i % p_.NP; }

    struct Nbr { int id; bool intra; };
    // The four +grid ISL neighbours; the orbit-index seam is shifted by the phasing factor f.
    std::vector<Nbr> neighbors(int i) const {
        const int q = orbitOf(i), x = slotOf(i), NO = p_.NO, NP = p_.NP;
        std::vector<Nbr> n;
        n.push_back({index(q, (x + 1) % NP), true});
        n.push_back({index(q, (x + NP - 1) % NP), true});
        n.push_back({q + 1 < NO ? index(q + 1, x) : index(0, (x + p_.f) % NP), false});
        n.push_back({q - 1 >= 0 ? index(q - 1, x) : index(NO - 1, ((x - p_.f) % NP + NP) % NP), false});
        return n;
    }

    std::vector<std::vector<int>> gridNeighbors() const {
        std::vector<std::vector<int>> g(N());
        for (int u = 0; u < N(); ++u)
            for (const Nbr& nb : neighbors(u)) g[u].push_back(nb.id);
        return g;
    }

    double meanAnomaly(int i, double t) const {
        const double r = kRe + p_.altKm, n = std::sqrt(kMu / (r * r * r));
        return 2.0 * M_PI * (double(slotOf(i)) / p_.NP + double(p_.f) * orbitOf(i) / (double(p_.NO) * p_.NP)) + n * t;
    }
    // Inertial-frame position [km] (circular orbit, no perturbations).
    Vec3 position(int i, double t) const {
        const double r = kRe + p_.altKm, M = meanAnomaly(i, t), inc = p_.incDeg * M_PI / 180.0;
        const double om = 2.0 * M_PI * orbitOf(i) / p_.NO;
        const double X = r * std::cos(M), Y = r * std::sin(M);
        const double Yp = Y * std::cos(inc), Zp = Y * std::sin(inc);
        return {X * std::cos(om) - Yp * std::sin(om), X * std::sin(om) + Yp * std::cos(om), Zp};
    }
    double latitudeDeg(int i, double t) const {
        return std::asin(std::sin(p_.incDeg * M_PI / 180.0) * std::sin(meanAnomaly(i, t))) * 180.0 / M_PI;
    }
    bool ascending(int i, double t) const { return std::cos(meanAnomaly(i, t)) > 0.0; }
    double distanceKm(int u, int v, double t) const {
        Vec3 a = position(u, t), b = position(v, t);
        return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
    }

    // Sec. V-C-1: inter-orbit ISL is preemptively closed near the inclination latitude.
    bool interOrbitLinkClosed(int u, int v, double t, double marginDeg) const {
        const double lim = p_.incDeg - marginDeg;
        return std::fabs(latitudeDeg(u, t)) >= lim || std::fabs(latitudeDeg(v, t)) >= lim;
    }

    BuiltTopology buildTopology(const TopologyOptions& o = {}) const {
        BuiltTopology bt{Graph(N()), 0, 0};
        double sIntra = 0, sInter = 0;
        long cIntra = 0, cInter = 0;
        for (int u = 0; u < N(); ++u)
            for (const Nbr& nb : neighbors(u)) {
                if (nb.id < u) continue;  // each undirected ISL once
                if (!nb.intra && o.closeHighLatInterOrbit && interOrbitLinkClosed(u, nb.id, o.t, o.handoverMarginDeg))
                    continue;
                const double D = distanceKm(u, nb.id, o.t) / kC * 1000.0;  // ms
                std::pair<double, double> aj = o.state ? o.state(u, nb.id, o.t) : std::make_pair(1.0, 0.0);
                bt.g.addUndirected(u, nb.id, PathTuple{D, aj.first, aj.second});
                (nb.intra ? sIntra : sInter) += D;
                (nb.intra ? cIntra : cInter) += 1;
            }
        bt.avgIntraD = cIntra ? sIntra / cIntra : 0;
        bt.avgInterD = cInter ? sInter / cInter : 0;
        return bt;
    }

    // Satellites above minElevDeg for a ground station; returns {sat, slant range km}.
    std::vector<std::pair<int, double>> visible(const GroundStation& gs, double t, double minElevDeg) const {
        const double la = gs.latDeg * M_PI / 180.0, th = gs.lonDeg * M_PI / 180.0 + kOmegaE * t;
        const Vec3 g{kRe * std::cos(la) * std::cos(th), kRe * std::cos(la) * std::sin(th), kRe * std::sin(la)};
        std::vector<std::pair<int, double>> out;
        for (int i = 0; i < N(); ++i) {
            Vec3 s = position(i, t);
            Vec3 d{s.x - g.x, s.y - g.y, s.z - g.z};
            double rng = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
            double sinEl = (d.x * g.x + d.y * g.y + d.z * g.z) / (rng * kRe);
            if (std::asin(sinEl) * 180.0 / M_PI >= minElevDeg) out.push_back({i, rng});
        }
        return out;
    }

private:
    WalkerParams p_;
};

}  // namespace mcsr
