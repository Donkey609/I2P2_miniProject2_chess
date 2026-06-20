#include <utility>
#include <algorithm>
#include <vector>
#include <cstdint>
#include <cstddef>

#include "state.hpp"
#include "pvs.hpp"

namespace {

/////////////////////////Searchl constnat

static constexpr int MAX_PLY = 128;

// 1M TT entries.
// Big enough to matter, small enough to not summon the memory police.
static constexpr std::size_t TT_SIZE = 1u << 20;
static constexpr std::size_t TT_MASK = TT_SIZE - 1;

// Move-ordering values, not evaluation values.
static const int order_piece_value[7] = {
    0,      // empty
    100,    // pawn
    500,    // rook
    320,    // knight
    330,    // bishop
    900,    // queen
    20000   // king
};

// Evaluation-scale values for quiescence delta pruning.
// This matches your KP material scale better.
static const int eval_piece_value[7] = {
    0, 20, 60, 70, 80, 200, 1000
};

///////////////////////////////////////////////Transposition Table

enum TTFlag : std::uint8_t {
    TT_EXACT = 0,
    TT_LOWER = 1,
    TT_UPPER = 2
};

struct TTEntry {
    std::uint64_t key = 0;
    int depth = -1;
    int score = 0;
    TTFlag flag = TT_EXACT;
    bool valid = false;
    bool has_best = false;
    Move best_move{};
};

static std::vector<TTEntry> tt_table(TT_SIZE);

/////////////////////////////////////////////////////////////Killer Moves + History Heuristic

static Move killer_moves[MAX_PLY][2];
static bool killer_valid[MAX_PLY][2] = {};

static int history_heuristic[2][BOARD_H][BOARD_W][BOARD_H][BOARD_W] = {};

///////////////////////////////////////////////////////////////Tiny Helpers
inline void update_seldepth(SearchContext& ctx, int ply) {
    if (ply > ctx.seldepth) {
        ctx.seldepth = ply;
    }
}

inline void ensure_legal_actions(State* state) {
    if (state->legal_actions.empty() && state->game_state == UNKNOWN) {
        state->get_legal_actions();
    }
}

inline bool same_move(const Move& a, const Move& b) {
    return a.first.first == b.first.first &&
           a.first.second == b.first.second &&
           a.second.first == b.second.first &&
           a.second.second == b.second.second;
}

inline int own_piece_at(State* state, const Move& action) {
    return state->piece_at(
        state->player,
        action.first.first,
        action.first.second
    );
}

inline int captured_piece_at(State* state, const Move& action) {
    return state->piece_at(
        1 - state->player,
        action.second.first,
        action.second.second
    );
}

inline bool is_capture(State* state, const Move& action) {
    return captured_piece_at(state, action) != 0;
}

inline bool is_promotion(State* state, const Move& action) {
    int piece = own_piece_at(state, action);

    if (piece != 1) {
        return false;
    }

    int to_r = action.second.first;

    return to_r == 0 || to_r == BOARD_H - 1;
}

inline int clamp_ply(int ply) {
    if (ply < 0) {
        return 0;
    }

    if (ply >= MAX_PLY) {
        return MAX_PLY - 1;
    }

    return ply;
}

inline bool is_quiet_move(State* state, const Move& action) {
    return !is_capture(state, action) && !is_promotion(state, action);
}

/////////////////////////////////////////////////////////////////// Mate Score TT Adjustment

inline int score_to_tt(int score, int ply) {
    if (score > P_MAX - 1024) {
        return score + ply;
    }

    if (score < M_MAX + 1024) {
        return score - ply;
    }

    return score;
}

inline int score_from_tt(int score, int ply) {
    if (score > P_MAX - 1024) {
        return score - ply;
    }

    if (score < M_MAX + 1024) {
        return score + ply;
    }

    return score;
}

inline TTEntry* tt_probe(std::uint64_t key) {
    TTEntry& entry = tt_table[key & TT_MASK];

    if (!entry.valid || entry.key != key) {
        return nullptr;
    }

    return &entry;
}

inline void tt_store(
    std::uint64_t key,
    int depth,
    int score,
    TTFlag flag,
    int ply,
    const Move& best_move,
    bool has_best
) {
    TTEntry& entry = tt_table[key & TT_MASK];

    // Replace if empty, same key, or deeper/equal search.
    if (!entry.valid || entry.key == key || depth >= entry.depth) {
        entry.valid = true;
        entry.key = key;
        entry.depth = depth;
        entry.score = score_to_tt(score, ply);
        entry.flag = flag;
        entry.best_move = best_move;
        entry.has_best = has_best;
    }
}

//////////////////////////////////////////////////////////////////////////Killer + History Update

inline void add_killer(const Move& action, int ply) {
    int p = clamp_ply(ply);

    if (!killer_valid[p][0] || !same_move(action, killer_moves[p][0])) {
        killer_moves[p][1] = killer_moves[p][0];
        killer_valid[p][1] = killer_valid[p][0];

        killer_moves[p][0] = action;
        killer_valid[p][0] = true;
    }
}

inline void add_history(State* state, const Move& action, int depth) {
    int side = state->player;

    int fr = action.first.first;
    int fc = action.first.second;
    int tr = action.second.first;
    int tc = action.second.second;

    int bonus = depth * depth;

    int& h = history_heuristic[side][fr][fc][tr][tc];

    h += bonus;

    // Avoid overflow. Humanity already has enough of that.
    if (h > 100000000) {
        h /= 2;
    }
}

inline int get_history_score(State* state, const Move& action) {
    int side = state->player;

    return history_heuristic[side]
        [action.first.first]
        [action.first.second]
        [action.second.first]
        [action.second.second];
}

/////////////////////////////////////////////////////////////Move Ordering

int move_order_score(
    State* state,
    const Move& action,
    const Move* tt_best,
    bool qsearch,
    int ply
) {
    int score = 0;

    if (!qsearch && tt_best && same_move(action, *tt_best)) {
        score += 100000000;
    }

    int captured = captured_piece_at(state, action);

    if (captured) {
        int attacker = own_piece_at(state, action);

        // MVV-LVA:
        // Most Valuable Victim - Least Valuable Attacker.
        score += 50000000;
        score += order_piece_value[captured] * 32;
        score -= order_piece_value[attacker];

        if (captured == 6) {
            score += 200000000;
        }

        return score;
    }

    if (qsearch) {
        return 0;
    }

    if (is_promotion(state, action)) {
        score += 20000000;
    }

    int p = clamp_ply(ply);

    if (killer_valid[p][0] && same_move(action, killer_moves[p][0])) {
        score += 10000000;
    } else if (killer_valid[p][1] && same_move(action, killer_moves[p][1])) {
        score += 9000000;
    }

    score += get_history_score(state, action);

    return score;
}

void order_actions(
    State* state,
    const Move* tt_best = nullptr,
    bool qsearch = false,
    int ply = 0
) {
    auto& actions = state->legal_actions;

    if (actions.size() < 2) {
        return;
    }

    std::stable_sort(
        actions.begin(),
        actions.end(),
        [state, tt_best, qsearch, ply](const Move& a, const Move& b) {
            return move_order_score(state, a, tt_best, qsearch, ply)
                 > move_order_score(state, b, tt_best, qsearch, ply);
        }
    );
}

/////////////////////////////////////////////////////////////////////// Terminal / Repetition

inline bool terminal_or_repetition_score(
    State* state,
    GameHistory& history,
    int ply,
    int& score
) {
    ensure_legal_actions(state);

    if (state->game_state == WIN) {
        score = P_MAX - ply;
        return true;
    }

    if (state->game_state == DRAW) {
        score = 0;
        return true;
    }

    int rep_score;
    if (state->check_repetition(history, rep_score)) {
        score = rep_score;
        return true;
    }

    return false;
}

///////////////////////////////////////////Late Move Reduction

inline int lmr_reduction(
    int depth,
    int move_number,
    bool quiet,
    bool first_child
) {
    if (first_child) {
        return 0;
    }

    if (!quiet) {
        return 0;
    }

    if (depth < 3) {
        return 0;
    }

    if (move_number < 3) {
        return 0;
    }

    if (depth >= 6 && move_number >= 6) {
        return 2;
    }

    return 1;
}

///////////////////////////////////////////////////////////Quiescence Search

int quiescence(
    State* state,
    GameHistory& history,
    int ply,
    SearchContext& ctx,
    const MMParams& p,
    int alpha,
    int beta
) {
    ctx.nodes++;
    update_seldepth(ctx, ply);

    if (ctx.stop) {
        return alpha;
    }

    int terminal_score;
    if (terminal_or_repetition_score(state, history, ply, terminal_score)) {
        return terminal_score;
    }

    history.push(state->hash());

    const int stand_pat = state->evaluate(
        p.use_kp_eval,
        p.use_eval_mobility,
        &history
    );

    if (stand_pat >= beta) {
        history.pop(state->hash());
        return stand_pat;
    }

    if (stand_pat > alpha) {
        alpha = stand_pat;
    }

    order_actions(state, nullptr, true, ply);

    for (const auto& action : state->legal_actions) {
        int captured = captured_piece_at(state, action);

        if (!captured) {
            break;
        }

        if (
            p.use_kp_eval &&
            captured != 6 &&
            stand_pat + eval_piece_value[captured] + 40 <= alpha
        ) {
            continue;
        }

        State* next = state->next_state(action);
        const bool same = next->same_player_as_parent();

        int score;

        if (same) {
            score = quiescence(next, history, ply + 1, ctx, p, alpha, beta);
        } else {
            score = -quiescence(next, history, ply + 1, ctx, p, -beta, -alpha);
        }

        delete next;

        if (ctx.stop) {
            break;
        }

        if (score >= beta) {
            history.pop(state->hash());
            return score;
        }

        if (score > alpha) {
            alpha = score;
        }
    }

    history.pop(state->hash());
    return alpha;
}

} // namespace

///////////////////////////////////////PVS — eval_ctx
int PVS::eval_ctx(
    State* state,
    int depth,
    GameHistory& history,
    int ply,
    SearchContext& ctx,
    const MMParams& p,
    int alpha,
    int beta
) {
    ctx.nodes++;
    update_seldepth(ctx, ply);

    if (ctx.stop) {
        return alpha;
    }

    int terminal_score;
    if (terminal_or_repetition_score(state, history, ply, terminal_score)) {
        return terminal_score;
    }

    if (depth <= 0) {
        return quiescence(state, history, ply, ctx, p, alpha, beta);
    }

    const std::uint64_t key = state->hash();
    const bool allow_tt = history.count(key) == 0;

    const int alpha_original = alpha;

    Move tt_best{};
    bool has_tt_best = false;

    if (allow_tt) {
        TTEntry* entry = tt_probe(key);

        if (entry) {
            if (entry->has_best) {
                tt_best = entry->best_move;
                has_tt_best = true;
            }

            if (entry->depth >= depth) {
                int tt_score = score_from_tt(entry->score, ply);

                if (entry->flag == TT_EXACT) {
                    return tt_score;
                }

                if (entry->flag == TT_LOWER && tt_score > alpha) {
                    alpha = tt_score;
                } else if (entry->flag == TT_UPPER && tt_score < beta) {
                    beta = tt_score;
                }

                if (alpha >= beta) {
                    return tt_score;
                }
            }
        }
    }

    history.push(key);

    order_actions(
        state,
        has_tt_best ? &tt_best : nullptr,
        false,
        ply
    );

    bool first_child = true;
    bool has_best = false;

    int best_score = M_MAX;
    Move best_move{};

    int move_number = 0;

    for (const auto& action : state->legal_actions) {
        move_number++;

        State* next = state->next_state(action);
        const bool same = next->same_player_as_parent();

        int score;
        int child_depth = depth - 1;

        bool quiet = is_quiet_move(state, action);

        int reduction = lmr_reduction(
            depth,
            move_number,
            quiet,
            first_child
        );

        int reduced_depth = child_depth - reduction;
        if (reduced_depth < 0) {
            reduced_depth = 0;
        }

        if (first_child) {
            // PV move gets full search.
            if (same) {
                score = eval_ctx(
                    next,
                    child_depth,
                    history,
                    ply + 1,
                    ctx,
                    p,
                    alpha,
                    beta
                );
            } else {
                score = -eval_ctx(
                    next,
                    child_depth,
                    history,
                    ply + 1,
                    ctx,
                    p,
                    -beta,
                    -alpha
                );
            }
        } else {
            // Later moves get null-window search first.
            if (same) {
                score = eval_ctx(
                    next,
                    reduced_depth,
                    history,
                    ply + 1,
                    ctx,
                    p,
                    alpha,
                    alpha + 1
                );
            } else {
                score = -eval_ctx(
                    next,
                    reduced_depth,
                    history,
                    ply + 1,
                    ctx,
                    p,
                    -alpha - 1,
                    -alpha
                );
            }

        
            if (reduction > 0 && score > alpha) {
                if (same) {
                    score = eval_ctx(
                        next,
                        child_depth,
                        history,
                        ply + 1,
                        ctx,
                        p,
                        alpha,
                        alpha + 1
                    );
                } else {
                    score = -eval_ctx(
                        next,
                        child_depth,
                        history,
                        ply + 1,
                        ctx,
                        p,
                        -alpha - 1,
                        -alpha
                    );
                }
            }

            // If it really looks good, do full-window re-search.
            if (score > alpha && score < beta) {
                if (same) {
                    score = eval_ctx(
                        next,
                        child_depth,
                        history,
                        ply + 1,
                        ctx,
                        p,
                        alpha,
                        beta
                    );
                } else {
                    score = -eval_ctx(
                        next,
                        child_depth,
                        history,
                        ply + 1,
                        ctx,
                        p,
                        -beta,
                        -alpha
                    );
                }
            }
        }

        delete next;

        if (ctx.stop) {
            break;
        }

        has_best = true;

        if (score > best_score) {
            best_score = score;
            best_move = action;
        }

        if (score > alpha) {
            alpha = score;
        }

        if (alpha >= beta) {
            if (quiet) {
                add_killer(action, ply);
                add_history(state, action, depth);
            }

            break;
        }

        first_child = false;
    }

    history.pop(key);

    int result = has_best ? best_score : alpha;

    if (!ctx.stop && allow_tt && has_best) {
        TTFlag flag = TT_EXACT;

        if (result <= alpha_original) {
            flag = TT_UPPER;
        } else if (result >= beta) {
            flag = TT_LOWER;
        }

        tt_store(
            key,
            depth,
            result,
            flag,
            ply,
            best_move,
            true
        );
    }

    return result;
}

////////////////////////////////////////////////////////////////////////PVS — search

SearchResult PVS::search(
    State* state,
    int depth,
    GameHistory& history,
    SearchContext& ctx
) {
    ctx.reset();

    MMParams p = MMParams::from_map(ctx.params);

    SearchResult result;
    result.depth = depth;

    ensure_legal_actions(state);

    if (state->legal_actions.empty()) {
        result.score = 0;
        result.nodes = ctx.nodes;
        result.seldepth = ctx.seldepth;
        return result;
    }

    const std::uint64_t root_key = state->hash();

    Move tt_best{};
    bool has_tt_best = false;

    if (TTEntry* entry = tt_probe(root_key)) {
        if (entry->has_best) {
            tt_best = entry->best_move;
            has_tt_best = true;
        }
    }

    order_actions(
        state,
        has_tt_best ? &tt_best : nullptr,
        false,
        0
    );

    int alpha = M_MAX;
    const int beta = P_MAX;

    int best_score = M_MAX;
    bool has_best = false;
    bool first_child = true;

    int move_index = 0;
    const int total_moves = static_cast<int>(state->legal_actions.size());

    result.best_move = state->legal_actions[0];

    for (const auto& action : state->legal_actions) {
        State* next = state->next_state(action);
        const bool same = next->same_player_as_parent();

        int score;
        int child_depth = depth - 1;

        if (first_child) {
            if (same) {
                score = eval_ctx(
                    next,
                    child_depth,
                    history,
                    1,
                    ctx,
                    p,
                    alpha,
                    beta
                );
            } else {
                score = -eval_ctx(
                    next,
                    child_depth,
                    history,
                    1,
                    ctx,
                    p,
                    -beta,
                    -alpha
                );
            }
        } else {
            if (same) {
                score = eval_ctx(
                    next,
                    child_depth,
                    history,
                    1,
                    ctx,
                    p,
                    alpha,
                    alpha + 1
                );
            } else {
                score = -eval_ctx(
                    next,
                    child_depth,
                    history,
                    1,
                    ctx,
                    p,
                    -alpha - 1,
                    -alpha
                );
            }

            if (score > alpha && score < beta) {
                if (same) {
                    score = eval_ctx(
                        next,
                        child_depth,
                        history,
                        1,
                        ctx,
                        p,
                        alpha,
                        beta
                    );
                } else {
                    score = -eval_ctx(
                        next,
                        child_depth,
                        history,
                        1,
                        ctx,
                        p,
                        -beta,
                        -alpha
                    );
                }
            }
        }

        delete next;

        if (ctx.stop) {
            break;
        }

        has_best = true;

        if (score > best_score) {
            best_score = score;
            result.best_move = action;

            if (p.report_partial && ctx.on_root_update) {
                ctx.on_root_update({
                    result.best_move,
                    best_score,
                    depth,
                    move_index + 1,
                    total_moves
                });
            }
        }

        if (score > alpha) {
            alpha = score;
        }

        first_child = false;
        move_index++;
    }

    result.score = has_best ? best_score : 0;
    result.nodes = ctx.nodes;
    result.seldepth = ctx.seldepth;

    if (!ctx.stop && has_best) {
        tt_store(
            root_key,
            depth,
            result.score,
            TT_EXACT,
            0,
            result.best_move,
            true
        );
    }

    return result;
}

//////////////////////////////////////////////////////////////////////PVS — default_params / param_defs

ParamMap PVS::default_params() {
    return {
        {"UseKPEval", "true"},
        {"UseEvalMobility", "true"},
        {"ReportPartial", "true"},
    };
}

std::vector<ParamDef> PVS::param_defs() {
    return {
        {"UseKPEval", ParamDef::CHECK, "true"},
        {"UseEvalMobility", ParamDef::CHECK, "true"},
        {"ReportPartial", ParamDef::CHECK, "true"},
    };
}