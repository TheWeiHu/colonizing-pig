// Agents: what a player may know, how a position is described to the evaluator, and lookahead.
#pragma once
#include "catan.hpp"

#include <string>
#include <vector>

namespace catan {

// One player's knowledge of a position. Resource hands are public in a two-player game; the rival's
// development cards are counted but not identified, and `s.deck` holds every card this player cannot
// see (the true deck plus the rival's hand), which is exactly the odds of their own next draw.
// Agents receive a View and never the true State, so they cannot read hidden cards or future dice.
struct View {
    State s;
    uint8_t me, rival_cards, rival_bought, deck_size;
};
View observe(const State& s, int player);

constexpr int FEATURES = 175;
constexpr int BUILD_FEATURES = FEATURES + 48;
constexpr int BOARD_FEATURES = BUILD_FEATURES + 106;
// Describes a view to the evaluator from the viewer's side. Values are stored in hundredths (0..255).
void features(const Board& b, const Config& c, const View& v, uint8_t out[FEATURES]);
// Adds resource profiles of potential builds and the income needed to fund them.
void build_features(const Board& b, const Config& c, const View& v, uint8_t out[BUILD_FEATURES]);
// Adds the wider board: the best open spots, who reaches each settlement spot first, a plan to settle them
// and the race for its first spot, robber exposure, income by dice number, and port access.
void board_features(const Board& b, const Config& c, const View& v, uint8_t out[BOARD_FEATURES]);
constexpr int CARD_FEATURES = BOARD_FEATURES + 22;
// Adds what development cards are worth here, for each player: a Monopoly's haul, what a Year of Plenty
// completes, where a Road Building reaches, and the army race. The rival's cards are expected from the unseen pool.
void card_features(const Board& b, const Config& c, const View& v, uint8_t out[CARD_FEATURES]);

// A small fully connected network: the probability that the viewer wins.
constexpr int MAX_WIDTH = 4096;  // widest hidden layer
struct Net {
    std::vector<int> sizes;                   // layer widths, input first
    std::vector<std::vector<float>> weights;  // per layer, [input][output]
    std::vector<std::vector<float>> biases;
    bool load(const std::string& path);
    float eval(const uint8_t* x) const;
    // The same score from first-layer sums kept between calls, so only the features that changed are added.
    // The sums are fixed point: exact in any order, and within rounding of the plain evaluator.
    struct Sums {
        uint8_t x[CARD_FEATURES];
        int32_t sum[MAX_WIDTH];
        bool primed = false;
    };
    float eval(const uint8_t* x, Sums& kept) const;
    std::vector<int32_t> fixed, fixed_bias;  // first layer in fixed point, [input][output]
    std::vector<int16_t> compact_fixed;  // lossless first layer: saturated weights plus sparse corrections
    struct FixedCorrection { int output; int32_t value; };
    std::vector<FixedCorrection> fixed_corrections;
    std::vector<size_t> correction_offsets;
    float unit = 0;                          // what one fixed-point step is worth

private:
    float finish(float* a) const;
};

struct GraphNet;  // the board-graph evaluator of graph.hpp

struct Agent {
    const Net* net = nullptr;  // null plays uniformly at random
    int depth = 1;             // own decisions searched ahead; chance is averaged exactly once per line
    double epsilon = 0;        // probability of a random move, for exploration in self-play
    double tie = 0;            // moves within this much of the best are chosen among at random
    // Build planning. A position is worth the evaluator's score plus `plan` times the gain of the best
    // build goal (a city, or a settlement up to `reach` roads away), discounted by `patience` for each
    // turn the player's income needs to afford it. Zero disables planning.
    float plan = 0, patience = 0.85f;
    int reach = 2;
    float threat = 0;  // weight on the rival's best build goal, scored as harm to the viewer
    int setup = 0;     // opening placements searched to the end of setup, this many settlements wide; 0 = off
    float economy = 0; // optional production prior in evaluator log odds
    const Net* extra[5] = {};  // more evaluators; a position scores the mean of them all
    bool quick = false;        // evaluate incrementally (see Net::Sums); scores differ from the plain ones by rounding
    bool richer = false;       // append the 48 build features; existing agents keep their original inputs
    bool staged = false;       // primary evaluator in midgame, extra[0] during setup; exactly two rich evaluators
    bool full_setup = false;   // consider every current settlement; recursive setup width stays unchanged
    bool memoize = false;      // reuse exact neural scores within a decision
    // MAIN plans use this beam width for up to depth own actions, valued after ending the turn.
    // Other phases retain two-decision search. Zero preserves every historical agent.
    int turn_beam = 0;
    bool turn_value_only = false;  // use the learned endpoint value without extra projected-build bonuses
    int turn_roll = 0;             // 1: next-roll expectation; 2: also one rival scoring purchase
    int turn_reply = 0;            // recheck this many complete-turn finalists against public rival replies
    bool pre_roll_normalize = false;  // field 16: finish pre-roll card placements before the ordinary roll horizon
    int rollout_samples = 0;       // field 17: three sampled reply finalists, paired one-SE selection; zero preserves history
    bool retention_belief = false; // field 19: fitted mature-card posterior for the same paired rollouts
    bool wide = false;             // field 20: append the board features; needs richer
    float cards = 0;               // field 21: evaluator log odds per development card held beyond the rival's count
    int finalists = 0;             // field 22: plans compared by sampled rollouts, at most eight; zero keeps three
    int rollout_turns = 0;         // field 23: turns the rollouts play on after the rival's
    bool rollout_plan = false;     // field 24: the rollout policy keeps the build-planning priors
    int rollout_beam = 0;          // field 25: the rollout policy plans complete turns this wide; zero keeps one decision
    int rollout_depth = 0;         // field 26: own actions that rollout turn plans look ahead; zero means three
    int opening_blend = 0;         // field 27: staged play also averages the midgame evaluator into the opening search; 2: the other
                                   // extra evaluators join the opening instead of the midgame
    int mandatory_depth = 0;       // decisions searched outside the main phase by a turn-planning agent; zero means two. Not a spec setting.
    // Field 28, with staged play: the midgame evaluators score the positions the opening search reaches once
    // setup is over (1), or join the opening evaluator there (2). Setup positions keep the opening evaluator.
    int opening_leaf = 0;
    const Net* leaf[3] = {};       // set by staged play: the midgame evaluators
    // Field 29: a robber moved during the viewer's own turn rechecks this many of the best spots by planning
    // the rest of the turn after each possible theft, instead of looking one action ahead. Zero is off.
    int robber_turn = 0;
    // Field 30: choose a discard by valuing every complete set of cards to give up, not one card at a time.
    bool discard_sets = false;
    bool opening_symmetry = false; // field 31: average complementary views in a public setup search, before the existing priors
    bool rollout_normalize = false; // field 32: normalize pre-roll choices inside the planned rollout policy
    bool rollout_control = false; // field 33: subtract centered chance-value changes from sampled rollout returns
    float awards = 0;             // field 35: log odds for Longest Road and Largest Army and for leading their races; a style for sparring
    bool card_inputs = false;     // field 34: append the development-card features; needs wide and a 351-input evaluator
    // Field 36, with staged play: the first evaluators listed after the primary are midgame mates, averaged with it
    // wherever it scores (midgame, opening leaf, opening blend). The opening evaluators follow. Zero keeps every
    // extra evaluator an opening one.
    int midgame_mates = 0;
    // Field 37: sampled rollouts run in rounds of rollout_samples, up to this many, while the paired comparison is
    // still close; finalists that fall clearly behind stop being sampled. Zero or one keeps a single round.
    int rollout_rounds = 0;
    // Field 38: threads that share one decision's sampled rollouts, each with private caches; the game's
    // wall-clock thinking time falls while its decisions stay the same up to evaluation rounding. 0 or 1: one thread.
    int workers = 0;
    // Field 39: the positions where the sampled rollouts stop are scored by `graph`, the board-graph evaluator
    // listed last among the weights; the rollouts themselves and every other score keep the primary evaluator.
    // 2: by the mean of `graph` and the primary evaluator.
    int graph_leaf = 0;
    const GraphNet* graph = nullptr;
};
// Picks a legal action for the viewer, who must be the player to move. `value` receives the searched
// win probability of the best move, or -1 when the move was forced or random. `options`, when given, receives
// every legal move of a searched main-phase turn with its planned end-of-turn value, for training.
struct Option { uint8_t action; float value; };
int choose(const Agent& agent, const Board& b, const Config& c, const View& v, Rng& rng, float* value = nullptr,
           std::vector<Option>* options = nullptr);
// What a search did besides choosing, for training. `scored`: every legal move with the value the search ended
// on, below zero where it gave none (a turn-planning agent that samples rollouts ends with a value for the chosen
// move alone). `finalists`: the planned turns those rollouts compared, first the one they were measured against.
// A plan is kept as the end-of-turn positions it was valued at, each chance outcome with its odds, in the agent's
// features; `prior` is what the agent's priors add to the evaluator there, in log odds, and `gain` the mean
// rollout return over the first finalist's.
struct Leaf { float weight, fixed, prior; uint8_t x[CARD_FEATURES]; };  // fixed: 1 or 0 when the game ended there, else -1
struct Finalist { uint8_t action; bool chosen; float gain; std::vector<Leaf> leaves; };
struct Trace { std::vector<Option> scored; std::vector<Finalist> finalists; };
int choose_traced(const Agent& agent, const Board& b, const Config& c, const View& v, Rng& rng, float* value,
                  std::vector<Option>* options, Trace& trace);

}  // namespace catan
