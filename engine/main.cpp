// SPDX-License-Identifier: GPL-3.0-only
// Modified/adapted 2026-11-16; see ../THIRD_PARTY_NOTICES.txt.
// Portable execution core for the supplied ground-node Dijkstra model.
#include "Global/Utilities/libnorad/cOrbitA.h"
#include "Global/Utilities/libnorad/cSite.h"
#include "vendor/json.hpp"
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <queue>
#include <random>
using J = nlohmann::json;
#include "algorithms.h"
struct Position {
  double lat, lon, alt;
};
double arc(Position a, Position b) {
  double x = (b.lat - a.lat) * RADS_PER_DEG / 2,
         y = (b.lon - a.lon) * RADS_PER_DEG / 2;
  double h = sin(x) * sin(x) + cos(a.lat * RADS_PER_DEG) *
                                   cos(b.lat * RADS_PER_DEG) * sin(y) * sin(y);
  return 2 * atan2(sqrt(h), sqrt(std::max(0., 1 - h)));
}
Position station(const J &s, double time) {
  Position p{s.value("lat", 0.), s.value("lon", 0.), s.value("alt", 0.)};
  if (!s.contains("route") || s.value("speed", 0.) <= 0)
    return p;
  std::vector<Position> ps;
  std::vector<double> lengths{0};
  for (auto v : s["route"])
    ps.push_back({v[0], v[1], p.alt});
  for (size_t i = 1; i < ps.size(); i++)
    lengths.push_back(lengths.back() + 6371000 * arc(ps[i - 1], ps[i]));
  double total = lengths.back();
  if (total <= 0)
    throw std::runtime_error("Zero length station route");
  double d = fmod(s.value("start", 0.) * total + s.value("speed", 0.) * time,
                  2 * total);
  if (d > total)
    d = 2 * total - d;
  size_t i = 1;
  while (i + 1 < ps.size() && lengths[i] < d)
    i++;
  double f = (d - lengths[i - 1]) / (lengths[i] - lengths[i - 1]);
  auto a = ps[i - 1], b = ps[i];
  double angle = arc(a, b);
  if (angle < 1e-12)
    return a;
  double A = sin((1 - f) * angle) / sin(angle), B = sin(f * angle) / sin(angle);
  double x = A * cos(a.lat * RADS_PER_DEG) * cos(a.lon * RADS_PER_DEG) +
             B * cos(b.lat * RADS_PER_DEG) * cos(b.lon * RADS_PER_DEG),
         y = A * cos(a.lat * RADS_PER_DEG) * sin(a.lon * RADS_PER_DEG) +
             B * cos(b.lat * RADS_PER_DEG) * sin(b.lon * RADS_PER_DEG),
         z = A * sin(a.lat * RADS_PER_DEG) + B * sin(b.lat * RADS_PER_DEG);
  return {atan2(z, hypot(x, y)) / RADS_PER_DEG, atan2(y, x) / RADS_PER_DEG,
          p.alt};
}
struct Packet {
  int source, dest, current, hops = 0;
  double created;
};
struct Event {
  long long tick, seq;
  std::function<void()> fn;
  bool operator<(const Event &o) const {
    return tick == o.tick ? seq > o.seq : tick > o.tick;
  }
};
class Simulation {
  J c;
  int n, g;
  double now = 0, limit, interval, gap;
  long long seq = 0, routeId = 0, sent = 0, received = 0, dropped = 0,
            noUplink = 0;
  double delaySum = 0;
  std::priority_queue<Event> events;
  std::vector<std::unique_ptr<cOrbitA>> orbits;
  std::vector<cEci> sat;
  std::vector<Position> ground;
  std::vector<std::vector<Edge>> graph;
  std::map<int, Tree> trees;
  std::map<std::pair<int, int>, double> busy;
  std::mt19937 rng;
  std::ofstream routes, trace;
  RoutingAlgorithm router;
  double random() { return double(rng()) / 4294967296.; }
  void schedule(double t, std::function<void()> fn) {
    if (!std::isfinite(t) || t < now - 1e-10)
      throw std::runtime_error("Invalid event time");
    events.push({llround(t * 1e11), seq++, std::move(fn)});
  }
  void update() {
    graph.assign(n + g, {});
    trees.clear();
    double tick = floor((now + 1e-9) / interval) * interval;
    for (int i = 0; i < n; i++)
      orbits[i]->getPosition((gap + tick) / 60, &sat[i]);
    for (int i = 0; i < g; i++)
      ground[i] = station(c["stations"][i], tick);
    auto add = [&](int a, int b, double d) {
      graph[a].push_back({b, d});
      graph[b].push_back({a, d});
    };
    int planes = c["planes"], per = n / planes, phase = c["phasing"];
    for (int i = 0; i < n; i++) {
      int plane = i / per, index = i % per;
      for (int k = 0; k < 2; k++) {
        if ((k == 0 && c.value("intraDisabled", false)) ||
            (k == 1 && c.value("interDisabled", false)))
          continue;
        int j = k == 0 ? plane * per + (index + 1) % per
                       : (plane == planes - 1 ? (index + phase) % per
                                              : (plane + 1) * per + index);
        auto a = sat[i].getPos(), b = sat[j].getPos();
        add(i, j,
            sqrt(sqr(a.m_x - b.m_x) + sqr(a.m_y - b.m_y) + sqr(a.m_z - b.m_z)) *
                c.value("islDelay", .0000033356));
      }
    }
    std::vector<int> counts(n + g);
    for (int i = 0; i < g; i++) {
      auto p = ground[i];
      cSite site(p.lat, p.lon, p.alt);
      for (int j = 0; j < n; j++) {
        auto look = site.getLookAngle(sat[j]);
        if (look.m_El / RADS_PER_DEG >= c["minimumElevation"].get<double>()) {
          if (++counts[n + i] > c.value("maxGroundLinks", 40) ||
              ++counts[j] > c.value("maxGroundLinks", 40))
            throw std::runtime_error("Ground link capacity exceeded");
          add(n + i, j, look.m_Range * c.value("groundlinkDelay", .0000033356));
        }
      }
    }
    if (now + interval < limit)
      schedule(now + interval, [this] { update(); });
  }
  const Tree &tree(int dest) {
    if (!trees.count(dest)) {
      auto a = graph;
      for (int i = 0; i < g; i++)
        if (i != dest) {
          for (auto e : a[n + i]) {
            auto &v = a[e.to];
            v.erase(std::remove_if(v.begin(), v.end(),
                                   [&](auto x) { return x.to == n + i; }),
                    v.end());
          }
          a[n + i].clear();
        }
      trees.emplace(dest, router.tree(a, n + dest));
    }
    return trees.at(dest);
  }
  double link(int a, int b) {
    for (auto e : graph[a])
      if (e.to == b)
        return e.delay;
    return -1;
  }
  void transmit(std::shared_ptr<Packet> p, int from, int to) {
    double d = link(from, to);
    if (d < 0) {
      drop(p, "link-down");
      return;
    }
    double rate = (from < n && to < n)
                      ? c.value("islDatarate", 100000000.)
                      : c.value("groundlinkDatarate", 50000000.);
    double start = std::max(now, busy[{from, to}]),
           duration =
               (c["stations"][p->source]["packetSize"].get<double>() + 24) * 8 /
               rate;
    busy[{from, to}] = start + duration;
    schedule(start, [this, p, from, to, duration] {
      double d = link(from, to);
      if (link(from, to) < 0) {
        drop(p, "link-down-in-queue");
        return;
      }
      schedule(now + d + duration, [this, p, to] { arrive(p, to); });
    });
  }
  void drop(std::shared_ptr<Packet> p, const char *reason) {
    dropped++;
    trace << now << ",drop," << p->source << "," << p->dest << "," << reason
          << "\n";
  }
  void arrive(std::shared_ptr<Packet> p, int node) {
    if (node >= n) {
      received++;
      delaySum += now - p->created;
      trace << now << ",receive," << p->source << "," << p->dest << ","
            << now - p->created << "\n";
      return;
    }
    p->current = node;
    auto &t = tree(p->dest);
    if (p->hops == 0 && std::isfinite(t.cost[node])) {
      std::vector<int> path;
      int v = node;
      for (int i = 0; i <= n && v < n && v >= 0; i++) {
        path.push_back(v);
        v = t.next[v];
      }
      if (v != n + p->dest) {
        drop(p, "route-loop");
        return;
      }
      double up = link(n + p->source, node);
      routes << now << "," << ++routeId << ",GS" << p->source << "," << p->dest
             << "," << path.front() << "," << path.back() << ","
             << path.size() - 1 << "," << up * 1000 << ","
             << t.delay[node] * 1000 << "," << (up + t.delay[node]) * 1000
             << ",";
      for (size_t i = 0; i < path.size(); i++)
        routes << (i ? "-" : "") << path[i];
      routes << "\n";
    }
    if (++p->hops > c.value("maxHops", 64)) {
      drop(p, "max-hops");
      return;
    }
    int next = t.next[node];
    if (next < 0) {
      drop(p, "no-route");
      return;
    }
    transmit(p, node, next);
  }
  void send(int source) {
    auto &s = c["stations"][source];
    std::vector<int> candidates;
    std::string want = s.value("dstType", std::string("station"));
    for (int j = 0; j < g; j++)
      if (j != source &&
          (want == "any" ||
           (c["stations"][j].value("mobile", false) ? "mobile" : "station") ==
               want))
        candidates.push_back(j);
    int dest = s.value("dstStation", -1);
    if (dest < 0 && !candidates.empty())
      dest = candidates[std::min(candidates.size() - 1,
                                 size_t(random() * candidates.size()))];
    int best = -1;
    double cost = INFINITY;
    if (dest >= 0 && dest < g) {
      auto &t = tree(dest);
      for (auto e : graph[n + source]) {
        double value = router.weight(e.delay) + t.cost[e.to];
        if (value < cost) {
          cost = value;
          best = e.to;
        }
      }
    }
    if (best < 0)
      noUplink++;
    else {
      sent++;
      auto p = std::make_shared<Packet>();
      p->source = source;
      p->dest = dest;
      p->created = now;
      transmit(p, n + source, best);
    }
    double next = now + s["sendInterval"].get<double>();
    if (next < limit)
      schedule(next, [this, source] { send(source); });
  }

public:
  Simulation(J cfg, const std::string &out)
      : c(cfg), n(c["numSatellites"]), g(c["stations"].size()),
        limit(c["simTimeLimit"]), interval(c.value("updateInterval", 1.)),
        sat(n), ground(g), rng(c.value("seed", 23542)) {
    if (n < 1 || n > 5000 || g < 1 || g > 5000 || interval <= 0 || limit <= 0 ||
        limit > 86400 || (c["planes"].get<int>() <= 1 || n % c["planes"].get<int>()))
      throw std::runtime_error("Unsupported dimensions/time");
    auto algorithms = routingAlgorithms();
    std::string algorithm = c.value("algorithm", std::string("dijkstra-delay"));
    if (!algorithms.count(algorithm))
      throw std::runtime_error("Unknown algorithm");
    router = algorithms.at(algorithm);
    auto year = [](int y) { return y < 57 ? y + 2000 : y + 1900; };
    cJulian base(year(c.value("baseYear", 20)), c.value("baseDay", 1.5)),
        epoch(year(c.value("epochYear", 20)), c.value("epochDay", 1.5));
    gap = base.spanSec(epoch);
    int planes = c["planes"], per = n / planes;
    for (int i = 0; i < n; i++)
      orbits.emplace_back(new cOrbitA(
          "sat", c.value("epochYear", 20), c.value("epochDay", 1.5),
          c["altitude"], c.value("eccentricity", .000001), c["inclination"],
          fmod((i / per) * 360. * c["phasing"].get<int>() / n +
                   (i % per) * 360. / per,
               360.),
          c.value("bstar", .00042243), .00001846, i, planes, per,
          (i / per) * 360. / planes, 0));
    routes.open(out + "/routes.csv");
    trace.open(out + "/events.csv");
    if (!routes || !trace)
      throw std::runtime_error("Cannot open output files");
    routes << std::setprecision(12)
           << "time,route,source,station,firstSat,exitSat,satHops,uplinkMs,"
              "satToStationMs,totalMs,path\n";
    trace << std::setprecision(12) << "time,event,source,destination,detail\n";
    update();
    for (int i = 0; i < g; i++)
      if (c["stations"][i].value("enabled", false)) {
        double start = c["stations"][i].contains("startTime")
                           ? c["stations"][i]["startTime"].get<double>()
                           : 1 + 4 * random();
        schedule(start, [this, i] { send(i); });
      }
  }
  void run(const std::string &out) {
    while (!events.empty()) {
      auto e = events.top();
      events.pop();
      now = e.tick / 1e11;
      if (now >= limit)
        break;
      e.fn();
    }
    J summary = {{"engine", "leo-native"},
                 {"version", "1.0.0"},
                 {"sent", sent},
                 {"received", received},
                 {"dropped", dropped},
                 {"noUplink", noUplink},
                 {"inFlight", sent - received - dropped},
                 {"delaySumSeconds", delaySum}};
    std::ofstream(out + "/summary.json") << summary.dump(2);
  }
};
int main(int argc, char **argv) {
  try {
    if (argc != 3)
      throw std::runtime_error("Usage: engine.exe input.json output-directory");
    std::ifstream f(argv[1]);
    J cfg;
    f >> cfg;
    Simulation sim(cfg, argv[2]);
    sim.run(argv[2]);
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << "\n";
    return 1;
  }
}
