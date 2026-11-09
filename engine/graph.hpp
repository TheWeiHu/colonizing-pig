// A second kind of evaluator. It reads the board item by item (19 tiles, 54 nodes, 72 edges) beside 245
// whole-position inputs, passes messages between neighbouring nodes, and scores the pooled result.
#pragma once
#include "agent.hpp"

#include <string>
#include <vector>

namespace catan {

constexpr int GRAPH_GLOBAL = 245, GRAPH_TILE = 15, GRAPH_NODE = 46, GRAPH_EDGE = 12;
constexpr int GRAPH_INPUTS = GRAPH_GLOBAL + TILES * GRAPH_TILE + NODES * GRAPH_NODE + EDGES * GRAPH_EDGE;  // 3878

// Describes a view from the side of the player to move: the global inputs, then every tile's, node's and
// edge's. The rival's development cards are expected from the unseen pool, never read.
void graph_features(const Board& b, const Config& c, const View& v, float out[GRAPH_INPUTS]);

struct GraphNet {
    struct Layer {
        int inputs = 0, outputs = 0;
        std::vector<float> weight, bias;  // weight[input][output]
    };
    Layer tile, node, edge;
    std::vector<Layer> rounds, hidden;
    std::vector<float> head;
    float head_bias = 0;
    bool load(const std::string& path);
    float eval(const float* x) const;                                      // the mover's expected result, -1 to 1
    float value(const Board& b, const Config& c, const View& v) const;     // the viewer's chance to win
};

}  // namespace catan
