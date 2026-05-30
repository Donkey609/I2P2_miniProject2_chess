#include <utility>
#include <algorithm>
#include "state.hpp"
#include "pvs.hpp"

namespace {
    //check whether a move captures an opponent piece
    //looks at dest squrae and checks the opp's piece there
    //if exist == true,
bool is_capture(State* state, const Move& action){
    int to_r = action.second.first;
    int to_c = action.second.second;
    int opp = 1 - state->player;
    return state->piece_at(opp, to_r, to_c) != 0;
    //used for move ordering and quiescence
}
//returns a score for the pos but only after checking dangerous sequences
int quiescence( //ini semua parameters
    State* state, //cur board pos
    GameHistory& history, // used to detect repetition/draw
    int ply, //how deep from the root-->used so faster wins score better
    SearchContext& ctx, //tracks nodes, max depth reached and stop signal
    const MMParams& p, //evaluate settings like kp eval and mob eval
    int alpha, //limits for alpha beta pruning
    int beta
){
    ctx.nodes++; //counts pos as searched

    if(ply > ctx.seldepth){ //updates the deepest search level reached
        ctx.seldepth = ply;
    }

    if(ctx.stop){
        return 0; //stop if engine is told to stop
    }

    if(state->legal_actions.empty() && state->game_state == UNKNOWN){
        state->get_legal_actions();
    } //generates legal moves if not ready yet

    if(state->game_state == WIN){
        return P_MAX - ply;
    }//win pos, faster wins better

    if(state->game_state == DRAW){
        return 0;
    }//neutral score if draw

    int rep_score;
    if(state->check_repetition(history, rep_score)){
        return rep_score; //check repeated postitions
    }

    history.push(state->hash()); //add cur board to history b4 searchind deeper

    int stand_pat = state->evaluate(
        p.use_kp_eval,
        p.use_eval_mobility,
        &history
    );//scroe if we stop and do nothing else

    if(stand_pat >= beta){
        history.pop(state->hash());
        return beta;
    } //if the pos is too good pruned

    if(stand_pat > alpha){
        alpha = stand_pat;
    }//if pos improves update alpha

    //skips all quient non capture moves -->quiescence isfor capture moves so the rest are skipped
    for(auto& action : state->legal_actions){
        if(!is_capture(state, action)){
            continue;
        } 

        State* next = state->next_state(action); //creates next board after capture
        bool same = next->same_player_as_parent();

        int raw = quiescence( //check more captures
            next,
            history,
            ply + 1,
            ctx,
            p,
            -beta,
            -alpha
        );

        int score = same ? raw : -raw; //converts score to player's perspective

        delete next;

        if(score >= beta){ //prune if capture is good
            history.pop(state->hash());
            return beta;
        }

        if(score > alpha){ //update best score
            alpha = score;
        }
    }

    history.pop(state->hash()); //remove cur state from history and ret the best score found
    return alpha;
}
}

/*============================================================
 * MiniMax — eval_ctx
 *
 * Negamax without pruning. Caller manages memory.
 *============================================================*/
int PVS::eval_ctx(
    State *state,
    int depth,
    GameHistory& history,
    int ply,
    SearchContext& ctx,
    const MMParams& p,
    //tambahin alpha beta
     int alpha,
    int beta
){
    ctx.nodes++;
    if(ply > ctx.seldepth){
        ctx.seldepth = ply;
    }
    if(ctx.stop){
        return 0;
    }

    /* === Lazy move generation (sets game_state) === */
    if(state->legal_actions.empty() && state->game_state == UNKNOWN){
        state->get_legal_actions();
    }

    /* === Terminal / leaf checks === */

    // [ Hackathon TODO 3-1 ]
    // return the score for a winning terminal state
    // Hint: prefer faster wins by using ply.
     if(state->game_state == WIN) return P_MAX - ply;
     //return large score for winning psitions 
     //sub ply makes the engine prefers faster wins so winning in 2 moves is better than 5 moves

    if(state->game_state == DRAW){
        return 0;
    }

    /* === Repetition check (game-specific) === */
    int rep_score;
    if(state->check_repetition(history, rep_score)){
        return rep_score;
    }
    //enters quescences search instead of evaluating immediately
    if(depth <= 0){
        return quiescence(state, history, ply, ctx, p, alpha, beta);
    }

    history.push(state->hash());

    /* === Negamax loop === */
    bool first_child = true;
    int best_score = M_MAX;
std::vector<Move> actions = state->legal_actions;
//first order captures first
std::stable_sort(actions.begin(), actions.end(),
    [&](const Move& a, const Move& b){
        return is_capture(state, a) && !is_capture(state, b);
    }
);

for(auto& action: actions){
    State *next = state->next_state(action);
    bool same = next->same_player_as_parent();

    int raw;
    //searches first move fully
    if(first_child){
        raw = eval_ctx(next, depth - 1, history, ply + 1, ctx, p, -beta, -alpha);
    }else{
        raw = eval_ctx(next, depth - 1, history, ply + 1, ctx, p, -alpha - 1, -alpha);

        int score_probe;
        if(same) score_probe = raw;
        else score_probe = -raw;

        //search properly if score is good
        if(score_probe > alpha && score_probe < beta){
            //later seach moves with tiny window
            raw = eval_ctx(next, depth - 1, history, ply + 1, ctx, p, -beta, -alpha);
        }
    }

   int score;
        if(same) score = raw;
        else score = -raw;

    delete next;

    if(score > best_score){
        best_score = score;
    }

    if(best_score > alpha){
        alpha = best_score;
    }

    if(alpha >= beta){
        break;
    }

    first_child = false;
}

    history.pop(state->hash());
    return best_score;
}


/*============================================================
 * MiniMax — search
 *
 * Iterate legal moves, call eval_ctx, return SearchResult.
 *============================================================*/
SearchResult PVS::search(
    State *state,
    int depth,
    GameHistory& history,
    SearchContext& ctx
){
    ctx.reset();
    MMParams p = MMParams::from_map(ctx.params);

    SearchResult result;
    result.depth = depth;

    if(state->legal_actions.empty()){
        state->get_legal_actions();
    }

    if(state->legal_actions.empty()){
        result.score = 0;
        result.nodes = ctx.nodes;
        result.seldepth = ctx.seldepth;
        return result;
    }

    int alpha = M_MAX;
    int beta = P_MAX;
    int best_score = M_MAX;
    bool first_child = true;

    int move_index = 0;
    int total_moves = (int)state->legal_actions.size();

    result.best_move = state->legal_actions[0];

    std::vector<Move> actions = state->legal_actions;

    std::stable_sort(actions.begin(), actions.end(),
        [&](const Move& a, const Move& b){
            return is_capture(state, a) && !is_capture(state, b);
        }
    );
    for(auto& action : actions){
        State* next = state->next_state(action);
        bool same = next->same_player_as_parent();

        int raw;

        if(first_child){
            raw = eval_ctx(next, depth - 1, history, 1, ctx, p, -beta, -alpha);
        }else{
            raw = eval_ctx(next, depth - 1, history, 1, ctx, p, -alpha - 1, -alpha);

            int probe_score = same ? raw : -raw;

            if(probe_score > alpha && probe_score < beta){
                raw = eval_ctx(next, depth - 1, history, 1, ctx, p, -beta, -alpha);
            }
        }

        int score = same ? raw : -raw;

        delete next;

        if(score > best_score){
            best_score = score;
            result.best_move = action;

            if(p.report_partial && ctx.on_root_update){
                ctx.on_root_update({
                    result.best_move,
                    best_score,
                    depth,
                    move_index + 1,
                    total_moves
                });
            }
        }

        if(score > alpha){
            alpha = score;
        }

        first_child = false;
        move_index++;

        if(ctx.stop){
            break;
        }
    }

    result.score = best_score;
    result.nodes = ctx.nodes;
    result.seldepth = ctx.seldepth;

    return result;
}

/*============================================================
 * MiniMax — default_params / param_defs
 *============================================================*/
ParamMap PVS::default_params(){
    return {
        {"UseKPEval", "true"},
        {"UseEvalMobility", "true"},
        {"ReportPartial", "true"},
    };
}

std::vector<ParamDef> PVS::param_defs(){
    return {
        {"UseKPEval", ParamDef::CHECK, "true"},
        {"UseEvalMobility", ParamDef::CHECK, "true"},
        {"ReportPartial", ParamDef::CHECK, "true"},
    };
}
