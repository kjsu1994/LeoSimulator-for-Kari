#pragma once
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
