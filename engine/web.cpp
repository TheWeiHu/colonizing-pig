#include "model.hpp"
#include "champion.hpp"
#include <iomanip>

using namespace catan;
namespace {
Contestant champion;
Board board;
State state;
Config config;
Rng chance(1);
uint32_t game_seed = 1;
int human = 0;
bool loaded = false, started = false;
std::vector<int> history;
std::string output;
struct Event { int player, action, phase, outcome; };
std::vector<Event> events;

template<class T> void array(std::ostream& out, const T* values, int size) {
    out << '[';
    for (int i = 0; i < size; ++i) { if (i) out << ','; out << int(values[i]); }
    out << ']';
}
void settle_chance() {
    while (is_chance(state)) {
        int player = state.cur, phase = state.phase;
        int outcome = sample_chance(state, config, chance);
        // Opponent development draws remain private even in the event log.
        events.push_back({player, -1, phase, phase == DEV_DRAW && player != human ? -1 : outcome});
        apply_chance(board, state, config, outcome);
    }
}
int move(int action) {
    if (!started || done(state, config)) return 0;
    uint8_t actions[ACTIONS];
    int count = legal_actions(board, state, config, actions);
    if (action < 0 || action >= ACTIONS || std::find(actions, actions + count, action) == actions + count) return 0;
    events.clear();
    events.push_back({state.cur, action, state.phase, -1});
    apply_action(board, state, config, action);
    history.push_back(action);
    settle_chance();
    return 1;
}
}

extern "C" {
int pig_init(int workers) {
    if (!loaded) { if (!load(champion, CHAMPION_SPEC)) return 0; loaded = true; }
#ifdef __EMSCRIPTEN_PTHREADS__
    champion.agent.workers = std::clamp(workers, 1, 4);
#elif defined(__EMSCRIPTEN__)
    champion.agent.workers = 1;
#else
    champion.agent.workers = std::clamp(workers, 1, 4);
#endif
    return 1;
}
void pig_new(uint32_t seed, int seat) {
    game_seed = seed;
    human = seat == 1 ? 1 : 0;
    Rng layout(uint64_t(seed) * 1000003 + 1);
    board = Board::random(layout);
    chance = Rng(uint64_t(seed) * 1000003 + 2);
    state = initial(board);
    history.clear(); events.clear(); started = true;
}
int pig_act(int action) { return started && state.cur == human ? move(action) : 0; }
int pig_replay(int action) { return move(action); }
int pig_bot() {
    if (!loaded || !started || done(state, config) || state.cur == human) return -1;
    Rng choice(uint64_t(game_seed) * 1000003 + uint64_t(state.steps) * 1009 + 3);
    int action = choose(champion.agent, board, config, observe(state, state.cur), choice);
    return move(action) ? action : -1;
}
const char* pig_snapshot() {
    if (!started) return "null";
    std::ostringstream out;
    out << "{\"seed\":" << game_seed << ",\"human\":" << human
        << ",\"current\":" << int(state.cur) << ",\"phase\":" << int(state.phase)
        << ",\"turn\":" << state.turn << ",\"steps\":" << state.steps
        << ",\"winner\":" << int(state.winner) << ",\"done\":" << (done(state, config) ? "true" : "false")
        << ",\"robber\":" << int(state.robber) << ",\"roll\":" << int(state.last_roll)
        << ",\"longestRoad\":" << int(state.longest_road) << ",\"largestArmy\":" << int(state.largest_army)
        << ",\"workers\":" << champion.agent.workers << ",\"tiles\":";
    array(out, board.resources, TILES); out << ",\"numbers\":"; array(out, board.numbers, TILES);
    out << ",\"ports\":"; array(out, board.port_resources, 9);
    out << ",\"bank\":"; array(out, state.bank, 5);
    const auto& topology = catan::topology();
    out << ",\"portNodes\":[";
    for (int i = 0; i < 9; ++i) { if (i) out << ','; array(out, topology.port_nodes[board.alternate_ports][i], 2); }
    out << "],\"tileNodes\":[";
    for (int i = 0; i < TILES; ++i) { if (i) out << ','; array(out, topology.tile_nodes[i], 6); }
    out << "],\"edges\":[";
    for (int i = 0; i < EDGES; ++i) { if (i) out << ','; array(out, topology.edge_nodes[i], 2); }
    out << "],\"players\":[";
    for (int p = 0; p < 2; ++p) {
        if (p) out << ',';
        const Player& player = state.p[p];
        out << "{\"points\":" << (p == human || done(state, config) ? total_points(state, p) : public_points(state, p))
            << ",\"publicPoints\":" << public_points(state, p) << ",\"hand\":";
        array(out, player.hand, 5); out << ",\"ratio\":"; array(out, player.ratio, 5);
        out << ",\"dev\":";
        if (p == human || done(state, config)) array(out, player.dev, 5); else out << "null";
        out << ",\"bought\":";
        if (p == human || done(state, config)) array(out, player.bought, 5); else out << "null";
        int cards = 0; for (int x : player.dev) cards += x;
        out << ",\"devCount\":" << cards << ",\"played\":"; array(out, player.played, 5);
        out << ",\"roadLength\":" << int(state.road_len[p]) << ",\"nodes\":[";
        for (int i = 0; i < NODES; ++i) { if (i) out << ','; out << ((player.cities >> i & 1) ? 2 : (player.settlements >> i & 1) ? 1 : 0); }
        out << "],\"roads\":[";
        for (int i = 0; i < EDGES; ++i) { if (i) out << ','; out << int(player.roads >> i & 1); }
        out << "]}";
    }
    out << "],\"legal\":";
    uint8_t actions[ACTIONS];
    int count = state.cur == human && !done(state, config) ? legal_actions(board, state, config, actions) : 0;
    array(out, actions, count);
    out << ",\"history\":"; array(out, history.data(), int(history.size()));
    out << ",\"events\":[";
    for (size_t i = 0; i < events.size(); ++i) {
        if (i) out << ',';
        auto e = events[i];
        out << '[' << e.player << ',' << e.action << ',' << e.phase << ',' << e.outcome << ']';
    }
    out << "]}";
    output = out.str(); return output.c_str();
}
}
