#include "graph.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace catan {
namespace {

const uint8_t PIPS[13] = {0, 0, 1, 2, 3, 4, 5, 0, 5, 4, 3, 2, 1};  // dice combinations of each total, sevens aside
const float CARDS_OF[5] = {14, 5, 2, 2, 2};                       // development cards of each kind
const float FADE[5] = {1.0f, 0.8f, 0.64f, 0.51f, 0.41f};          // a settlement spot's worth per road it needs
const float GAIN_WEIGHTS[5] = {1.0f, 1.0f, 1.0f, 1.5f, 2.0f};     // grain and ore build cities and cards
const uint8_t ROAD_COST[5] = {1, 1, 0, 0, 0}, SETTLEMENT_COST[5] = {1, 1, 1, 1, 0}, CITY_COST[5] = {0, 0, 0, 2, 3},
              CARD_COST[5] = {0, 0, 1, 1, 1};
const int ROUTE_SPOTS = 6, ROUTE_REACH = 5, MAX_DETOUR = 3, STRONG_PIPS = 9;
const uint8_t FAR = 255;
const Nodes ALL_NODES = (Nodes(1) << NODES) - 1;

inline Nodes node_bit(int n) { return Nodes(1) << n; }
inline Edges edge_bit(int e) { return Edges(1) << e; }
inline int low(Nodes m) { return __builtin_ctzll(m); }
inline int low_edge(Edges m) { return uint64_t(m) ? __builtin_ctzll(uint64_t(m)) : 64 + __builtin_ctzll(uint64_t(m >> 64)); }
inline int pop(Nodes m) { return __builtin_popcountll(m); }
inline int pop_edges(Edges m) { return __builtin_popcountll(uint64_t(m)) + __builtin_popcountll(uint64_t(m >> 64)); }
inline int sum5(const uint8_t* v) { return v[0] + v[1] + v[2] + v[3] + v[4]; }
inline Nodes buildings(const Player& p) { return p.settlements | p.cities; }

struct Ends {  // both end nodes of every edge, as a mask
    Nodes of[EDGES];
    Ends() {
        const Topology& t = topology();
        for (int e = 0; e < EDGES; e++) of[e] = node_bit(t.edge_nodes[e][0]) | node_bit(t.edge_nodes[e][1]);
    }
};
const Ends& ends() {
    static const Ends table;
    return table;
}
Nodes endpoints(Edges edges) {
    Nodes nodes = 0;
    for (; edges; edges &= edges - 1) nodes |= ends().of[low_edge(edges)];
    return nodes;
}

// ---- longest road ------------------------------------------------------------------------------------

// The longest walk over unvisited roads from `node`, which a rival building (`walls`) ends.
int walk(int node, Edges visited, Edges roads, Nodes walls) {
    if (walls & node_bit(node)) return 0;
    int best = 0;
    for (Edges reach = topology().node_edge_mask[node] & roads & ~visited; reach; reach &= reach - 1) {
        const int e = low_edge(reach), next = low(ends().of[e] & ~node_bit(node));
        best = std::max(best, walk(next, visited | edge_bit(e), roads, walls) + 1);
    }
    return best;
}
// The same walk, with the nodes of the first longest one found.
int walk_nodes(int node, Edges visited, Edges roads, Nodes walls, Nodes& path) {
    if (walls & node_bit(node)) return 0;
    path = node_bit(node);
    int best = 0;
    for (Edges reach = topology().node_edge_mask[node] & roads & ~visited; reach; reach &= reach - 1) {
        const int e = low_edge(reach), next = low(ends().of[e] & ~node_bit(node));
        Nodes child = 0;
        const int length = walk_nodes(next, visited | edge_bit(e), roads, walls, child) + 1;
        if (length > best) best = length, path = node_bit(node) | child;
    }
    return best;
}
Edges component(int node, Edges roads) {  // the roads connected to a node, buildings ignored
    Edges found = 0;
    Nodes seen = node_bit(node), frontier = seen;
    while (frontier) {
        const int n = low(frontier);
        frontier &= frontier - 1;
        Edges fresh = topology().node_edge_mask[n] & roads & ~found;
        found |= fresh;
        for (; fresh; fresh &= fresh - 1) {
            const Nodes far = ends().of[low_edge(fresh)] & ~seen;
            seen |= far, frontier |= far;
        }
    }
    return found;
}
// The longest road and, when `path` is given, the nodes on the first longest one found. Walks start where
// a road ends, branches or meets a rival building; a closed loop has no such node and starts anywhere.
int longest(Edges roads, Nodes walls, Nodes* path = nullptr) {
    if (!roads) return 0;
    const Topology& t = topology();
    uint8_t degree[NODES] = {};
    for (Edges rest = roads; rest; rest &= rest - 1)
        for (Nodes both = ends().of[low_edge(rest)]; both; both &= both - 1) degree[low(both)]++;
    int best = 0;
    Edges covered = 0;
    auto start_from = [&](int node) {
        for (Edges rest = t.node_edge_mask[node] & roads; rest; rest &= rest - 1) {
            const int e = low_edge(rest), other = low(ends().of[e] & ~node_bit(node));
            if (path) {
                Nodes nodes = 0;
                const int length = walk_nodes(other, edge_bit(e), roads, walls, nodes) + 1;
                if (length > best) best = length, *path = nodes | node_bit(node);
            } else best = std::max(best, walk(other, edge_bit(e), roads, walls) + 1);
        }
    };
    for (int node = 0; node < NODES; node++) {
        if (!degree[node] || (degree[node] == 2 && !(walls & node_bit(node)))) continue;
        if (t.node_edge_mask[node] & roads & ~covered) covered |= component(node, roads);
        start_from(node);
    }
    for (Edges loops = roads & ~covered; loops;) {
        const int start = low(ends().of[low_edge(loops)]);
        start_from(start);
        loops &= ~component(start, roads);
    }
    return best;
}

// ---- roads still to build ----------------------------------------------------------------------------

// Roads a player needs to reach each node, up to `most`: from their buildings and road ends, never along an
// edge that has a road, never through a rival building.
void road_distances(const State& s, int p, int most, uint8_t dist[NODES]) {
    const Topology& t = topology();
    const Nodes walls = buildings(s.p[1 - p]);
    const Edges taken = s.p[0].roads | s.p[1].roads;
    Nodes frontier = (buildings(s.p[p]) | endpoints(s.p[p].roads)) & ~walls, seen = frontier;
    std::fill(dist, dist + NODES, FAR);
    for (int step = 0; step <= most; step++) {
        Nodes next = 0;
        for (Nodes m = frontier; m; m &= m - 1) {
            const int n = low(m);
            dist[n] = uint8_t(step);
            next |= endpoints(t.node_edge_mask[n] & ~taken);
        }
        next &= ~seen;
        seen |= next;
        frontier = next & ~walls;
    }
}
// Steps from a player's roads and buildings to each node along any edges, as min(steps, 6) / 6.
void network_distances(const Player& p, float out[NODES]) {
    const Topology& t = topology();
    Nodes visited = buildings(p) | endpoints(p.roads);
    if (!visited) {
        std::fill(out, out + NODES, 1.0f);
        return;
    }
    uint8_t dist[NODES];
    std::fill(dist, dist + NODES, FAR);
    for (Nodes m = visited; m; m &= m - 1) dist[low(m)] = 0;
    Nodes frontier = visited;
    for (int step = 1; frontier && step <= 6; step++) {
        Nodes next = 0;
        for (Nodes m = frontier; m; m &= m - 1) next |= t.neighbors[low(m)] & ~visited;
        for (Nodes m = next; m; m &= m - 1) dist[low(m)] = uint8_t(step);
        visited |= next;
        frontier = next;
    }
    for (int n = 0; n < NODES; n++) out[n] = dist[n] == FAR ? 1.0f : float(dist[n]) / 6.0f;
}
// The empty edges on some shortest road from a player's network to `target`, given their road distances.
Edges path_edges(const State& s, int p, const uint8_t dist[NODES], int target) {
    const Topology& t = topology();
    const int total = dist[target];
    if (total == 0 || total == FAR) return 0;
    const Nodes walls = buildings(s.p[1 - p]);
    const Edges taken = s.p[0].roads | s.p[1].roads;
    uint8_t back[NODES];
    std::fill(back, back + NODES, FAR);
    back[target] = 0;
    Nodes frontier = node_bit(target), seen = frontier;
    for (int step = 1; step <= total; step++) {
        Nodes next = 0;
        for (Nodes m = frontier; m; m &= m - 1) next |= endpoints(t.node_edge_mask[low(m)] & ~taken);
        next &= ~seen & ~walls;
        seen |= next;
        for (Nodes m = next; m; m &= m - 1) back[low(m)] = uint8_t(step);
        frontier = next;
    }
    Edges edges = 0;
    for (int e = 0; e < EDGES; e++) {
        if (taken & edge_bit(e)) continue;
        const int a = t.edge_nodes[e][0], z = t.edge_nodes[e][1];
        for (int flip = 0; flip < 2; flip++) {
            const int from = flip ? z : a, to = flip ? a : z;
            if (dist[from] != FAR && back[to] != FAR && dist[from] + 1 + back[to] == total) edges |= edge_bit(e);
        }
    }
    return edges;
}

// The ways from a player's network to their best open spots: the empty edges and open nodes every shortest
// road to one of them passes, and how many roads the way grows by if each were blocked (three if cut off).
struct Routes {
    Edges chokes = 0;
    Nodes node_chokes = 0;
    uint8_t detour[EDGES] = {}, node_detour[NODES] = {};
};
// Roads from `start` to `target` with the road between nodes a and z unusable too, or FAR beyond `most`.
int roads_without(const Nodes near[NODES], Nodes start, Nodes walls, int a, int z, int target, int most) {
    const Nodes goal = node_bit(target);
    Nodes frontier = start & ~walls;
    if (frontier & goal) return 0;
    Nodes seen = frontier | walls;
    for (int step = 1; step <= most; step++) {
        Nodes next = 0;
        for (Nodes m = frontier; m; m &= m - 1) {
            const int n = low(m);
            const Nodes cut = n == a ? node_bit(z) : n == z ? node_bit(a) : 0;
            next |= near[n] & ~cut;
        }
        next &= ~seen;
        if (next & goal) return step;
        if (!next) break;
        seen |= next;
        frontier = next;
    }
    return FAR;
}
void routes_of(const State& s, int p, Nodes open, const uint8_t pips[NODES], const Nodes near[NODES], Routes& routes) {
    const Topology& t = topology();
    const Nodes walls = buildings(s.p[1 - p]), start = buildings(s.p[p]) | endpoints(s.p[p].roads);
    if (!(start & ~walls)) return;
    // Roads to every node within reach and how many shortest roads reach it.
    uint8_t dist[NODES];
    uint32_t ways_to[NODES] = {};
    std::fill(dist, dist + NODES, FAR);
    Nodes frontier = start & ~walls;
    for (Nodes m = frontier; m; m &= m - 1) dist[low(m)] = 0, ways_to[low(m)] = 1;
    for (int step = 1; step <= ROUTE_REACH; step++) {
        Nodes next = 0;
        for (Nodes m = frontier; m; m &= m - 1) {
            const int n = low(m);
            for (Nodes k = near[n] & ~walls; k; k &= k - 1) {
                const int to = low(k);
                if (dist[to] == FAR) dist[to] = uint8_t(step), next |= node_bit(to);
                if (dist[to] == step) ways_to[to] += ways_to[n];
            }
        }
        if (!next) break;
        frontier = next;
    }
    // The best open spots one to five roads away: most pips, then fewest roads, then lowest node.
    int spots[ROUTE_SPOTS] = {}, found = 0;
    auto before = [&](int a, int z) {
        if (pips[a] != pips[z]) return pips[a] > pips[z];
        if (dist[a] != dist[z]) return dist[a] < dist[z];
        return a < z;
    };
    for (Nodes m = open; m; m &= m - 1) {
        const int n = low(m);
        if (dist[n] < 1 || dist[n] > ROUTE_REACH) continue;
        int at = found;
        while (at > 0 && before(n, spots[at - 1])) at--;
        if (at < ROUTE_SPOTS) {
            const int end = std::min(found + 1, ROUTE_SPOTS);
            for (int k = end - 1; k > at; k--) spots[k] = spots[k - 1];
            spots[at] = n;
            found = end;
        }
    }
    for (int k = 0; k < found; k++) {
        const int spot = spots[k], roads = dist[spot];
        const uint64_t ways = ways_to[spot];
        // Walk back from the spot a layer at a time, counting the ways on from each node: a road or a node is
        // on every shortest road when the ways to it times the ways on from it are all of them.
        uint32_t on[NODES] = {};
        on[spot] = 1;
        Nodes layer = node_bit(spot);
        for (int d = roads; d >= 1; d--) {
            Nodes previous = 0;
            for (Nodes m = layer; m; m &= m - 1) {
                const int v = low(m);
                if (d < roads && (open & node_bit(v)) && uint64_t(ways_to[v]) * on[v] == ways) {
                    routes.node_chokes |= node_bit(v);
                    const int around = roads_without(near, start, walls | node_bit(v), NODES, NODES, spot, roads + MAX_DETOUR);
                    routes.node_detour[v] = std::max<uint8_t>(routes.node_detour[v], uint8_t(std::min(std::max(around - roads, 0), MAX_DETOUR)));
                }
                for (Nodes k2 = near[v]; k2; k2 &= k2 - 1) {
                    const int u = low(k2);
                    if (dist[u] != d - 1) continue;
                    on[u] += on[v];
                    previous |= node_bit(u);
                    if (uint64_t(ways_to[u]) * on[v] != ways) continue;
                    const int e = low_edge(t.node_edge_mask[u] & t.node_edge_mask[v]);
                    routes.chokes |= edge_bit(e);
                    const int around = roads_without(near, start, walls, u, v, spot, roads + MAX_DETOUR);
                    routes.detour[e] = std::max<uint8_t>(routes.detour[e], uint8_t(std::min(std::max(around - roads, 0), MAX_DETOUR)));
                }
            }
            layer = previous;
        }
    }
}

// ---- paying for things -------------------------------------------------------------------------------

// Turns from `start` on until the hand pays for a cost, trading surplus at the player's rates, with `income`
// cards per roll and two rolls a turn; -1 beyond ten turns.
int turns_to_fund(int start, const uint8_t hand[5], const float income[5], const uint8_t ratio[5], const uint8_t cost[5]) {
    for (int turn = start; turn <= 10; turn++) {
        float missing = 0.0f, trades = 0.0f;
        for (int r = 0; r < 5; r++) {
            const float have = float(hand[r]) + 2.0f * float(turn) * income[r], need = float(cost[r]);
            if (have < need) missing += need - have;
            else if (turn == 0) trades += float((hand[r] - cost[r]) / ratio[r]);
            else trades += (have - need) / float(ratio[r]);
        }
        if (trades >= missing) return turn;
    }
    return -1;
}
float turns_input(const Player& p, const float income[5], const uint8_t cost[5]) {
    const int turns = turns_to_fund(0, p.hand, income, p.ratio, cost);
    return turns < 0 ? 1.0f : float(turns) / 12.0f;
}
float covered(const uint8_t hand[5], const uint8_t cost[5]) {  // the share of a cost already in hand
    int have = 0, need = 0;
    for (int r = 0; r < 5; r++) have += std::min(hand[r], cost[r]), need += cost[r];
    return float(have) / float(need);
}
float gain_curve(float x) { return 4.0f * x / (1.0f + 4.0f * x); }
float nearness(int roads) { return roads <= 5 ? float(6 - roads) / 6.0f : 0.0f; }

// What the inputs read of one position, worked out once.
struct Facts {
    const Board& b;
    const State& s;
    int me, cur, opp;            // the viewer, the player to move, and the other
    int hidden = 0, hidden_bought = 0, cards_left = 0;  // the rival's unseen cards, those bought this turn, and the deck
    float weight[2][TILES] = {};  // each player's buildings on each tile: settlement 1, city 2
    uint8_t pips[NODES] = {};     // pips of each node's producing tiles
    Edges frontier[2] = {};       // empty edges each player could build on now
    int road[2] = {};             // longest road, counted from the fifth road on
    int left[2][3] = {};          // settlements, cities and roads left
    float prod[2][5] = {};        // pips per resource, robber excluded
    float income[2][5] = {}, settle[2][6] = {};  // cards per roll, and turns/12 to pay for a settlement 0..5 roads away
    uint8_t dist[2][NODES] = {};  // roads to each node
    uint8_t deck[11] = {};        // the dice cards the next roll draws from
    float roll[11] = {};          // the next roll's odds
    Nodes open = 0;               // nodes a settlement could go on
    Facts(const Board& board, const View& v) : b(board), s(v.s), me(v.me), cur(v.s.cur), opp(1 - v.s.cur) {}
};

int public_points_of(const Facts& f, int p) {
    return pop(f.s.p[p].settlements) + 2 * pop(f.s.p[p].cities) + 2 * (f.s.longest_road == p) + 2 * (f.s.largest_army == p);
}
void roll_odds(const State& s, int roller, float out[11]) {
    uint32_t weights[11], total = 0;
    s.dice.weights(roller, weights);
    for (uint32_t w : weights) total += w;
    if (!total) {
        for (int i = 0; i < 11; i++) out[i] = float(DICE_DECK[i]) / 36.0f;
        return;
    }
    const float inverse = 1.0f / float(total);
    for (int i = 0; i < 11; i++) out[i] = float(weights[i]) * inverse;
}
// `count` of the rival's unseen cards by kind, as the viewer expects them from the cards it cannot see.
void expected_cards(const Facts& f, int count, float out[5]) {
    const Player &mine = f.s.p[f.me], &theirs = f.s.p[1 - f.me];
    const float unknown = float(f.cards_left) + float(f.hidden);
    std::fill(out, out + 5, 0.0f);
    if (count == 0 || unknown == 0.0f) return;
    for (int k = 0; k < 5; k++) {
        const float of_kind = CARDS_OF[k] - float(mine.dev[k]) - float(mine.played[k]) - float(theirs.played[k]);
        out[k] = std::max(of_kind, 0.0f) * float(count) / unknown / CARDS_OF[k];
    }
}

float* put_player(const Facts& f, int player, float* out) {
    const State& s = f.s;
    const Player& p = s.p[player];
    const bool known = player == f.me, moving = player == f.cur;
    for (int r = 0; r < 5; r++) *out++ = float(p.hand[r]) / 19.0f;
    for (int r = 0; r < 5; r++) *out++ = float(4 - std::min<int>(p.ratio[r], 4)) / 4.0f;
    float by_resource[5] = {}, by_number[11] = {};
    for (int i = 0; i < TILES; i++) {
        const int number = f.b.numbers[i];
        if (f.b.resources[i] < 0 || !number) continue;
        by_resource[f.b.resources[i]] += f.weight[player][i] * float(PIPS[number]);
        by_number[number - 2] += f.weight[player][i];
    }
    for (float x : by_resource) *out++ = x / 50.0f;
    for (int i = 0; i < 11; i++) if (i != 5) *out++ = by_number[i] / 10.0f;
    *out++ = float(f.left[player][0]) / 5.0f;
    *out++ = float(f.left[player][1]) / 4.0f;
    *out++ = float(f.left[player][2]) / 15.0f;
    *out++ = s.longest_road == player;
    *out++ = float(f.road[player]) / 15.0f;
    *out++ = s.largest_army == player;
    const int points = public_points_of(f, player) + (known ? p.dev[VICTORY_POINT] : 0);
    *out++ = float(points) / 15.0f;
    *out++ = std::min(float(std::max(sum5(p.hand) - 9, 0)) / 19.0f, 1.0f);
    if (known) for (int k = 0; k < 5; k++) *out++ = float(std::max(p.dev[k] - p.bought[k], 0)) / CARDS_OF[k];
    else expected_cards(f, f.hidden, out), out += 5;
    for (int k = 0; k < 5; k++) if (k != VICTORY_POINT) *out++ = float(p.played[k]) / CARDS_OF[k];
    *out++ = std::min(float(s.card_held[player]) / 20.0f, 1.0f);
    *out++ = std::min(float(s.card_turns[player]) / 60.0f, 1.0f);
    *out++ = std::min(float(s.card_since[player]) / 20.0f, 1.0f);
    if (moving) {
        if (known) for (int k = 0; k < 5; k++) *out++ = float(p.bought[k]) / CARDS_OF[k];
        else expected_cards(f, f.hidden_bought, out), out += 5;
        *out++ = p.played_dev;
    }
    return out;
}

// The strategic summary of one player (13). `now` receives the open spots on their roads.
float* put_summary(const Facts& f, int p, Nodes& now, float* out) {
    const Player& pl = f.s.p[p];
    const float* prod = f.prod[p];
    float total = 0.0f, least = 3.4028235e38f, port = 0.0f;
    int kinds = 0;
    for (int r = 0; r < 5; r++) {
        total += prod[r], kinds += prod[r] > 0.0f, least = std::min(least, prod[r]);
        if (pl.ratio[r] == 2) port = std::max(port, prod[r]);
    }
    *out++ = total / 100.0f;
    *out++ = float(kinds) / 5.0f;
    *out++ = least / 20.0f;
    *out++ = port / 50.0f;
    const Nodes on_road = endpoints(pl.roads), one_more = endpoints(f.frontier[p]) & ~on_road;
    now = f.open & on_road;
    const Nodes next = f.open & one_more;
    auto best = [&](Nodes mask) {
        int most = 0;
        for (; mask; mask &= mask - 1) most = std::max<int>(most, f.pips[low(mask)]);
        return float(most) / 13.0f;
    };
    *out++ = std::min(float(pop(now)) / 10.0f, 1.0f);
    *out++ = std::min(float(pop(next)) / 10.0f, 1.0f);
    *out++ = best(now);
    *out++ = best(next);
    for (const uint8_t* cost : {CITY_COST, SETTLEMENT_COST, CARD_COST, ROAD_COST}) *out++ = covered(pl.hand, cost);
    const int cards = sum5(pl.hand), lost = cards > 9 ? cards / 2 : 0;
    *out++ = std::min(f.roll[5] * float(lost) / 4.0f, 1.0f);
    return out;
}

// The plan summary of one player (30). `spot` receives the first spot of their greedy plan, or -1.
float* put_plan(const Facts& f, int p, int& spot, float* out) {
    const State& s = f.s;
    const Player& pl = s.p[p];
    const int rival = 1 - p;
    const uint8_t *mine = f.dist[p], *theirs = f.dist[rival];
    float cards = 0.0f;
    for (uint8_t count : f.deck) cards += float(count);
    float deck_income[5] = {}, all = 0.0f, top = 0.0f, second = 0.0f, shared = 0.0f;
    for (int i = 0; i < TILES; i++) {
        const int number = f.b.numbers[i];
        const float weight = f.weight[p][i];
        if (f.b.resources[i] < 0 || !number || weight == 0.0f) continue;
        if (i != s.robber) deck_income[f.b.resources[i]] += weight * float(f.deck[number - 2]) / cards;
        const float pips = weight * float(PIPS[number]);
        all += pips;
        if (pips > top) second = top, top = pips;
        else second = std::max(second, pips);
        if (f.weight[rival][i] > 0.0f) shared += pips;
    }
    for (float x : deck_income) *out++ = x;
    *out++ = turns_input(pl, f.income[p], ROAD_COST);
    *out++ = f.settle[p][0];
    *out++ = turns_input(pl, f.income[p], CITY_COST);
    *out++ = turns_input(pl, f.income[p], CARD_COST);
    int first = 0, tied = 0, far[3] = {};
    float ports[6] = {}, best = 0.0f;
    spot = -1;
    for (Nodes m = f.open; m; m &= m - 1) {
        const int n = low(m), d = mine[n], other = theirs[n];
        if (d > 5) continue;
        if (d < other) first++;
        else if (d == other) tied++;
        if (d >= 2 && d <= 4) far[d - 2]++;
        if (d > 4) continue;
        if (f.b.node_port[n] > -2) {
            const int kind = f.b.node_port[n] < 0 ? 5 : f.b.node_port[n];
            ports[kind] = std::max(ports[kind], float(5 - d) / 6.0f);
        }
        if (f.left[p][0] > 0 && d <= other && d <= f.left[p][2]) {
            const float value = float(f.pips[n]) * FADE[d];
            if (spot < 0 || value > best) best = value, spot = n;
        }
    }
    *out++ = std::min(float(first) / 30.0f, 1.0f);
    *out++ = std::min(float(tied) / 10.0f, 1.0f);
    if (spot >= 0) {
        const int d = mine[spot], other = theirs[spot];
        *out++ = float(f.pips[spot]) / 15.0f;
        *out++ = float(d) / 4.0f;
        *out++ = f.settle[p][d];
        if (other <= 5 && f.left[rival][0] > 0) *out++ = float(6 - other) / 6.0f, *out++ = f.settle[rival][other];
        else *out++ = 0.0f, *out++ = 1.0f;
    } else {
        for (float x : {0.0f, 0.0f, 1.0f, 0.0f, 1.0f}) *out++ = x;
    }
    for (int count : far) *out++ = std::min(float(count) / 15.0f, 1.0f);
    if (all > 0.0f) *out++ = top / all, *out++ = (top + second) / all, *out++ = shared / all;
    else out[0] = out[1] = out[2] = 0.0f, out += 3;
    bool generic = true;
    for (int r = 0; r < 5; r++) {
        if (pl.ratio[r] == 2) ports[r] = 1.0f;
        generic = generic && pl.ratio[r] <= 3;
    }
    if (generic) ports[5] = 1.0f;
    for (float x : ports) *out++ = x;
    int trades = 0, upgrade = 0;
    for (int r = 0; r < 5; r++) trades += pl.hand[r] / pl.ratio[r];
    *out++ = std::min(float(trades) / 4.0f, 1.0f);
    if (f.left[p][1] > 0) for (Nodes m = pl.settlements; m; m &= m - 1) upgrade = std::max<int>(upgrade, f.pips[low(m)]);
    *out++ = float(upgrade) / 15.0f;
    return out;
}

}  // namespace

void graph_features(const Board& b, const Config&, const View& v, float out[GRAPH_INPUTS]) {
    const Topology& t = topology();
    const State& s = v.s;
    Facts f(b, v);
    const int rival = 1 - v.me, cur = f.cur, opp = f.opp;
    f.hidden = v.rival_cards + sum5(s.p[rival].dev);
    f.hidden_bought = v.rival_bought + sum5(s.p[rival].bought);
    f.cards_left = 25 - sum5(s.p[0].played) - sum5(s.p[1].played) - sum5(s.p[v.me].dev) - f.hidden;
    for (int p = 0; p < 2; p++) {
        const Player& q = s.p[p];
        const Nodes theirs = buildings(s.p[1 - p]);
        for (int i = 0; i < TILES; i++) {
            float w = 0.0f;
            for (int node : t.tile_nodes[i]) w += (q.cities >> node & 1) ? 2.0f : (q.settlements >> node & 1) ? 1.0f : 0.0f;
            f.weight[p][i] = w;
        }
        for (int n = 0; n < NODES; n++)
            if ((buildings(q) & node_bit(n)) || ((t.node_edge_mask[n] & q.roads) && !(theirs & node_bit(n)))) f.frontier[p] |= t.node_edge_mask[n];
        f.frontier[p] &= ~(s.p[0].roads | s.p[1].roads);
        f.road[p] = pop_edges(q.roads) >= 5 ? longest(q.roads, theirs) : 0;
        f.left[p][0] = 5 - pop(q.settlements), f.left[p][1] = 4 - pop(q.cities), f.left[p][2] = 15 - pop_edges(q.roads);
        for (int i = 0; i < TILES; i++)
            if (i != s.robber && b.numbers[i] && b.resources[i] >= 0) f.prod[p][b.resources[i]] += f.weight[p][i] * float(PIPS[b.numbers[i]]);
        road_distances(s, p, 5, f.dist[p]);
        for (int r = 0; r < 5; r++) f.income[p][r] = f.prod[p][r] / 36.0f;
        // A settlement one road further costs at least as much of everything, so it is never paid for sooner.
        int from = 0;
        for (int d = 0; d < 6; d++) {
            const uint8_t cost[5] = {uint8_t(1 + d), uint8_t(1 + d), 1, 1, 0};
            if (from >= 0) from = turns_to_fund(from, q.hand, f.income[p], q.ratio, cost);
            f.settle[p][d] = from < 0 ? 1.0f : float(from) / 12.0f;
        }
    }
    for (int n = 0; n < NODES; n++)
        for (int k = 0; k < t.node_tile_count[n]; k++) {
            const int tile = t.node_tiles[n][k];
            if (b.resources[tile] >= 0) f.pips[n] += PIPS[b.numbers[tile]];
        }
    int dice_cards = 0;
    for (uint8_t count : s.dice.deck) dice_cards += count;
    std::copy(dice_cards >= 13 ? s.dice.deck : DICE_DECK, (dice_cards >= 13 ? s.dice.deck : DICE_DECK) + 11, f.deck);
    const int turn_of = s.phase == DISCARD ? s.turn_player : s.cur;
    roll_odds(s, s.pre_roll ? turn_of : 1 - turn_of, f.roll);
    f.open = ~blocked_nodes(s) & ALL_NODES;

    // ---- global inputs ----
    float* at = out;
    for (int k = 0; k < 10; k++) *at++ = k != s.phase ? 0.0f : s.phase == DISCARD ? std::min(float(s.discards[0].remaining) / 10.0f, 1.0f) : 1.0f;
    *at++ = v.me == cur;
    at = put_player(f, cur, at);
    at = put_player(f, opp, at);
    for (float x : f.roll) *at++ = x;
    *at++ = float(dice_cards) / 36.0f;
    int board_pips[5] = {};
    for (int i = 0; i < TILES; i++) if (b.resources[i] >= 0) board_pips[b.resources[i]] += PIPS[b.numbers[i]];
    for (int x : board_pips) *at++ = float(x) / 20.0f;
    float blocked[5] = {};
    if (b.resources[s.robber] >= 0 && b.numbers[s.robber]) blocked[b.resources[s.robber]] = f.weight[opp][s.robber] * float(PIPS[b.numbers[s.robber]]);
    for (float x : blocked) *at++ = x / 30.0f;
    *at++ = std::min(float(s.card_tested[rival]) / 5.0f, 1.0f);
    *at++ = float(f.cards_left) / 25.0f;
    *at++ = float(s.phase == ROAD_BUILDING ? s.free_roads : 0) / 2.0f;
    for (uint8_t count : s.bank) *at++ = float(count) / 19.0f;
    *at++ = std::clamp((float(f.road[cur]) - float(f.road[opp])) / 5.0f, -1.0f, 1.0f);
    *at++ = std::clamp((float(s.p[cur].played[KNIGHT]) - float(s.p[opp].played[KNIGHT])) / 3.0f, -1.0f, 1.0f);
    Nodes settle_now[2];
    at = put_summary(f, cur, settle_now[0], at);
    at = put_summary(f, opp, settle_now[1], at);
    int plan[2];
    at = put_plan(f, cur, plan[0], at);
    at = put_plan(f, opp, plan[1], at);
    for (uint8_t count : f.deck) *at++ = float(count) / 6.0f;
    for (int p : {cur, opp}) {
        float odds[11];
        roll_odds(s, p, odds);
        *at++ = std::min(odds[5] * 3.0f, 1.0f);
    }
    *at++ = std::min(float(s.turn) / 200.0f, 1.0f);
    *at++ = std::min(float(pop(f.open)) / 40.0f, 1.0f);
    {  // strong open spots: how many each player reaches first, how many are tied, and the best each reaches first
        int firsts[2] = {}, tied = 0, best[2] = {};
        for (Nodes m = f.open; m; m &= m - 1) {
            const int n = low(m), mine = f.dist[cur][n], theirs = f.dist[opp][n];
            if (f.pips[n] < STRONG_PIPS || std::min(mine, theirs) > 5) continue;
            if (mine == theirs) { tied++; continue; }
            const int first = mine > theirs;
            firsts[first]++, best[first] = std::max<int>(best[first], f.pips[n]);
        }
        for (int x : {firsts[0], firsts[1], tied}) *at++ = std::min(float(x) / 5.0f, 1.0f);
        for (int x : best) *at++ = float(x) / 15.0f;
    }

    // ---- tiles ----
    const Player &mover = s.p[cur], &other = s.p[opp];
    const Nodes cur_buildings = buildings(mover), opp_buildings = buildings(other);
    const bool cur_robbable = public_points_of(f, cur) >= 3, opp_robbable = public_points_of(f, opp) >= 3;
    float total_pips[2] = {};
    for (int p = 0; p < 2; p++) for (int i = 0; i < TILES; i++) total_pips[p] += f.weight[p][i] * float(PIPS[b.numbers[i]]);
    for (int i = 0; i < TILES; i++) {
        const int number = b.numbers[i];
        for (int r = 0; r < 5; r++) *at++ = b.resources[i] == r;
        *at++ = number ? float(PIPS[number]) / 5.0f : 0.0f;
        *at++ = number ? f.roll[number - 2] : 0.0f;
        *at++ = s.robber == i;
        const float cur_w = f.weight[cur][i], opp_w = f.weight[opp][i], pips = float(PIPS[number]);
        *at++ = cur_w / 6.0f;
        *at++ = opp_w / 6.0f;
        const bool free = s.robber != i;
        *at++ = free && opp_robbable && (t.tile_mask[i] & opp_buildings);
        *at++ = free && cur_robbable && (t.tile_mask[i] & cur_buildings);
        *at++ = std::clamp(pips * (opp_w - cur_w) / 15.0f, -1.0f, 1.0f);
        *at++ = total_pips[cur] > 0.0f ? pips * cur_w / total_pips[cur] : 0.0f;
        *at++ = total_pips[opp] > 0.0f ? pips * opp_w / total_pips[opp] : 0.0f;
    }

    // ---- nodes ----
    float network[2][NODES];
    network_distances(mover, network[0]);
    network_distances(other, network[1]);
    Nodes on_road[2] = {}, cuts[2] = {};
    for (int k = 0; k < 2; k++) {  // each longest road's nodes, and those where a rival building would shorten it
        const Player& q = k ? other : mover;
        const Nodes walls = k ? cur_buildings : opp_buildings;
        if (pop_edges(q.roads) < 5) continue;
        longest(q.roads, walls, &on_road[k]);
        const int length = f.road[k ? opp : cur];
        for (Nodes m = on_road[k]; m; m &= m - 1) {
            const int n = low(m);
            if (pop_edges(t.node_edge_mask[n] & q.roads) >= 2 && longest(q.roads, walls | node_bit(n)) < length) cuts[k] |= node_bit(n);
        }
        cuts[k] &= f.open;
    }
    Nodes near[NODES];  // each node's neighbours along edges with no road
    const Edges taken = mover.roads | other.roads;
    for (int n = 0; n < NODES; n++) near[n] = endpoints(t.node_edge_mask[n] & ~taken) & ~node_bit(n);
    Routes routes[2];
    routes_of(s, cur, f.open, f.pips, near, routes[0]);
    routes_of(s, opp, f.open, f.pips, near, routes[1]);
    float cards = 0.0f;
    for (uint8_t count : f.deck) cards += float(count);
    cards = std::max(cards, 1.0f);
    float per13[TILES], per5[TILES], per36[TILES], deck_share[TILES], base[2][5], turns[2][6];
    for (int i = 0; i < TILES; i++) {
        const float pips = float(PIPS[b.numbers[i]]);
        per13[i] = pips / 13.0f, per5[i] = pips / 5.0f, per36[i] = pips / 36.0f;
        deck_share[i] = b.numbers[i] ? float(f.deck[b.numbers[i] - 2]) / cards : 0.0f;
    }
    for (int k = 0; k < 2; k++) {
        const int p = k ? opp : cur;
        for (int r = 0; r < 5; r++) base[k][r] = gain_curve(f.income[p][r]);
        for (int d = 0; d < 6; d++) turns[k][d] = f.left[p][0] == 0 || d > f.left[p][2] ? 1.0f : f.settle[p][d];
    }
    auto gain = [&](int k, const float add[5]) {
        const float* income = f.income[k ? opp : cur];
        float sum = 0.0f;
        for (int r = 0; r < 5; r++) if (add[r] != 0.0f) sum += GAIN_WEIGHTS[r] * (gain_curve(income[r] + add[r]) - base[k][r]);
        return std::min(sum / 1.5f, 1.0f);
    };
    for (int n = 0; n < NODES; n++) {
        const Nodes bit = node_bit(n);
        for (const Player* q : {&mover, &other}) *at++ = (q->cities & bit) ? 1.0f : (q->settlements & bit) ? 0.5f : 0.0f;
        const int port = b.node_port[n] == -2 ? 6 : b.node_port[n] == -1 ? 5 : b.node_port[n];
        for (int k = 0; k < 6; k++) *at++ = k == port;
        float yields[10] = {}, add[5] = {}, deck_income = 0.0f;
        for (int k = 0; k < t.node_tile_count[n]; k++) {
            const int tile = t.node_tiles[n][k], r = b.resources[tile];
            if (r < 0) continue;
            if (b.numbers[tile]) yields[tile == s.robber ? 5 + r : r] += tile == s.robber ? per5[tile] : per13[tile];
            if (tile != s.robber) deck_income += deck_share[tile];
            add[r] += per36[tile];
        }
        for (float x : yields) *at++ = x;
        *at++ = float(pop_edges(t.node_edge_mask[n] & mover.roads)) / 3.0f;
        *at++ = float(pop_edges(t.node_edge_mask[n] & other.roads)) / 3.0f;
        *at++ = network[0][n];
        *at++ = network[1][n];
        *at++ = bool(on_road[0] & bit);
        *at++ = bool(on_road[1] & bit);
        const bool is_open = f.open & bit;
        *at++ = is_open;
        *at++ = bool(settle_now[0] & bit);
        *at++ = bool(settle_now[1] & bit);
        const int cd = f.dist[cur][n], od = f.dist[opp][n];
        *at++ = nearness(cd);
        *at++ = nearness(od);
        *at++ = std::min(cd, od) <= 5 ? std::clamp((float(std::min(od, 6)) - float(std::min(cd, 6))) / 5.0f, -1.0f, 1.0f) : 0.0f;
        *at++ = is_open && cd == od && cd <= 5;
        *at++ = is_open && cd <= 1;
        *at++ = is_open && od <= 1;
        *at++ = is_open && cd <= 2;
        *at++ = is_open && od <= 2;
        *at++ = std::min(deck_income / 0.5f, 1.0f);
        *at++ = gain(0, add);
        *at++ = gain(1, add);
        *at++ = bool(cuts[0] & bit);
        *at++ = bool(cuts[1] & bit);
        *at++ = is_open && cd <= 5 ? turns[0][cd] : 1.0f;
        *at++ = is_open && od <= 5 ? turns[1][od] : 1.0f;
        *at++ = bool(routes[0].node_chokes & bit);
        *at++ = bool(routes[1].node_chokes & bit);
        *at++ = float(routes[0].node_detour[n]) / float(MAX_DETOUR);
        *at++ = float(routes[1].node_detour[n]) / float(MAX_DETOUR);
    }

    // ---- edges ----
    Edges touch[2] = {}, path[2];
    for (int k = 0; k < 2; k++) {
        const int p = k ? opp : cur;
        for (Edges m = f.frontier[p]; m; m &= m - 1) if (ends().of[low_edge(m)] & on_road[k]) touch[k] |= edge_bit(low_edge(m));
        path[k] = plan[k] < 0 ? 0 : path_edges(s, p, f.dist[p], plan[k]);
    }
    for (int e = 0; e < EDGES; e++) {
        const Edges bit = edge_bit(e);
        *at++ = bool(mover.roads & bit);
        *at++ = bool(other.roads & bit);
        *at++ = bool(f.frontier[cur] & bit);
        *at++ = bool(f.frontier[opp] & bit);
        *at++ = bool(touch[0] & bit);
        *at++ = bool(touch[1] & bit);
        *at++ = bool(path[0] & bit);
        *at++ = bool(path[1] & bit);
        *at++ = bool(routes[0].chokes & bit);
        *at++ = bool(routes[1].chokes & bit);
        *at++ = float(routes[0].detour[e]) / float(MAX_DETOUR);
        *at++ = float(routes[1].detour[e]) / float(MAX_DETOUR);
    }
    assert(at == out + GRAPH_INPUTS);
}

// ---- the network -----------------------------------------------------------------------------------------

namespace {

// y = relu(x W + b) for `rows` rows. Each output sums its inputs in order, so every machine of a kind agrees.
void dense(const GraphNet::Layer& layer, const float* x, int rows, float* y) {
    const int ins = layer.inputs, outs = layer.outputs;
    for (int row = 0; row < rows; row++, x += ins, y += outs) {
        std::copy(layer.bias.begin(), layer.bias.end(), y);
        for (int i = 0; i < ins; i++) {
            const float value = x[i];
            if (value == 0.0f) continue;
            const float* w = layer.weight.data() + size_t(i) * outs;
            for (int o = 0; o < outs; o++) y[o] += value * w[o];
        }
        for (int o = 0; o < outs; o++) y[o] = std::max(y[o], 0.0f);
    }
}
bool read_layer(FILE* file, GraphNet::Layer& layer) {
    uint32_t shape[2];
    if (fread(shape, 4, 2, file) != 2 || shape[0] > 4096 || shape[1] > 4096) return false;
    layer.inputs = int(shape[0]), layer.outputs = int(shape[1]);
    layer.weight.resize(size_t(shape[0]) * shape[1]), layer.bias.resize(shape[1]);
    return fread(layer.weight.data(), 4, layer.weight.size(), file) == layer.weight.size() &&
           fread(layer.bias.data(), 4, layer.bias.size(), file) == layer.bias.size();
}

}  // namespace

// File: "CATG", version 1, the round and hidden layer counts, then each layer's input and output widths, its
// weights [input][output] and its biases: tile, node, edge, the rounds, the hidden layers, and the output.
bool GraphNet::load(const std::string& path) {
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) return false;
    uint32_t header[4];
    bool ok = fread(header, 4, 4, file) == 4 && header[0] == 0x47544143 && header[1] == 1 && header[2] <= 8 && header[3] <= 8;
    if (ok) rounds.resize(header[2]), hidden.resize(header[3]);
    ok = ok && read_layer(file, tile) && read_layer(file, node) && read_layer(file, edge);
    for (Layer& layer : rounds) ok = ok && read_layer(file, layer);
    for (Layer& layer : hidden) ok = ok && read_layer(file, layer);
    Layer out;
    ok = ok && read_layer(file, out) && out.outputs == 1;
    fclose(file);
    if (!ok) return false;
    head = out.weight, head_bias = out.bias[0];
    const int joined = 2 * node.outputs + tile.outputs + edge.outputs;
    ok = tile.inputs == GRAPH_TILE && node.inputs == GRAPH_NODE && edge.inputs == GRAPH_EDGE && !hidden.empty();
    for (const Layer& layer : rounds) ok = ok && layer.inputs == joined && layer.outputs == node.outputs;
    int width = GRAPH_GLOBAL + joined;
    for (const Layer& layer : hidden) ok = ok && layer.inputs == width, width = layer.outputs;
    return ok && int(head.size()) == width;
}

float GraphNet::eval(const float* x) const {
    const Topology& t = topology();
    const int dt = tile.outputs, dn = node.outputs, de = edge.outputs, joined = 2 * dn + dt + de;
    static thread_local std::vector<float> space;
    space.resize(size_t(TILES) * dt + size_t(EDGES) * de + 2 * size_t(NODES) * dn + size_t(NODES) * joined + 8192);
    float* tiles = space.data();
    float* edges = tiles + TILES * dt;
    float* nodes = edges + EDGES * de;
    float* next = nodes + NODES * dn;
    float* join = next + NODES * dn;
    float* work = join + NODES * joined;
    const float* tile_x = x + GRAPH_GLOBAL;
    const float* node_x = tile_x + TILES * GRAPH_TILE;
    const float* edge_x = node_x + NODES * GRAPH_NODE;
    dense(tile, tile_x, TILES, tiles);
    dense(node, node_x, NODES, nodes);
    dense(edge, edge_x, EDGES, edges);
    // Each node's fixed part of the message: the mean of its tiles and of its edges.
    for (int n = 0; n < NODES; n++) {
        float* row = join + n * joined + 2 * dn;
        std::fill(row, row + dt + de, 0.0f);
        for (int k = 0; k < t.node_tile_count[n]; k++) for (int j = 0; j < dt; j++) row[j] += tiles[t.node_tiles[n][k] * dt + j];
        const float tile_share = 1.0f / float(t.node_tile_count[n]);
        for (int j = 0; j < dt; j++) row[j] *= tile_share;
        for (int k = 0; k < t.node_edge_count[n]; k++) for (int j = 0; j < de; j++) row[dt + j] += edges[t.node_edges[n][k] * de + j];
        const float edge_share = 1.0f / float(t.node_edge_count[n]);
        for (int j = 0; j < de; j++) row[dt + j] *= edge_share;
    }
    for (const Layer& round : rounds) {
        for (int n = 0; n < NODES; n++) {
            float* row = join + n * joined;
            std::copy(nodes + n * dn, nodes + (n + 1) * dn, row);
            std::fill(row + dn, row + 2 * dn, 0.0f);
            for (Nodes m = t.neighbors[n]; m; m &= m - 1) for (int j = 0; j < dn; j++) row[dn + j] += nodes[low(m) * dn + j];
            const float share = 1.0f / float(pop(t.neighbors[n]));
            for (int j = 0; j < dn; j++) row[dn + j] *= share;
        }
        dense(round, join, NODES, next);
        std::swap(nodes, next);
    }
    // Pool: the global inputs, the nodes' mean and largest, the tiles' mean and the edges' mean.
    float* pooled = work;
    std::copy(x, x + GRAPH_GLOBAL, pooled);
    float* node_mean = pooled + GRAPH_GLOBAL;
    float* node_max = node_mean + dn;
    float* tile_mean = node_max + dn;
    float* edge_mean = tile_mean + dt;
    std::fill(node_mean, edge_mean + de, 0.0f);
    std::copy(nodes, nodes + dn, node_max);
    for (int n = 0; n < NODES; n++) for (int j = 0; j < dn; j++) node_mean[j] += nodes[n * dn + j], node_max[j] = std::max(node_max[j], nodes[n * dn + j]);
    for (int j = 0; j < dn; j++) node_mean[j] /= float(NODES);
    for (int i = 0; i < TILES; i++) for (int j = 0; j < dt; j++) tile_mean[j] += tiles[i * dt + j];
    for (int j = 0; j < dt; j++) tile_mean[j] /= float(TILES);
    for (int e = 0; e < EDGES; e++) for (int j = 0; j < de; j++) edge_mean[j] += edges[e * de + j];
    for (int j = 0; j < de; j++) edge_mean[j] /= float(EDGES);
    float* in = pooled;
    float* outp = pooled + 4096;
    for (const Layer& layer : hidden) {
        dense(layer, in, 1, outp);
        std::swap(in, outp);
    }
    float sum = 0.0f;
    for (size_t j = 0; j < head.size(); j++) sum += in[j] * head[j];
    return std::tanh(sum + head_bias);
}

float GraphNet::value(const Board& b, const Config& c, const View& v) const {
    float x[GRAPH_INPUTS];
    graph_features(b, c, v, x);
    const float mover = eval(x);
    return 0.5f * ((v.s.cur == v.me ? mover : -mover) + 1.0f);
}

}  // namespace catan
