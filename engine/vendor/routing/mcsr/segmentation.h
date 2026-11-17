// SRD segmentation (Sec. II-C, Algorithm 1): choose SRD size (K,L) minimising T_Cov (Eq. 1) and
// split the NO x NP grid into non-intersecting SRDs, balanced for the non-divisible cases 2-4.
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace mcsr {

// Eq. (2) as printed: T_Flood ~= L*Tx + K*Ty  (PaperLiteral).
// Axis-consistent variant (K orbits cross inter-orbit links, L slots cross intra-orbit links):
// T_Flood ~= K*Tx + L*Ty  (Consistent). See notes: the printed form mixes the axes.
enum class FloodModel { PaperLiteral, Consistent };

struct SizeChoice { int K, L; double tcov; };

inline double tcov(int NO, int NP, int K, int L, double Tx, double Ty, FloodModel m) {
    const double flood = (m == FloodModel::PaperLiteral) ? L * Tx + K * Ty : K * Tx + L * Ty;
    return double(NO) / K * Tx + double(NP) / L * Ty + flood;  // T_Cal is constant -> dropped from argmin
}

// Algorithm 1, lines 1-10.
inline SizeChoice chooseSrdSize(int NO, int NP, double Tx, double Ty, FloodModel m = FloodModel::PaperLiteral) {
    SizeChoice best{1, 1, 1e300};
    for (int i = 1; i <= NO; ++i)
        for (int j = 1; j <= NP; ++j) {
            double t = tcov(NO, NP, i, j, Tx, Ty, m);
            if (t < best.tcov) best = {i, j, t};
        }
    return best;
}

// Cases 1-4: n items, nominal size `size`. m = ceil(n/size) segments; (n mod m) of them get
// ceil(n/m), the rest floor(n/m). Divisible case reduces to exactly n/size segments of `size`.
// Returns segment start offsets, last element == n.
inline std::vector<int> splitBalanced(int n, int size) {
    const int m = (n + size - 1) / size;
    const int base = n / m, rem = n % m;
    std::vector<int> cuts{0};
    for (int s = 0; s < m; ++s) cuts.push_back(cuts.back() + base + (s < rem ? 1 : 0));
    return cuts;
}

struct SrdRect { int qlo, qhi, xlo, xhi; };  // inclusive

struct SrdPlan {
    int NO = 0, NP = 0, K = 0, L = 0;
    std::vector<int> orbitCuts, slotCuts;
    int r = 0;                              // number of SRDs
    std::vector<int> srdOf;                 // per satellite
    std::vector<SrdRect> rect;              // per SRD
    std::vector<std::vector<int>> members;  // per SRD
    int srdAt(int q, int x) const { return srdOf[NP * q + x]; }
};

inline SrdPlan makePlan(int NO, int NP, int K, int L) {
    SrdPlan p;
    p.NO = NO; p.NP = NP; p.K = K; p.L = L;
    p.orbitCuts = splitBalanced(NO, K);
    p.slotCuts = splitBalanced(NP, L);
    const int mo = int(p.orbitCuts.size()) - 1, ms = int(p.slotCuts.size()) - 1;
    p.r = mo * ms;
    p.srdOf.assign(NO * NP, -1);
    p.rect.resize(p.r);
    p.members.resize(p.r);
    for (int a = 0; a < mo; ++a)
        for (int b = 0; b < ms; ++b) {
            const int k = a * ms + b;
            p.rect[k] = {p.orbitCuts[a], p.orbitCuts[a + 1] - 1, p.slotCuts[b], p.slotCuts[b + 1] - 1};
            for (int q = p.rect[k].qlo; q <= p.rect[k].qhi; ++q)
                for (int x = p.rect[k].xlo; x <= p.rect[k].xhi; ++x) {
                    p.srdOf[NP * q + x] = k;
                    p.members[k].push_back(NP * q + x);
                }
        }
    return p;
}

inline SrdPlan segment(int NO, int NP, double Tx, double Ty, FloodModel m = FloodModel::PaperLiteral) {
    SizeChoice c = chooseSrdSize(NO, NP, Tx, Ty, m);
    return makePlan(NO, NP, c.K, c.L);
}

}  // namespace mcsr
