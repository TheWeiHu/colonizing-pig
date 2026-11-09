#include "agent.hpp"
#include "graph.hpp"

#include <algorithm>
#include <atomic>
#include <thread>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <utility>

// Optional external diagnostics; disabled observers do not evaluate their arguments.
#ifndef CATAN_ROLLOUT_OBSERVER
#define CATAN_ROLLOUT_OBSERVER(search, stage, state, seed, detail, x, y, z) ((void)0)
#endif

// Keep the baseline target portable; AVX2 is confined to dispatched helpers.
#if defined(__x86_64__) || defined(__i386__)
#if defined(__clang__)
#if __has_attribute(target) && __has_builtin(__builtin_cpu_supports)
#define CATAN_AVX2_FIXED_UPDATE 1
#endif
#elif defined(__GNUC__) && __GNUC__ >= 5
#define CATAN_AVX2_FIXED_UPDATE 1
#endif
#endif
#ifndef CATAN_AVX2_FIXED_UPDATE
#define CATAN_AVX2_FIXED_UPDATE 0
#endif
#if CATAN_AVX2_FIXED_UPDATE
#include <immintrin.h>
#elif defined(__aarch64__)
#include <arm_neon.h>
#endif
// Match the baseline's single-precision SSE rounding. Other architectures,
// x87/excess-precision builds, and builds permitting different arithmetic keep
// the original dense loop. In particular, a baseline FMA build may round once.
#if CATAN_AVX2_FIXED_UPDATE && defined(__x86_64__) && defined(__SSE2_MATH__) && \
    defined(__FLT_EVAL_METHOD__) && __FLT_EVAL_METHOD__ == 0 && \
    !defined(__FAST_MATH__) && !defined(__FMA__)
#define CATAN_AVX2_DENSE_UPDATE 1
#else
#define CATAN_AVX2_DENSE_UPDATE 0
#endif

namespace catan {
namespace { struct Search; }
// choose() on a search kept from an earlier decision, whose caches then carry over.
int choose_in(Search* reuse, const Agent& agent, const Board& b, const Config& c, const View& v, Rng& rng, float* value, std::vector<Option>* options,
              Trace* trace = nullptr);
namespace {

inline int pop(Nodes m) { return __builtin_popcountll(m); }
inline int pop(Edges m) { return __builtin_popcountll(uint64_t(m)) + __builtin_popcountll(uint64_t(m >> 64)); }
inline int sum5(const uint8_t* v) { return v[0] + v[1] + v[2] + v[3] + v[4]; }

const uint8_t COSTS[4][5] = {{1, 1, 0, 0, 0}, {1, 1, 1, 1, 0}, {0, 0, 0, 2, 3}, {0, 0, 1, 1, 1}};  // road, settlement, city, card

// Retained rival-card counts follow a conditioned Fisher noncentral hypergeometric.
// Coefficient DP is prepared once at the original root, then reused by all samples.
// Four fitted log-weights are relative to KNIGHT; theta=0 is the old exchangeable law.
// See training/retention/ for the fixed offline fit and public-View-only context.
class RetentionBelief {
public:
    static constexpr int TYPES = 5, VP = 1, MAX_CARDS = 25;
    using Counts = std::array<int, TYPES>;
    using LogWeights = std::array<double, TYPES>;

    bool prepare(const Counts& pool, int held, int root_cap,
                 const LogWeights& theta, int bought = 0) {
        ready_ = false;
        constexpr Counts initial = {14, 5, 2, 2, 2};
        int total = 0;
        for (int t = 0; t < TYPES; ++t) {
            if (pool[t] < 0 || pool[t] > initial[t] || !std::isfinite(theta[t])) return false;
            total += pool[t];
        }
        if (held < 0 || held > total || total > MAX_CARDS || root_cap < 0 || bought != 0) return false;
        held_ = held;
        shift_ = *std::max_element(theta.begin(), theta.end());
        for (auto& row : log_factor_) row.fill(negative_infinity());
        for (auto& row : log_suffix_) row.fill(negative_infinity());

        // Common shifts cancel because every feasible hand has the same size.
        // Log-space coefficients also avoid losing all feasible mass when a
        // constrained hand must contain an extremely low-weight type.
        for (int t = 0; t < TYPES; ++t) {
            const double centered = theta[t] - shift_;
            if (!std::isfinite(centered)) return false;
            int limit = std::min(pool[t], held);
            if (t == VP) limit = std::min(limit, root_cap);
            limits_[t] = limit;
            double log_choose = 0;
            for (int j = 0; j <= limit; ++j) {
                if (j) log_choose += std::log(double(pool[t] - j + 1)) - std::log(double(j));
                const double term = log_choose + centered * j;
                if (!std::isfinite(term)) return false;
                log_factor_[t][j] = term;
            }
        }

        log_suffix_[TYPES][0] = 0;
        for (int t = TYPES - 1; t >= 0; --t) {
            for (int left = 0; left <= held; ++left) {
                double largest = negative_infinity();
                const int limit = std::min(limits_[t], left);
                for (int j = 0; j <= limit; ++j)
                    largest = std::max(largest, log_factor_[t][j] + log_suffix_[t + 1][left - j]);
                if (!std::isfinite(largest)) continue;
                double mass = 0;
                for (int j = 0; j <= limit; ++j)
                    mass += std::exp(log_factor_[t][j] + log_suffix_[t + 1][left - j] - largest);
                log_suffix_[t][left] = largest + std::log(mass);
            }
        }
        ready_ = std::isfinite(log_suffix_[0][held]);
        return ready_;
    }

    bool ready() const { return ready_; }

    // Normalizer under the original, uncentered weights; useful for likelihood.
    double log_partition() const {
        return ready_ ? log_suffix_[0][held_] + shift_ * held_ : negative_infinity();
    }

    double log_probability(const Counts& counts) const {
        if (!ready_) return negative_infinity();
        int total = 0;
        double log_mass = 0;
        for (int t = 0; t < TYPES; ++t) {
            if (counts[t] < 0 || counts[t] > limits_[t]) return negative_infinity();
            total += counts[t];
            log_mass += log_factor_[t][counts[t]];
        }
        return total == held_ ? log_mass - log_suffix_[0][held_] : negative_infinity();
    }

    // uniform() must supply independent doubles in [0,1). The caller owns a
    // private belief stream; this class never reads any engine/random state.
    // Five uniforms are consumed per successful draw, even for forced types.
    // No DP or enumeration is repeated here: only five conditional CDF scans.
    template<class Uniform>
    bool sample(Counts& counts, Uniform&& uniform) const {
        if (!ready_) return false;
        counts.fill(0);
        int left = held_;
        for (int t = 0; t < TYPES; ++t) {
            const double u = uniform();
            if (!(u >= 0 && u < 1)) return false;
            const int limit = std::min(limits_[t], left);
            const double denominator = log_suffix_[t][left];
            double cumulative = 0;
            int selected = -1, last_feasible = -1;
            for (int j = 0; j <= limit; ++j) {
                const double suffix = log_suffix_[t + 1][left - j];
                if (!std::isfinite(suffix)) continue;
                last_feasible = j;
                cumulative += std::exp(log_factor_[t][j] + suffix - denominator);
                if (u < cumulative) { selected = j; break; }
            }
            // Protect the last interval against a few ulps of CDF roundoff.
            if (selected < 0) selected = last_feasible;
            if (selected < 0) return false;
            counts[t] = selected;
            left -= selected;
        }
        return left == 0;
    }

private:
    static constexpr double negative_infinity() { return -std::numeric_limits<double>::infinity(); }
    bool ready_ = false;
    Counts limits_{};
    int held_ = 0;
    double shift_ = 0;
    std::array<std::array<double, MAX_CARDS + 1>, TYPES> log_factor_{};
    std::array<std::array<double, MAX_CARDS + 1>, TYPES + 1> log_suffix_{};
};



#if CATAN_AVX2_FIXED_UPDATE
// Net::load bounds each first-layer sum, including mixed old/new inputs during an
// update. Output lanes are independent; input order and all floating arithmetic
// stay unchanged. Unaligned loads also cover arbitrary vector/stack alignment.
__attribute__((target("avx2"), noinline))
void update_fixed_avx2(const int32_t* fixed, int inputs, int width, const uint8_t* x, Net::Sums& kept) {
    for (int i = 0; i < inputs; i++) {
        if (x[i] == kept.x[i]) continue;
        const int32_t change = x[i] - kept.x[i], *w = fixed + size_t(i) * width;
        const __m256i delta = _mm256_set1_epi32(change);
        int o = 0;
        for (; o + 8 <= width; o += 8) {
            const __m256i weight = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w + o));
            const __m256i sum = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(kept.sum + o));
            const __m256i next = _mm256_add_epi32(sum, _mm256_mullo_epi32(delta, weight));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(kept.sum + o), next);
        }
        for (; o < width; o++) kept.sum[o] += change * w[o];
        kept.x[i] = x[i];
    }
    _mm256_zeroupper();  // return to the unchanged baseline-target floating code
}
#endif

#if CATAN_AVX2_FIXED_UPDATE || defined(__aarch64__)
// Saturated int16 weights plus sparse residuals reproduce the int32 weights exactly.
// Saturation preserves sign and magnitude bounds, including partially applied updates.
#if CATAN_AVX2_FIXED_UPDATE
__attribute__((target("avx2"), noinline))
#endif
void update_compact(const Net& net, const uint8_t* x, Net::Sums& kept) {
    const int width = net.sizes[1];
#if CATAN_AVX2_FIXED_UPDATE
    // Adjacent inputs share a widening dot product; an odd final input is padded with zero.
    for (int i = 0; i < net.sizes[0]; i += 2) {
        const int change0 = x[i] - kept.x[i];
        const int change1 = i + 1 < net.sizes[0] ? x[i + 1] - kept.x[i + 1] : 0;
        if (!change0 && !change1) continue;
        const int16_t* w = net.compact_fixed.data() + size_t(i / 2) * width * 2;
        const uint32_t packed = uint16_t(change0) | (uint32_t(uint16_t(change1)) << 16);
        const __m256i delta = _mm256_set1_epi32(int32_t(packed));
        int o = 0;
        for (; o + 8 <= width; o += 8) {
            const __m256i weight = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w + 2 * o));
            const __m256i sum = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(kept.sum + o));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(kept.sum + o),
                _mm256_add_epi32(sum, _mm256_madd_epi16(delta, weight)));
        }
        for (; o < width; o++) kept.sum[o] += change0 * w[2 * o] + change1 * w[2 * o + 1];
        for (int j = i; j < std::min(i + 2, net.sizes[0]); j++) {
            const int change = x[j] - kept.x[j];
            for (size_t k = net.correction_offsets[j]; k < net.correction_offsets[j + 1]; k++) {
                const auto& correction = net.fixed_corrections[k];
                kept.sum[correction.output] += change * correction.value;
            }
            kept.x[j] = x[j];
        }
    }
    _mm256_zeroupper();
#else
    for (int i = 0; i < net.sizes[0]; i++) {
        if (x[i] == kept.x[i]) continue;
        const int32_t change = x[i] - kept.x[i];
        const int16_t* w = net.compact_fixed.data() + size_t(i) * width;
        int o = 0;
        for (; o + 8 <= width; o += 8) {
            const int16x8_t weight = vld1q_s16(w + o);
            vst1q_s32(kept.sum + o, vmlal_n_s16(vld1q_s32(kept.sum + o), vget_low_s16(weight), int16_t(change)));
            vst1q_s32(kept.sum + o + 4, vmlal_high_n_s16(vld1q_s32(kept.sum + o + 4), weight, int16_t(change)));
        }
        for (; o < width; o++) kept.sum[o] += change * w[o];
        for (size_t k = net.correction_offsets[i]; k < net.correction_offsets[i + 1]; k++) {
            const auto& correction = net.fixed_corrections[k];
            kept.sum[correction.output] += change * correction.value;
        }
        kept.x[i] = x[i];
    }
#endif
}
#endif

#if CATAN_AVX2_DENSE_UPDATE
// Vectorize independent outputs, never the input reduction. Each output keeps
// the original sequence of separately rounded multiply and add operations.
__attribute__((target("avx2,no-fma"), noinline))
void update_dense_avx2(const float* a, const float* weights, int width, int outs, float* z) {
    if (outs == 64) {
        __m256 sums[8];
        for (int k = 0; k < 8; k++) sums[k] = _mm256_loadu_ps(z + 8 * k);
        for (int i = 0; i < width; i++) {
            if (a[i] <= 0) continue;
            const __m256 input = _mm256_set1_ps(a[i]);
            const float* w = weights + size_t(i) * 64;
            for (int k = 0; k < 8; k++)
                sums[k] = _mm256_add_ps(sums[k], _mm256_mul_ps(input, _mm256_loadu_ps(w + 8 * k)));
        }
        for (int k = 0; k < 8; k++) _mm256_storeu_ps(z + 8 * k, sums[k]);
        _mm256_zeroupper();
        return;
    }
    for (int i = 0; i < width; i++) {
        if (a[i] <= 0) continue;  // preserve the original ReLU comparison
        const float scale = a[i], *w = weights + size_t(i) * outs;
        const __m256 input = _mm256_set1_ps(scale);
        int o = 0;
        for (; o + 8 <= outs; o += 8) {
            const __m256 weight = _mm256_loadu_ps(w + o);
            const __m256 sum = _mm256_loadu_ps(z + o);
            const __m256 product = _mm256_mul_ps(input, weight);
            _mm256_storeu_ps(z + o, _mm256_add_ps(sum, product));
        }
        for (; o < outs; o++) z[o] += scale * w[o];
    }
    _mm256_zeroupper();
}
#endif

// Turns of income until `cost` is affordable, trading surplus at the bank; 0 = now, 99 = out of reach.
// Income arrives on both players' rolls, so one turn brings two rolls' worth.
int turns_to_fund(const uint8_t* hand, const double* income, const uint8_t* ratio, const uint8_t* cost) {
    for (int turn = 0; turn <= 10; turn++) {
        double missing = 0, trades = 0;
        for (int r = 0; r < 5; r++) {
            double have = hand[r] + 2 * turn * income[r];
            if (have < cost[r]) missing += cost[r] - have;
            else trades += turn ? (have - cost[r]) / ratio[r] : double((hand[r] - cost[r]) / ratio[r]);
        }
        if (trades >= missing) return turn;
    }
    return 99;
}

struct Search {
    const Board& b;
    const Config& c;
    const Net& net;
    View base;
    const Agent& agent;
    Net::Sums kept[6] = {};
    struct Cache {
        uint64_t hashes[1024] = {};
        uint8_t keys[1024][CARD_FEATURES];
        float values[1024];
    };
    // Only hashes need initialization: a nonzero matching hash and the complete
    // feature key are required before reading a cached value.
    std::unique_ptr<Cache> cache = agent.memoize ? std::unique_ptr<Cache>(new Cache) : nullptr;

    template <int Inputs> struct ScoreCacheEntry {
        std::array<uint8_t, Inputs> x;
        uint64_t hash = 0;
        float score = 0;
        bool filled = false;
    };
    // Keep the frozen 175-feature turn cache's layout, capacity, and hash unchanged.
    // Rich turn searches have separate entries whose keys include all 223 features.
    std::vector<ScoreCacheEntry<FEATURES>> score_cache = {};
    std::vector<ScoreCacheEntry<BUILD_FEATURES>> rich_score_cache = {};
    std::vector<ScoreCacheEntry<BOARD_FEATURES>> wide_score_cache = {};
    std::vector<ScoreCacheEntry<CARD_FEATURES>> card_score_cache = {};

    // The full semantic State occupies 177 serialized bytes; deck_size adds four.
    // This cache is local to a Search, whose board, config, base View metadata, and
    // selected evaluator are fixed. It never receives an unsanitized game state.
    static constexpr size_t STATE_KEY_BYTES = 181, STATE_CACHE_SLOTS = 2048;
    using StateScoreKey = std::array<uint8_t, STATE_KEY_BYTES>;
    struct StateScoreCache {
        uint64_t hashes[STATE_CACHE_SLOTS];
        StateScoreKey keys[STATE_CACHE_SLOTS];
        float values[STATE_CACHE_SLOTS];
        bool filled[STATE_CACHE_SLOTS] = {};
    };
    // Board, config and policy are fixed for this search; all mutable View fields enter the key.
    using ViewKey = std::array<uint8_t, STATE_KEY_BYTES + 3>;
    struct ChoiceCache {
        static constexpr size_t SLOTS = 2048;
        uint64_t hashes[SLOTS] = {};
        ViewKey keys[SLOTS];
        uint8_t actions[SLOTS][ACTIONS];
        int counts[SLOTS];
    };
    std::unique_ptr<ChoiceCache> choice_cache = {};
    static ViewKey view_key(const View& view) {
        StateScoreKey state;
        write_state_key(view.s, view.deck_size, state);
        ViewKey key;
        std::copy(state.begin(), state.end(), key.begin());
        key[STATE_KEY_BYTES] = view.me;
        key[STATE_KEY_BYTES + 1] = view.rival_cards;
        key[STATE_KEY_BYTES + 2] = view.rival_bought;
        return key;
    }
    std::unique_ptr<StateScoreCache> state_score_cache = {};
    size_t state_cache_hits = 0, state_cache_misses = 0, state_cache_collisions = 0;
    size_t rollout_paths = 0, rollout_policy_calls = 0, rollout_events = 0;
    size_t rollout_forced_ends = 0, rollout_truncated = 0;
    size_t rollout_endpoint_candidates = 0, rollout_endpoint_duplicates = 0, rollout_distinct_finalists = 0;
    size_t rollout_anchor_switches = 0;

    static void write_state_key(const State& s, int deck_size, StateScoreKey& key) {
        size_t at = 0;
        auto integer = [&](uint64_t value, int bytes) {
            for (int i = 0; i < bytes; i++, value >>= 8) key[at++] = uint8_t(value);
        };
        auto bytes = [&](const auto& values) {
            for (uint8_t value : values) key[at++] = value;
        };
        // Bind every member: adding a member to these aggregates requires updating
        // this serializer. Values are emitted explicitly, never through object bytes.
        const auto& [players, bank, deck, cur, turn_player, phase, turn, steps,
                     setup_count, robber, free_roads, last_roll, road_len,
                     last_setup_node, longest_road, largest_army, winner,
                     pre_roll, discard_count, discards, dice,
                     card_tested, card_held, card_since, card_turns] = s;
        (void)card_tested, (void)card_held, (void)card_since, (void)card_turns;  // no keyed score reads the card history
        for (const Player& player : players) {
            const auto& [hand, dev, bought, played, ratio, played_dev, settlements, cities, roads] = player;
            bytes(hand), bytes(dev), bytes(bought), bytes(played), bytes(ratio);
            integer(played_dev ? 1 : 0, 1);
            integer(settlements, 8), integer(cities, 8);
            integer(uint64_t(roads), 8), integer(uint64_t(roads >> 64), 8);
        }
        bytes(bank), bytes(deck);
        integer(cur, 1), integer(turn_player, 1), integer(uint8_t(phase), 1);
        integer(turn, 4), integer(steps, 4);
        integer(setup_count, 1), integer(robber, 1), integer(free_roads, 1), integer(last_roll, 1);
        bytes(road_len);
        integer(uint8_t(last_setup_node), 1), integer(uint8_t(longest_road), 1);
        integer(uint8_t(largest_army), 1), integer(uint8_t(winner), 1);
        integer(pre_roll ? 1 : 0, 1), integer(discard_count, 1);
        for (const auto& discard : discards) {
            const auto& [player, remaining] = discard;
            integer(player, 1), integer(remaining, 1);
        }
        const auto& [dice_deck, recent, recent_len, total_sevens, streak_player, streak_count] = dice;
        bytes(dice_deck), bytes(recent), integer(recent_len, 1);
        for (uint16_t count : total_sevens) integer(count, 2);
        integer(uint8_t(streak_player), 1), integer(streak_count, 2);
        integer(uint32_t(deck_size), 4);
        assert(at == key.size());
    }

    static uint64_t feature_hash(const uint8_t* x, int count = FEATURES) {
        uint64_t hash = 0xcbf29ce484222325ULL;
        int i = 0;
        for (; i + 8 <= count; i += 8) {
            uint64_t word;
            std::memcpy(&word, x + i, 8);
            hash = (hash ^ word) * 0x100000001b3ULL;
        }
        uint64_t tail = 0;
        std::memcpy(&tail, x + i, count - i);
        hash = (hash ^ tail) * 0x100000001b3ULL;
        hash ^= hash >> 33;
        hash *= 0xff51afd7ed558ccdULL;
        hash ^= hash >> 33;
        hash *= 0xc4ceb9fe1a85ec53ULL;
        return hash ^ (hash >> 33);
    }

    template <int Inputs>
    float evaluate_turn(const uint8_t* x, std::vector<ScoreCacheEntry<Inputs>>& scores) {
        if (scores.empty()) scores.resize(8192);
        uint64_t hash = feature_hash(x, Inputs);
        auto& cached = scores[hash & (scores.size() - 1)];
        if (cached.filled && cached.hash == hash && !std::memcmp(cached.x.data(), x, Inputs)) return cached.score;
        float score = agent.quick ? net.eval(x, kept[0]) : net.eval(x);
        int nets = 1;
        for (const Net* other : agent.extra)
            if (other) score += agent.quick ? other->eval(x, kept[nets]) : other->eval(x), nets++;
        score /= nets;
        std::copy(x, x + Inputs, cached.x.begin());
        cached.hash = hash;
        cached.score = score;  // only the raw ensemble; economy remains state-dependent
        cached.filled = true;
        return score;
    }

    // Rollout players use the same raw evaluators. Priors and search scores stay local to each player.
    Search* evaluation_owner = nullptr;
    float evaluate(const uint8_t* x) {
        if (evaluation_owner) return evaluation_owner->evaluate(x);
        if (agent.turn_beam > 0) {
            if (agent.card_inputs) return evaluate_turn<CARD_FEATURES>(x, card_score_cache);
            if (agent.wide) return evaluate_turn<BOARD_FEATURES>(x, wide_score_cache);
            if (agent.richer) return evaluate_turn<BUILD_FEATURES>(x, rich_score_cache);
            return evaluate_turn<FEATURES>(x, score_cache);
        }
        int count = agent.card_inputs ? CARD_FEATURES : agent.wide ? BOARD_FEATURES : agent.richer ? BUILD_FEATURES : FEATURES;
        uint64_t hash = 1469598103934665603ULL;
        if (cache) for (int i = 0; i < count; i++) hash = (hash ^ x[i]) * 1099511628211ULL;
        int slot = int(hash & 1023);
        if (cache && hash && cache->hashes[slot] == hash && !std::memcmp(cache->keys[slot], x, count)) return cache->values[slot];
        float score = agent.quick ? net.eval(x, kept[0]) : net.eval(x);
        int nets = 1;
        for (const Net* other : agent.extra)
            if (other) score += agent.quick ? other->eval(x, kept[nets]) : other->eval(x), nets++;
        score /= nets;
        if (cache) {
            cache->hashes[slot] = hash;
            std::memcpy(cache->keys[slot], x, count);
            cache->values[slot] = score;
        }
        return score;
    }

    struct ViewValueCache {
        static constexpr size_t SLOTS = 4096;
        uint64_t hashes[SLOTS] = {};
        ViewKey keys[SLOTS];
        float values[SLOTS];
    };
    std::unique_ptr<ViewValueCache> view_value_cache = {};
    float evaluate_view(const View& view) {
        if (evaluation_owner) return evaluation_owner->evaluate_view(view);
        View normalized = view;
        // None of the feature sets reads these fields; decision keys retain them for rule cutoffs.
        normalized.s.steps = 0;
        normalized.s.last_roll = 0;
        normalized.s.last_setup_node = 0;
        const ViewKey key = view_key(normalized);
        const uint64_t hash = feature_hash(key.data(), int(key.size()));
        const size_t slot = hash & (ViewValueCache::SLOTS - 1);
        if (!view_value_cache) view_value_cache.reset(new ViewValueCache);
        auto& cached = *view_value_cache;
        if (hash && cached.hashes[slot] == hash && cached.keys[slot] == key) return cached.values[slot];
        uint8_t x[CARD_FEATURES];
        if (agent.card_inputs) card_features(b, c, view, x);
        else if (agent.wide) board_features(b, c, view, x);
        else if (agent.richer) build_features(b, c, view, x);
        else features(b, c, view, x);
        const float value = evaluate(x);
        cached.hashes[slot] = hash;
        cached.keys[slot] = key;
        cached.values[slot] = value;
        return value;
    }

    // The evaluator's score, plus the discounted gain of the viewer's best build goal, minus the
    // discounted harm of the rival's best one.
    float leaf(const State& s, int deck_size) {
        float score = score_of(s, deck_size);
        if (s.winner >= 0) return score;
        if (agent.turn_beam > 0 && agent.turn_value_only) return std::clamp(score, 0.000001f, 0.999999f);
        if (agent.plan > 0) score += agent.plan * best_goal(s, deck_size, base.me, score);
        if (agent.threat > 0) score -= agent.threat * best_goal(s, deck_size, 1 - base.me, score);
        // A guaranteed win must outrank every nonterminal planning estimate.
        if (agent.turn_beam > 0) score = std::clamp(score, 0.000001f, 0.999999f);
        return score;
    }

    // The largest discounted change in the viewer's score from one build by `who`: a city on one of their
    // two richest settlements, or a settlement on one of the three richest spots within reach.
    float best_goal(const State& s, int deck_size, int who, float score) {
        const Topology& t = topology();
        const Player &mine = s.p[who], &rival = s.p[1 - who];
        const float sign = who == base.me ? 1 : -1;
        double income[5] = {};
        for (int tile = 0; tile < TILES; tile++) {
            int weight = pop(mine.settlements & t.tile_mask[tile]) + 2 * pop(mine.cities & t.tile_mask[tile]);
            if (weight && b.resources[tile] >= 0 && tile != s.robber) income[b.resources[tile]] += weight * DICE_DECK[b.numbers[tile] - 2] / 36.0;
        }
        int wait = s.phase == MAIN && s.cur == who ? 0 : 1;  // nothing can be built before their next main phase
        float best = 0;
        auto consider = [&](const State& built, const uint8_t* cost) {
            int turns = std::max(wait, turns_to_fund(mine.hand, income, mine.ratio, cost));
            if (turns > 10) return;
            State after = built;
            for (int r = 0; r < 5; r++) after.p[who].hand[r] = uint8_t(std::max(0, mine.hand[r] - cost[r]));
            best = std::max(best, std::pow(agent.patience, float(turns)) * sign * (score_of(after, deck_size) - score));
        };
        if (pop(mine.cities) < 4) {
            int nodes[5], n = 0;
            for (Nodes m = mine.settlements; m; m &= m - 1) nodes[n++] = __builtin_ctzll(m);
            std::sort(nodes, nodes + n, [&](int x, int y) { return b.node_pips[x] > b.node_pips[y]; });
            for (int i = 0; i < std::min(n, 2); i++) {
                State built = s;
                built.p[who].settlements &= ~(Nodes(1) << nodes[i]), built.p[who].cities |= Nodes(1) << nodes[i];
                consider(built, COSTS[2]);
            }
        }
        if (pop(mine.settlements) < 5) {
            Nodes walls = rival.settlements | rival.cities, blocked = blocked_nodes(s);
            Nodes frontier = (mine.settlements | mine.cities | road_endpoints(mine.roads)) & ~walls, seen = frontier;
            Edges taken = mine.roads | rival.roads, path[NODES] = {};
            struct Spot { int node, roads; } spots[NODES];
            int n = 0;
            for (Nodes m = frontier & ~blocked; m; m &= m - 1) spots[n++] = {__builtin_ctzll(m), 0};
            for (int step = 1; step <= agent.reach && pop(mine.roads) + step <= 15; step++) {
                Nodes next = 0;
                for (Nodes m = frontier; m; m &= m - 1) {
                    int node = __builtin_ctzll(m);
                    for (int k = 0; k < t.node_edge_count[node]; k++) {
                        int e = t.node_edges[node][k], other = t.edge_nodes[e][0] == node ? t.edge_nodes[e][1] : t.edge_nodes[e][0];
                        if ((taken >> e & 1) || (seen >> other & 1) || (next >> other & 1)) continue;
                        next |= Nodes(1) << other, path[other] = path[node] | Edges(1) << e;
                    }
                }
                seen |= next, frontier = next & ~walls;
                for (Nodes m = next & ~blocked; m; m &= m - 1) spots[n++] = {__builtin_ctzll(m), step};
            }
            std::sort(spots, spots + n, [&](const Spot& x, const Spot& y) { return b.node_pips[x.node] - 2 * x.roads > b.node_pips[y.node] - 2 * y.roads; });
            for (int i = 0; i < std::min(n, 3); i++) {
                State built = s;
                Player& p = built.p[who];
                p.roads |= path[spots[i].node], p.settlements |= Nodes(1) << spots[i].node;
                int port = b.node_port[spots[i].node];
                if (port == -1) for (auto& ratio : p.ratio) ratio = std::min<uint8_t>(ratio, 3);
                else if (port >= 0) p.ratio[port] = 2;
                uint8_t cost[5] = {uint8_t(1 + spots[i].roads), uint8_t(1 + spots[i].roads), 1, 1, 0};
                consider(built, cost);
            }
        }
        return best;
    }

    // Opening placements: both players' remaining settlements are searched `agent.setup` wide, the rival
    // choosing what is worst for the viewer. Roads and the ranking use the plain evaluator. The value is
    // exact when it lies between `low` and `high`; outside them the search stops early and returns a bound.
    float opening(const State& s, int deck_size, float low = -2, float high = 2) {
        if (s.phase != PLACE_SETTLEMENT && s.phase != PLACE_ROAD) return leaf(s, deck_size);
        const float side = s.cur == base.me ? 1 : -1;
        uint8_t actions[ACTIONS];
        int n = legal(s, deck_size, actions);
        std::pair<float, int> ranked[ACTIONS];
        for (int i = 0; i < n; i++) {
            State next = s;
            apply_action(b, next, c, actions[i]);
            ranked[i] = {-side * score_of(next, deck_size), actions[i]};
        }
        std::sort(ranked, ranked + n);
        float best = -2;
        for (int i = 0; i < std::min(n, s.phase == PLACE_ROAD ? 1 : agent.setup); i++) {
            State next = s;
            apply_action(b, next, c, ranked[i].second);
            best = std::max(best, side * (side > 0 ? opening(next, deck_size, std::max(low, best), high)
                                                   : opening(next, deck_size, low, std::min(high, -best))));
            if (side > 0 ? best >= high : -best <= low) break;  // the player above already has something better
        }
        return side * best;
    }

    float score_of(const State& s, int deck_size) {
        if (s.winner >= 0) return s.winner == base.me;
        const bool cache_state = agent.richer && agent.turn_beam > 0;
        StateScoreKey state_key;
        uint64_t state_hash = 0;
        size_t state_slot = 0;
        if (cache_state) {
            write_state_key(s, deck_size, state_key);
            state_hash = feature_hash(state_key.data(), int(state_key.size()));
            state_slot = state_hash & (STATE_CACHE_SLOTS - 1);
            if (!state_score_cache) state_score_cache.reset(new StateScoreCache);
            if (state_score_cache->filled[state_slot] && state_score_cache->hashes[state_slot] == state_hash &&
                !std::memcmp(state_score_cache->keys[state_slot].data(), state_key.data(), state_key.size())) {
                state_cache_hits++;
                return state_score_cache->values[state_slot];
            }
            state_cache_misses++;
            state_cache_collisions += state_score_cache->filled[state_slot];
        }
        View v = base;
        v.s = s;
        v.deck_size = uint8_t(deck_size);
        uint8_t x[CARD_FEATURES];
        const bool cached_view = agent.turn_beam > 0 && !agent.opening_leaf && !agent.opening_symmetry;
        if (!cached_view) {
            if (agent.card_inputs) card_features(b, c, v, x);
            else if (agent.wide) board_features(b, c, v, x);
            else if (agent.richer) build_features(b, c, v, x);
            else features(b, c, v, x);
        }
        float score;
        if (agent.opening_leaf && agent.leaf[0] && s.phase != PLACE_SETTLEMENT && s.phase != PLACE_ROAD) {
            // Setup is over: the midgame evaluators have seen far more such positions than the opening one.
            score = 0;
            int nets = 0;
            for (const Net* mid : agent.leaf)
                if (mid) score += mid->eval(x), nets++;
            score /= nets;
            if (agent.opening_leaf == 2) score = 0.5f * (score + evaluate(x));
        } else score = cached_view ? evaluate_view(v) : evaluate(x);
        bool public_setup = agent.opening_symmetry && agent.full_setup &&
            (base.s.phase == PLACE_SETTLEMENT || base.s.phase == PLACE_ROAD) &&
            (s.phase == PLACE_SETTLEMENT || s.phase == PLACE_ROAD || s.phase == PRE_ROLL) &&
            base.rival_cards == 0 && base.rival_bought == 0;
        if (public_setup) {
            for (int who = 0; who < 2; who++) {
                public_setup = public_setup && sum5(base.s.p[who].dev) == 0 && sum5(base.s.p[who].bought) == 0 &&
                    sum5(s.p[who].dev) == 0 && sum5(s.p[who].bought) == 0;
            }
        }
        if (public_setup) {
            // Setup has no private cards, including the PRE_ROLL leaf after its final road.
            // Select the same raw evaluator branch for both viewers; apply the priors once below.
            View other = v;
            other.me = uint8_t(1 - v.me);
            uint8_t other_x[CARD_FEATURES];
            if (agent.card_inputs) card_features(b, c, other, other_x);
            else if (agent.wide) board_features(b, c, other, other_x);
            else if (agent.richer) build_features(b, c, other, other_x);
            else features(b, c, other, other_x);
            float other_score;
            if (agent.opening_leaf && agent.leaf[0] && s.phase != PLACE_SETTLEMENT && s.phase != PLACE_ROAD) {
                other_score = 0;
                int nets = 0;
                for (const Net* mid : agent.leaf)
                    if (mid) other_score += mid->eval(other_x), nets++;
                other_score /= nets;
                if (agent.opening_leaf == 2) other_score = 0.5f * (other_score + evaluate(other_x));
            } else other_score = evaluate(other_x);
            score = 0.5f + 0.5f * (score - other_score);
        }
        if (agent.economy != 0) {
            // Favor diverse production, especially city resources, with diminishing returns.
            double production[2] = {};
            const double weight[5] = {1, 1, 1, 1.5, 2};
            const Topology& t = topology();
            for (int who = 0; who < 2; who++) {
                double income[5] = {};
                for (int tile = 0; tile < TILES; tile++) {
                    if (b.resources[tile] < 0 || tile == s.robber) continue;
                    int count = pop(s.p[who].settlements & t.tile_mask[tile]) + 2 * pop(s.p[who].cities & t.tile_mask[tile]);
                    income[b.resources[tile]] += count * DICE_DECK[b.numbers[tile] - 2] / 36.0;
                }
                for (int r = 0; r < 5; r++) production[who] += weight[r] * std::log1p(4 * income[r]);
            }
            float probability = std::clamp(score, 0.00001f, 0.99999f);
            double odds = std::log(probability / (1 - probability));
            score = float(1 / (1 + std::exp(-odds - agent.economy * (production[base.me] - production[1 - base.me]))));
        }
        if (agent.awards != 0) {
            // A sparring style that chases the two awards: each held is worth `awards`, and a lead in either race half that.
            const int me = base.me, rival = 1 - me;
            double held = (s.longest_road == me) - (s.longest_road == rival) + (s.largest_army == me) - (s.largest_army == rival);
            double races = std::clamp(int(s.road_len[me]) - int(s.road_len[rival]), -3, 3) / 3.0 +
                           std::clamp(int(s.p[me].played[KNIGHT]) - int(s.p[rival].played[KNIGHT]), -3, 3) / 3.0;
            float probability = std::clamp(score, 0.00001f, 0.99999f);
            double odds = std::log(probability / (1 - probability)) + agent.awards * (held + 0.5 * races);
            score = float(1 / (1 + std::exp(-odds)));
        }
        if (agent.cards != 0) {
            float probability = std::clamp(score, 0.00001f, 0.99999f);
            double odds = std::log(probability / (1 - probability)) + agent.cards * (sum5(s.p[base.me].dev) - int(base.rival_cards));
            score = float(1 / (1 + std::exp(-odds)));
        }
        if (cache_state) {
            state_score_cache->keys[state_slot] = state_key;
            state_score_cache->hashes[state_slot] = state_hash;
            state_score_cache->values[state_slot] = score;
            state_score_cache->filled[state_slot] = true;
        }
        return score;
    }
    int legal(const State& s, int deck_size, uint8_t* out) {
        int n = legal_actions(b, s, c, out);
        if (!deck_size) n = int(std::remove(out, out + n, uint8_t(BUY_DEV)) - out);  // the view's deck is never empty
        return n;
    }
    // Only deterministic own decisions enter the same beam layer. Dice, deck, turn number, and
    // setup fields are therefore constant throughout it; compare every field those decisions can
    // change. This keeps equivalent trade/build orderings from using several beam slots.
    static bool same_beam_state(const State& a, const State& z) {
        if (a.phase != z.phase || a.cur != z.cur || a.turn_player != z.turn_player || a.pre_roll != z.pre_roll ||
            a.steps != z.steps || a.robber != z.robber || a.free_roads != z.free_roads ||
            a.longest_road != z.longest_road || a.largest_army != z.largest_army ||
            a.road_len[0] != z.road_len[0] || a.road_len[1] != z.road_len[1] ||
            a.discard_count != z.discard_count || std::memcmp(a.bank, z.bank, 5)) return false;
        for (int i = 0; i < 2; i++) {
            const Player &p = a.p[i], &q = z.p[i];
            if (p.settlements != q.settlements || p.cities != q.cities || p.roads != q.roads ||
                p.played_dev != q.played_dev || std::memcmp(p.hand, q.hand, 5) || std::memcmp(p.dev, q.dev, 5) ||
                std::memcmp(p.bought, q.bought, 5) || std::memcmp(p.played, q.played, 5) || std::memcmp(p.ratio, q.ratio, 5) ||
                a.discards[i].player != z.discards[i].player || a.discards[i].remaining != z.discards[i].remaining) return false;
        }
        return true;
    }
    float turn_endpoint(const State& s, int deck_size) {
        if (!agent.turn_roll || done(s, c) || s.phase != PRE_ROLL || s.cur == base.me)
            return leaf(s, deck_size);
        // The next roller may play a hidden card first; this bounded forecast assumes
        // an immediate roll. It averages every public dice outcome without sampling
        // future randomness; mode two adds one public scoring purchase afterward.
        State rolling = s;
        apply_action(b, rolling, c, ROLL_DICE);
        uint32_t weights[11];
        int slots = chance_weights(rolling, c, weights);
        double total = 0, result = 0;
        for (int i = 0; i < slots; i++) if (weights[i]) {
            State next = rolling;
            apply_chance(b, next, c, i);
            float value = leaf(next, deck_size);
            if (agent.turn_roll >= 2 && !done(next, c) && next.phase == MAIN && next.cur != base.me) {
                // Public resource hands suffice to enumerate these paid builds.
                // Hidden rival development cards are neither assigned nor exposed.
                uint8_t actions[ACTIONS];
                int count = legal(next, deck_size, actions), rival = 1 - base.me;
                int points = public_points(next, rival);
                for (int j = 0; j < count; j++) if (actions[j] < ROLL_DICE) {
                    State built = next;
                    apply_action(b, built, c, actions[j]);
                    if (public_points(built, rival) <= points) continue;
                    value = std::min(value, leaf(built, deck_size));
                    if (value == 0) break;
                }
            }
            result += weights[i] * value;
            total += weights[i];
        }
        return float(result / total);
    }
    // Resolve a line at the same turn boundary. Hidden draws and theft are averaged; choices
    // made after seeing their outcomes are confined to completing mandatory phases.
    float finish_turn(const State& s, int deck_size) {
        if (s.winner >= 0 || done(s, c) || s.cur != base.me) return turn_endpoint(s, deck_size);
        if (is_chance(s)) {
            uint32_t weights[11];
            int slots = chance_weights(s, c, weights);
            double total = 0, result = 0;
            for (int i = 0; i < slots; i++) if (weights[i]) {
                State next = s;
                apply_chance(b, next, c, i);
                result += weights[i] * finish_turn(next, deck_size - (s.phase == DEV_DRAW));
                total += weights[i];
            }
            return float(result / total);
        }
        if (s.phase == MAIN) {
            State next = s;
            apply_action(b, next, c, END_TURN);
            return turn_endpoint(next, deck_size);
        }
        if (s.phase == ROAD_BUILDING || s.phase == MOVE_ROBBER) {
            uint8_t actions[ACTIONS];
            int n = legal(s, deck_size, actions);
            std::pair<float, int> ranked[ACTIONS];
            for (int i = 0; i < n; i++) {
                State next = s;
                apply_action(b, next, c, actions[i]);
                ranked[i] = {-score_of(next, deck_size), actions[i]};
            }
            std::sort(ranked, ranked + n);
            int count = s.phase == ROAD_BUILDING && s.free_roads == 2 ? std::min(n, 3) : n;
            float best = 0;
            for (int i = 0; i < count; i++) {
                State next = s;
                apply_action(b, next, c, ranked[i].second);
                best = std::max(best, finish_turn(next, deck_size));
            }
            return best;
        }
        return leaf(s, deck_size);
    }
    // Prove an immediate public winning purchase without relying on the neural
    // beam to retain intermediate trades. Preserve the complete purchase cost
    // in the hand and trade only surplus, using actual bank stock and ratios.
    bool public_winning_purchase(const State& s, int deck_size) {
        const int who = s.cur, points = public_points(s, who);
        if (points + 3 < c.victory_points) return false;
        auto fund = [&](const int* cost, State& funded) {
            const Player& player = funded.p[who];
            for (int r = 0; r < 5; r++)
                if (funded.bank[r] < std::max(0, cost[r] - int(player.hand[r]))) return false;
            for (int receive = 0; receive < 5; receive++) {
                while (player.hand[receive] < cost[receive]) {
                    int give = -1;
                    for (int r = 0; r < 5; r++)
                        if (int(player.hand[r]) - cost[r] >= player.ratio[r]) { give = r; break; }
                    if (give < 0 || done(funded, c)) return false;
                    int action = TRADE + give * 4 + receive - (receive > give);
                    apply_action(b, funded, c, action);
                }
            }
            return !done(funded, c);
        };
        for (int kind = 0; kind < 3; kind++) {
            State funded = s;
            int cost[5];
            for (int r = 0; r < 5; r++) cost[r] = COSTS[kind][r];
            if (!fund(cost, funded)) continue;
            uint8_t actions[ACTIONS];
            int n = legal(funded, deck_size, actions);
            for (int i = 0; i < n; i++) {
                int action = actions[i];
                if (action >= ROLL_DICE || (action < NODES ? 1 : action < ROLL_DICE - NODES ? 0 : 2) != kind) continue;
                State built = funded;
                apply_action(b, built, c, action);
                if (built.winner == who) return true;
            }
        }
        // Two upgrades do not depend on road geometry and can need more trades
        // than the response beam's four-action horizon. Verify both canonically.
        const Player& player = s.p[who];
        if (points + 2 >= c.victory_points && pop(player.cities) <= 2 && pop(player.settlements) >= 2) {
            State funded = s;
            const int cost[5] = {0, 0, 0, 4, 6};
            if (fund(cost, funded)) {
                uint8_t actions[ACTIONS];
                for (int upgrade = 0; upgrade < 2; upgrade++) {
                    int n = legal(funded, deck_size, actions), action = -1;
                    for (int i = 0; i < n; i++)
                        if (actions[i] >= ROLL_DICE - NODES && actions[i] < ROLL_DICE) { action = actions[i]; break; }
                    if (action < 0) break;
                    apply_action(b, funded, c, action);
                    if (funded.winner == who) return true;
                }
            }
        }
        return false;
    }
    // A bounded response using only public resource hands. The rival's unknown
    // development cards stay unknown: this forecast searches paid builds and
    // bank trades, never assigns a hidden hand or samples future randomness.
    float public_reply(const State& s, int deck_size) {
        if (done(s, c) || (s.phase == PRE_ROLL && s.cur == base.me))
            return leaf(s, deck_size);
        if (s.phase == PRE_ROLL) {
            State rolling = s;
            apply_action(b, rolling, c, ROLL_DICE);
            return public_reply(rolling, deck_size);
        }
        if (is_chance(s)) {
            uint32_t weights[11];
            int slots = chance_weights(s, c, weights);
            double result = 0, total = 0;
            for (int k = 0; k < slots; k++) if (weights[k]) {
                State next = s;
                apply_chance(b, next, c, k);
                result += weights[k] * public_reply(next, deck_size - (s.phase == DEV_DRAW));
                total += weights[k];
            }
            return float(result / total);
        }
        if (s.phase == DISCARD || s.phase == MOVE_ROBBER) {
            // Complete mandatory choices greedily before searching the rival's
            // purchases. Each chooser prefers its own side; theft is averaged.
            uint8_t actions[ACTIONS];
            int n = legal(s, deck_size, actions), chosen = -1;
            const float side = s.cur == base.me ? 1 : -1;
            float best = -2;
            for (int i = 0; i < n; i++) {
                State next = s;
                apply_action(b, next, c, actions[i]);
                float value;
                if (is_chance(next)) {
                    uint32_t weights[11];
                    int slots = chance_weights(next, c, weights);
                    double result = 0, total = 0;
                    for (int k = 0; k < slots; k++) if (weights[k]) {
                        State outcome = next;
                        apply_chance(b, outcome, c, k);
                        result += weights[k] * score_of(outcome, deck_size);
                        total += weights[k];
                    }
                    value = float(result / total);
                } else value = score_of(next, deck_size);
                if (side * value > best) best = side * value, chosen = actions[i];
            }
            if (chosen < 0) return leaf(s, deck_size);
            State next = s;
            apply_action(b, next, c, chosen);
            return public_reply(next, deck_size);
        }
        if (s.phase != MAIN || s.cur == base.me) return leaf(s, deck_size);
        if (public_winning_purchase(s, deck_size)) return 0;
        struct Line { State state; float rank; };
        std::vector<Line> frontier{{s, 0}}, candidates;
        float best = 1;
        // Two lines and four decisions can see trade/trade/build, road/settle,
        // and multiple scoring purchases without an unbounded opponent search.
        for (int ply = 0; ply < 4 && !frontier.empty(); ply++) {
            candidates.clear();
            for (const Line& line : frontier) {
                uint8_t actions[ACTIONS];
                int n = legal(line.state, deck_size, actions);
                for (int i = 0; i < n; i++) {
                    int action = actions[i];
                    if (!(action < ROLL_DICE || action == END_TURN || action >= TRADE)) continue;
                    State next = line.state;
                    apply_action(b, next, c, action);
                    if (done(next, c)) {
                        best = std::min(best, leaf(next, deck_size));
                        if (best == 0) return 0;
                    } else if (next.cur == base.me) best = std::min(best, leaf(next, deck_size));
                    else {
                        State ended = next;
                        apply_action(b, ended, c, END_TURN);
                        float rank = score_of(ended, deck_size);
                        best = std::min(best, rank);
                        candidates.push_back({next, rank});
                    }
                }
            }
            std::stable_sort(candidates.begin(), candidates.end(), [](const Line& a, const Line& z) { return a.rank < z.rank; });
            frontier.clear();
            for (const Line& line : candidates) {
                bool duplicate = false;
                for (const Line& kept : frontier)
                    if (same_beam_state(line.state, kept.state)) { duplicate = true; break; }
                if (duplicate) continue;
                frontier.push_back(line);
                if (frontier.size() == 2) break;
            }
        }
        return std::clamp(best, 0.0f, 1.0f);
    }
    float finish_with_reply(const State& s, int deck_size) {
        if (done(s, c) || s.cur != base.me) return public_reply(s, deck_size);
        if (is_chance(s)) {
            uint32_t weights[11];
            int slots = chance_weights(s, c, weights);
            double result = 0, total = 0;
            for (int k = 0; k < slots; k++) if (weights[k]) {
                State next = s;
                apply_chance(b, next, c, k);
                result += weights[k] * finish_with_reply(next, deck_size - (s.phase == DEV_DRAW));
                total += weights[k];
            }
            return float(result / total);
        }
        if (s.phase == MAIN) {
            State ended = s;
            apply_action(b, ended, c, END_TURN);
            return public_reply(ended, deck_size);
        }
        if (s.phase == ROAD_BUILDING || s.phase == MOVE_ROBBER) {
            uint8_t actions[ACTIONS];
            int n = legal(s, deck_size, actions);
            std::pair<float, int> ranked[ACTIONS];
            for (int i = 0; i < n; i++) {
                State next = s;
                apply_action(b, next, c, actions[i]);
                ranked[i] = {-finish_turn(next, deck_size), actions[i]};
            }
            std::sort(ranked, ranked + n);
            // Rank mandatory paths by the original endpoint value, then assess
            // the selected path's public reply. All first roads are ranked here.
            if (n) {
                State next = s;
                apply_action(b, next, c, ranked[0].second);
                return finish_with_reply(next, deck_size);
            }
        }
        return leaf(s, deck_size);
    }
    // Only the original sanitized View and public board seed these streams.
    // Every finalist uses the same belief and separate dice/dev/theft streams.
    uint64_t rollout_seed() const {
        StateScoreKey key;
        write_state_key(base.s, base.deck_size, key);
        uint64_t seed = feature_hash(key.data(), int(key.size()));
        auto add = [&](uint8_t byte) { seed = (seed ^ byte) * 0x100000001b3ULL; };
        add(base.me), add(base.rival_cards), add(base.rival_bought), add(base.deck_size);
        for (int i = 0; i < TILES; i++) add(uint8_t(b.resources[i])), add(b.numbers[i]);
        for (int i = 0; i < 9; i++) add(uint8_t(b.port_resources[i]));
        add(b.alternate_ports ? 1 : 0);
        return seed;
    }
    static uint64_t combinations(int n, int k) {
        if (k < 0 || k > n) return 0;
        k = std::min(k, n - k);
        uint64_t result = 1;
        for (int i = 1; i <= k; i++) result = result * uint64_t(n - k + i) / uint64_t(i);
        return result;
    }
    bool prepare_retention_belief(RetentionBelief& prepared) const {
        if (base.s.phase != MAIN || base.s.turn_player != base.me || done(base.s, c)) return false;
        // The other player ended their turn before this MAIN root, clearing bought[].
        // Unexpected age information is not approximated by the fitted mature-hand law.
        if (base.rival_bought != 0 || sum5(base.s.deck) != base.rival_cards + base.deck_size) return false;
        RetentionBelief::Counts pool;
        for (int t = 0; t < 5; ++t) pool[t] = base.s.deck[t];
        // Exact train-only fit from training/retention/model.json; KNIGHT is baseline.
        const RetentionBelief::LogWeights theta = {
            0.0, 1.9643513439850364, 0.007863944175760883, -0.320691297631005, 0.12540603456233995
        };
        const int cap = int(c.victory_points) - 1 - public_points(base.s, 1 - base.me);
        return prepared.prepare(pool, base.rival_cards, cap, theta);
    }
    bool sample_retention_belief(const RetentionBelief& prepared, uint64_t seed, State& sampled) const {
        Rng belief(seed ^ 0xa49b8132f093ca61ULL);
        auto uniform = [&] { return double(belief.next() >> 11) * 0x1.0p-53; };
        RetentionBelief::Counts cards;
        if (!prepared.sample(cards, uniform)) return false;
        sampled = base.s;
        Player& hidden = sampled.p[1 - base.me];
        for (int t = 0; t < 5; ++t) {
            hidden.dev[t] = uint8_t(cards[t]);
            hidden.bought[t] = 0;
            sampled.deck[t] -= uint8_t(cards[t]);
        }
        return sum5(hidden.dev) == base.rival_cards && sum5(sampled.deck) == base.deck_size &&
               total_points(sampled, 1 - base.me) < c.victory_points;
    }
    // An exchangeable-card prior conditioned on the observed game not being
    // over. On our turn rival points cannot increase: our builds/knights can
    // only take their bonuses. They therefore did not hold winning hidden VP
    // on their preceding turn. Keep this cap at the ORIGINAL root, even if a
    // candidate later takes a bonus. This is not an opponent-history posterior.
    bool rollout_belief(uint64_t seed, State& sampled) const {
        if (base.s.phase != MAIN || base.s.turn_player != base.me || done(base.s, c)) return false;
        int rival = 1 - base.me, cards = base.rival_cards, total = sum5(base.s.deck);
        if (total != cards + base.deck_size || total > 25 || base.rival_bought > cards) return false;
        int vp = base.s.deck[VICTORY_POINT], other = total - vp;
        int cap = int(c.victory_points) - 1 - public_points(base.s, rival);
        int low = std::max(0, cards - other), high = std::min({cards, vp, cap});
        uint64_t weights[26] = {}, mass = 0;
        for (int k = low; k <= high; k++) mass += weights[k] = combinations(vp, k) * combinations(other, cards - k);
        if (!mass || mass > UINT32_MAX) return false;
        Rng belief(seed ^ 0xa49b8132f093ca61ULL);
        uint64_t pick = belief.below(uint32_t(mass));
        int hidden_vp = low;
        while (hidden_vp < high && pick >= weights[hidden_vp]) pick -= weights[hidden_vp++];
        sampled = base.s;
        Player& hidden = sampled.p[rival];
        std::fill(hidden.dev, hidden.dev + 5, uint8_t(0));
        std::fill(hidden.bought, hidden.bought + 5, uint8_t(0));
        hidden.dev[VICTORY_POINT] = uint8_t(hidden_vp);
        sampled.deck[VICTORY_POINT] -= uint8_t(hidden_vp);
        for (int i = hidden_vp; i < cards; i++) {
            int draw = int(belief.below(uint32_t(other--)));
            for (int k = 0; k < 5; k++) if (k != VICTORY_POINT) {
                if (draw < sampled.deck[k]) { sampled.deck[k]--, hidden.dev[k]++; break; }
                draw -= sampled.deck[k];
            }
        }
        uint8_t available[5];
        std::copy(hidden.dev, hidden.dev + 5, available);
        for (int i = 0; i < base.rival_bought; i++) {
            int draw = int(belief.below(uint32_t(cards - i)));
            for (int k = 0; k < 5; k++) {
                if (draw < available[k]) { available[k]--, hidden.bought[k]++; break; }
                draw -= available[k];
            }
        }
        return sum5(sampled.deck) == base.deck_size && total_points(sampled, rival) < c.victory_points;
    }
    float rollout_value(const State& state, bool stopped = false) {
        if (state.winner >= 0) return state.winner == base.me;
        // Never pass a fully instantiated belief to an evaluator as a View.
        // observe re-hides the other seat's current dev cards and rebuilds the
        // unseen pool, including cards drawn/played inside the simulation.
        View terminal = observe(state, base.me);
        if (stopped && agent.graph_leaf == 1) return agent.graph->value(b, c, terminal);
        if (stopped && agent.graph_leaf == 2) return 0.5f * (agent.graph->value(b, c, terminal) + evaluate_view(terminal));
        return evaluate_view(terminal);
    }
    // Planned rollout turns keep one search per seat across every sample and finalist of a decision: scores are
    // exact functions of the keyed state and viewer, so a cache shared across beliefs returns the same values.
    Agent rollout_policy = {};
    std::unique_ptr<Search> rollout_search[2] = {};
    // Worker searches over the same root for field 38: private caches, rollout searches and counters, so a
    // thread never touches another's state. Values differ from one thread's only by evaluation rounding.
    std::vector<std::unique_ptr<Search>> helpers = {};
    float rollout_endpoint(const State& endpoint, const State& belief, uint64_t seed) {
        State simulated = endpoint;
        const int rival = 1 - base.me;
        std::copy(belief.deck, belief.deck + 5, simulated.deck);
        std::copy(belief.p[rival].dev, belief.p[rival].dev + 5, simulated.p[rival].dev);
        std::copy(belief.p[rival].bought, belief.p[rival].bought + 5, simulated.p[rival].bought);
        CATAN_ROLLOUT_OBSERVER(*this, "begin", simulated, seed, 0, 0., 0., 0.);
        Rng dice(seed ^ 0x2dd79a409a716c17ULL), dev(seed ^ 0x1cb875d064a0ba11ULL);
        Rng theft(seed ^ 0xe133ab27fc80150dULL);
        Rng policies[2] = {Rng(seed ^ 0x871893dcc7a5c363ULL), Rng(seed ^ 0x9c18e08134fe6aa5ULL)};
        Agent policy = agent;
        policy.depth = 1;
        policy.economy = 0;
        if (!agent.rollout_plan) policy.plan = policy.threat = 0;
        policy.epsilon = policy.tie = 0;
        policy.setup = 0;
        policy.full_setup = policy.staged = false;
        policy.memoize = true;
        policy.turn_beam = policy.turn_roll = policy.turn_reply = policy.rollout_samples = 0;
        policy.turn_value_only = false;
        policy.pre_roll_normalize = false;  // keep the sampled continuation policy at one decision
        policy.graph_leaf = 0;
        const bool planned = agent.rollout_beam > 0;
        if (planned) {  // plan whole turns inside the rollouts, with no further rollouts or replies
            policy.turn_beam = agent.rollout_beam;
            policy.depth = agent.rollout_depth > 0 ? agent.rollout_depth : 3;
            policy.rollout_plan = false, policy.rollout_beam = policy.rollout_depth = 0;
            policy.memoize = false;  // the turn caches serve instead
            policy.mandatory_depth = 1;  // robber, discard and setup choices stay one decision deep, as in the plain policy
            policy.pre_roll_normalize = agent.rollout_normalize;
            if (!rollout_search[0]) {
                rollout_policy = policy;
                for (int seat = 0; seat < 2; seat++) {
                    View seen = observe(simulated, seat);
                    rollout_search[seat].reset(new Search{b, c, *policy.net, seen, rollout_policy});
                    rollout_search[seat]->evaluation_owner = this;
                }
            }
        }
        const uint64_t stop_turn = uint64_t(base.s.turn) + 2 + agent.rollout_turns;  // finish this turn and one rival turn
        uint32_t counted_turn = simulated.turn;
        int main_actions = 0, events = 0;
        double correction = 0;
        rollout_paths++;
        for (; events < 96 * (1 + agent.rollout_turns) && !done(simulated, c) && uint64_t(simulated.turn) < stop_turn; events++) {
            if (simulated.turn != counted_turn) counted_turn = simulated.turn, main_actions = 0;
            if (is_chance(simulated)) {
                Rng& chance = simulated.phase == ROLL ? dice : simulated.phase == DEV_DRAW ? dev : theft;
                const int outcome = sample_chance(simulated, c, chance);
                if (agent.rollout_control) {
                    // Center within this sampled physical world; phi still sees only the root player's View.
                    uint32_t weights[11];
                    const int slots = chance_weights(simulated, c, weights);
                    double total = 0, mean = 0;
                    float realized = 0;
                    for (int i = 0; i < slots; i++) if (weights[i]) {
                        State next = simulated;
                        apply_chance(b, next, c, i);
                        const float value = rollout_value(next);
                        mean += weights[i] * double(value);
                        total += weights[i];
                        if (i == outcome) realized = value;
                    }
                    const double expected = mean / total;
                    correction += double(realized) - expected;
                    CATAN_ROLLOUT_OBSERVER(*this, "chance", simulated, seed, outcome, expected, realized, correction);
                }
                apply_chance(b, simulated, c, outcome);
                CATAN_ROLLOUT_OBSERVER(*this, "chance_step", simulated, seed, outcome, 0., 0., 0.);
                continue;
            }
            int action;
            // The finalist already selected our voluntary line. Complete only
            // mandatory phases, then end it; later rival choices use its View.
            if (simulated.phase == MAIN && simulated.turn == base.s.turn) action = END_TURN;
            else if (simulated.phase == MAIN && main_actions >= 12) action = END_TURN, rollout_forced_ends++;
            else {
                View seen = observe(simulated, simulated.cur);
                action = planned ? choose_in(rollout_search[simulated.cur].get(), rollout_policy, b, c, seen, policies[simulated.cur], nullptr, nullptr)
                                 : choose(policy, b, c, seen, policies[simulated.cur]);
                rollout_policy_calls++;
            }
            if (simulated.phase == MAIN) main_actions++;
            apply_action(b, simulated, c, action);
            CATAN_ROLLOUT_OBSERVER(*this, "action_step", simulated, seed, action, 0., 0., 0.);
        }
        rollout_events += size_t(events);
        if (!done(simulated, c) && uint64_t(simulated.turn) < stop_turn) {
            rollout_truncated++;
            CATAN_ROLLOUT_OBSERVER(*this, "truncated", simulated, seed, events, 0., correction, 0.);
            return 0;  // caller retains the deterministic shortlist, never a partial-horizon value
        }
        const float raw = rollout_value(simulated, true);
        const float result = agent.rollout_control ? float(double(raw) - correction) : raw;
        CATAN_ROLLOUT_OBSERVER(*this, "end", simulated, seed, events, raw, correction, result);
        return result;
    }
    // Finalists are compared by paired differences against the deterministic reply's best one, with a
    // one-SE penalty: a variance-control heuristic, not a confidence guarantee. An uncertain gain keeps index 0.
    static constexpr int MAX_FINALISTS = 8, MAX_SAMPLES = 192;
    void complete_turn_values(const uint8_t* roots, int count, float* values, std::vector<Option>* options = nullptr) {
        struct Line { State state; int first; float rank; };
        struct Endpoint { State state; bool present = false; };
        std::array<Endpoint, ACTIONS> endpoints;
        const int width = std::clamp(agent.turn_beam, 1, 128), horizon = std::max(1, agent.depth);
        std::fill(values, values + count, -2.0f);
        auto record = [&](int first, const State& state) {
            float value = finish_turn(state, base.deck_size);
            if (value > values[first]) {
                values[first] = value;
                if (agent.turn_reply > 0 || agent.rollout_samples > 0) endpoints[first] = {state, true};
            }
        };
        std::vector<Line> frontier{{base.s, -1, 0}}, candidates;
        candidates.reserve(size_t(width) * ACTIONS);
        for (int ply = 0; ply < horizon && !frontier.empty(); ply++) {
            candidates.clear();
            for (const Line& line : frontier) {
                uint8_t moves[ACTIONS];
                int n;
                if (!ply) std::copy(roots, roots + count, moves), n = count;
                else n = legal(line.state, base.deck_size, moves);
                for (int i = 0; i < n; i++) {
                    State next = line.state;
                    apply_action(b, next, c, moves[i]);
                    int first = ply ? line.first : i;
                    if (next.winner == base.me) { values[first] = 1; return; }
                    if (done(next, c) || is_chance(next) || next.cur != base.me) {
                        record(first, next);
                    } else {
                        // The cheap ranking is also normalized to the end of the turn where legal.
                        State ended = next;
                        if (ended.phase == MAIN) apply_action(b, ended, c, END_TURN);
                        candidates.push_back({next, first, score_of(ended, base.deck_size)});
                    }
                }
            }
            std::stable_sort(candidates.begin(), candidates.end(), [](const Line& a, const Line& z) { return a.rank > z.rank; });
            frontier.clear();
            for (const Line& line : candidates) {
                bool duplicate = false;
                for (const Line& kept : frontier) if (same_beam_state(line.state, kept.state)) { duplicate = true; break; }
                if (duplicate) continue;
                frontier.push_back(line);
                if (int(frontier.size()) == width) break;
            }
        }
        for (const Line& line : frontier) record(line.first, line.state);
        if (options)
            for (int i = 0; i < count; i++)
                if (values[i] > -1) options->push_back({roots[i], values[i]});
        auto ranked_endpoints = [&](bool retained_only, bool distinct) {
            std::vector<std::pair<float, int>> ranked;
            for (int i = 0; i < count; i++) if (endpoints[i].present && (!retained_only || values[i] > -1))
                ranked.push_back({-values[i], i});
            std::sort(ranked.begin(), ranked.end());
            if (distinct) {
                rollout_endpoint_candidates += ranked.size();
                std::vector<StateScoreKey> keys;
                std::vector<std::pair<float, int>> unique;
                for (const auto& item : ranked) {
                    State normalized = endpoints[item.second].state;
                    if (normalized.phase == MAIN && !done(normalized, c)) apply_action(b, normalized, c, END_TURN);
                    StateScoreKey key;
                    write_state_key(normalized, base.deck_size, key);
                    if (std::find(keys.begin(), keys.end(), key) != keys.end()) {
                        rollout_endpoint_duplicates++;
                        continue;
                    }
                    keys.push_back(key);
                    unique.push_back(item);  // preserve this representative's original first action/path
                }
                ranked = std::move(unique);
            }
            return ranked;
        };
        if (agent.turn_reply > 0) {
            // Sampled replies must spend their budget on distinct plans, before
            // the public-reply pruning could retain only aliases of one plan.
            auto ranked = ranked_endpoints(false, agent.rollout_samples > 0);
            std::fill(values, values + count, -2.0f);
            for (int k = 0; k < std::min<int>(agent.turn_reply, ranked.size()); k++) {
                int first = ranked[k].second;
                values[first] = finish_with_reply(endpoints[first].state, base.deck_size);
            }
        }
        if (agent.rollout_samples > 0) {
            auto ranked = ranked_endpoints(true, agent.turn_reply <= 0);
            if (ranked.empty()) return;
            ranked.resize(std::min<size_t>(agent.finalists > 0 ? std::min(agent.finalists, MAX_FINALISTS) : 3, ranked.size()));
            rollout_distinct_finalists += ranked.size();
            const int samples = std::clamp(agent.rollout_samples, 1, MAX_SAMPLES);
            const int rounds = std::clamp(agent.rollout_rounds, 1, 8);
            const int count_f = int(ranked.size());
            const uint64_t root_seed = rollout_seed();
            std::unique_ptr<RetentionBelief> retention;
            if (agent.retention_belief) {
                retention.reset(new RetentionBelief);
                if (!prepare_retention_belief(*retention)) return;
            }
            // Rounds of paired samples. After each round the one-SE choice is made on every sample so far; a
            // finalist whose gain sits two standard errors from the current choice's stops being sampled, and
            // the search stops when only the choice is left. One round is the historical behaviour.
            std::vector<float> returns[MAX_FINALISTS];
            bool alive[MAX_FINALISTS];
            std::fill(alive, alive + count_f, true);
            const size_t previous_truncations = rollout_truncated;
            int selected = 0;
            for (int round = 0; round < rounds; round++) {
                std::vector<State> beliefs(samples);
                uint64_t seeds[MAX_SAMPLES];
                for (int i = 0; i < samples; i++) {
                    const int sample = round * samples + i;
                    seeds[i] = root_seed ^ (0x9e3779b97f4a7c15ULL * uint64_t(sample + 1));
                    if (retention) {
                        if (!sample_retention_belief(*retention, seeds[i], beliefs[i])) return;
                    } else if (!rollout_belief(seeds[i], beliefs[i])) return;
                }
                const int workers = std::clamp(agent.workers, 1, 8);
                if (workers <= 1 || samples < 2) {
                    for (int i = 0; i < samples; i++)
                        for (int k = 0; k < count_f; k++) {
                            if (!alive[k] && k != 0) continue;
                            returns[k].push_back(rollout_endpoint(endpoints[ranked[k].second].state, beliefs[i], seeds[i]));
                            if (rollout_truncated != previous_truncations) return;
                        }
                } else {
                    while (int(helpers.size()) < workers - 1) helpers.emplace_back(new Search{b, c, net, base, agent});
                    std::vector<std::array<float, MAX_FINALISTS>> round_returns(samples);
                    std::atomic<int> next{0};
                    std::atomic<bool> truncated{false};
                    auto run = [&](Search& worker) {
                        const size_t before = worker.rollout_truncated;
                        for (int i; (i = next.fetch_add(1)) < samples && !truncated.load();)
                            for (int k = 0; k < count_f; k++) {
                                if (!alive[k] && k != 0) continue;
                                round_returns[i][k] = worker.rollout_endpoint(endpoints[ranked[k].second].state, beliefs[i], seeds[i]);
                                if (worker.rollout_truncated != before) { truncated = true; break; }
                            }
                    };
                    std::vector<std::thread> pool;
                    for (int w = 0; w + 1 < workers; w++) pool.emplace_back([&run, this, w] { run(*helpers[size_t(w)]); });
                    run(*this);
                    for (std::thread& thread : pool) thread.join();
                    if (truncated) { rollout_truncated++; return; }
                    for (int i = 0; i < samples; i++)
                        for (int k = 0; k < count_f; k++)
                            if (alive[k] || k == 0) returns[k].push_back(round_returns[i][k]);
                }
                double means[MAX_FINALISTS] = {}, errors[MAX_FINALISTS] = {}, best_gain = 0;
                selected = 0;
                for (int k = 1; k < count_f; k++) {
                    const int n = int(returns[k].size());
                    if (n < 2) continue;
                    double sum = 0, squares = 0;
                    for (int i = 0; i < n; i++) {
                        double gain = double(returns[k][i]) - returns[0][i];
                        sum += gain;
                        squares += gain * gain;
                    }
                    errors[k] = std::sqrt(std::max(0., squares - sum * sum / n) / (double(n) * (n - 1)));
                    means[k] = sum / n;
                    double adjusted = means[k] - errors[k];
                    if (adjusted > best_gain) best_gain = adjusted, selected = k;
                }
                if (round + 1 == rounds) break;
                int contenders = 0;
                for (int k = 0; k < count_f; k++) {
                    if (!alive[k]) continue;
                    if (k != selected && std::fabs(means[k] - means[selected]) >= 2 * std::max(errors[k], errors[selected])) {
                        alive[k] = false;
                        continue;
                    }
                    contenders++;
                }
                if (contenders <= 1) break;
            }
            if (trace) {
                std::vector<Finalist> compared;
                bool whole = true;
                for (int k = 0; k < count_f && whole; k++) {
                    double gain = 0;
                    for (size_t i = 0; i < returns[k].size(); i++) gain += double(returns[k][i]) - returns[0][i];
                    compared.push_back({roots[ranked[k].second], k == selected, float(gain / double(std::max<size_t>(1, returns[k].size()))), {}});
                    whole = turn_leaves(endpoints[ranked[k].second].state, base.deck_size, 1, compared.back().leaves);
                }
                if (whole) trace->finalists = std::move(compared);
            }
            rollout_anchor_switches += selected != 0;
            double mean = 0;
            for (float value : returns[selected]) mean += value;
            // Exclude nonpositive/uncertain gains rather than tying them with
            // the anchor: choose() randomly selects among equal best values.
            std::fill(values, values + count, -2.0f);
            const float selected_mean = float(mean / double(returns[selected].size()));
            values[ranked[selected].second] = agent.rollout_control
                ? std::clamp(selected_mean, 0.000001f, 0.999999f) : selected_mean;
        }
    }
    // The positions finish_turn values a planned turn at, each chance outcome with its odds. False when the plan
    // stops inside a robber move or a free road, where finish_turn searches on.
    Trace* trace = nullptr;
    bool turn_leaves(const State& s, int deck_size, double weight, std::vector<Leaf>& out) {
        auto keep = [&](const State& ended) {
            Leaf leaf{float(weight), ended.winner >= 0 ? float(ended.winner == base.me) : -1.0f, 0, {}};
            if (ended.winner < 0) {
                View v = base;
                v.s = ended;
                v.deck_size = uint8_t(deck_size);
                if (agent.card_inputs) card_features(b, c, v, leaf.x);
                else if (agent.wide) board_features(b, c, v, leaf.x);
                else if (agent.richer) build_features(b, c, v, leaf.x);
                else features(b, c, v, leaf.x);
                const float raw = std::clamp(evaluate(leaf.x), 0.000001f, 0.999999f);
                const float full = std::clamp(score_of(ended, deck_size), 0.000001f, 0.999999f);
                leaf.prior = std::log(full / (1 - full)) - std::log(raw / (1 - raw));
            }
            out.push_back(leaf);
        };
        if (s.winner >= 0 || done(s, c) || s.cur != base.me) return keep(s), true;
        if (is_chance(s)) {
            uint32_t weights[11];
            int slots = chance_weights(s, c, weights);
            double total = 0;
            for (int i = 0; i < slots; i++) total += weights[i];
            for (int i = 0; i < slots; i++) if (weights[i]) {
                State next = s;
                apply_chance(b, next, c, i);
                if (!turn_leaves(next, deck_size - (s.phase == DEV_DRAW), weight * weights[i] / total, out)) return false;
            }
            return true;
        }
        if (s.phase != MAIN) return false;
        State next = s;
        apply_action(b, next, c, END_TURN);
        return keep(next), true;
    }
    // Match the native two-decision mandatory chooser. Stable ties make the
    // forecast's chosen placement identical to the next real mandatory choice.
    int pre_roll_action(const State& s, int deck_size, float* value = nullptr) {
        uint8_t actions[ACTIONS];
        int n = legal(s, deck_size, actions), chosen = -1;
        float best = -2;
        for (int i = 0; i < n; i++) {
            State next = s;
            apply_action(b, next, c, actions[i]);
            float score = after(next, deck_size, 1);
            if (score > best) best = score, chosen = actions[i];
        }
        if (value) *value = best;
        return chosen;
    }
    // Knight and Road Building consume mandatory choices before the dice. Follow
    // their native mandatory policy, then compare every pre-roll card with the
    // same unchanged after(ROLL,1) horizon. Only the selected placement is expanded;
    // theft is averaged exactly and neither dice nor real choice RNG is sampled.
    float finish_pre_roll(const State& s, int deck_size) {
        if (done(s, c) || s.cur != base.me || !s.pre_roll) return leaf(s, deck_size);
        if (s.phase == ROLL) return after(s, deck_size, 1);
        if (s.phase == STEAL) {
            uint32_t weights[11];
            int count = chance_weights(s, c, weights);
            double total = 0, value = 0;
            for (int i = 0; i < count; i++) if (weights[i]) {
                State next = s;
                apply_chance(b, next, c, i);
                value += weights[i] * finish_pre_roll(next, deck_size);
                total += weights[i];
            }
            return total ? float(value / total) : leaf(s, deck_size);
        }
        if (s.phase == MOVE_ROBBER || s.phase == ROAD_BUILDING) {
            int action = pre_roll_action(s, deck_size);
            if (action >= 0) {
                State next = s;
                apply_action(b, next, c, action);
                return finish_pre_roll(next, deck_size);
            }
        }
        return leaf(s, deck_size);
    }
    // Value of a position reached by an action: chance is averaged over its exact odds.
    float after(const State& s, int deck_size, int depth) {
        if (s.winner >= 0 || !is_chance(s)) return node(s, deck_size, depth);
        uint32_t weights[11];
        int slots = chance_weights(s, c, weights);
        double total = 0, value = 0;
        for (int i = 0; i < slots; i++) total += weights[i];
        for (int i = 0; i < slots; i++) {
            if (!weights[i]) continue;
            State next = s;
            apply_chance(b, next, c, i);
            value += weights[i] * node(next, deck_size - (s.phase == DEV_DRAW), depth);
        }
        return float(value / total);
    }
    float node(const State& s, int deck_size, int depth) {
        if (s.winner >= 0 || depth == 0 || is_chance(s) || s.cur != base.me) return leaf(s, deck_size);
        uint8_t actions[ACTIONS];
        int n = legal(s, deck_size, actions);
        float best = 0;
        for (int i = 0; i < n; i++) {
            State next = s;
            apply_action(b, next, c, actions[i]);
            best = std::max(best, after(next, deck_size, depth - 1));
        }
        return best;
    }
};

}  // namespace

View observe(const State& s, int player) {
    View v{s, uint8_t(player), 0, 0, uint8_t(sum5(s.deck))};
    Player& rival = v.s.p[1 - player];
    v.rival_cards = uint8_t(sum5(rival.dev));
    v.rival_bought = uint8_t(sum5(rival.bought));
    for (int k = 0; k < 5; k++) {
        v.s.deck[k] += rival.dev[k];
        rival.dev[k] = rival.bought[k] = 0;
    }
    return v;
}

void features(const Board& b, const Config& c, const View& v, uint8_t out[FEATURES]) {
    const Topology& t = topology();
    const State& s = v.s;
    int n = 0;
    auto put = [&](double x) { out[n++] = uint8_t(std::clamp(int(std::lround(x * 100)), 0, 255)); };

    for (int phase = 0; phase <= GAME_OVER; phase++) put(s.phase == phase);
    put(s.cur == v.me), put(s.turn_player == v.me), put(s.pre_roll), put(s.setup_count / 4.0);
    // Odds of the next roll: the turn player's if they have not rolled yet, otherwise the other player's.
    uint32_t weights[11];
    double roll[11], total = 0;
    if (c.balanced_dice)
        s.dice.weights(s.pre_roll || s.phase == ROLL ? s.turn_player : 1 - s.turn_player, weights);
    else
        std::copy(DICE_DECK, DICE_DECK + 11, weights);
    for (uint32_t w : weights) total += w;
    for (int i = 0; i < 11; i++) roll[i] = total ? weights[i] / total : DICE_DECK[i] / 36.0, put(roll[i] * 6);
    int cards = 0;
    for (uint8_t count : s.dice.deck) cards += count;
    put(cards / 36.0);
    for (int r = 0; r < 5; r++) put(s.bank[r] / 19.0);
    put(v.deck_size / 25.0), put(std::min<uint32_t>(s.turn, 200) / 200.0), put(s.free_roads / 2.0);
    double owed[2] = {0, 0};
    for (int i = 0; i < s.discard_count; i++) owed[s.discards[i].player != v.me] = s.discards[i].remaining / 10.0;
    put(owed[0]), put(owed[1]);
    int unseen = sum5(s.deck);
    for (int k = 0; k < 5; k++) put(unseen ? double(s.deck[k]) / unseen : 0);

    Nodes blocked = blocked_nodes(s);
    put(pop(~blocked & ((Nodes(1) << NODES) - 1)) / 10.0);  // settlement spots left anywhere
    for (int i : {int(v.me), 1 - v.me}) {
        const Player& p = s.p[i];
        bool mine = i == v.me;
        int pub = public_points(s, i), points = mine ? total_points(s, i) : pub, hand = sum5(p.hand);
        put(pub / 15.0), put(points / 15.0), put((c.victory_points - points) / 15.0);
        put(pop(p.settlements) / 5.0), put(pop(p.cities) / 4.0), put(pop(p.roads) / 15.0);
        put(s.road_len[i] / 15.0), put(s.longest_road == i), put(s.largest_army == i), put(std::min<int>(p.played[KNIGHT], 6) / 6.0);
        for (int r = 0; r < 5; r++) put(std::min<int>(p.hand[r], 12) / 6.0);
        put(hand / 20.0), put(hand > c.discard_threshold);
        for (int r = 0; r < 5; r++) put((4 - p.ratio[r]) / 2.0);
        // Income per roll by resource: long-run, under the next roll's odds, and what the robber is blocking.
        double income[5] = {}, next[5] = {}, robbed = 0;
        for (int tile = 0; tile < TILES; tile++) {
            int weight = pop(p.settlements & t.tile_mask[tile]) + 2 * pop(p.cities & t.tile_mask[tile]);
            if (!weight || b.resources[tile] < 0) continue;
            double chance = DICE_DECK[b.numbers[tile] - 2] / 36.0;
            if (tile == s.robber) {
                robbed += weight * chance;
            } else {
                income[b.resources[tile]] += weight * chance;
                next[b.resources[tile]] += weight * roll[b.numbers[tile] - 2];
            }
        }
        double all = 0;
        int kinds = 0;
        for (int r = 0; r < 5; r++) put(income[r] * 3), all += income[r], kinds += income[r] > 0;
        put(robbed * 3);
        for (int r = 0; r < 5; r++) put(next[r] * 3);
        put(kinds / 5.0), put(all * 1.5);
        // Development cards: the viewer's own by kind, the rival's only as counts.
        for (int k : {int(KNIGHT), int(ROAD_BUILDING_CARD), int(YEAR_OF_PLENTY_CARD), int(MONOPOLY_CARD)})
            put(mine ? std::min(p.dev[k] - p.bought[k], 3) / 3.0 : 0);
        put(mine ? p.dev[VICTORY_POINT] / 3.0 : 0);
        put((mine ? sum5(p.bought) : v.rival_bought) / 3.0);
        put(mine ? 0 : std::min<int>(v.rival_cards, 8) / 4.0);
        put(p.played[ROAD_BUILDING_CARD] / 2.0), put(p.played[YEAR_OF_PLENTY_CARD] / 2.0), put(p.played[MONOPOLY_CARD] / 2.0);
        put(p.played_dev);
        for (auto& cost : COSTS) {
            bool can = true;
            for (int r = 0; r < 5; r++) can &= p.hand[r] >= cost[r];
            put(can);
        }
        // Room to grow: settlement spots already connected, the best of them, and free road edges.
        Nodes spots = road_endpoints(p.roads) & ~blocked;
        int best = 0;
        for (Nodes m = spots; m; m &= m - 1) best = std::max<int>(best, b.node_pips[__builtin_ctzll(m)]);
        put(std::min(pop(spots), 4) / 4.0), put(best / 15.0), put(std::min(pop(road_edges(s, i)), 10) / 10.0);
        // Spots one and two roads away. Roads cannot use a taken edge or pass a rival building.
        const Player& rival = s.p[1 - i];
        Nodes walls = rival.settlements | rival.cities, frontier = (p.settlements | p.cities | road_endpoints(p.roads)) & ~walls;
        Nodes seen = frontier;
        Edges taken = p.roads | rival.roads;
        for (int step = 1; step <= 2; step++) {
            Nodes next = 0;
            for (Nodes m = frontier; m; m &= m - 1) {
                int node = __builtin_ctzll(m);
                for (int k = 0; k < t.node_edge_count[node]; k++) {
                    int e = t.node_edges[node][k];
                    if (taken >> e & 1) continue;
                    next |= Nodes(1) << (t.edge_nodes[e][0] == node ? t.edge_nodes[e][1] : t.edge_nodes[e][0]);
                }
            }
            next &= ~seen, seen |= next, frontier = next & ~walls;
            int top = 0;
            for (Nodes m = next & ~blocked; m; m &= m - 1) top = std::max<int>(top, b.node_pips[__builtin_ctzll(m)]);
            put(std::min(pop(next & ~blocked), 6) / 6.0), put(top / 15.0);
        }
        // Cards short of each purchase, and how many bank trades the hand could fund.
        int trades = 0;
        for (auto& cost : COSTS) {
            int shortfall = 0;
            for (int r = 0; r < 5; r++) shortfall += std::max(0, cost[r] - p.hand[r]);
            put(shortfall / 3.0);
        }
        for (int r = 0; r < 5; r++) trades += p.hand[r] / p.ratio[r];
        put(std::min(trades, 4) / 2.0);
        // Races: knights played plus held (expected, for the rival), and the road-length lead.
        double knights = p.played[KNIGHT] + (mine ? p.dev[KNIGHT] : unseen ? v.rival_cards * double(s.deck[KNIGHT]) / unseen : 0);
        put(std::min(knights, 6.0) / 3.0), put(std::clamp(s.road_len[i] - s.road_len[1 - i] + 5, 0, 10) / 5.0);
        // Income under the dice cards still in the deck, and the best settlement to upgrade.
        double remaining = 0;
        for (int tile = 0; tile < TILES; tile++) {
            int weight = pop(p.settlements & t.tile_mask[tile]) + 2 * pop(p.cities & t.tile_mask[tile]);
            if (weight && b.resources[tile] >= 0 && tile != s.robber)
                remaining += weight * (cards >= 13 ? double(s.dice.deck[b.numbers[tile] - 2]) / cards : DICE_DECK[b.numbers[tile] - 2] / 36.0);
        }
        int upgrade = 0;
        for (Nodes m = p.settlements; m; m &= m - 1) upgrade = std::max<int>(upgrade, b.node_pips[__builtin_ctzll(m)]);
        put(remaining * 1.5), put(upgrade / 15.0);
    }
    if (n != FEATURES) fprintf(stderr, "feature count is %d, expected %d\n", n, FEATURES), abort();
}

void build_features(const Board& b, const Config& c, const View& v, uint8_t out[BUILD_FEATURES]) {
    features(b, c, v, out);
    const Topology& t = topology();
    const State& s = v.s;
    Nodes blocked = blocked_nodes(s);
    double production[NODES][5] = {};
    for (int tile = 0; tile < TILES; tile++) {
        if (b.resources[tile] < 0 || tile == s.robber) continue;
        for (int k = 0; k < 6; k++)
            production[t.tile_nodes[tile][k]][b.resources[tile]] += DICE_DECK[b.numbers[tile] - 2] / 36.0;
    }
    int at = FEATURES;
    auto put = [&](double x) { out[at++] = uint8_t(std::clamp(int(std::lround(x * 100)), 0, 255)); };
    for (int who : {int(v.me), 1 - v.me}) {
        const Player& p = s.p[who];
        const Player& rival = s.p[1 - who];
        double income[5] = {};
        for (int node = 0; node < NODES; node++) {
            int count = int(p.settlements >> node & 1) + 2 * int(p.cities >> node & 1);
            for (int r = 0; r < 5; r++) income[r] += count * production[node][r];
        }
        double baseline[5];
        for (int r = 0; r < 5; r++) baseline[r] = std::log1p(4 * income[r]);
        auto profile = [&](Nodes spots) {
            const double weights[5] = {1, 1, 1, 1.5, 2};
            int best = -1;
            double value = -1;
            for (Nodes m = spots; m; m &= m - 1) {
                int node = __builtin_ctzll(m);
                double score = 0;
                for (int r = 0; r < 5; r++)
                    if (production[node][r]) score += weights[r] * (std::log1p(4 * (income[r] + production[node][r])) - baseline[r]);
                if (score > value) value = score, best = node;
            }
            for (int r = 0; r < 5; r++) put(best < 0 ? 0 : 3 * production[best][r]);
        };
        profile(pop(p.cities) < 4 ? p.settlements : 0);
        Nodes walls = rival.settlements | rival.cities;
        Nodes frontier = (p.settlements | p.cities | road_endpoints(p.roads)) & ~walls, seen = frontier;
        Edges taken = p.roads | rival.roads;
        for (int step = 0; step <= 2; step++) {
            profile(pop(p.settlements) < 5 && pop(p.roads) + step <= 15 ? frontier & ~blocked : 0);
            Nodes next = 0;
            for (Nodes m = frontier; m; m &= m - 1) {
                int node = __builtin_ctzll(m);
                for (int k = 0; k < t.node_edge_count[node]; k++) {
                    int edge = t.node_edges[node][k];
                    if (!(taken >> edge & 1)) next |= Nodes(1) << (t.edge_nodes[edge][0] == node ? t.edge_nodes[edge][1] : t.edge_nodes[edge][0]);
                }
            }
            next &= ~seen;
            seen |= next;
            frontier = next & ~walls;
        }
        for (auto& cost : COSTS) put(std::min(12, turns_to_fund(p.hand, income, p.ratio, cost)) / 12.0);
    }
}

void board_features(const Board& b, const Config& c, const View& v, uint8_t out[BOARD_FEATURES]) {
    build_features(b, c, v, out);
    const Topology& t = topology();
    const State& s = v.s;
    int at = BUILD_FEATURES;
    auto put = [&](double x) { out[at++] = uint8_t(std::min(int(x * 100 + 0.5), 255)); };
    const Nodes open = ~blocked_nodes(s) & ((Nodes(1) << NODES) - 1);
    double supply[5] = {};
    for (int tile = 0; tile < TILES; tile++)
        if (b.resources[tile] >= 0) supply[b.resources[tile]] += DICE_DECK[b.numbers[tile] - 2] / 36.0;
    for (double x : supply) put(x * 2);  // how plentiful each resource is on this board
    int richest[4] = {}, rich = 0;  // the best open spots anywhere
    for (Nodes m = open; m; m &= m - 1) {
        int pips = b.node_pips[__builtin_ctzll(m)];
        rich += pips >= 10;
        for (int& held : richest)
            if (pips > held) std::swap(pips, held);
    }
    for (int pips : richest) put(pips / 15.0);
    put(std::min(rich, 6) / 6.0);
    double income[2][5] = {};
    for (int tile = 0; tile < TILES; tile++)
        if (b.resources[tile] >= 0 && tile != s.robber)
            for (int k = 0; k < 2; k++) {
                const Player& p = s.p[k ? 1 - v.me : v.me];
                income[k][b.resources[tile]] += (pop(p.settlements & t.tile_mask[tile]) + 2 * pop(p.cities & t.tile_mask[tile])) * DICE_DECK[b.numbers[tile] - 2] / 36.0;
            }
    int cards = 0;
    for (uint8_t count : s.dice.deck) cards += count;
    // Roads each player needs to reach every node; 9 = more than five.
    uint8_t reach[2][NODES];
    for (int k = 0; k < 2; k++) {
        const Player &p = s.p[k ? 1 - v.me : v.me], &rival = s.p[k ? v.me : 1 - v.me];
        std::fill(reach[k], reach[k] + NODES, uint8_t(9));
        Nodes walls = rival.settlements | rival.cities, frontier = (p.settlements | p.cities | road_endpoints(p.roads)) & ~walls, seen = frontier;
        Edges taken = p.roads | rival.roads;
        for (int step = 0; step <= 5 && frontier; step++) {
            Nodes next = 0;
            for (Nodes m = frontier; m; m &= m - 1) {
                int node = __builtin_ctzll(m);
                reach[k][node] = uint8_t(step);
                for (int j = 0; j < t.node_edge_count[node]; j++) {
                    int e = t.node_edges[node][j];
                    if (!(taken >> e & 1)) next |= Nodes(1) << (t.edge_nodes[e][0] == node ? t.edge_nodes[e][1] : t.edge_nodes[e][0]);
                }
            }
            next &= ~seen, seen |= next, frontier = next & ~walls;
        }
    }
    for (int k = 0; k < 2; k++) {
        const Player &p = s.p[k ? 1 - v.me : v.me], &rival = s.p[k ? v.me : 1 - v.me];
        const uint8_t *mine = reach[k], *theirs = reach[1 - k];
        // Spots three and four roads away; spots this player reaches first and spots both reach equally
        // soon; and the nearest open spot on each kind of port.
        int far[2] = {}, far_top[2] = {}, secure = 0, secure_pips = 0, secure_top = 0, contested = 0, contested_top = 0, n = 0;
        int nearest[6] = {9, 9, 9, 9, 9, 9};
        struct Spot { int node, value; } spots[NODES];
        for (Nodes m = open; m; m &= m - 1) {
            int node = __builtin_ctzll(m), pips = b.node_pips[node], roads = mine[node];
            if (roads > 5) continue;
            if (roads == 3 || roads == 4) far[roads - 3]++, far_top[roads - 3] = std::max(far_top[roads - 3], pips);
            if (roads < theirs[node]) secure++, secure_pips += pips, secure_top = std::max(secure_top, pips);
            else if (roads == theirs[node]) contested++, contested_top = std::max(contested_top, pips);
            if (roads <= theirs[node] && roads <= 4) spots[n++] = {node, pips - 2 * roads};
            if (b.node_port[node] >= -1) nearest[b.node_port[node] + 1] = std::min(nearest[b.node_port[node] + 1], roads);
        }
        for (int i = 0; i < 2; i++) put(std::min(far[i], 6) / 6.0), put(far_top[i] / 15.0);
        put(std::min(secure, 8) / 8.0), put(std::min(secure_pips, 80) / 40.0), put(secure_top / 15.0);
        put(std::min(contested, 4) / 4.0), put(contested_top / 15.0);
        // A greedy plan to settle them: the best spot first, then the best that still fits.
        const double fade[5] = {1, 0.8, 0.64, 0.51, 0.41};
        Nodes chosen = 0;
        int roads = 0, built = 0, room = 5 - pop(p.settlements), spare = 15 - pop(p.roads), picks[2] = {-1, -1};
        double gain[5] = {};
        while (built < room) {
            int best = -1;
            for (int i = 0; i < n; i++)
                if (spots[i].node >= 0 && (best < 0 || spots[i].value > spots[best].value) && !(t.neighbors[spots[i].node] & chosen) &&
                    roads + mine[spots[i].node] <= spare) best = i;
            if (best < 0) break;
            int node = spots[best].node;
            if (built < 2) picks[built] = node;
            spots[best].node = -1, chosen |= Nodes(1) << node, roads += mine[node], built++;
            for (int j = 0; j < t.node_tile_count[node]; j++) {
                int tile = t.node_tiles[node][j];
                if (b.resources[tile] >= 0 && tile != s.robber) gain[b.resources[tile]] += fade[mine[node]] * DICE_DECK[b.numbers[tile] - 2] / 36.0;
            }
        }
        put(built / 4.0), put(roads / 8.0);
        for (double x : gain) put(x * 3);
        // The race for the first spot of the plan: how soon each player could pay for the roads and the settlement.
        if (int node = picks[0]; node >= 0) {
            auto funded = [&](int who, int steps) {
                uint8_t cost[5] = {uint8_t(1 + steps), uint8_t(1 + steps), 1, 1, 0};
                const Player& payer = who == k ? p : rival;
                return std::min(12, turns_to_fund(payer.hand, income[who], payer.ratio, cost)) / 12.0;
            };
            bool raced = theirs[node] <= 5 && pop(rival.settlements) < 5;
            put(b.node_pips[node] / 15.0), put(mine[node] / 4.0), put(funded(k, mine[node]));
            put(raced ? (6 - theirs[node]) / 6.0 : 0), put(raced ? funded(1 - k, theirs[node]) : 1);
        } else put(0), put(0), put(1), put(0), put(1);
        put(picks[1] >= 0 ? b.node_pips[picks[1]] / 15.0 : 0), put(picks[1] >= 0 ? mine[picks[1]] / 4.0 : 0);
        // What the robber can take: the two richest tiles, and the share of income on tiles the rival also uses.
        double top = 0, second = 0, all = 0, shared = 0, exposed = 0, by_number[13] = {}, remaining[5] = {};
        const Nodes theirs_built = rival.settlements | rival.cities, built_on = p.settlements | p.cities;
        for (int tile = 0; tile < TILES; tile++) {
            if (!(built_on & t.tile_mask[tile]) || b.resources[tile] < 0) continue;
            int weight = pop(p.settlements & t.tile_mask[tile]) + 2 * pop(p.cities & t.tile_mask[tile]), number = b.numbers[tile];
            double yield = weight * DICE_DECK[number - 2] / 36.0;
            all += yield;
            if (theirs_built & t.tile_mask[tile]) shared += yield;
            else exposed = std::max(exposed, yield);
            if (yield > top) second = top, top = yield;
            else second = std::max(second, yield);
            if (tile == s.robber) continue;
            by_number[number] += weight;
            remaining[b.resources[tile]] += weight * (cards >= 13 ? double(s.dice.deck[number - 2]) / cards : DICE_DECK[number - 2] / 36.0);
        }
        put(top * 3), put(second * 3), put(all > 0 ? shared / all : 0), put(exposed * 3);
        for (int number = 2; number <= 12; number++)
            if (number != 7) put(std::min(by_number[number], 8.0) / 4.0);
        for (double x : remaining) put(x * 1.5);
        for (int kind = -1; kind < 5; kind++) {  // each port: held, or how near the closest open one is
            bool held = kind < 0 ? *std::max_element(p.ratio, p.ratio + 5) <= 3 : p.ratio[kind] == 2;
            put(held ? 1 : nearest[kind + 1] <= 4 ? (5 - nearest[kind + 1]) / 6.0 : 0);
        }
    }
    if (at != BOARD_FEATURES) fprintf(stderr, "board feature count is %d\n", at), abort();
}

void card_features(const Board& b, const Config& c, const View& v, uint8_t out[CARD_FEATURES]) {
    board_features(b, c, v, out);
    const Topology& t = topology();
    const State& s = v.s;
    int at = BOARD_FEATURES;
    auto put = [&](double x) { out[at++] = uint8_t(std::clamp(int(std::lround(x * 100)), 0, 255)); };
    const int unseen = sum5(s.deck);
    const Nodes blocked = blocked_nodes(s);
    for (int who : {int(v.me), 1 - v.me}) {
        const Player &p = s.p[who], &o = s.p[1 - who];
        // Cards held: the viewer's own exactly, the rival's as expected from the unseen pool.
        double held[5];
        for (int k = 0; k < 5; k++)
            held[k] = who == v.me ? p.dev[k] : unseen ? double(v.rival_cards) * s.deck[k] / unseen : 0;
        auto any = [&](int k) { return std::min(held[k], 1.0); };
        int haul = 0;
        for (int r = 0; r < 5; r++) haul = std::max<int>(haul, o.hand[r]);
        put(any(MONOPOLY_CARD) * haul / 6.0), put(any(MONOPOLY_CARD) * o.hand[ORE] / 6.0), put(any(MONOPOLY_CARD) * o.hand[GRAIN] / 6.0);
        for (int kind : {2, 1, 3}) {  // a city, a settlement, a card: two more cards complete it
            int shortfall = 0;
            for (int r = 0; r < 5; r++) shortfall += std::max(0, COSTS[kind][r] - p.hand[r]);
            put(shortfall >= 1 && shortfall <= 2 ? any(YEAR_OF_PLENTY_CARD) : 0);
        }
        // Road Building: the best open spot two roads away, and whether it would take Longest Road.
        const Nodes walls = o.settlements | o.cities;
        Nodes frontier = (p.settlements | p.cities | road_endpoints(p.roads)) & ~walls, seen = frontier;
        const Edges taken = p.roads | o.roads;
        int top = 0;
        for (int step = 1; step <= 2; step++) {
            Nodes next = 0;
            for (Nodes m = frontier; m; m &= m - 1) {
                int node = __builtin_ctzll(m);
                for (int k = 0; k < t.node_edge_count[node]; k++) {
                    int e = t.node_edges[node][k];
                    if (!(taken >> e & 1)) next |= Nodes(1) << (t.edge_nodes[e][0] == node ? t.edge_nodes[e][1] : t.edge_nodes[e][0]);
                }
            }
            next &= ~seen, seen |= next, frontier = next & ~walls;
            for (Nodes m = next & ~blocked; m; m &= m - 1) top = std::max<int>(top, b.node_pips[__builtin_ctzll(m)]);
        }
        put(pop(p.settlements) < 5 ? any(ROAD_BUILDING_CARD) * top / 15.0 : 0);
        bool road = s.longest_road != who && s.road_len[who] + 2 >= 5 && s.road_len[who] + 2 > s.road_len[1 - who];
        put(road ? any(ROAD_BUILDING_CARD) : 0);
        // The army race with the knights held, and whether one more knight takes Largest Army.
        double army = p.played[KNIGHT] + held[KNIGHT] - o.played[KNIGHT];
        put((std::clamp(army, -3.0, 3.0) + 3) / 6);
        bool takes = s.largest_army != who && p.played[KNIGHT] + 1 >= 3 && p.played[KNIGHT] + 1 > o.played[KNIGHT];
        put(takes ? any(KNIGHT) : 0);
        put(held[VICTORY_POINT] / 3);
    }
    if (at != CARD_FEATURES) fprintf(stderr, "card feature count is %d\n", at), abort();
}

bool Net::load(const std::string& path) {
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) return false;
    uint32_t header[2];
    bool ok = fread(header, 4, 2, file) == 2 && header[0] == 0x4E544143 && header[1] >= 1 && header[1] <= 8;
    std::vector<uint32_t> widths(ok ? header[1] + 1 : 0);
    ok = ok && fread(widths.data(), 4, widths.size(), file) == widths.size() &&
         (widths[0] == FEATURES || widths[0] == BUILD_FEATURES || widths[0] == BOARD_FEATURES || widths[0] == CARD_FEATURES) &&
         widths.back() == 1;
    sizes.assign(widths.begin(), widths.end());
    weights.clear(), biases.clear();
    for (size_t layer = 0; ok && layer + 1 < sizes.size(); layer++) {
        int in = sizes[layer], outs = sizes[layer + 1];
        std::vector<float> rows(size_t(in) * outs), columns(size_t(in) * outs), bias(outs);
        ok = outs <= MAX_WIDTH && fread(rows.data(), 4, rows.size(), file) == rows.size() && fread(bias.data(), 4, outs, file) == size_t(outs);
        for (int o = 0; o < outs; o++)  // stored [output][input]; kept [input][output] so a zero input skips a whole row
            for (int i = 0; i < in; i++) columns[size_t(i) * outs + o] = rows[size_t(o) * in + i];
        weights.push_back(std::move(columns)), biases.push_back(std::move(bias));
    }
    fclose(file);
    if (ok) {  // the largest power-of-two scale at which no first-layer sum can overflow
        int width = sizes[1];
        double bound = 1;
        for (int o = 0; o < width; o++) {
            double reach = std::fabs(biases[0][o]) * 100;
            for (int i = 0; i < sizes[0]; i++) reach += 255 * std::fabs(weights[0][size_t(i) * width + o]);
            bound = std::max(bound, reach);
        }
        double scale = std::exp2(std::floor(std::log2(double(1 << 30) / bound)));
        fixed.resize(weights[0].size()), fixed_bias.resize(width);
        for (size_t k = 0; k < fixed.size(); k++) fixed[k] = int32_t(std::lround(weights[0][k] * scale));
        for (int o = 0; o < width; o++) fixed_bias[o] = int32_t(std::lround(biases[0][o] * 100 * scale));
        compact_fixed.assign(size_t((sizes[0] + 1) / 2) * 2 * width, 0);
        fixed_corrections.clear();
        correction_offsets.clear();
        for (int i = 0; i < sizes[0]; i++) {
            correction_offsets.push_back(fixed_corrections.size());
            for (int o = 0; o < width; o++) {
                const size_t at = size_t(i) * width + o;
                const int32_t small = std::clamp(fixed[at], int32_t(-32768), int32_t(32767));
#if CATAN_AVX2_FIXED_UPDATE
                compact_fixed[(size_t(i / 2) * width + o) * 2 + i % 2] = int16_t(small);
#else
                compact_fixed[at] = int16_t(small);
#endif
                if (small != fixed[at]) fixed_corrections.push_back({o, fixed[at] - small});
            }
        }
        correction_offsets.push_back(fixed_corrections.size());
        if (fixed_corrections.size() > fixed.size() / 32) compact_fixed.clear();  // dense exceptions use the full-width kernel
        unit = float(1 / (100 * scale));
    }
    return ok;
}

float Net::eval(const uint8_t* x) const {
    float a[MAX_WIDTH];
    int width = sizes[1];
    std::copy(biases[0].begin(), biases[0].end(), a);
    for (int i = 0; i < sizes[0]; i++) {
        if (!x[i]) continue;
        const float scale = x[i] * 0.01f, *w = &weights[0][size_t(i) * width];
        for (int o = 0; o < width; o++) a[o] += scale * w[o];
    }
    return finish(a);
}

float Net::eval(const uint8_t* x, Sums& kept) const {
    int width = sizes[1];
    if (!kept.primed) {
        std::copy(fixed_bias.begin(), fixed_bias.end(), kept.sum);
        memset(kept.x, 0, sizes[0]);
        kept.primed = true;
    }
#if CATAN_AVX2_FIXED_UPDATE
    static const bool use_avx2 = __builtin_cpu_supports("avx2");
    if (use_avx2) {
        if (!compact_fixed.empty()) update_compact(*this, x, kept);
        else update_fixed_avx2(fixed.data(), sizes[0], width, x, kept);
    } else
#elif defined(__aarch64__)
    if (!compact_fixed.empty()) update_compact(*this, x, kept);
    else
#endif
    for (int i = 0; i < sizes[0]; i++) {
        if (x[i] == kept.x[i]) continue;
        const int32_t change = x[i] - kept.x[i], *w = &fixed[size_t(i) * width];
        for (int o = 0; o < width; o++) kept.sum[o] += change * w[o];
        kept.x[i] = x[i];
    }
    float a[MAX_WIDTH];
    for (int o = 0; o < width; o++) a[o] = kept.sum[o] * unit;
    return finish(a);
}

// Every layer after the first.
float Net::finish(float* a) const {
    float z[MAX_WIDTH];
    int width = sizes[1];
#if CATAN_AVX2_DENSE_UPDATE
    static const bool use_avx2 = __builtin_cpu_supports("avx2");
#endif
    for (size_t layer = 1; layer + 1 < sizes.size(); layer++) {
        int outs = sizes[layer + 1];
        std::copy(biases[layer].begin(), biases[layer].end(), z);
#if CATAN_AVX2_DENSE_UPDATE
        if (use_avx2 && outs >= 8) update_dense_avx2(a, weights[layer].data(), width, outs, z);
        else
#endif
        for (int i = 0; i < width; i++) {
            if (a[i] <= 0) continue;  // ReLU
            const float scale = a[i], *w = &weights[layer][size_t(i) * outs];
            for (int o = 0; o < outs; o++) z[o] += scale * w[o];
        }
        std::copy(z, z + outs, a);
        width = outs;
    }
    return 1.0f / (1.0f + std::exp(-a[0]));
}

int choose(const Agent& agent, const Board& b, const Config& c, const View& v, Rng& rng, float* value, std::vector<Option>* options) {
    return choose_in(nullptr, agent, b, c, v, rng, value, options);
}

int choose_traced(const Agent& agent, const Board& b, const Config& c, const View& v, Rng& rng, float* value,
                  std::vector<Option>* options, Trace& trace) {
    return choose_in(nullptr, agent, b, c, v, rng, value, options, &trace);
}

int choose_in(Search* reuse, const Agent& agent, const Board& b, const Config& c, const View& v, Rng& rng, float* value, std::vector<Option>* options,
              Trace* trace) {
    if (agent.staged) {
        bool opening = v.s.phase == PLACE_SETTLEMENT || v.s.phase == PLACE_ROAD;
        Agent selected = agent;
        selected.staged = false;
        int total = 0;
        while (total < 5 && agent.extra[total]) total++;
        const int mates = std::clamp(agent.midgame_mates, 0, total - 1);  // agent.extra[total - mates ...]: midgame mates
        const int openers = total - mates;                                 // agent.extra[0 ... openers): opening evaluators
        if (opening) {
            selected.net = agent.extra[0];
            selected.turn_beam = 0;  // preserve the opening specialist's original search and value range
            if (agent.opening_leaf) {  // with opening_blend=2 the other evaluators are opening ones
                selected.leaf[0] = agent.net;
                for (int m = 0; m < mates; m++) selected.leaf[1 + m] = agent.extra[total - mates + m];
                if (agent.opening_blend < 2 && !mates) selected.leaf[1] = agent.extra[1], selected.leaf[2] = agent.extra[2];
            }
        }
        // The last evaluator opens; any others join the first for the rest of the game.
        const bool others_open = agent.opening_blend >= 2;
        for (const Net*& slot : selected.extra) slot = nullptr;
        int filled = 0;
        if (opening) {
            if (agent.opening_blend) {
                selected.extra[filled++] = agent.net;
                for (int m = 0; m < mates; m++) selected.extra[filled++] = agent.extra[total - mates + m];
            }
            if (others_open) for (int k = 1; k < openers; k++) selected.extra[filled++] = agent.extra[k];
        } else {
            for (int m = 0; m < mates; m++) selected.extra[filled++] = agent.extra[total - mates + m];
            if (!others_open) for (int k = 1; k < openers; k++) selected.extra[filled++] = agent.extra[k];
        }
        selected.full_setup = opening;
        selected.memoize = true;
        return choose_in(nullptr, selected, b, c, v, rng, value, options, trace);
    }
    uint8_t actions[ACTIONS];
    int n = legal_actions(b, v.s, c, actions);
    if (!v.deck_size) n = int(std::remove(actions, actions + n, uint8_t(BUY_DEV)) - actions);
    if (value) *value = -1;
    if (n == 1) return actions[0];
    if (!agent.net || (agent.epsilon > 0 && rng.below(1000000) < agent.epsilon * 1000000)) return actions[rng.below(n)];
    std::unique_ptr<Search> own;
    if (reuse) reuse->base = v;
    else own.reset(new Search{b, c, *agent.net, v, agent});
    Search& search = reuse ? *reuse : *own;
    if (own) own->trace = trace;
    Search::ViewKey choice_key;
    uint64_t choice_hash = 0;
    size_t choice_slot = 0;
    const bool cache_choice = reuse && !value && !options && agent.epsilon == 0;
    if (cache_choice) {
        choice_key = Search::view_key(v);
        choice_hash = Search::feature_hash(choice_key.data(), int(choice_key.size()));
        choice_slot = choice_hash & (Search::ChoiceCache::SLOTS - 1);
        if (!search.choice_cache) search.choice_cache.reset(new Search::ChoiceCache);
        const auto& cached = *search.choice_cache;
        if (choice_hash && cached.hashes[choice_slot] == choice_hash && cached.keys[choice_slot] == choice_key)
            return cached.actions[choice_slot][rng.below(cached.counts[choice_slot])];
    }
    const bool normalize_pre_roll = agent.pre_roll_normalize && agent.turn_beam > 0;
    if (normalize_pre_roll && v.s.pre_roll && (v.s.phase == MOVE_ROBBER || v.s.phase == ROAD_BUILDING)) {
        int action = search.pre_roll_action(v.s, v.deck_size, value);
        rng.below(1);  // retain the ordinary choice draw while using the forecast's stable tie rule
        return action;
    }
    if (agent.discard_sets && v.s.phase == DISCARD && v.s.cur == v.me) {
        // Every way to give up the cards still owed, each valued once all of them are gone.
        const uint8_t* hand = v.s.p[v.me].hand;
        int owed = v.s.discards[0].remaining, set[5] = {}, chosen = -1;
        float top = -2;
        std::function<void(int, int)> visit = [&](int r, int left) {
            if (r == 4) {
                if (left > hand[4]) return;
                set[4] = left;
                State gone = v.s;
                int first = -1;
                for (int k = 0; k < 5; k++)
                    for (int j = 0; j < set[k]; j++) {
                        if (first < 0) first = k;
                        apply_action(b, gone, c, DISCARD_ONE + k);
                    }
                float score = search.after(gone, v.deck_size, 1);
                if (score > top) top = score, chosen = DISCARD_ONE + first;
                return;
            }
            for (int take = 0; take <= std::min<int>(hand[r], left); take++) set[r] = take, visit(r + 1, left - take);
        };
        visit(0, owed);
        if (chosen >= 0) {
            if (value) *value = top;
            return chosen;
        }
    }
    float values[ACTIONS], best = -1;
    bool opening = agent.setup > 0 && (v.s.phase == PLACE_SETTLEMENT || v.s.phase == PLACE_ROAD);
    float cut = -1;
    if (opening && agent.full_setup && v.s.phase == PLACE_SETTLEMENT) {
        // Search every current settlement, promising ones first for pruning.
        // Descendant placements retain the original narrow beam.
        std::pair<float, int> ordered[ACTIONS];
        for (int i = 0; i < n; i++) {
            State next = v.s;
            apply_action(b, next, c, actions[i]);
            ordered[i] = {-search.score_of(next, v.deck_size), actions[i]};
        }
        std::sort(ordered, ordered + n);
        for (int i = 0; i < n; i++) actions[i] = uint8_t(ordered[i].second), values[i] = -ordered[i].first;
    } else if (opening && n > agent.setup) {  // only the most promising placements get the full opening search
        for (int i = 0; i < n; i++) {
            State next = v.s;
            apply_action(b, next, c, actions[i]);
            values[i] = search.score_of(next, v.deck_size);
        }
        std::vector<float> sorted(values, values + n);
        std::nth_element(sorted.begin(), sorted.begin() + agent.setup - 1, sorted.end(), std::greater<float>());
        cut = sorted[agent.setup - 1];
    }
    if (agent.turn_beam > 0 && v.s.phase == MAIN) {
        search.complete_turn_values(actions, n, values, options);
        best = *std::max_element(values, values + n);
    } else {
        for (int i = 0; i < n; i++) {
            State next = v.s;
            apply_action(b, next, c, actions[i]);
            if (opening) {
                // A quick agent also stops searching a placement once it cannot come within `tie` of the best so far.
                float low = agent.quick ? best - float(agent.tie) : -2;
                values[i] = values[i] >= cut || cut < 0 ? search.opening(next, v.deck_size, low) : -1;
                if (agent.quick && values[i] <= low) values[i] = -1;
            } else if (normalize_pre_roll && v.s.phase == PRE_ROLL) values[i] = search.finish_pre_roll(next, v.deck_size);
            else values[i] = search.after(next, v.deck_size, (agent.turn_beam > 0 ? (agent.mandatory_depth > 0 ? agent.mandatory_depth : 2) : agent.depth) - 1);
            best = std::max(best, values[i]);
        }
    }
    if (agent.robber_turn > 0 && agent.turn_beam > 0 && v.s.phase == MOVE_ROBBER && !v.s.pre_roll && v.s.turn_player == v.me && n > 1) {
        // Plan the rest of the turn after each theft, without the rival rollouts, for the most promising spots.
        Agent planner = agent;
        planner.rollout_samples = planner.turn_reply = planner.robber_turn = 0;
        std::vector<std::pair<float, int>> ranked;
        for (int i = 0; i < n; i++) ranked.push_back({-values[i], i});
        std::sort(ranked.begin(), ranked.end());
        float rechecked[ACTIONS];
        std::fill(rechecked, rechecked + n, -1.0f);
        auto planned = [&](const State& s) {
            if (s.winner >= 0 || done(s, c) || s.phase != MAIN || s.cur != v.me) return search.leaf(s, v.deck_size);
            View w = v;
            w.s = s;
            Search inner{b, c, *agent.net, w, planner};
            uint8_t roots[ACTIONS];
            int count = inner.legal(s, v.deck_size, roots);
            float plans[ACTIONS];
            inner.complete_turn_values(roots, count, plans);
            return *std::max_element(plans, plans + count);
        };
        best = -1;
        for (int k = 0; k < std::min<int>(agent.robber_turn, n); k++) {
            int i = ranked[k].second;
            State next = v.s;
            apply_action(b, next, c, actions[i]);
            double total = 0, sum = 0;
            if (is_chance(next)) {
                uint32_t weights[11];
                int slots = chance_weights(next, c, weights);
                for (int o = 0; o < slots; o++) if (weights[o]) {
                    State stolen = next;
                    apply_chance(b, stolen, c, o);
                    sum += weights[o] * planned(stolen), total += weights[o];
                }
            } else sum = planned(next), total = 1;
            rechecked[i] = float(sum / total);
            best = std::max(best, rechecked[i]);
        }
        std::copy(rechecked, rechecked + n, values);
    }
    if (value) *value = best;
    if (trace)
        for (int i = 0; i < n; i++) trace->scored.push_back({actions[i], values[i]});
    int close = 0;
    for (int i = 0; i < n; i++)
        if (values[i] >= best - agent.tie) actions[close++] = actions[i];
    if (cache_choice) {
        // Cache all tied choices, not the draw: hits consume the same RNG call as a fresh search.
        auto& cached = *search.choice_cache;
        cached.hashes[choice_slot] = choice_hash;
        cached.keys[choice_slot] = choice_key;
        std::copy(actions, actions + close, cached.actions[choice_slot]);
        cached.counts[choice_slot] = close;
    }
    return actions[rng.below(close)];
}

}  // namespace catan
