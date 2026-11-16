// SPDX-License-Identifier: GPL-3.0-only
// Modified/adapted 2026-11-16; see ../THIRD_PARTY_NOTICES.txt.
#pragma once
#include <cmath>
#include <functional>
#include <map>
#include <queue>
#include <string>
#include <vector>
struct Edge {
  int to;
  double delay;
};
struct Tree {
  std::vector<int> next, hops;
  std::vector<double> cost, delay;
};
// Algorithm extension boundary: a graph and destination in, next-hop tree out.
using Router = std::function<Tree(const std::vector<std::vector<Edge>> &, int)>;
Tree dijkstra(const std::vector<std::vector<Edge>> &adj, int root, bool hop) {
  int n = adj.size();
  Tree t;
  t.next.assign(n, -1);
  t.hops.assign(n, -1);
  t.cost.assign(n, INFINITY);
  t.delay.assign(n, INFINITY);
  using Q = std::pair<double, int>;
  std::priority_queue<Q, std::vector<Q>, std::greater<Q>> q;
  t.cost[root] = 0;
  t.delay[root] = 0;
  q.push({0, root});
  while (!q.empty()) {
    auto [d, u] = q.top();
    q.pop();
    if (d > t.cost[u])
      continue;
    for (auto e : adj[u]) {
      double nd = d + (hop ? 1 + 1e-6 * e.delay : e.delay);
      if (nd < t.cost[e.to] - 1e-15 ||
          (std::abs(nd - t.cost[e.to]) <= 1e-15 && u < t.next[e.to])) {
        t.cost[e.to] = nd;
        t.delay[e.to] = t.delay[u] + e.delay;
        t.next[e.to] = u;
        t.hops[e.to] = t.hops[u] + 1;
        q.push({nd, e.to});
      }
    }
  }
  return t;
}

struct RoutingAlgorithm {
  Router tree;
  std::function<double(double)> weight;
};
inline std::map<std::string, RoutingAlgorithm> routingAlgorithms() {
  return {{"dijkstra-delay",
           {[](auto &a, int r) { return dijkstra(a, r, false); },
            [](double d) { return d; }}},
          {"dijkstra-hop",
           {[](auto &a, int r) { return dijkstra(a, r, true); },
            [](double d) { return 1 + 1e-6 * d; }}}};
}
