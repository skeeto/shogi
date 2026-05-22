// mate.cpp - proof-number checkmate search (tsume).
#include "mate.hpp"
#include <cstdint>
#include <vector>

namespace shogi {
namespace {

constexpr uint32_t PN_INF = 1u << 30;          // "infinity" for proof numbers

// One node of the AND/OR proof-number search tree.
struct PNNode {
  Position pos;
  uint32_t pn = 1, dn = 1;          // proof / disproof numbers
  int   parent     = -1;
  int   firstChild = -1;            // children occupy a contiguous arena range
  int   childCount = 0;
  Move  move;                       // move from the parent
  bool  orNode   = true;            // true = attacker to move
  bool  expanded = false;
};

class ProofSearch {
 public:
  ProofSearch(const Position& root, int budget) : budget_(budget) {
    nodes_.reserve(size_t(budget) + 320);       // mmap'd; pages commit lazily
    PNNode r;
    r.pos = root;
    r.orNode = true;
    nodes_.push_back(r);
  }

  // Returns the mate length in plies (>= 1) and sets `mateMove`, or 0.
  int solve(Move& mateMove) {
    while (nodes_[0].pn != 0 && nodes_[0].dn != 0 &&
           int(nodes_.size()) + 320 < budget_) {
      int mpn = selectMostProving();
      if (nodes_[mpn].expanded) break;          // defensive: no progress
      expand(mpn);
      backup(mpn);
    }
    if (nodes_[0].pn != 0) return 0;            // no forced mate proven
    for (int i = 0; i < nodes_[0].childCount; ++i) {
      int c = nodes_[0].firstChild + i;
      if (nodes_[c].pn == 0) { mateMove = nodes_[c].move; break; }
    }
    return proofDepth(0);
  }

 private:
  // Descend min-pn (OR) / min-dn (AND) to the most-proving unexpanded leaf.
  int selectMostProving() {
    int n = 0;
    while (nodes_[n].expanded && nodes_[n].childCount > 0) {
      const PNNode& nd = nodes_[n];
      int best = nd.firstChild;
      uint32_t bestVal = PN_INF + 1;
      for (int i = 0; i < nd.childCount; ++i) {
        int c = nd.firstChild + i;
        uint32_t v = nd.orNode ? nodes_[c].pn : nodes_[c].dn;
        if (v < bestVal) { bestVal = v; best = c; }
      }
      n = best;
    }
    return n;
  }

  // Is `hash` already on the path from the root down through `ancestor`?
  bool repeats(int ancestor, uint64_t hash) const {
    for (int a = ancestor; a != -1; a = nodes_[a].parent)
      if (nodes_[a].pos.hash == hash) return true;
    return false;
  }

  void expand(int ni) {
    const bool orNode = nodes_[ni].orNode;
    const Position pos = nodes_[ni].pos;        // copy: the arena may grow
    const Color stm = pos.stm();

    std::vector<Move> legal;
    generateLegalMoves(pos, legal, sc_);

    // OR node: only checking moves continue the mate.  AND node: all evasions.
    std::vector<Move> branch;
    if (orNode) {
      for (const Move& m : legal) {
        Position c = pos;
        doMove(c, m);
        if (inCheck(c, opp(stm))) branch.push_back(m);
      }
    } else {
      branch.swap(legal);
    }

    nodes_[ni].expanded = true;
    if (branch.empty()) {
      // OR with no check: the attack stalls.  AND with no move: checkmate.
      nodes_[ni].pn = orNode ? PN_INF : 0;
      nodes_[ni].dn = orNode ? 0 : PN_INF;
      return;
    }
    const int first = int(nodes_.size());
    nodes_[ni].firstChild = first;
    nodes_[ni].childCount = int(branch.size());
    for (const Move& m : branch) {
      PNNode ch;
      ch.pos = pos;
      doMove(ch.pos, m);
      ch.parent = ni;
      ch.move = m;
      ch.orNode = !orNode;
      if (repeats(ni, ch.pos.hash)) {           // a repetition never mates
        ch.pn = PN_INF;
        ch.dn = 0;
        ch.expanded = true;
      }
      nodes_.push_back(ch);
    }
  }

  // Recompute pn/dn from `ni` up to the root.
  void backup(int ni) {
    for (int n = ni; n != -1; n = nodes_[n].parent) {
      PNNode& nd = nodes_[n];
      if (nd.childCount == 0) continue;         // terminal: pn/dn already set
      uint32_t pn, dn;
      if (nd.orNode) {                          // OR: pn = min, dn = sum
        pn = PN_INF; dn = 0;
        for (int i = 0; i < nd.childCount; ++i) {
          const PNNode& c = nodes_[nd.firstChild + i];
          if (c.pn < pn) pn = c.pn;
          dn += c.dn; if (dn > PN_INF) dn = PN_INF;
        }
      } else {                                  // AND: pn = sum, dn = min
        pn = 0; dn = PN_INF;
        for (int i = 0; i < nd.childCount; ++i) {
          const PNNode& c = nodes_[nd.firstChild + i];
          pn += c.pn; if (pn > PN_INF) pn = PN_INF;
          if (c.dn < dn) dn = c.dn;
        }
      }
      nd.pn = pn;
      nd.dn = dn;
    }
  }

  // Mate length in plies of the proven subtree at `n` (requires pn == 0):
  // the attacker takes the proven move, the defender holds out longest.
  int proofDepth(int n) const {
    const PNNode& nd = nodes_[n];
    if (nd.childCount == 0) return 0;           // already a checkmate
    if (nd.orNode) {
      for (int i = 0; i < nd.childCount; ++i) {
        int c = nd.firstChild + i;
        if (nodes_[c].pn == 0) return 1 + proofDepth(c);
      }
      return 1;
    }
    int d = 0;
    for (int i = 0; i < nd.childCount; ++i) {
      int cd = proofDepth(nd.firstChild + i);
      if (cd > d) d = cd;
    }
    return 1 + d;
  }

  int budget_;
  std::vector<PNNode> nodes_;
  Scratch sc_;
};

}  // namespace

int dfpnMate(const Position& p, int nodeBudget, Move& mateMove) {
  if (nodeBudget < 400) return 0;
  ProofSearch ps(p, nodeBudget);
  return ps.solve(mateMove);
}

}  // namespace shogi
