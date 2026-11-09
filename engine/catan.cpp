#include "catan.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace catan {
namespace {

const int LAND[TILES][2] = {{0, -2}, {1, -2}, {2, -2}, {2, -1}, {2, 0}, {1, 1}, {0, 2}, {-1, 2}, {-2, 2}, {-2, 1},
                            {-2, 0}, {-1, -1}, {0, -1}, {1, -1}, {1, 0}, {0, 1}, {-1, 1}, {-1, 0}, {0, 0}};
const int DIRS[6][2] = {{1, 0}, {0, 1}, {-1, 1}, {-1, 0}, {0, -1}, {1, -1}};
// For each hex corner: the two neighbouring hexes sharing it, as (direction, their corner).
const int SHARED[6][2][2] = {{{5, 4}, {4, 2}}, {{5, 3}, {0, 5}}, {{0, 4}, {1, 0}},
                             {{1, 5}, {2, 1}}, {{2, 0}, {3, 2}}, {{3, 1}, {4, 3}}};
struct PortSpec { int q, r, direction; };  // water hex and the direction of its land neighbour
const PortSpec PORTS[2][9] = {
    {{3, 0, 3}, {1, 2, 4}, {-1, 3, 4}, {-3, 3, 5}, {-3, 1, 0}, {-2, -1, 0}, {0, -3, 1}, {2, -3, 2}, {3, -2, 2}},
    {{1, -3, 1}, {3, -3, 2}, {3, -1, 3}, {2, 1, 3}, {0, 3, 4}, {-2, 3, 5}, {-3, 2, 5}, {-3, 0, 0}, {-1, -2, 1}}};
const uint8_t TOKENS[18] = {5, 2, 6, 3, 8, 10, 9, 12, 11, 4, 8, 10, 9, 4, 5, 6, 3, 11};
const uint8_t DEV_DECK[5] = {14, 5, 2, 2, 2};
const uint8_t ROAD_COST[5] = {1, 1, 0, 0, 0}, SETTLEMENT_COST[5] = {1, 1, 1, 1, 0};
const uint8_t CITY_COST[5] = {0, 0, 0, 2, 3}, DEV_COST[5] = {0, 0, 1, 1, 1};
const uint8_t YOP_PAIRS[15][2] = {{0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}, {1, 1}, {1, 2}, {1, 3},
                                  {1, 4}, {2, 2}, {2, 3}, {2, 4}, {3, 3}, {3, 4}, {4, 4}};
const Nodes ALL_NODES = (Nodes(1) << NODES) - 1;

int hex_id(int q, int r) {
    for (int i = 0; i < TILES; i++)
        if (LAND[i][0] == q && LAND[i][1] == r) return i;
    return -1;
}

Topology build_topology() {
    Topology t{};
    int node_of[TILES * 6], nodes = 0;
    std::fill(node_of, node_of + TILES * 6, -1);
    for (int tile = 0; tile < TILES; tile++)
        for (int corner = 0; corner < 6; corner++) {
            int key = tile * 6 + corner;  // a shared corner is named by its lowest (tile, corner)
            for (auto& shared : SHARED[corner]) {
                int other = hex_id(LAND[tile][0] + DIRS[shared[0]][0], LAND[tile][1] + DIRS[shared[0]][1]);
                if (other >= 0) key = std::min(key, other * 6 + shared[1]);
            }
            if (node_of[key] < 0) node_of[key] = nodes++;
            t.tile_nodes[tile][corner] = uint8_t(node_of[key]);
            t.tile_mask[tile] |= Nodes(1) << node_of[key];
        }
    static int edge_of[NODES][NODES];
    for (auto& row : edge_of) std::fill(row, row + NODES, -1);
    int edges = 0;
    for (int tile = 0; tile < TILES; tile++)
        for (int corner = 0; corner < 6; corner++) {
            int a = t.tile_nodes[tile][corner], b = t.tile_nodes[tile][(corner + 1) % 6];
            if (a > b) std::swap(a, b);
            if (edge_of[a][b] >= 0) continue;
            edge_of[a][b] = edges;
            t.edge_nodes[edges][0] = uint8_t(a);
            t.edge_nodes[edges][1] = uint8_t(b);
            edges++;
        }
    for (int e = 0; e < EDGES; e++)
        for (int k = 0; k < 2; k++) {
            int node = t.edge_nodes[e][k];
            t.neighbors[node] |= Nodes(1) << t.edge_nodes[e][1 - k];
            t.node_edges[node][t.node_edge_count[node]++] = uint8_t(e);
            t.node_edge_mask[node] |= Edges(1) << e;
        }
    for (int tile = 0; tile < TILES; tile++)
        for (int corner = 0; corner < 6; corner++) {
            int node = t.tile_nodes[tile][corner];
            t.node_tiles[node][t.node_tile_count[node]++] = uint8_t(tile);
        }
    for (int alt = 0; alt < 2; alt++)
        for (int i = 0; i < 9; i++) {
            const PortSpec& p = PORTS[alt][i];
            int land = hex_id(p.q + DIRS[p.direction][0], p.r + DIRS[p.direction][1]);
            int corner = ((p.direction + 3) % 6 + 1) % 6;
            t.port_nodes[alt][i][0] = t.tile_nodes[land][corner];
            t.port_nodes[alt][i][1] = t.tile_nodes[land][(corner + 1) % 6];
        }
    return t;
}

inline int pop(Nodes m) { return __builtin_popcountll(m); }
inline int pop(Edges m) { return __builtin_popcountll(uint64_t(m)) + __builtin_popcountll(uint64_t(m >> 64)); }
inline Nodes buildings(const Player& p) { return p.settlements | p.cities; }
inline int hand_total(const Player& p) { return p.hand[0] + p.hand[1] + p.hand[2] + p.hand[3] + p.hand[4]; }
inline int sum5(const uint8_t* v) { return v[0] + v[1] + v[2] + v[3] + v[4]; }
inline bool affordable(const uint8_t* hand, const uint8_t* cost) {
    for (int r = 0; r < 5; r++)
        if (hand[r] < cost[r]) return false;
    return true;
}

template <class F> inline void each_node(Nodes m, F f) {
    for (; m; m &= m - 1) f(__builtin_ctzll(m));
}
template <class F> inline void each_edge(Edges m, F f) {
    for (uint64_t lo = uint64_t(m); lo; lo &= lo - 1) f(__builtin_ctzll(lo));
    for (uint64_t hi = uint64_t(m >> 64); hi; hi &= hi - 1) f(64 + __builtin_ctzll(hi));
}

int walk(const Topology& t, int node, Edges roads, bool moved, Nodes blocked) {
    if (moved && (blocked >> node & 1)) return 0;
    int best = 0;
    for (int k = 0; k < t.node_edge_count[node]; k++) {
        int e = t.node_edges[node][k];
        Edges bit = Edges(1) << e;
        if (!(roads & bit)) continue;
        int next = t.edge_nodes[e][0] == node ? t.edge_nodes[e][1] : t.edge_nodes[e][0];
        best = std::max(best, 1 + walk(t, next, roads & ~bit, true, blocked));
    }
    return best;
}

// Longest trail that uses each road once; a rival building ends it but may start it.
int longest_road(Edges roads, Nodes blocked) {
    const Topology& t = topology();
    int best = 0;
    each_node(road_endpoints(roads), [&](int n) { best = std::max(best, walk(t, n, roads, false, blocked)); });
    return best;
}

int8_t award(int a, int b, int threshold, int8_t holder) {
    if (std::max(a, b) < threshold) return -1;
    return a == b ? holder : (a > b ? 0 : 1);
}

void pay(State& s, const uint8_t* cost) {
    for (int r = 0; r < 5; r++) {
        s.p[s.cur].hand[r] -= cost[r];
        s.bank[r] += cost[r];
    }
}
void take(State& s, int resource) {
    s.bank[resource]--;
    s.p[s.cur].hand[resource]++;
}
void play_dev(State& s, int kind) {
    Player& me = s.p[s.cur];
    me.dev[kind]--;
    me.played[kind]++;
    me.played_dev = true;
    s.card_tested[s.cur] = kind == KNIGHT ? 0 : uint8_t(std::min<int>(s.card_tested[s.cur], sum5(me.dev)));
}
void return_from_dev(State& s) { s.phase = s.pre_roll ? ROLL : MAIN; }

void place_settlement(const Board& b, State& s, int node) {
    const Topology& t = topology();
    Player &me = s.p[s.cur], &other = s.p[1 - s.cur];
    me.settlements |= Nodes(1) << node;
    int port = b.node_port[node];
    if (port == -1)
        for (auto& ratio : me.ratio) ratio = std::min<uint8_t>(ratio, 3);
    else if (port >= 0)
        me.ratio[port] = 2;
    // Only a building in the middle of a rival road can shorten it.
    if (pop(t.node_edge_mask[node] & other.roads) >= 2) {
        s.road_len[1 - s.cur] = uint8_t(longest_road(other.roads, buildings(me)));
        s.longest_road = award(s.road_len[0], s.road_len[1], 5, s.longest_road);
    }
}

void check_victory(State& s, const Config& c) {
    if (s.winner < 0 && s.setup_count == 4 && total_points(s, s.turn_player) >= c.victory_points) {
        s.winner = int8_t(s.turn_player);
        s.phase = GAME_OVER;
        s.cur = s.turn_player;
    }
}

int dev_actions(const State& s, uint8_t* out) {
    const Player& me = s.p[s.cur];
    if (me.played_dev) return 0;
    int n = 0;
    auto available = [&](int kind) { return me.dev[kind] > me.bought[kind]; };
    if (available(KNIGHT)) out[n++] = PLAY_KNIGHT;
    if (available(ROAD_BUILDING_CARD) && pop(me.roads) < 15) out[n++] = PLAY_ROAD_BUILDING;
    if (available(YEAR_OF_PLENTY_CARD))
        for (int i = 0; i < 15; i++) {
            int a = YOP_PAIRS[i][0], b = YOP_PAIRS[i][1];
            if (s.bank[a] >= (a == b ? 2 : 1) && s.bank[b] > 0) out[n++] = uint8_t(YEAR_OF_PLENTY + i);
        }
    if (available(MONOPOLY_CARD))
        for (int r = 0; r < 5; r++) out[n++] = uint8_t(MONOPOLY + r);
    return n;
}

}  // namespace

const uint8_t DICE_DECK[11] = {1, 2, 3, 4, 5, 6, 5, 4, 3, 2, 1};

Nodes road_endpoints(Edges roads) {
    const Topology& t = topology();
    Nodes nodes = 0;
    each_edge(roads, [&](int e) { nodes |= Nodes(1) << t.edge_nodes[e][0] | Nodes(1) << t.edge_nodes[e][1]; });
    return nodes;
}

// The distance rule forbids a settlement on or next to any building.
Nodes blocked_nodes(const State& s) {
    const Topology& t = topology();
    Nodes occupied = buildings(s.p[0]) | buildings(s.p[1]), blocked = occupied;
    each_node(occupied, [&](int n) { blocked |= t.neighbors[n]; });
    return blocked;
}

// A rival building stops a road from continuing through it.
Edges road_edges(const State& s, int player) {
    const Topology& t = topology();
    const Player &me = s.p[player], &other = s.p[1 - player];
    Edges edges = 0;
    each_node(buildings(me) | (road_endpoints(me.roads) & ~buildings(other)), [&](int n) { edges |= t.node_edge_mask[n]; });
    return edges & ~(me.roads | other.roads);
}

const Topology& topology() {
    static const Topology t = build_topology();
    return t;
}

Rng::Rng(uint64_t seed) {
    for (auto& word : s) {  // splitmix64
        uint64_t z = (seed += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        word = z ^ (z >> 31);
    }
}
uint64_t Rng::next() {
    auto rotl = [](uint64_t x, int k) { return (x << k) | (x >> (64 - k)); };
    uint64_t result = rotl(s[1] * 5, 7) * 9, t = s[1] << 17;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl(s[3], 45);
    return result;
}
uint32_t Rng::below(uint32_t n) {  // Lemire's unbiased bounded integer
    uint64_t m = uint64_t(uint32_t(next() >> 32)) * n;
    if (uint32_t(m) < n)
        for (uint32_t threshold = -n % n; uint32_t(m) < threshold;) m = uint64_t(uint32_t(next() >> 32)) * n;
    return uint32_t(m >> 32);
}

Board Board::from_layout(const int8_t* resources, const uint8_t* numbers, const int8_t* ports, bool alternate) {
    const Topology& t = topology();
    Board b{};
    std::copy(resources, resources + TILES, b.resources);
    std::copy(numbers, numbers + TILES, b.numbers);
    std::copy(ports, ports + 9, b.port_resources);
    b.alternate_ports = alternate;
    std::fill(b.node_port, b.node_port + NODES, int8_t(-2));
    for (int i = 0; i < 9; i++)
        for (int k = 0; k < 2; k++) b.node_port[t.port_nodes[alternate][i][k]] = ports[i];
    for (int tile = 0; tile < TILES; tile++)
        if (int number = numbers[tile]) {
            b.number_tiles[number][b.number_tile_count[number]++] = uint8_t(tile);
            for (int node : t.tile_nodes[tile]) b.node_pips[node] += DICE_DECK[number - 2];
        }
    return b;
}

Board Board::random(Rng& rng) {
    int8_t resources[TILES] = {0, 0, 0, 0, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, -1};
    int8_t ports[9] = {0, 1, 2, 3, 4, -1, -1, -1, -1};
    for (int i = TILES - 1; i > 0; i--) std::swap(resources[i], resources[rng.below(i + 1)]);
    for (int i = 8; i > 0; i--) std::swap(ports[i], ports[rng.below(i + 1)]);
    uint8_t numbers[TILES];
    for (int tile = 0, token = 0; tile < TILES; tile++) numbers[tile] = resources[tile] < 0 ? 0 : TOKENS[token++];
    return from_layout(resources, numbers, ports, rng.below(2));
}

void Dice::weights(int player, uint32_t out[11]) const {
    int cards = 0, sevens = total_sevens[0] + total_sevens[1];
    for (uint8_t count : deck) cards += count;
    const uint8_t* counts = cards < 13 ? DICE_DECK : deck;  // a thin deck is about to be reshuffled
    int imbalance = sevens >= 2 ? 200 - 200 * total_sevens[player] / sevens : 100;
    int streak = 40 * streak_count * (streak_player == player ? -1 : 1);
    int seven_factor = std::clamp(imbalance + streak, 0, 200);
    for (int i = 0; i < 11; i++) {
        int repeats = 0;
        for (int k = 0; k < recent_len; k++) repeats += recent[k] == i + 2;
        out[i] = uint32_t(counts[i]) * uint32_t(std::max(0, 100 - 34 * repeats)) * uint32_t(i == 5 ? seven_factor : 100);
    }
}

void Dice::draw(int total, int player) {
    int cards = 0;
    for (uint8_t count : deck) cards += count;
    if (cards < 13) std::copy(DICE_DECK, DICE_DECK + 11, deck);
    deck[total - 2]--;
    if (recent_len == 5) std::copy(recent + 1, recent + 5, recent), recent_len = 4;
    recent[recent_len++] = uint8_t(total);
    if (total == 7) {
        total_sevens[player]++;
        streak_count = streak_player == player ? streak_count + 1 : 1;
        streak_player = int8_t(player);
    }
}

State initial(const Board& board) {
    State s{};
    for (auto& player : s.p) std::fill(player.ratio, player.ratio + 5, uint8_t(4));
    std::fill(s.bank, s.bank + 5, uint8_t(19));
    std::copy(DEV_DECK, DEV_DECK + 5, s.deck);
    std::copy(DICE_DECK, DICE_DECK + 11, s.dice.deck);
    s.dice.streak_player = s.last_setup_node = s.longest_road = s.largest_army = s.winner = -1;
    s.phase = PLACE_SETTLEMENT;
    for (int tile = 0; tile < TILES; tile++)
        if (board.resources[tile] < 0) s.robber = uint8_t(tile);
    return s;
}

int public_points(const State& s, int player) {
    return pop(s.p[player].settlements) + 2 * pop(s.p[player].cities) + 2 * (s.longest_road == player) +
           2 * (s.largest_army == player);
}
int total_points(const State& s, int player) { return public_points(s, player) + s.p[player].dev[VICTORY_POINT]; }

int legal_actions(const Board&, const State& s, const Config& c, uint8_t out[ACTIONS]) {
    if (done(s, c) || is_chance(s)) return 0;
    const Topology& t = topology();
    const Player &me = s.p[s.cur], &other = s.p[1 - s.cur];
    int n = 0;
    switch (s.phase) {
    case PLACE_SETTLEMENT:
        each_node(~blocked_nodes(s) & ALL_NODES, [&](int node) { out[n++] = uint8_t(node); });
        break;
    case PLACE_ROAD:
        each_edge(t.node_edge_mask[s.last_setup_node] & ~(me.roads | other.roads), [&](int e) { out[n++] = uint8_t(54 + e); });
        break;
    case PRE_ROLL:
        out[n++] = ROLL_DICE;
        n += dev_actions(s, out + n);
        break;
    case DISCARD:
        for (int r = 0; r < 5; r++)
            if (me.hand[r]) out[n++] = uint8_t(DISCARD_ONE + r);
        break;
    case MOVE_ROBBER: {
        Nodes shielded = 0;  // friendly robber: players still on few points cannot be blocked
        for (int i = 0; i < 2; i++)
            if (public_points(s, i) < c.friendly_robber_points) shielded |= buildings(s.p[i]);
        for (int tile = 0; tile < TILES; tile++)
            if (tile != s.robber && !(t.tile_mask[tile] & shielded)) out[n++] = uint8_t(MOVE_ROBBER_TO + tile);
        if (!n)
            for (int tile = 0; tile < TILES; tile++)
                if (tile != s.robber) out[n++] = uint8_t(MOVE_ROBBER_TO + tile);
        break;
    }
    case ROAD_BUILDING:
        if (pop(me.roads) < 15 && s.free_roads) each_edge(road_edges(s, s.cur), [&](int e) { out[n++] = uint8_t(54 + e); });
        if (!n) out[n++] = END_TURN;
        break;
    case MAIN:
        if (affordable(me.hand, SETTLEMENT_COST) && pop(me.settlements) < 5)
            each_node(road_endpoints(me.roads) & ~blocked_nodes(s), [&](int node) { out[n++] = uint8_t(node); });
        if (affordable(me.hand, ROAD_COST) && pop(me.roads) < 15)
            each_edge(road_edges(s, s.cur), [&](int e) { out[n++] = uint8_t(54 + e); });
        if (affordable(me.hand, CITY_COST) && pop(me.cities) < 4)
            each_node(me.settlements, [&](int node) { out[n++] = uint8_t(126 + node); });
        out[n++] = END_TURN;
        if (affordable(me.hand, DEV_COST) && sum5(s.deck)) out[n++] = BUY_DEV;
        n += dev_actions(s, out + n);
        for (int give = 0; give < 5; give++)
            for (int receive = 0; receive < 5; receive++)
                if (give != receive && me.hand[give] >= me.ratio[give] && s.bank[receive])
                    out[n++] = uint8_t(TRADE + give * 4 + (receive < give ? receive : receive - 1));
        break;
    default:
        break;
    }
    return n;
}

void apply_action(const Board& b, State& s, const Config& c, int action) {
    const Topology& t = topology();
    const int who = s.cur;
    Player &me = s.p[who], &other = s.p[1 - who];
    if (action < 54) {
        if (s.phase == PLACE_SETTLEMENT) {
            place_settlement(b, s, action);
            s.setup_count++;
            s.last_setup_node = int8_t(action);
            if (s.setup_count >= 3)  // the second settlement pays out its tiles
                for (int k = 0; k < t.node_tile_count[action]; k++) {
                    int resource = b.resources[t.node_tiles[action][k]];
                    if (resource >= 0 && s.bank[resource]) take(s, resource);
                }
            s.phase = PLACE_ROAD;
        } else {
            pay(s, SETTLEMENT_COST);
            place_settlement(b, s, action);
        }
    } else if (action < 126) {
        if (s.phase != PLACE_ROAD && s.phase != ROAD_BUILDING) pay(s, ROAD_COST);
        me.roads |= Edges(1) << (action - 54);
        s.road_len[who] = uint8_t(longest_road(me.roads, buildings(other)));
        s.longest_road = award(s.road_len[0], s.road_len[1], 5, s.longest_road);
        if (s.phase == PLACE_ROAD) {
            s.last_setup_node = -1;
            if (s.setup_count < 4) {  // snake order: 0, 1, 1, 0
                s.cur = s.turn_player = s.setup_count == 3 ? 0 : 1;
                s.phase = PLACE_SETTLEMENT;
            } else {
                s.cur = s.turn_player = 0;
                s.pre_roll = true;
                s.phase = PRE_ROLL;
            }
        } else if (s.phase == ROAD_BUILDING) {
            if (--s.free_roads == 0 || pop(me.roads) == 15) {
                s.free_roads = 0;
                return_from_dev(s);
            }
        }
    } else if (action < 180) {
        pay(s, CITY_COST);
        me.settlements &= ~(Nodes(1) << (action - 126));
        me.cities |= Nodes(1) << (action - 126);
    } else if (action == ROLL_DICE) {
        s.phase = ROLL;
    } else if (action == END_TURN) {
        if (s.phase == ROAD_BUILDING) {
            s.free_roads = 0;
            return_from_dev(s);
        } else {
            const int kept = sum5(me.dev) - sum5(me.bought);  // the card history, before this turn's counts reset
            if (!me.played_dev && b.resources[s.robber] >= 0 && (buildings(me) & t.tile_mask[s.robber]) && kept > 0)
                s.card_tested[who] = uint8_t(std::max<int>(s.card_tested[who], kept));
            s.card_held[who] = kept > 0 ? uint8_t(std::min(255, s.card_held[who] + 1)) : 0;
            s.card_turns[who] = uint16_t(std::min(65535, s.card_turns[who] + std::max(kept, 0)));
            s.card_since[who] = me.played_dev ? 0 : uint8_t(std::min(255, s.card_since[who] + 1));
            std::fill(me.bought, me.bought + 5, uint8_t(0));
            me.played_dev = false;
            s.turn++;
            s.cur = s.turn_player = uint8_t(1 - s.turn_player);
            s.pre_roll = true;
            s.phase = PRE_ROLL;
        }
    } else if (action == BUY_DEV) {
        pay(s, DEV_COST);
        s.phase = DEV_DRAW;
    } else if (action == PLAY_KNIGHT) {
        play_dev(s, KNIGHT);
        s.largest_army = award(s.p[0].played[KNIGHT], s.p[1].played[KNIGHT], 3, s.largest_army);
        s.phase = MOVE_ROBBER;
    } else if (action == PLAY_ROAD_BUILDING) {
        play_dev(s, ROAD_BUILDING_CARD);
        s.free_roads = 2;
        s.phase = ROAD_BUILDING;
    } else if (action < MONOPOLY) {
        play_dev(s, YEAR_OF_PLENTY_CARD);
        take(s, YOP_PAIRS[action - YEAR_OF_PLENTY][0]);
        take(s, YOP_PAIRS[action - YEAR_OF_PLENTY][1]);
        return_from_dev(s);
    } else if (action < MOVE_ROBBER_TO) {
        play_dev(s, MONOPOLY_CARD);
        int resource = action - MONOPOLY;
        me.hand[resource] += other.hand[resource];
        other.hand[resource] = 0;
        return_from_dev(s);
    } else if (action < DISCARD_ONE) {
        s.robber = uint8_t(action - MOVE_ROBBER_TO);
        if ((buildings(other) & t.tile_mask[s.robber]) && hand_total(other) &&
            public_points(s, 1 - who) >= c.friendly_robber_points)
            s.phase = STEAL;
        else
            return_from_dev(s);
    } else if (action < TRADE) {
        int resource = action - DISCARD_ONE;
        me.hand[resource]--;
        s.bank[resource]++;
        if (--s.discards[0].remaining == 0) {
            s.discards[0] = s.discards[1];
            if (--s.discard_count) {
                s.cur = s.discards[0].player;
            } else {
                s.cur = s.turn_player;
                s.phase = MOVE_ROBBER;
            }
        }
    } else {
        int give = (action - TRADE) / 4, receive = (action - TRADE) % 4;
        receive += receive >= give;
        me.hand[give] -= me.ratio[give];
        s.bank[give] += me.ratio[give];
        take(s, receive);
    }
    s.steps++;
    check_victory(s, c);
}

int chance_weights(const State& s, const Config& c, uint32_t out[11]) {
    if (s.phase == ROLL) {
        if (c.balanced_dice)
            s.dice.weights(s.cur, out);
        else
            std::copy(DICE_DECK, DICE_DECK + 11, out);
        return 11;
    }
    if (s.phase != STEAL && s.phase != DEV_DRAW) return 0;
    const uint8_t* counts = s.phase == STEAL ? s.p[1 - s.cur].hand : s.deck;
    std::copy(counts, counts + 5, out);
    return 5;
}

void apply_chance(const Board& b, State& s, const Config& c, int outcome) {
    const Topology& t = topology();
    if (s.phase == ROLL) {
        int total = outcome + 2;
        if (c.balanced_dice) s.dice.draw(total, s.cur);
        s.last_roll = uint8_t(total);
        s.pre_roll = false;
        if (total == 7) {
            s.discard_count = 0;
            for (int player : {int(s.turn_player), 1 - s.turn_player})
                if (int cards = hand_total(s.p[player]); cards > c.discard_threshold)
                    s.discards[s.discard_count++] = {uint8_t(player), uint8_t(cards / 2)};
            if (s.discard_count) {
                s.cur = s.discards[0].player;
                s.phase = DISCARD;
            } else {
                s.phase = MOVE_ROBBER;
            }
        } else {
            uint8_t gains[2][5] = {};
            for (int k = 0; k < b.number_tile_count[total]; k++) {
                int tile = b.number_tiles[total][k];
                if (tile == s.robber) continue;
                for (int i = 0; i < 2; i++)
                    gains[i][b.resources[tile]] += uint8_t(pop(s.p[i].settlements & t.tile_mask[tile]) +
                                                           2 * pop(s.p[i].cities & t.tile_mask[tile]));
            }
            for (int r = 0; r < 5; r++) {
                int demand = gains[0][r] + gains[1][r];
                if (s.bank[r] < demand) continue;  // a shortage cancels this resource for everyone
                s.p[0].hand[r] += gains[0][r];
                s.p[1].hand[r] += gains[1][r];
                s.bank[r] -= uint8_t(demand);
            }
            s.phase = MAIN;
        }
    } else if (s.phase == STEAL) {
        s.p[1 - s.cur].hand[outcome]--;
        s.p[s.cur].hand[outcome]++;
        return_from_dev(s);
    } else {
        s.deck[outcome]--;
        s.p[s.cur].dev[outcome]++;
        s.p[s.cur].bought[outcome]++;
        s.phase = MAIN;
    }
    check_victory(s, c);
}

int sample_chance(const State& s, const Config& c, Rng& rng) {
    uint32_t weights[11], total = 0;
    int n = chance_weights(s, c, weights);
    for (int i = 0; i < n; i++) total += weights[i];
    uint32_t pick = rng.below(total);
    for (int i = 0; i < n; i++) {
        if (pick < weights[i]) return i;
        pick -= weights[i];
    }
    return -1;
}

std::string dump(const Board& b, const State& s, const Config& c) {
    std::string out;
    char buffer[64];
    auto text = [&](const char* format, long value) {
        snprintf(buffer, sizeof buffer, format, value);
        out += buffer;
    };
    auto list = [&](const char* name, const uint8_t* values, int n) {
        out += name;
        for (int i = 0; i < n; i++) text(i ? ",%ld" : "%ld", values[i]);
        if (!n) out += '-';
    };
    text("ph=%ld", s.phase), text(" cp=%ld", s.cur), text(" tp=%ld", s.turn_player), text(" t=%ld", s.turn);
    text(" sc=%ld", s.setup_count), text(" ls=%ld", s.last_setup_node), text(" rb=%ld", s.robber);
    text(" pr=%ld", s.pre_roll), text(" fr=%ld", s.free_roads), text(" lr=%ld", s.longest_road);
    text(" la=%ld", s.largest_army), text(" rl=%ld", s.road_len[0]), text(",%ld", s.road_len[1]);
    text(" roll=%ld", s.last_roll), text(" st=%ld", s.steps), text(" w=%ld", s.winner);
    list(" bank=", s.bank, 5), list(" deck=", s.deck, 5);
    out += " disc=";
    for (int i = 0; i < s.discard_count; i++) text(i ? ";%ld" : "%ld", s.discards[i].player), text(":%ld", s.discards[i].remaining);
    if (!s.discard_count) out += '-';
    list(" dice=", s.dice.deck, 11), list("/", s.dice.recent, s.dice.recent_len);
    text("/%ld", s.dice.total_sevens[0]), text(",%ld", s.dice.total_sevens[1]);
    text("/%ld", s.dice.streak_player), text("/%ld", s.dice.streak_count);
    for (const Player& p : s.p) {
        uint8_t ids[EDGES];
        int n = 0;
        list(" | hand=", p.hand, 5), list(" dev=", p.dev, 5), list(" bought=", p.bought, 5), list(" played=", p.played, 5);
        each_node(p.settlements, [&](int node) { ids[n++] = uint8_t(node); });
        list(" s=", ids, n), n = 0;
        each_node(p.cities, [&](int node) { ids[n++] = uint8_t(node); });
        list(" c=", ids, n), n = 0;
        each_edge(p.roads, [&](int e) { ids[n++] = uint8_t(e); });
        list(" r=", ids, n);
        text(" pd=%ld", p.played_dev), list(" tr=", p.ratio, 5);
    }
    uint8_t legal[ACTIONS];
    list(" | L=", legal, legal_actions(b, s, c, legal));
    uint32_t weights[11];
    int slots = done(s, c) ? 0 : chance_weights(s, c, weights), shown = 0;
    out += " C=";
    for (int i = 0; i < slots; i++)
        if (weights[i]) text(shown++ ? ",%ld" : "%ld", i), text(":%ld", weights[i]);
    if (!shown) out += '-';
    return out;
}

}  // namespace catan
