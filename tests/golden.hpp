#pragma once
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
uint64_t fnv(uint64_t hash, const std::string& line) {
    for (unsigned char ch : line) hash = (hash ^ ch) * 0x100000001b3ULL;
    return (hash ^ '\n') * 0x100000001b3ULL;
}
const uint64_t FNV_START = 0xcbf29ce484222325ULL;

// "G" line: 19 tile resources (-1 desert), 19 numbers (0 desert), 9 port resources (-1 generic), alternate flag.
Board parse_board(std::istringstream& in) {
    int8_t resources[TILES], ports[9];
    uint8_t numbers[TILES];
    int value, alternate;
    for (auto& r : resources) in >> value, r = int8_t(value);
    for (auto& n : numbers) in >> value, n = uint8_t(value);
    for (auto& p : ports) in >> value, p = int8_t(value);
    in >> alternate;
    Board board = Board::from_layout(resources, numbers, ports, alternate);
    // External boards may place ports on a different coastal offset. Optional
    // node pairs preserve the old trace format while representing those boards exactly.
    int port_node;
    if (in >> port_node) {
        int port_nodes[18] = {port_node};
        for (int i = 1; i < 18; i++) in >> port_nodes[i];
        std::fill(board.node_port, board.node_port + NODES, int8_t(-2));
        for (int i = 0; i < 9; i++) {
            for (int k = 0; k < 2; k++) {
                int node = port_nodes[i * 2 + k];
                if (node >= 0 && node < NODES) board.node_port[node] = ports[i];
            }
        }
    }
    return board;
}
std::string board_line(const Board& b) {
    std::string out = "G";
    for (int r : b.resources) out += ' ' + std::to_string(r);
    for (int n : b.numbers) out += ' ' + std::to_string(n);
    for (int p : b.port_resources) out += ' ' + std::to_string(p);
    return out + ' ' + std::to_string(b.alternate_ports);
}

bool playable(const Board& b, const State& s, const Config& c, int event) {
    if (done(s, c)) return false;
    if (is_chance(s)) {
        uint32_t weights[11];
        return event >= 0 && event < chance_weights(s, c, weights) && weights[event];
    }
    uint8_t legal[ACTIONS];
    int n = legal_actions(b, s, c, legal);
    for (int i = 0; i < n; i++)
        if (legal[i] == event) return true;
    return false;
}
void play(const Board& b, State& s, const Config& c, int event) {
    is_chance(s) ? apply_chance(b, s, c, event) : apply_action(b, s, c, event);
}

// Replays every "G"/"E" game on the stream, calling visit(dump) for the start position and after each event.
// Returns false at the first event the engine rejects.
template <class Visit> bool replay_game(const Board& b, const std::vector<int>& events, Visit visit) {
    Config c;
    State s = initial(b);
    visit(dump(b, s, c));
    for (int event : events) {
        if (!playable(b, s, c, event)) return false;
        play(b, s, c, event);
        visit(dump(b, s, c));
    }
    return true;
}

int replay(std::istream& in) {
    std::string line;
    Board b{};
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        std::string tag;
        fields >> tag;
        if (tag == "G") {
            b = parse_board(fields);
            puts("G");
        } else if (tag == "E") {
            std::vector<int> events;
            for (int event; fields >> event;) events.push_back(event);
            if (!replay_game(b, events, [](const std::string& d) { puts(d.c_str()); })) puts("ILLEGAL");
        }
    }
    return 0;
}

// Golden file: "G" board, "E" events, "H" hash of every dump line as produced by the reference simulator.
int golden(const char* path) {
    std::ifstream in(path);
    if (!in) return fprintf(stderr, "cannot open %s\n", path), 2;
    std::string line;
    Board b{};
    uint64_t hash = 0;
    long games = 0, positions = 0, failures = 0;
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        std::string tag;
        fields >> tag;
        if (tag == "G") {
            b = parse_board(fields);
        } else if (tag == "E") {
            std::vector<int> events;
            for (int event; fields >> event;) events.push_back(event);
            hash = FNV_START;
            if (!replay_game(b, events, [&](const std::string& d) { hash = fnv(hash, d), positions++; })) hash = 0;
        } else if (tag == "H") {
            uint64_t expected = strtoull(line.c_str() + 2, nullptr, 16);
            games++;
            if (hash != expected) failures++, fprintf(stderr, "game %ld: hash mismatch\n", games);
        }
    }
    printf("golden: %ld games, %ld positions, %ld failures\n", games, positions, failures);
    return failures || !games;
}
