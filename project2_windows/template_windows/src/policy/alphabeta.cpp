#include <utility>
#include "state.hpp"
#include "AlphaBeta.hpp"


/*============================================================
 * MiniMax — eval_ctx
 *
 * Negamax without pruning. Caller manages memory.
 *============================================================*/
//check move bisa vapture opp g
static bool is_capture(State *state, const Move& move){
    Point to = move.second;
    //gets dest square
    int opp = 1 - state->player; //gets opp player
    return state->board.board[opp][to.first][to.second] != 0; //true if the opp has a piece on dest square
}

int AlphaBeta::quiescence( // used when normal search reaches depth 0
    State *state,
    GameHistory& history,
    int ply,
    SearchContext& ctx,
    const MMParams& p,
    int alpha,
    int beta
){
    ctx.nodes++;
    //eval cur board
    int stand_pat = state->evaluate(
        p.use_kp_eval,
        p.use_eval_mobility,
        &history
    );
    //prune if post is good
    if(stand_pat >= beta){
        return beta;
    }
    //update alpha if cur eval is better
    if(stand_pat > alpha){
        alpha = stand_pat;
    }

    //generate moves if needed
    if(state->legal_actions.empty() && state->game_state == UNKNOWN){
        state->get_legal_actions();
    }

    if(state->game_state == WIN){
        return P_MAX - ply;
    }//return smaller steps is better

    for(auto& action : state->legal_actions){ //oly searches capture moves - non cap moves are skipped
        if(!is_capture(state, action)){
            continue;
        }

        State *next = state->next_state(action);
        bool same = next->same_player_as_parent();

        int raw = quiescence(next, history, ply + 1, ctx, p, -beta, -alpha);

        int score = same ? raw : -raw;

        delete next;

        if(score >= beta){
            return beta;
        }

        if(score > alpha){
            alpha = score;
        }
    }

    return alpha;
}

int AlphaBeta::eval_ctx(
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
    ctx.nodes++; //count one searched node
    if(ply > ctx.seldepth){ //track deepest lvl reached
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
    } //handle repeated pos
    history.push(state->hash()); // save cur board to hitory

    //does quescence
    if(depth <= 0){
        int score = quiescence(state, history, ply, ctx, p, alpha, beta );

        history.pop(state->hash());
        return score;
    }

    /* === Negamax loop === */
    int best_score = M_MAX;
    //try every legal move, unless pruning stops early

    for(auto& action : state->legal_actions){
        // [ Hackathon TODO 3-2 ]
        // create the child state after applying action
        State *next = state->next_state(action); //creates board pos after making a move, each legal move produces a new child node in the search tree
        //minimax explores these child pos to det the best move

        bool same = next->same_player_as_parent();
        //check if same player moves again
        // [Hackathon TODO 3-3]
        // search the child one level deeper //TAMBAHIN ALPHA BETA
        int raw = eval_ctx(next, depth - 1, history, ply + 1, ctx, p, -beta, -alpha);
        //recursive ly evaluates child pos, ply increase by 1 cuz we moved 1 lvl deeper in the tree 
        // [Hackathon TODO 3-4]
        // convert raw to the current player's perspective.
        //kek evaluasinya itu dari current player's pov, jadi kalo kita switch role otomatis score buat opponent lebih rendah kalo misal score buat
        //self tinggi
        int score;
        if(same) score = raw;
        else score = -raw;

        delete next;

        // [ Hackathon TODO 3-5 ]
        // update best_score if this child is better.
        if(score > best_score) best_score = score;
        //TAMBAHIN PRUNING
        if(best_score > alpha) alpha = best_score;
        if(alpha >= beta) break;

    }

    history.pop(state->hash());
    return best_score;
}


/*============================================================
 * MiniMax — search
 *
 * Iterate legal moves, call eval_ctx, return SearchResult.
 *============================================================*/
//root func that choosest best move
SearchResult AlphaBeta::search(
    State *state,
    int depth,
    GameHistory& history,
    SearchContext& ctx
){
    //clear old search data 
    ctx.reset();
    //load settings
    MMParams p = MMParams::from_map(ctx.params);
    //prepare answer
    SearchResult result;
    result.depth = depth;

    //generate root moves
    if(!state->legal_actions.size()){
        state->get_legal_actions();
    }


    int best_score = M_MAX - 10; //start with bad score
    int move_index = 0;
    int total_moves = (int)state->legal_actions.size();

    //try each root move
    for(auto& action : state->legal_actions){
        /* [ Hackathon TODO 4-1 ]
         * search this move like TODO 3, but starting from the root */ 
        State* next = state->next_state(action);
        bool same = next->same_player_as_parent();
        //create  board after that move
        //search move using alpha beta
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
ParamMap AlphaBeta::default_params(){
    return {
        {"UseKPEval", "true"},
        {"UseEvalMobility", "true"},
        {"ReportPartial", "true"},
    };
}

std::vector<ParamDef> AlphaBeta::param_defs(){
    return {
        {"UseKPEval", ParamDef::CHECK, "true"},
        {"UseEvalMobility", ParamDef::CHECK, "true"},
        {"ReportPartial", ParamDef::CHECK, "true"},
    };
}
