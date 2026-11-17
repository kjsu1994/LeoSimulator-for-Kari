/*
 * zones.h - regional congestion model (header only, no OMNeT++ dependency, unit-tested in mcsr-core/tests).
 *
 * NOT part of the MCSR paper. Scenario tool: "environment mode" congestion that is declared once (zone, load rho,
 * start, duration) and needs no packets. A zone selects satellites (by where they are above the Earth right now, or
 * by id). For every directed ISL u->v (u's transmit queue) it yields
 *     rho_eff    = rho * coefficient                     capacity C*(1-rho_eff), MCSR availability A' = min(A, 1-rho_eff)
 *     extraDelay = D_q(rho_eff) + penalty delay          added to the channel delay
 *     jitter     = sqrt(J_q(rho_eff)^2 + penalty^2)      added in quadrature to the jitter MCSR sees
 * with the M/M/1 queue model (S = 8L/C, capped at B*S):
 *     D_q = rho/(1-rho) * S           J_q = sqrt(rho(2-rho))/(1-rho) * S
 * Same model as the UI's congestion scenario (project doc leo-congestion-backend.md).
 *
 * Zone entries (whitespace or ';' separated):
 *     circle(lat,lon,radiusKm)=rho[/penaltyDelayMs][{options}]@T[+D]
 *     box(latMin,lonMin,latMax,lonMax)=rho[/penaltyDelayMs][{options}]@T[+D]   lonMin > lonMax wraps over +-180 deg
 *     sat(id|a-b|...)=rho[/penaltyDelayMs][{options}]@T[+D]                     fixed satellites (coefficient 1)
 *   T start [s], D duration [s] (omitted or 0 = until the end)
 *   options (comma separated key=value):
 *     dir=both|out|in     which end of the link must be inside: out = sending satellite, in = receiving satellite,
 *                         both = see rule                                     (default both)
 *     rule=any|both       dir=both: any = one end inside is enough, both = both ends inside   (default: injector's linkRule)
 *     edge=<percent>      hotspot: coefficient 1 in the centre falling linearly to edge/100 at the border   (default 100 = uniform)
 *     jitter=<ms>         penalty jitter (random), combined in quadrature with the queue jitter
 */
#ifndef GLOBAL_UTILITIES_ROUTING_CORE_CONGESTION_ZONES_H_
#define GLOBAL_UTILITIES_ROUTING_CORE_CONGESTION_ZONES_H_

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace leocongestion {

struct LinkLoad {
    double rho = 0;          // effective load 0..1
    double extraDelay = 0;   // [s] queueing delay (M/M/1) + penalty delay
    double jitter = 0;       // [s] sqrt(queue jitter^2 + penalty jitter^2)
    bool operator==(const LinkLoad &o) const { return rho == o.rho && extraDelay == o.extraDelay && jitter == o.jitter; }
    bool operator!=(const LinkLoad &o) const { return !(*this == o); }
};

/** Directed ISL (from, to) -> load. Absent = unloaded. */
using LinkLoadMap = std::map<std::pair<int, int>, LinkLoad>;

enum class LinkRule { Any, Both };          // dir=both: one end inside is enough / both ends must be inside
enum class Direction { Both, Out, In };     // which end of u->v must be inside the zone

struct Zone {
    enum Shape { Circle, Box, Sats };
    Shape shape = Circle;
    double lat = 0, lon = 0, radiusKm = 0;                  // circle
    double latMin = 0, lonMin = 0, latMax = 0, lonMax = 0;  // box
    std::vector<int> sats;                                  // sat(...)
    double rho = 0;
    double penaltyDelayMs = 0, penaltyJitterMs = 0;
    Direction dir = Direction::Both;
    LinkRule rule = LinkRule::Any;
    double edgeFraction = 1.0;                              // coefficient at the border (1 = uniform)
    double start = 0, duration = 0;                         // duration 0 = permanent

    bool activeAt(double t) const { return t >= start && (duration <= 0 || t < start + duration); }
    bool geographic() const { return shape != Sats; }
};

inline double greatCircleKm(double lat1, double lon1, double lat2, double lon2)
{
    const double d2r = 3.14159265358979323846 / 180.0;
    const double p1 = lat1 * d2r, p2 = lat2 * d2r;
    const double dphi = p2 - p1, dl = (lon2 - lon1) * d2r;
    const double a = std::sin(dphi / 2) * std::sin(dphi / 2) + std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
    return 2.0 * 6371.0 * std::asin(std::min(1.0, std::sqrt(a)));
}

inline double normLon(double x)
{
    x = std::fmod(x + 180.0, 360.0);
    if (x < 0) x += 360.0;
    return x - 180.0;
}

inline bool lonInRange(double lon, double lo, double hi)
{
    lon = normLon(lon); lo = normLon(lo); hi = normLon(hi);
    return lo <= hi ? (lon >= lo && lon <= hi) : (lon >= lo || lon <= hi);
}

/** Position of a satellite inside the zone as a coefficient in [edgeFraction, 1]; 0 = outside. */
inline double coefficient(const Zone &z, int satId, double lat, double lon)
{
    auto hotspot = [&](double r) { return 1.0 - (1.0 - z.edgeFraction) * std::min(1.0, std::max(0.0, r)); };
    switch (z.shape) {
        case Zone::Circle: {
            const double d = greatCircleKm(z.lat, z.lon, lat, lon);
            return d <= z.radiusKm ? hotspot(d / z.radiusKm) : 0.0;
        }
        case Zone::Box: {
            if (!(lat >= z.latMin && lat <= z.latMax && lonInRange(lon, z.lonMin, z.lonMax)))
                return 0.0;
            const double halfLat = (z.latMax - z.latMin) / 2, centreLat = (z.latMin + z.latMax) / 2;
            double width = normLon(z.lonMax) - normLon(z.lonMin);
            if (width < 0) width += 360.0;
            const double centreLon = normLon(z.lonMin) + width / 2;
            const double dlon = std::fabs(normLon(lon - centreLon));
            const double rLat = halfLat > 0 ? std::fabs(lat - centreLat) / halfLat : 0.0;
            const double rLon = width > 0 ? dlon / (width / 2) : 0.0;
            return hotspot(std::max(rLat, rLon));     // square "radius": 0 in the centre, 1 on the border
        }
        case Zone::Sats:
            return std::find(z.sats.begin(), z.sats.end(), satId) != z.sats.end() ? 1.0 : 0.0;
    }
    return 0.0;
}

inline bool contains(const Zone &z, int satId, double lat, double lon) { return coefficient(z, satId, lat, lon) > 0; }

namespace detail {
inline std::vector<double> numbers(const std::string &s, const std::string &entry)
{
    std::vector<double> v;
    std::string t = s;
    std::replace(t.begin(), t.end(), ',', ' ');
    std::istringstream in(t);
    std::string tok;
    while (in >> tok) {
        char *end = nullptr;
        const double x = std::strtod(tok.c_str(), &end);
        if (end == tok.c_str() || *end != '\0')
            throw std::invalid_argument("congestion zone '" + entry + "': '" + tok + "' is not a number");
        v.push_back(x);
    }
    return v;
}
inline std::vector<int> idList(const std::string &s, const std::string &entry)
{
    std::vector<int> ids;
    std::string t = s;
    std::replace(t.begin(), t.end(), '|', ' ');
    std::replace(t.begin(), t.end(), ',', ' ');
    std::istringstream in(t);
    std::string tok;
    while (in >> tok) {
        const std::size_t dash = tok.find('-', 1);
        char *end = nullptr;
        const long a = std::strtol(tok.c_str(), &end, 10);
        if (end == tok.c_str())
            throw std::invalid_argument("congestion zone '" + entry + "': bad satellite id '" + tok + "'");
        long b = a;
        if (dash != std::string::npos) {
            b = std::strtol(tok.c_str() + dash + 1, &end, 10);
            if (b < a)
                throw std::invalid_argument("congestion zone '" + entry + "': bad id range '" + tok + "'");
        }
        for (long i = a; i <= b; i++)
            ids.push_back((int)i);
    }
    return ids;
}
inline double number(const std::string &s, const std::string &entry, const char *what)
{
    char *end = nullptr;
    const double x = std::strtod(s.c_str(), &end);
    if (s.empty() || *end != '\0')
        throw std::invalid_argument("congestion zone '" + entry + "': " + what + " '" + s + "' is not a number");
    return x;
}
}  // namespace detail

/** Parses one entry, throws std::invalid_argument naming the offending text. defaultRule: used when no rule= option is given. */
inline Zone parseZone(const std::string &entry, LinkRule defaultRule = LinkRule::Any)
{
    Zone z;
    z.rule = defaultRule;
    const std::size_t open = entry.find('('), close = entry.find(')'), eq = entry.find('=');
    if (open == std::string::npos || close == std::string::npos || eq == std::string::npos || !(open < close && close < eq))
        throw std::invalid_argument("congestion zone '" + entry + "': expected shape(args)=rho[/delayMs][{options}]@T[+D]");
    const std::string shape = entry.substr(0, open);
    const std::string args = entry.substr(open + 1, close - open - 1);
    if (shape == "circle") {
        const auto v = detail::numbers(args, entry);
        if (v.size() != 3 || v[2] <= 0)
            throw std::invalid_argument("congestion zone '" + entry + "': circle needs lat,lon,radiusKm (radius > 0)");
        z.shape = Zone::Circle; z.lat = v[0]; z.lon = v[1]; z.radiusKm = v[2];
    }
    else if (shape == "box") {
        const auto v = detail::numbers(args, entry);
        if (v.size() != 4 || v[0] > v[2])
            throw std::invalid_argument("congestion zone '" + entry + "': box needs latMin,lonMin,latMax,lonMax (latMin <= latMax)");
        z.shape = Zone::Box; z.latMin = v[0]; z.lonMin = v[1]; z.latMax = v[2]; z.lonMax = v[3];
    }
    else if (shape == "sat") {
        z.shape = Zone::Sats;
        z.sats = detail::idList(args, entry);
        if (z.sats.empty())
            throw std::invalid_argument("congestion zone '" + entry + "': sat() needs at least one id");
    }
    else
        throw std::invalid_argument("congestion zone '" + entry + "': unknown shape '" + shape + "' (circle, box, sat)");

    std::string rest = entry.substr(eq + 1);
    std::string when;
    const std::size_t at = rest.find('@');
    if (at != std::string::npos) { when = rest.substr(at + 1); rest = rest.substr(0, at); }

    const std::size_t ob = rest.find('{');
    if (ob != std::string::npos) {
        const std::size_t cb = rest.find('}', ob);
        if (cb == std::string::npos || cb != rest.size() - 1)
            throw std::invalid_argument("congestion zone '" + entry + "': options must be written {key=value,...} after the load");
        std::string opts = rest.substr(ob + 1, cb - ob - 1);
        rest = rest.substr(0, ob);
        std::replace(opts.begin(), opts.end(), ',', ' ');
        std::istringstream in(opts);
        std::string kv;
        while (in >> kv) {
            const std::size_t e2 = kv.find('=');
            if (e2 == std::string::npos)
                throw std::invalid_argument("congestion zone '" + entry + "': option '" + kv + "' is not key=value");
            const std::string k = kv.substr(0, e2), val = kv.substr(e2 + 1);
            if (k == "dir") {
                if (val == "both") z.dir = Direction::Both;
                else if (val == "out") z.dir = Direction::Out;
                else if (val == "in") z.dir = Direction::In;
                else throw std::invalid_argument("congestion zone '" + entry + "': dir must be both, out or in");
            }
            else if (k == "rule") {
                if (val == "any") z.rule = LinkRule::Any;
                else if (val == "both") z.rule = LinkRule::Both;
                else throw std::invalid_argument("congestion zone '" + entry + "': rule must be any or both");
            }
            else if (k == "edge") {
                const double pct = detail::number(val, entry, "edge");
                if (pct < 0 || pct > 100)
                    throw std::invalid_argument("congestion zone '" + entry + "': edge must be a percentage in [0,100]");
                z.edgeFraction = pct / 100.0;
            }
            else if (k == "jitter") {
                z.penaltyJitterMs = detail::number(val, entry, "jitter");
                if (z.penaltyJitterMs < 0)
                    throw std::invalid_argument("congestion zone '" + entry + "': jitter must be >= 0");
            }
            else
                throw std::invalid_argument("congestion zone '" + entry + "': unknown option '" + k + "' (dir, rule, edge, jitter)");
        }
    }

    const std::size_t slash = rest.find('/');
    std::string rhoText = rest, delayText;
    if (slash != std::string::npos) { rhoText = rest.substr(0, slash); delayText = rest.substr(slash + 1); }
    char *end = nullptr;
    z.rho = std::strtod(rhoText.c_str(), &end);
    if (rhoText.empty() || *end != '\0' || z.rho < 0 || z.rho > 1)
        throw std::invalid_argument("congestion zone '" + entry + "': rho must be a number in [0,1]");
    if (!delayText.empty()) {
        z.penaltyDelayMs = std::strtod(delayText.c_str(), &end);
        if (*end != '\0' || z.penaltyDelayMs < 0)
            throw std::invalid_argument("congestion zone '" + entry + "': penalty delay must be >= 0 ms");
    }
    if (!when.empty()) {
        const std::size_t plus = when.find('+');
        z.start = std::strtod(when.substr(0, plus).c_str(), &end);
        if (*end != '\0' || z.start < 0)
            throw std::invalid_argument("congestion zone '" + entry + "': bad start time");
        if (plus != std::string::npos) {
            z.duration = std::strtod(when.substr(plus + 1).c_str(), &end);
            if (*end != '\0' || z.duration < 0)
                throw std::invalid_argument("congestion zone '" + entry + "': bad duration");
        }
    }
    return z;
}

inline std::vector<Zone> parseZones(const std::string &text, LinkRule defaultRule = LinkRule::Any)
{
    std::string t = text;
    std::replace(t.begin(), t.end(), ';', ' ');
    std::vector<Zone> zones;
    std::istringstream in(t);
    std::string tok;
    while (in >> tok)
        zones.push_back(parseZone(tok, defaultRule));
    return zones;
}

/** M/M/1 queue of a link with transmit queue of B packets (paper Table II: B = 500, L = 1380 B). */
struct QueueModel {
    int capacityPackets = 500;
    int packetBytes = 1380;
    double islDatarate = 1e9;        // original link capacity C [bit/s] (before the capacity reduction)
    double serviceTime() const { return packetBytes * 8.0 / islDatarate; }               // S = 8L/C [s]
    double maxDelay() const { return capacityPackets * serviceTime(); }                   // B*S
    /** Mean queueing delay at load rho, capped at B*S. */
    double delay(double rho) const {
        if (rho <= 0) return 0.0;
        if (rho >= 1) return maxDelay();
        return std::min(maxDelay(), rho / (1.0 - rho) * serviceTime());
    }
    /** Queueing jitter (std. deviation of the sojourn time) at load rho, capped at B*S. */
    double jitter(double rho) const {
        if (rho <= 0) return 0.0;
        if (rho >= 1) return maxDelay();
        return std::min(maxDelay(), std::sqrt(rho * (2.0 - rho)) / (1.0 - rho) * serviceTime());
    }
};

/** Position of a satellite: false when unknown. */
using PositionFn = std::function<bool(int sat, double &lat, double &lon)>;

/** Directed link load table at time t. links: each ISL once (a,b); both directions u->v are evaluated separately.
 *  Overlapping zones: the strongest effective load wins; delay and jitter take the maximum. */
inline LinkLoadMap computeLinkLoads(const std::vector<Zone> &zones, double t, const std::vector<std::pair<int, int>> &links,
                                    const PositionFn &position, const QueueModel &q)
{
    LinkLoadMap out;
    for (const Zone &z : zones) {
        if (!z.activeAt(t) || z.rho <= 0)
            continue;
        std::unordered_map<int, double> coef;   // per satellite, evaluated once per zone and refresh
        auto c = [&](int sat) {
            auto it = coef.find(sat);
            if (it != coef.end())
                return it->second;
            double lat = 0, lon = 0, v = 0;
            if (z.shape == Zone::Sats || position(sat, lat, lon))
                v = coefficient(z, sat, lat, lon);
            coef[sat] = v;
            return v;
        };
        auto apply = [&](int u, int v) {   // directed u->v, u owns the transmit queue
            const double cu = c(u), cv = c(v);
            double k;
            switch (z.dir) {
                case Direction::Out: k = cu; break;
                case Direction::In: k = cv; break;
                default: k = z.rule == LinkRule::Any ? std::max(cu, cv) : std::min(cu, cv); break;
            }
            const double rho = z.rho * k;
            if (rho <= 0)
                return;
            const double pj = z.penaltyJitterMs * 1e-3, qj = q.jitter(rho);
            LinkLoad &ll = out[{u, v}];
            ll.rho = std::max(ll.rho, rho);
            ll.extraDelay = std::max(ll.extraDelay, q.delay(rho) + z.penaltyDelayMs * 1e-3);
            ll.jitter = std::max(ll.jitter, std::sqrt(qj * qj + pj * pj));
        };
        for (const auto &l : links) {
            apply(l.first, l.second);
            apply(l.second, l.first);
        }
    }
    return out;
}

/** Times at which the set of active zones changes (start and end of every zone). */
inline std::vector<double> boundaryTimes(const std::vector<Zone> &zones)
{
    std::vector<double> ts;
    for (const Zone &z : zones) {
        ts.push_back(z.start);
        if (z.duration > 0)
            ts.push_back(z.start + z.duration);
    }
    std::sort(ts.begin(), ts.end());
    ts.erase(std::unique(ts.begin(), ts.end()), ts.end());
    return ts;
}

}  // namespace leocongestion

#endif /* GLOBAL_UTILITIES_ROUTING_CORE_CONGESTION_ZONES_H_ */
