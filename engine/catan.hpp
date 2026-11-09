// Two-player Colonist-rules Catan: exact rules, bitboard state, no dependencies.
#pragma once
#include <cstdint>
#include <string>

namespace catan {

constexpr int NODES = 54, EDGES = 72, TILES = 19, ACTIONS = 249;
using Nodes = uint64_t;           // one bit per node
using Edges = unsigned __int128;  // one bit per edge

enum Phase : uint8_t {
    PLACE_SETTLEMENT, PLACE_ROAD, PRE_ROLL, ROLL, DISCARD, MOVE_ROBBER, STEAL, MAIN, DEV_DRAW, ROAD_BUILDING, GAME_OVER
};
// Action IDs: settlement = node, road = 54 + edge, city = 126 + node, then:
enum : int {
    ROLL_DICE = 180, END_TURN = 181, BUY_DEV = 182, PLAY_KNIGHT = 183, PLAY_ROAD_BUILDING = 184,
    YEAR_OF_PLENTY = 185,  // + index into the 15 unordered resource pairs
    MONOPOLY = 200,        // + resource
    MOVE_ROBBER_TO = 205,  // + tile
    DISCARD_ONE = 224,     // + resource
    TRADE = 229            // + give * 4 + (receive < give ? receive : receive - 1)
};
enum { LUMBER, BRICK, WOOL, GRAIN, ORE };
enum { KNIGHT, VICTORY_POINT, ROAD_BUILDING_CARD, YEAR_OF_PLENTY_CARD, MONOPOLY_CARD };

struct Topology {  // identical for every board
    uint8_t tile_nodes[TILES][6], edge_nodes[EDGES][2];
    uint8_t node_edges[NODES][3], node_edge_count[NODES], node_tiles[NODES][3], node_tile_count[NODES];
    Nodes neighbors[NODES], tile_mask[TILES];
    Edges node_edge_mask[NODES];
    uint8_t port_nodes[2][9][2];  // [alternate layout][port][node]
};
const Topology& topology();

struct Rng {  // xoshiro256**
    uint64_t s[4];
    explicit Rng(uint64_t seed);
    uint64_t next();
    uint32_t below(uint32_t n);  // uniform in [0, n)
};

struct Board {
    int8_t resources[TILES];   // -1 = desert
    uint8_t numbers[TILES];    // 0 = desert
    int8_t port_resources[9];  // -1 = generic 3:1
    bool alternate_ports;
    int8_t node_port[NODES];   // -2 = none, -1 = generic, else resource
    uint8_t number_tiles[13][2], number_tile_count[13];
    uint8_t node_pips[NODES];  // dice combinations (out of 36) that pay a building on this node
    static Board from_layout(const int8_t* resources, const uint8_t* numbers, const int8_t* ports, bool alternate);
    static Board random(Rng& rng);
};

struct Config {
    uint8_t victory_points = 15, discard_threshold = 9, friendly_robber_points = 3;
    bool balanced_dice = true;
    uint32_t max_steps = 10000;
};

struct Dice {  // Colonist's balanced dice: a 36-card deck plus streak and recency corrections
    uint8_t deck[11], recent[5], recent_len;
    uint16_t total_sevens[2];
    int8_t streak_player;  // -1 = none
    uint16_t streak_count;
    void weights(int player, uint32_t out[11]) const;
    void draw(int total, int player);
};

struct Player {
    uint8_t hand[5], dev[5], bought[5], played[5], ratio[5];
    bool played_dev;
    Nodes settlements, cities;
    Edges roads;
};

struct State {
    Player p[2];
    uint8_t bank[5], deck[5];
    uint8_t cur, turn_player;
    Phase phase;
    uint32_t turn, steps;
    uint8_t setup_count, robber, free_roads, last_roll, road_len[2];
    int8_t last_setup_node, longest_road, largest_army, winner;  // -1 = none
    bool pre_roll;
    uint8_t discard_count;
    struct { uint8_t player, remaining; } discards[2];
    Dice dice;
    // Each player's public development-card history, which no rule reads: the most old cards kept through a turn
    // that the robber spent on their tile, turns in a row ended holding an old card, turns since a card was
    // played, and old cards held at each turn's end, summed.
    uint8_t card_tested[2], card_held[2], card_since[2];
    uint16_t card_turns[2];
};
static_assert(sizeof(State) == 208, "saved positions keep their size");

State initial(const Board& board);
inline bool is_chance(const State& s) { return s.phase == ROLL || s.phase == STEAL || s.phase == DEV_DRAW; }
inline bool done(const State& s, const Config& c) { return s.winner >= 0 || (s.steps >= c.max_steps && !is_chance(s)); }
int public_points(const State& s, int player);
int total_points(const State& s, int player);
Nodes road_endpoints(Edges roads);
Nodes blocked_nodes(const State& s);            // occupied nodes and their neighbours
Edges road_edges(const State& s, int player);   // free edges that extend this player's network
extern const uint8_t DICE_DECK[11];             // ways to roll each total, 2..12

// Player decisions. legal_actions writes ascending IDs and returns the count (0 when done or at a chance node).
int legal_actions(const Board& b, const State& s, const Config& c, uint8_t out[ACTIONS]);
void apply_action(const Board& b, State& s, const Config& c, int action);

// Chance nodes. Outcomes: roll total - 2, stolen resource, or drawn card. Returns the slot count; weights may be 0.
int chance_weights(const State& s, const Config& c, uint32_t out[11]);
void apply_chance(const Board& b, State& s, const Config& c, int outcome);
int sample_chance(const State& s, const Config& c, Rng& rng);

// Canonical one-line text form of a position with its legal actions and chance odds, for differential tests.
std::string dump(const Board& b, const State& s, const Config& c);

}  // namespace catan
