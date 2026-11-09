#include "../engine/web.cpp"
#include "golden.hpp"
#include <cassert>
#include <chrono>
#include <iostream>

int main(int argc, char** argv) {
    assert(pig_init(4));
    if (argc > 1 && std::string(argv[1]) == "fixtures") {
        // Real champion self-play, later replayed in WASM to check the same decisions.
        pig_new(2026110921, 0);
        std::cout << "[";
        int checked = 0;
        while (!done(state, config)) {
            Rng choice(uint64_t(game_seed) * 1000003 + uint64_t(state.steps) * 1009 + 3);
            auto start = std::chrono::steady_clock::now();
            int action = choose(champion.agent, board, config, observe(state, state.cur), choice);
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            if (history.size() < 8 || history.size() % 25 == 0) {
                if (checked++) std::cout << ',';
                std::cout << "{\"seed\":" << game_seed << ",\"human\":" << (1-state.cur)
                    << ",\"action\":" << action << ",\"phase\":" << int(state.phase)
                    << ",\"nativeMs\":" << ms << ",\"history\":";
                array(std::cout, history.data(), int(history.size()));
                std::cout << "}" << std::flush;
            }
            assert(move(action));
        }
        std::cout << "]\n";
        return 0;
    }
    assert(golden("tests/golden.txt") == 0);
    Rng random(2026110919);
    bool seen[GAME_OVER + 1] = {};
    for (int game = 0; game < 24; ++game) {
        pig_new(random.below(1000000000), game % 2);
        assert(!pig_replay(-1) && !pig_replay(249));
        while (!done(state, config)) {
            seen[state.phase] = true;
            uint8_t actions[ACTIONS];
            int count = legal_actions(board, state, config, actions);
            assert(count > 0);
            int action = actions[random.below(count)];
            if (state.cur != human) assert(pig_act(action) == 0);
            assert(pig_replay(action));
            View v = observe(state, human);
            for (int i = 0; i < 5; ++i) assert(v.s.p[1-human].dev[i] == 0);
        }
        State end = state;
        auto moves = history;
        pig_new(game_seed, human);
        for (int action : moves) assert(pig_replay(action));
        assert(dump(board, state, config) == dump(board, end, config));
    }
    assert(seen[PLACE_SETTLEMENT] && seen[PLACE_ROAD] && seen[MAIN] && seen[DISCARD] && seen[MOVE_ROBBER]);
    puts("Bridge: 24 complete games, invalid actions rejected, private cards hidden, exact replay round trips");
}
