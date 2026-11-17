#include <cmath>
#include <cstdio>
#include <stdexcept>

#include "congestion/zones.h"

using namespace leocongestion;

static int g_fail = 0, g_checks = 0;
#define CHECK(c, ...) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("FAIL %s:%d  %s  ", __FILE__, __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
static bool throws(const char *text) { try { parseZones(text); } catch (const std::invalid_argument &) { return true; } return false; }

int main()
{
    // ---- geometry
    CHECK(near(greatCircleKm(35.68, 139.69, 37.57, 126.98), 1158, 15), "Tokyo-Seoul");
    CHECK(near(greatCircleKm(0, 0, 0, 180), 20015, 5), "half circumference");
    CHECK(near(greatCircleKm(10, 20, 10, 20), 0, 1e-9), "same point");
    CHECK(lonInRange(179, 170, -170) && lonInRange(-179, 170, -170) && !lonInRange(0, 170, -170), "antimeridian wrap");
    CHECK(lonInRange(10, -20, 30) && !lonInRange(40, -20, 30), "plain range");

    // ---- parsing
    {
        auto z = parseZones("circle(35.7,139.7,1500)=0.9@60+120 box(30,-10,50,40)=0.5/2.5{dir=out,rule=both,edge=30,jitter=1.5}@10 sat(3|5-7)=1@0+5");
        CHECK(z.size() == 3, "three zones");
        CHECK(z[0].shape == Zone::Circle && z[0].lat == 35.7 && z[0].lon == 139.7 && z[0].radiusKm == 1500 && z[0].rho == 0.9, "circle fields");
        CHECK(z[0].start == 60 && z[0].duration == 120 && z[0].penaltyDelayMs == 0 && z[0].dir == Direction::Both && z[0].rule == LinkRule::Any && z[0].edgeFraction == 1.0, "circle defaults");
        CHECK(z[1].shape == Zone::Box && z[1].penaltyDelayMs == 2.5 && z[1].start == 10 && z[1].duration == 0, "box fields");
        CHECK(z[1].dir == Direction::Out && z[1].rule == LinkRule::Both && near(z[1].edgeFraction, 0.3, 1e-12) && z[1].penaltyJitterMs == 1.5, "box options");
        CHECK(z[2].shape == Zone::Sats && z[2].sats == std::vector<int>({3, 5, 6, 7}), "sat list/range");
        CHECK(parseZones("").empty() && parseZones("  ;  ").empty(), "empty schedule");
        CHECK(parseZones("circle(0,0,100)=0.5")[0].start == 0, "no time = from start");
        CHECK(parseZones("circle(0,0,100)=0.5;box(0,0,1,1)=0.2")[1].shape == Zone::Box, "';' separator");
        CHECK(parseZones("sat(1)=0.5", LinkRule::Both)[0].rule == LinkRule::Both, "injector default rule");
        CHECK(parseZones("sat(1)=0.5{rule=any}", LinkRule::Both)[0].rule == LinkRule::Any, "option beats default rule");
        CHECK(parseZones("sat(1)=0.5{dir=in}@3+4")[0].dir == Direction::In && parseZones("sat(1)=0.5{dir=in}@3+4")[0].duration == 4, "options before time");
    }
    CHECK(throws("circle(1,2)=0.5"), "circle needs 3 args");
    CHECK(throws("circle(1,2,0)=0.5"), "radius > 0");
    CHECK(throws("box(50,0,30,10)=0.5"), "latMin <= latMax");
    CHECK(throws("circle(1,2,3)=1.5"), "rho <= 1");
    CHECK(throws("circle(1,2,3)=-0.1"), "rho >= 0");
    CHECK(throws("circle(1,2,3)=x"), "rho number");
    CHECK(throws("ring(1,2,3)=0.5"), "unknown shape");
    CHECK(throws("circle(1,2,3)"), "missing rho");
    CHECK(throws("circle(1,2,3)=0.5@-4"), "negative start");
    CHECK(throws("circle(1,2,3)=0.5/-1"), "negative delay");
    CHECK(throws("sat()=0.5"), "empty sat list");
    CHECK(throws("sat(5-2)=0.5"), "reversed range");
    CHECK(throws("circle(a,2,3)=0.5"), "non-numeric arg");
    CHECK(throws("sat(1)=0.5{dir=sideways}"), "bad dir");
    CHECK(throws("sat(1)=0.5{rule=some}"), "bad rule");
    CHECK(throws("sat(1)=0.5{edge=150}"), "edge > 100");
    CHECK(throws("sat(1)=0.5{edge}"), "option without value");
    CHECK(throws("sat(1)=0.5{colour=red}"), "unknown option");
    CHECK(throws("sat(1)=0.5{dir=out"), "unterminated options");
    CHECK(throws("sat(1)=0.5{jitter=-1}"), "negative jitter");

    // ---- activity window
    {
        Zone z = parseZone("sat(0)=0.5@10+20");
        CHECK(!z.activeAt(9.99) && z.activeAt(10) && z.activeAt(29.99) && !z.activeAt(30), "window [10,30)");
        Zone p = parseZone("sat(0)=0.5@10");
        CHECK(!p.activeAt(9) && p.activeAt(1e9), "permanent");
    }

    // ---- M/M/1 queue model: C = 1 Gbit/s, L = 1380 B, B = 500 -> S = 11.04 us, B*S = 5.52 ms
    QueueModel q;
    const double S = 11.04e-6;
    CHECK(near(q.serviceTime(), S, 1e-12) && near(q.maxDelay(), 500 * S, 1e-12), "S and B*S");
    CHECK(q.delay(0) == 0 && q.jitter(0) == 0, "empty queue");
    CHECK(near(q.delay(0.5), 1.0 * S, 1e-12), "D_q(0.5) = S");
    CHECK(near(q.delay(0.9), 9.0 * S, 1e-12), "D_q(0.9) = 9 S");
    CHECK(near(q.jitter(0.5), std::sqrt(0.75) / 0.5 * S, 1e-12), "J_q(0.5)");
    CHECK(near(q.jitter(0.9), std::sqrt(0.99) / 0.1 * S, 1e-12), "J_q(0.9)");
    CHECK(near(q.delay(0.999), 500 * S, 1e-12) && near(q.jitter(0.999), 500 * S, 1e-12), "capped at B*S");
    CHECK(near(q.delay(1.0), 500 * S, 1e-12) && near(q.jitter(1.0), 500 * S, 1e-12), "rho = 1 is the cap");
    CHECK(q.delay(0.6) > q.delay(0.5) && q.jitter(0.6) > q.jitter(0.5), "monotone");

    // ---- link table. Satellites 0..5 on a line along the equator, lon = 10*id; links 0-1, 1-2, ...
    const std::vector<std::pair<int, int>> links = {{0, 1}, {1, 2}, {2, 3}, {3, 4}, {4, 5}};
    PositionFn pos = [](int s, double &lat, double &lon) { lat = 0; lon = 10.0 * s; return s >= 0 && s <= 5; };
    // circle(0,25,600): sats 2 (lon 20) and 3 (lon 30) are 5 deg = 556 km from the centre, 1 and 4 are 1668 km
    {
        auto m = computeLinkLoads(parseZones("circle(0,25,600)=0.8@0"), 5, links, pos, q);
        CHECK(m.size() == 6 && m.count({1, 2}) && m.count({2, 1}) && m.count({2, 3}) && m.count({3, 2}) && m.count({3, 4}) && m.count({4, 3}),
              "any: links with one end inside, both directions, got %zu", m.size());
        CHECK(near(m[{2, 3}].rho, 0.8, 1e-12) && near(m[{2, 3}].extraDelay, q.delay(0.8), 1e-12) && near(m[{2, 3}].jitter, q.jitter(0.8), 1e-12), "rho, D_q, J_q");
        auto b = computeLinkLoads(parseZones("circle(0,25,600)=0.8{rule=both}@0"), 5, links, pos, q);
        CHECK(b.size() == 2 && b.count({2, 3}) && b.count({3, 2}), "rule=both: only the link inside the zone");
        auto o = computeLinkLoads(parseZones("circle(0,25,600)=0.8{dir=out}@0"), 5, links, pos, q);
        CHECK(o.size() == 4 && o.count({2, 1}) && o.count({2, 3}) && o.count({3, 2}) && o.count({3, 4}) && !o.count({1, 2}) && !o.count({4, 3}),
              "dir=out: the sending satellite is inside, got %zu", o.size());
        auto in = computeLinkLoads(parseZones("circle(0,25,600)=0.8{dir=in}@0"), 5, links, pos, q);
        CHECK(in.size() == 4 && in.count({1, 2}) && in.count({3, 2}) && in.count({2, 3}) && in.count({4, 3}) && !in.count({2, 1}) && !in.count({3, 4}),
              "dir=in: the receiving satellite is inside, got %zu", in.size());
        // dir=out/in ignore the rule
        auto ob = computeLinkLoads(parseZones("circle(0,25,600)=0.8{dir=out,rule=both}@0"), 5, links, pos, q);
        CHECK(ob.size() == 4, "dir=out ignores rule");
    }
    {   // hotspot: circle, coefficient 1 in the centre, edge/100 at the border
        const double d = greatCircleKm(0, 25, 0, 20);                                    // ~556 km
        auto m = computeLinkLoads(parseZones("circle(0,25,600)=0.8{edge=50}@0"), 0, links, pos, q);
        const double k = 1.0 - 0.5 * (d / 600.0);
        CHECK(near(m[{2, 3}].rho, 0.8 * k, 1e-9), "hotspot rho %.4f vs %.4f", m[{2, 3}].rho, 0.8 * k);
        CHECK(near(m[{2, 3}].extraDelay, q.delay(0.8 * k), 1e-12), "delay uses the effective rho");
        // link coefficient: any = max of both ends, both = min (the end outside counts 0)
        auto mm = computeLinkLoads(parseZones("circle(0,25,600)=0.8{edge=50}@0"), 0, links, pos, q);
        CHECK(near(mm[{1, 2}].rho, 0.8 * k, 1e-9), "any: coefficient of the inside end");
        Zone zc = parseZone("circle(0,20,600)=1{edge=0}");
        double lat = 0, lon = 20;
        CHECK(near(coefficient(zc, 0, lat, lon), 1.0, 1e-12), "centre = 1");
        lon = 20 + 600 / 111.19492664;                                                    // ~ border
        CHECK(coefficient(zc, 0, 0, lon - 0.05) > 0 && near(coefficient(zc, 0, 0, lon - 0.05), 0.0, 0.01), "border -> edge fraction 0");
        CHECK(coefficient(zc, 0, 0, lon + 0.5) == 0, "outside = 0");
    }
    {   // box hotspot and antimeridian wrap
        Zone b = parseZone("box(-5,-5,5,5)=1{edge=0}");
        CHECK(near(coefficient(b, 0, 0, 0), 1.0, 1e-12) && near(coefficient(b, 0, 0, 4), 0.2, 1e-12) && near(coefficient(b, 0, 2.5, 0), 0.5, 1e-12), "box coefficient");
        CHECK(coefficient(b, 0, 0, 6) == 0 && coefficient(b, 0, 6, 0) == 0, "box outside");
        Zone w = parseZone("box(-10,170,10,-170)=1{edge=0}");
        CHECK(near(coefficient(w, 0, 0, 180), 1.0, 1e-9) && near(coefficient(w, 0, 0, 175), 0.5, 1e-9) && near(coefficient(w, 0, 0, -175), 0.5, 1e-9), "wrapped box");
        CHECK(coefficient(w, 0, 0, 0) == 0 && coefficient(w, 0, 0, 160) == 0, "wrapped box outside");
        Zone sat = parseZone("sat(4)=1{edge=0}");
        CHECK(coefficient(sat, 4, 0, 0) == 1.0 && coefficient(sat, 5, 0, 0) == 0, "sat zone ignores the hotspot");
    }
    {   // penalty delay adds to the queue delay, penalty jitter in quadrature
        auto m = computeLinkLoads(parseZones("sat(2)=0.5/1{jitter=3}@0"), 0, links, pos, q);
        CHECK(near(m[{2, 3}].extraDelay, q.delay(0.5) + 1e-3, 1e-12), "D = D_q + penalty");
        CHECK(near(m[{2, 3}].jitter, std::sqrt(q.jitter(0.5) * q.jitter(0.5) + 9e-6), 1e-12), "J = sqrt(J_q^2 + penalty^2)");
    }
    {   // time window and no load before start
        auto zones = parseZones("sat(2)=0.6@10+10");
        CHECK(computeLinkLoads(zones, 9, links, pos, q).empty(), "before start");
        CHECK(computeLinkLoads(zones, 15, links, pos, q).size() == 4, "sat 2 has links 1-2 and 2-3");
        CHECK(computeLinkLoads(zones, 20, links, pos, q).empty(), "after end");
    }
    {   // overlap: strongest effective load wins, delay/jitter maxima
        auto m = computeLinkLoads(parseZones("sat(2)=0.3/1 sat(2)=0.7/0.5"), 0, links, pos, q);
        CHECK(near(m[{2, 3}].rho, 0.7, 1e-12), "max rho");
        CHECK(near(m[{2, 3}].extraDelay, std::max(q.delay(0.3) + 1e-3, q.delay(0.7) + 0.5e-3), 1e-12), "max delay");
    }
    {   // inert zones
        CHECK(computeLinkLoads(parseZones("sat(2)=0"), 0, links, pos, q).empty(), "rho 0");
        PositionFn none = [](int, double &, double &) { return false; };
        CHECK(computeLinkLoads(parseZones("circle(0,0,20000)=0.5"), 0, links, none, q).empty(), "no positions");
        CHECK(computeLinkLoads(parseZones("sat(2)=0.5"), 0, links, none, q).size() == 4, "sat() does not need positions");
        CHECK(computeLinkLoads(parseZones("circle(0,0,20000)=0.5{edge=0}"), 0, links, pos, q).size() > 0, "whole-Earth circle with hotspot still loads");
    }
    {   // the zone follows the satellites: positions change with time
        double shift = 0;
        PositionFn moving = [&](int s, double &lat, double &lon) { lat = 0; lon = 10.0 * s + shift; return true; };
        auto zones = parseZones("box(-5,-5,5,5)=0.9@0");
        auto m0 = computeLinkLoads(zones, 0, links, moving, q);
        CHECK(m0.count({0, 1}) && !m0.count({3, 4}), "t0: sat 0 inside box");
        shift = 30;                                  // sat 0 moves to lon 30 (outside), nothing is left near lon 0
        CHECK(computeLinkLoads(zones, 1, links, moving, q).empty(), "satellite left the box");
        shift = -20;                                 // sat 2 now at lon 0
        auto m2 = computeLinkLoads(zones, 2, links, moving, q);
        CHECK(m2.count({1, 2}) && m2.count({2, 3}) && !m2.count({4, 5}), "sat 2 inside, load follows the geography");
    }
    {
        auto bt = boundaryTimes(parseZones("sat(0)=0.5@10+20 sat(1)=0.5@10 sat(2)=0.5@30+5"));
        CHECK(bt == std::vector<double>({10, 30, 35}), "boundary times");
    }

    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
