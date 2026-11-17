#pragma once
#include <vector>

#include "types.h"

namespace mcsr {

struct Edge {
    int to;
    PathTuple t;
    int ex = -1;  // optional index into a caller-owned table of "expanded" sub-paths (virtual edges)
};

class Graph {
public:
    explicit Graph(int n = 0) : adj(n) {}
    int size() const { return static_cast<int>(adj.size()); }
    void addUndirected(int u, int v, const PathTuple& t) {
        adj[u].push_back({v, t, -1});
        adj[v].push_back({u, t, -1});
    }
    void addDirected(int u, int v, const PathTuple& t, int ex = -1) { adj[u].push_back({v, t, ex}); }
    std::vector<std::vector<Edge>> adj;
};

}  // namespace mcsr
