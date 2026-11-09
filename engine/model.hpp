#pragma once
#include "agent.hpp"
#include "graph.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <sstream>
namespace catan {
struct Contestant {
    std::string name;
    Net net, extra[5];
    GraphNet graph;
    Agent agent;
};
bool load(Contestant& who, std::string spec) {
    who.name = spec;
    if (spec == "random") return true;
    if (size_t at = spec.find('@'); at != std::string::npos) {
        int quick = 0, richer = 0, staged = 0, value_only = 0, turn_roll = 0, retention_belief = 0, reserved[4] = {};
        int pre_roll_normalize = 0, wide = 0, rollout_plan = 0, opening_blend = 0, discard_sets = 0;
        int parsed = 0;
        sscanf(spec.c_str() + at + 1, "%f,%f,%d,%f,%d,%f,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%f,%d,%d,%d,%d,%d,%d,%d,%d,%d%n", &who.agent.plan, &who.agent.patience, &who.agent.reach, &who.agent.threat, &who.agent.setup, &who.agent.economy, &quick, &richer, &staged, &who.agent.turn_beam, &value_only, &turn_roll, &who.agent.turn_reply, &reserved[0], &reserved[1], &pre_roll_normalize, &who.agent.rollout_samples, &reserved[3], &retention_belief, &wide, &who.agent.cards, &who.agent.finalists, &who.agent.rollout_turns, &rollout_plan, &who.agent.rollout_beam, &who.agent.rollout_depth, &opening_blend, &who.agent.opening_leaf, &who.agent.robber_turn, &discard_sets, &parsed);
        who.agent.opening_symmetry = false;
        who.agent.rollout_normalize = false;
        who.agent.rollout_control = false;
        // Keep the original partial-field parsing; validate supplied fields 31 through 33.
        if (parsed && spec[at + 1 + parsed] == ',') {
            const size_t start = at + 2 + parsed, end = spec.find(',', start);
            const std::string field = spec.substr(start, end == std::string::npos ? end : end - start);
            if (!field.empty() && field != "0" && field != "1")
                return fprintf(stderr, "opening_symmetry is 0 or 1\n"), false;
            who.agent.opening_symmetry = field == "1";
            if (end != std::string::npos) {
                const size_t next = end + 1, last = spec.find(',', next);
                const std::string normalized = spec.substr(next, last == std::string::npos ? last : last - next);
                if (!normalized.empty() && normalized != "0" && normalized != "1")
                    return fprintf(stderr, "rollout_normalize is 0 or 1\n"), false;
                who.agent.rollout_normalize = normalized == "1";
                if (last != std::string::npos) {
                    const size_t control_start = last + 1, control_end = spec.find(',', control_start);
                    const std::string controlled = spec.substr(control_start,
                        control_end == std::string::npos ? control_end : control_end - control_start);
                    if (!controlled.empty() && controlled != "0" && controlled != "1")
                        return fprintf(stderr, "rollout_control is 0 or 1\n"), false;
                    who.agent.rollout_control = controlled == "1";
                }
            }
        }
        who.agent.rollout_plan = rollout_plan;
        who.agent.opening_blend = opening_blend;
        if (opening_blend < 0 || opening_blend > 2) return fprintf(stderr, "opening_blend is 0, 1 or 2\n"), false;
        if (reserved[0] || reserved[1] || reserved[3])
            return fprintf(stderr, "spec fields 14-15 and 18 are reserved on this branch; use zero\n"), false;
        who.agent.quick = quick;
        who.agent.richer = richer;
        who.agent.staged = staged;
        who.agent.turn_value_only = value_only;
        who.agent.turn_roll = turn_roll;
        who.agent.pre_roll_normalize = pre_roll_normalize;
        who.agent.retention_belief = retention_belief;
        who.agent.wide = wide;
        who.agent.discard_sets = discard_sets;
        {  // field 34 and later, read by position
            std::vector<std::string> fields;
            std::istringstream list(spec.substr(at + 1));
            for (std::string field; std::getline(list, field, ',');) fields.push_back(field);
            who.agent.card_inputs = fields.size() >= 34 && fields[33] == "1";
            who.agent.awards = fields.size() >= 35 ? float(atof(fields[34].c_str())) : 0;
            if (fields.size() >= 34 && fields[33] != "0" && fields[33] != "1") return fprintf(stderr, "card_inputs is 0 or 1\n"), false;
            who.agent.midgame_mates = fields.size() >= 36 ? atoi(fields[35].c_str()) : 0;
            who.agent.rollout_rounds = fields.size() >= 37 ? atoi(fields[36].c_str()) : 0;
            if (who.agent.midgame_mates < 0 || who.agent.midgame_mates > 3) return fprintf(stderr, "midgame_mates is 0 to 3\n"), false;
            if (who.agent.rollout_rounds < 0 || who.agent.rollout_rounds > 8) return fprintf(stderr, "rollout_rounds is 0 to 8\n"), false;
            who.agent.workers = fields.size() >= 38 ? atoi(fields[37].c_str()) : 0;
            if (who.agent.workers < 0 || who.agent.workers > 8) return fprintf(stderr, "workers is 0 to 8\n"), false;
            if (fields.size() >= 39 && fields[38] != "0" && fields[38] != "1" && fields[38] != "2") return fprintf(stderr, "graph_leaf is 0, 1 or 2\n"), false;
            who.agent.graph_leaf = fields.size() >= 39 ? atoi(fields[38].c_str()) : 0;
        }
        spec.resize(at);
    }
    size_t colon = spec.rfind(':');
    bool has_depth = colon != std::string::npos && colon + 1 < spec.size() && isdigit(spec[colon + 1]);
    who.agent.depth = has_depth ? atoi(spec.c_str() + colon + 1) : 1;
    if (has_depth) spec.resize(colon);
    if (size_t plus = spec.rfind('+'); plus != std::string::npos && who.graph.load(spec.substr(plus + 1))) {
        who.agent.graph = &who.graph;
        spec.resize(plus);
    }
    if (who.agent.graph_leaf && (!who.agent.graph || who.agent.rollout_samples <= 0))
        return fprintf(stderr, "graph_leaf needs a board-graph evaluator listed last and sampled rollouts\n"), false;
    for (int k = 0; k < 5; k++) {  // several evaluators, averaged
        size_t plus = spec.rfind('+');
        if (plus == std::string::npos) break;
        if (!who.extra[k].load(spec.substr(plus + 1))) return fprintf(stderr, "cannot load %s\n", spec.c_str()), false;
        who.agent.extra[k] = &who.extra[k];
        spec.resize(plus);
    }
    if (!who.net.load(spec)) return fprintf(stderr, "cannot load %s\n", spec.c_str()), false;
    who.agent.net = &who.net;
    int inputs = who.net.sizes[0];
    for (const Net* other : who.agent.extra) if (other) inputs = std::max(inputs, other->sizes[0]);
    if (who.agent.wide && !who.agent.richer) return fprintf(stderr, "wide needs richer\n"), false;
    if (who.agent.card_inputs && !who.agent.wide) return fprintf(stderr, "card_inputs needs wide\n"), false;
    if (inputs != (who.agent.card_inputs ? CARD_FEATURES : who.agent.wide ? BOARD_FEATURES : who.agent.richer ? BUILD_FEATURES : FEATURES))
        return fprintf(stderr, "feature setting does not match %s\n", who.name.c_str()), false;
    // A wide agent may mix evaluators: each reads the leading features it was trained on.
    if (who.agent.staged && (!who.agent.richer || !who.agent.extra[0] || who.net.sizes[0] < BUILD_FEATURES || who.agent.extra[0]->sizes[0] < BUILD_FEATURES))
        return fprintf(stderr, "staged play needs rich evaluators: MIDGAME[+MIDGAME...]+OPENING\n"), false;
    return true;
}

}
