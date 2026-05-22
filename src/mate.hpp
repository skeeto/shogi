// mate.hpp - proof-number checkmate search.
//
// Searches for a forced checkmate the side to move can deliver by a sequence
// of checks (a tsume search): OR nodes are the attacker, restricted to
// checking moves; AND nodes are the defender, with every legal evasion.  The
// AND/OR tree is held in an arena and bounded by a node budget, so the search
// always terminates; a position repeated on the search path counts as no mate
// (perpetual check does not checkmate).  See Allis' proof-number search and
// Nagai's df-pn - this is the plain (non-depth-first) form, which is enough
// because the node budget bounds the tree to a few tens of MB.
#pragma once
#include "board.hpp"

namespace shogi {

// If `p`'s side to move has a forced checkmate, returns its length in plies
// (>= 1) and sets `mateMove` to the first move of the mate; otherwise returns
// 0.  `nodeBudget` caps the search.
int dfpnMate(const Position& p, int nodeBudget, Move& mateMove);

}  // namespace shogi
