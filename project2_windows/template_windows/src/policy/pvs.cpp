#include <utility>
#include "state.hpp"
#include "pvs.hpp"


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
    history.push(state->hash());

    if(depth <= 0){
        int score = state->evaluate(
            p.use_kp_eval, p.use_eval_mobility, &history
        ); 
        history.pop(state->hash());
        return score;
    }

    /* === Negamax loop === */
    bool first_child = true;
    int best_score = M_MAX;

for(auto& action : state->legal_actions){
    State *next = state->next_state(action);
    bool same = next->same_player_as_parent();

    int raw;

    if(first_child){
        raw = eval_ctx(next, depth - 1, history, ply + 1, ctx, p, -beta, -alpha);
    }else{
        raw = eval_ctx(next, depth - 1, history, ply + 1, ctx, p, -alpha - 1, -alpha);

        int score_probe;
        if(same) score_probe = raw;
        else score_probe = -raw;

        if(score_probe > alpha && score_probe < beta){
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

    if(!state->legal_actions.size()){
        state->get_legal_actions();
    }


    int best_score = M_MAX - 10;
    int move_index = 0;
    int total_moves = (int)state->legal_actions.size();

    for(auto& action : state->legal_actions){
        /* [ Hackathon TODO 4-1 ]
         * search this move like TODO 3, but starting from the root */ 
        State* next = state->next_state(action);
        bool same = next->same_player_as_parent();
        int raw = eval_ctx(next, depth - 1, history, 1, ctx, p, M_MAX, P_MAX); //copas but from the root, root is 0 and next is 1 so automatically next is alr one move away from root
        int score;
        if(same) score = raw;
        else score = -raw;

        delete next;
        
            if(score > best_score){
                // [ Hackathon TODO 4-2 ]
                // keep this move if it is the best so far
                best_score = score; //update best score
                result.best_move = action; //same the move

                if(p.report_partial && ctx.on_root_update){
                   ctx.on_root_update({result.best_move, best_score, depth, move_index + 1, total_moves});
                }
            }  
        move_index++;
    }

    // [ Hackathon TODO 4-3 ]
    // update result and return
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
