# Classical (Non-NN) MCTS for Shogi on CPU-Only Desktops: A Survey and Build Guide

## TL;DR
- **Don't build a pure classical MCTS shogi engine if your goal is maximum strength** — the historical evidence (Sato/Takahashi/Grimbergen 2010 reached only ~amateur 1-dan and won just 4% against a 1-dan conventional engine even with shogi-specific tuning) is unambiguous that PVS/alpha-beta with KPPT-style evaluation (Bonanza, Apery, YaneuraOu) is far stronger on the same CPU hardware; MCTS only became competitive in shogi when paired with deep neural network policy/value heads (dlshogi, AobaZero).
- **Do build it if your goal is the algorithm**, but build a *hybrid* from day one: UCT + MCTS-Solver (Winands/Björnsson/Saito 2008) + an embedded df-pn (Nagai 2002) mate-search module + shallow alpha-beta verifications at leaves (Baier & Winands 2015 / Lanctot et al. 2014 implicit minimax backups); a pure UCT-with-random-playouts engine will hang pieces, walk into mate, and be unplayable.
- **The two highest-leverage techniques specific to shogi are (a) a strong, biased playout policy** (Bradley-Terry/Elo-trained move features in the style of Coulom 2007 raised Sato et al.'s playout termination rate from 19.3% to 90.5%) **and (b) tight df-pn integration** for mate-to-N and brinkmate; without both, MCTS rollouts in shogi essentially never terminate naturally (long captured-piece economies + drops = no natural attrition).

## Key Findings

1. **MCTS without NN never beat strong alpha-beta in shogi.** The only published full-game classical MCTS shogi engine (Sato, Takahashi & Grimbergen, ICGA Journal 33(2):80–92, 2010) tested every Computer-Go-era enhancement and topped out at amateur 1-dan on tactical test sets; in match play, pure MCTS scored 0.04 vs a 1-dan conventional engine and rose only to 0.32 after adding quiescence search.
2. **Shogi's drops + long captures kill random/light playouts.** With Elo-trained move features, Sato et al. lifted the rate of playouts terminating within 256 plies from 19.3% to 90.5% — a 4.7× improvement that is the single biggest reported MCTS-in-shogi gain.
3. **df-pn (Nagai 2002) is the right mate solver, integrated as a co-process to MCTS.** Modern engines (e.g., TadaoYamaoka's DeepLearningShogi) run df-pn alongside the main search rather than inside rollouts. PN*/PDS (Seo, Iida, Uiterwijk 2001) and Weak PNS (Ueda et al. 2008) are alternatives; df-pn with the `1+ε` trick (Pawlewicz & Lew) and Kishimoto's GHI fix (df-pn(r), 2005) is standard.
4. **MCTS-Solver and Score-Bounded MCTS are mandatory, not optional, for shogi.** Proven-win/proven-loss propagation (Winands, Björnsson & Saito 2008; Cazenave & Saffidine 2010 for draws/repetitions) is the cleanest way to handle terminal mate values and sennichite cycles inside the tree.
5. **Shallow alpha-beta at MCTS nodes attacks shogi's "shallow trap" problem.** Ramanujan, Sabharwal & Selman (ICAPS 2010) showed why MCTS fails in tactical games like chess (and a fortiori shogi); Baier & Winands' MCTS-Minimax hybrids (TCIAIG 2015; JAIR 2018, MCTS-IP-M-k) and Lanctot et al.'s implicit minimax backups (CIG 2014) are the standard remedies and they work.
6. **PN-MCTS (proof-number-augmented UCT) has been measured on MiniShogi.** Doe, Winands, Soemers & Browne (IEEE CoG 2022) report PN-MCTS beating vanilla MCTS in MiniShogi by 86.0% (±4.30) at 0.1 s/move, 76.8% (±5.23) at 1 s, and 67.2% (±5.82) at 2 s (250 games per cell) — diminishing as simulation count grows, which tells you the PN signal is most valuable when rollouts are scarce (exactly your situation on CPU-only).
7. **Bitboards are doable in shogi but cost two 64-bit words.** Grimbergen (ICGA Journal 30(1):25–34, 2007), Apery, and YaneuraOu all use a 9×9 bitboard packed into a 128-bit value (two 64-bit lanes or `__m128i` under SSE2/SSE4). This is the standard representation; expect a 48.8% move-generation speedup vs piece-list attack tables (Grimbergen 2007).
8. **There is essentially zero open-source classical MCTS shogi code worth forking.** Sato et al.'s engine is not public. Bonanza, Apery, YaneuraOu, Tacos, Gekisashi are all alpha-beta. Modern MCTS shogi engines (dlshogi, AobaZero, FukaurOu) all require neural nets. You will be writing this from scratch.

## Details

### 1. Core MCTS variants and how they apply to shogi

**UCT (Kocsis & Szepesvári 2006)** is the spine. The canonical selection rule

```
a* = argmax_a  ( Q(s,a) + C · sqrt( ln N(s) / N(s,a) ) )
```

works in shogi but is *severely* hurt by the branching factor. Sato/Takahashi/Grimbergen used C=1.0 (UCB1) and explicitly noted that for nodes with low visit count, allocation cannot be done efficiently and selection becomes nearly random — which in shogi means you spend your first few thousand simulations sampling piece sacrifices and silly drops uniformly. Their fix was to *boost C locally* for nodes whose move has high killer/history-heuristic score, biasing exploration toward tactically plausible moves before visit counts stabilize.

**FPU (Wang & Gelly 2007, "Modification of UCT with Patterns in Monte-Carlo Go", §3.4):** Set the urgency of unvisited children to a finite value (e.g., the parent's win-rate or a Bradley-Terry prior on the move) instead of infinity. In shogi with branching factor 80–250 this is essential — otherwise UCB1 force-visits every legal move (including every silly drop on every empty square) once before exploiting anything. A practical rule is `FPU = parent_value − reduction`; the small (~0.2) reduction came out of Leela Zero's Go experiments (leela-zero/leela-zero issue #696, where reducing FPU by 0.2 won 5/0 in a small test); note that LeelaChessZero (lc0) for chess uses a much larger default `--fpu-reduction 1.20`. Tune for your evaluator and time control.

**RAVE / AMAF (Gelly & Silver 2011, "Monte-Carlo tree search and rapid action value estimation in computer Go", *Artificial Intelligence* 175(11))**: RAVE shares move statistics across positions where the same move is played. Gelly & Silver's Figure 5 shows MC-RAVE roughly doubled MoGo's win rate vs GnuGo level 10 on 9×9 Go at 3,000 simulations (~24% → 50–60%), making RAVE central to the pre-DCNN Go era. **In shogi, RAVE is risky** because the same move (e.g., R*8h — rook drop on 8h) is good in one position and disastrous in another, far more than in Go where local-shape AMAF is informative. GRAVE (Cazenave, IJCAI 2015) — which uses a hierarchy of ancestors to choose the level of generalization — is a safer default if you want any AMAF at all.

**Progressive bias / progressive widening (Chaslot, Winands, van den Herik, Uiterwijk, Bouzy 2008, "Progressive Strategies for Monte-Carlo Tree Search", *New Math. and Natural Computation* 4(3))**: This is the single most useful family for shogi. **Progressive bias** adds `H_i / (n_i + 1)` to the UCB term, where `H_i` is a domain-heuristic prior; **progressive widening** delays the consideration of moves beyond a small initial set until simulation count grows. Sato/Takahashi/Grimbergen ported it to shogi but explicitly *weakened* the pruning: `⌈d·R_i + c'·√(2 ln n)/e⌉` with c'=0.5, d=4.0, e=10.0, justifying the weaker variant as: in shogi, positions that look like a piece loss can turn out to be the best move when read deeply — i.e., shogi sacrifices are real, and aggressive widening prunes them away.

**Decisive moves / anti-decisive moves (Teytaud & Teytaud, CIG 2010)**: At every selection and rollout step, check whether the current player has a 1-ply win (in shogi: an immediate checkmate via a check that has no legal escape — detectable by `mate_in_1()`), or whether the opponent threatens one. If so, force the decisive/anti-decisive move. This is cheap (mate-in-1 in shogi is `O(king_evasions × attacker_moves)`) and catches a huge fraction of MCTS blunders in tactical positions.

**Virtual loss (Chaslot et al. 2008; Cazenave & Jouandeau 2008)**: When a thread descends through a node, temporarily add a loss to its statistics so other threads diverge. Enzenberger & Müller (ACG 2010) showed lock-free tree parallelization with virtual loss is the practical method for CPU-bound MCTS. Note Mirsoleimani, Plaat, van den Herik (ICAART 2017) found that on some non-game domains virtual loss can actually *hurt* — for shogi where playouts are expensive, keep it but tune the magnitude.

### 2. Shogi-specific challenges

- **Branching factor 80–250 with drops.** A captured piece is potentially droppable on any empty square, so the legal-move count balloons in the mid- and end-game. In endgames, "the branching factor in the endgame often increases beyond 200" (Iida, Sakuta & Rollason 2002, "Computer shogi", *Artificial Intelligence* 134(1–2)). This breaks vanilla UCT exploration: see FPU + progressive widening above.
- **Tactical density.** Ramanujan, Sabharwal & Selman ("On Adversarial Search Spaces and Sampling-Based Planning", ICAPS 2010): "certain 'early loss' or 'shallow trap' configurations, while unlikely in Go, occur surprisingly often in games like Chess (even in grandmaster games)... UCT, unlike minimax search, is unable to identify such traps in Chess and spends a great deal of time exploring much deeper game play than needed." Shogi is *more* tactical than chess (drops re-enliven dead positions), so the trap problem is strictly worse.
- **Long-running endgames.** Without termination heuristics, shogi self-play between random policies almost never ends — pieces don't leave the game, so material doesn't drain. Sato et al.'s 19.3%→90.5% termination jump is exactly this problem.
- **Sennichite (千日手) and perpetual check.** Fourfold repetition draws the game, *unless* one side is delivering perpetual check, in which case that side loses. This is graph-history-interaction (GHI) and breaks naive transposition tables in both MCTS and df-pn. Solutions:
  - Kishimoto 2005 ("A solution to the GHI problem for depth-first proof-number search", *Information Sciences* 175(4)) — store the path history (hash of recent positions) along with the TT key, treat as separate node if histories differ.
  - In MCTS, score sennichite leaves as 0.5 (draw) and perpetual-check leaves as 0 for the checking side; rely on path-aware hashing to avoid loops.
- **Promotion.** Move generators must yield both promoted and unpromoted variants when the destination is in the promotion zone (or both source and dest are, for a knight/lance). For MCTS this just inflates branching by ~1.3×; for df-pn, the promotion-vs-defer choice is often the critical mate move.

### 3. Mate search and tactical integration

**df-pn (Nagai, PhD thesis, University of Tokyo, 2002)** is the standard shogi mate solver. It is a depth-first variant of proof-number search guided by twin thresholds `(thpn, thdn)` and uses a transposition table for memory efficiency. The behavior is equivalent to PN in trees but vastly more memory-efficient. **df-pn variants worth knowing for shogi**:
- **df-pn+ (Nagai & Imai 1999):** initialize pn/dn from heuristic evaluation functions H_pn, H_dn (e.g., 2-ply mate-search-heuristic) plus a per-node Cost penalty for sacrificing moves. Significantly faster in practice.
- **PN\* (Seo, Iida, Uiterwijk, *Artificial Intelligence* 129(1–2), 2001):** earlier depth-first PN variant; df-pn dominates but the PN* paper is the seminal tsume-shogi reference.
- **PDS (Nagai 1998):** uses disproof numbers in addition to PN; slower than df-pn but more robust on some hard positions.
- **Weak PNS (Ueda, Hashimoto, Hashimoto & Iida, CG 2008):** uses branching factor as an estimator alongside PN; counters df-pn's "double-counting problem" (Kakinoki 2005) where sibling values are highly correlated (typical of tsume-shogi interpose-piece sequences).
- **Kishimoto's df-pn(r) (2005):** the GHI fix; cures infinite loops on directed cyclic graphs (DCG). Mandatory for shogi because of repetition.
- **λ-search-driven df-pn (Soeda, ICGA 2006):** uses Thomsen's λ-search threats; "seems to be better than Nagai's method in solving Shogi brinkmate problems."
- **Parallel df-pn (Kaneko, AAAI 2010 / Hoki, Kaneko, Kishimoto, Ito 2013, "Parallel Dovetailing and its Application to Depth-First Proof-Number Search", *ICGA Journal* 36(1)):** virtual proof/disproof numbers for shared-memory parallelization; small (<50%) memory overhead.

**How to integrate df-pn with MCTS**. Three viable architectures:

1. **Co-process / parallel solver (recommended)**: run df-pn in a separate thread on the root position and on each "interesting" leaf (a position where the side to move can give check). When df-pn finds a mate, write a proven-win flag to the corresponding MCTS node; MCTS-Solver propagates it. dlshogi uses this pattern (see TadaoYamaoka/DeepLearningShogi engine architecture).
2. **Leaf-call mate solver**: at each MCTS leaf expansion, call df-pn with a small node budget (say 10k–100k nodes) restricted to checking moves only. Cheap because most positions have few or no checks; catches all mate-in-N for N ≤ a few.
3. **Rollout-time mate-in-1**: in every rollout step, run `mate_in_1()` for the side to move; if it succeeds, terminate with a win. This is essentially the decisive-moves enhancement specialized to shogi.

**Quiescence in MCTS rollouts.** The Sato et al. paper's biggest single match-result improvement came from adding *quiescence search* to terminate rollouts — pure MCTS won 4% vs a 1-dan conventional engine; with quiescence, 32%. The quiescence search resolves captures/checks before scoring; you compute a static eval at the quiescent position and convert to a pseudo-win-rate via sigmoid (e.g., `1/(1+exp(-eval/k))`).

**Seeding MCTS with mate threats.** When df-pn proves a mate at a node:
- Backed-up value: WIN (1.0 for side to move) or LOSS (0.0).
- MCTS-Solver propagation: if a node has a proven-win child, mark the node as a proven win (negamax sense). If all children are proven losses, mark the node as proven loss.
- For Score-Bounded MCTS (Cazenave & Saffidine, CG 2010): maintain optimistic/pessimistic bounds `[α, β]` per node; sennichite gives bound `[0.5, 0.5]`, mate gives `[0,0]` or `[1,1]`; allow αβ-style cutoffs when bounds become disjoint from parent.

### 4. Blunder avoidance in MCTS

In tactical games, MCTS-IP-M-k (Baier & Winands, JAIR 2018, "MCTS-minimax hybrids with state evaluations") is the strongest known generic technique: at each newly-expanded node, run a shallow αβ (k=2 or k=4) with the static evaluator to assign a **prior bias** used inside the UCB1 formula. Quote: "the use of enhanced minimax for computing node priors results in the strongest MCTS-minimax hybrid in the three test domains of Othello, Breakthrough, and Catch the Lion."

**Concrete shogi build**:
- **Mate-in-1 check at every node expansion.** Detects king-capture-in-one trivially.
- **2-ply αβ at every node expansion** with a Bonanza/KPPT-lite evaluator. Provides the H_i used by progressive bias and the implicit-minimax-backup signal.
- **MCTS-Solver everywhere.** Propagate proven values up the tree per Winands/Björnsson/Saito 2008 (the MCTS-Solver paper has C-like pseudocode in negamax form).
- **Implicit minimax backups (Lanctot, Winands, Pepels, Sturtevant, CIG 2014)**: store two values per node — the rollout-average `Q_MC` and the minimax-of-evaluations `Q_mm` — and use a weighted combination `α·Q_mm + (1-α)·Q_MC` in selection (α around 0.4–0.6 typically).
- **Move filtering in rollouts**: never make a move that drops a piece on a square attacked by a less-valuable enemy piece without a justification (escape/check/recapture); never make a move that leaves your king in check that you can resolve elsewhere. These two filters alone eliminate >90% of nonsense rollout moves in shogi.

### 5. Rollout / simulation policy without NNs

**Hand-crafted policies.**
- **Bradley-Terry / Elo-rated move features (Coulom, ICGA Journal 30(4), 2007).** This is what Sato/Takahashi/Grimbergen used and it is the most thoroughly documented working approach for shogi MCTS rollouts. Features they used (~50 total): SEE on captures, recapture flag, promotion, check, escape-from-threat, KPPT-style position-table delta for both moves and *drops* (separate γ tables per piece type for drop locations), king-zone control, "relation to piece moved n plies ago." Trained on ~300 pro Meijin-sen games. Piece values P=100, L=280, N=300, S=420, G=530, B=620, R=700; promoted variants +P=270, +L=320, +N=250, +S=430, +B=710, +R=850. Heavy policy dropped nps from 3,500 to 900 but quadrupled tactical accuracy and lifted within-256-ply termination from 19.3% to 90.5%.
- **Move-pattern heuristics.** 3×3 patterns work in Go; in shogi the equivalent is "king safety patterns" around your king's 24 neighborhood (`around24_bb()` in YaneuraOu) — biased toward defensive drops/moves near the king. Less mature than Go patterns but standard in Japanese GPW papers from 2005–2012.
- **Last-good-reply (Drake, ICGA Journal 32(4), 2009; Baier & Drake, IEEE TCIAIG 2010 "The Power of Forgetting")**: store the most recent move that succeeded as a reply to a given opponent move, replay it. Cheap, generic, and helped in Go significantly. In shogi expect modest gains because move identity (especially drop moves) is more context-dependent.
- **N-gram Selection Technique / NST (Tak, Winands, Björnsson, IEEE TCIAIG 4(2), 2012)**: store success statistics for sequences of N consecutive moves; use ε-greedy (ε=0.1) selection. Beats MAST in general game playing.
- **Playout Policy Adaptation / PPA (Cazenave, ACG 2015)**: learn the playout policy online during the search via policy-gradient updates from playout outcomes. The Sironi, Cazenave, Winands 2021 enhancements (N-grams + payoff-proportional update + depth discounting) improve PPA further.

**Heavy vs light playouts.** Light random playouts in shogi don't terminate (the 19.3% number above). **Verdict for shogi**: always heavy, with static-eval cutoff after ~40–60 plies of rollout if no terminal, to bound playout cost.

**Static evaluation cutoff in rollouts.** Truncate rollouts after N plies and convert the evaluator's score to a win-rate via sigmoid. Use a Bonanza-style KPP (King-Piece-Piece) or KPPT (King-Piece-Piece-Turn) hand-crafted evaluator: piece values + king-safety + king-piece-piece tables. The first such large-scale feature-tuned shogi evaluator was Bonanza's (Hoki, "Optimal control of minimax search results to learn positional evaluation", *Game Programming Workshop 2008*); the resulting "Bonanza method" / MMTO (Minimax Tree Optimization; Hoki & Kaneko, *JAIR* 49:527–568, 2014) is the standard training procedure if you want a tuned eval.

### 6. Existing engines and prior work

- **Sato, Takahashi & Grimbergen (2010)** — only published full-game MCTS shogi engine of the classical era. ICGA Journal 33(2):80–92. Strength ~amateur 1-dan, 4% match score vs a 1-dan conventional engine (pure MCTS), 32% with quiescence. Conclusion (verbatim from abstract): "it seems unlikely that a pure MCTS-based shogi program will surpass the level of the best conventional shogi programs."
- **Yokoyama & Kitsuregawa (PRICAI 2014, "A Randomized Game-Tree Search Algorithm for Shogi Based on Bayesian Approach", LNAI 8862:937–944)** — replaces averaging in MCTS with Bayesian propagation of *value distributions*. Quote: "(1) using multiple game-tree search with a randomized evaluation function as simulations, (2) treating evaluated values as probability distribution and propagating it through the game-tree using the Bayesian Approach concept. Proposed method is focusing on applying to tactical games such as Shogi, in which MCTS is not currently effective." Implemented inside Gekisashi; reaches >50% win rate vs unmodified Gekisashi only with 30–50× more computing resource — interesting algorithmically but didn't pay for itself.
- **Ugajin & Kotani (Game Programming Workshop 2009)** — early Japanese GPW work on playout transition-probability tuning for Monte-Carlo Shogi.
- **Sasaki & Kotani (GPW 2009)** — MCTS in Blokus-Duo; the closest Japanese GPW work showing MCTS techniques transferring to a non-Go board game.
- **Doe, Winands, Soemers & Browne (IEEE CoG 2022, "Combining Monte-Carlo Tree Search with Proof-Number Search", arXiv:2206.03965)** and extended **Kowalski, Doe, Winands, Górski & Soemers (arXiv:2303.09449, 2023/24)**: PN-MCTS tested on MiniShogi (5×5 shogi via Ludii). Selection rule:
  ```
  argmax_i [ v_i + C · sqrt(ln n_p / n_i) + C_pn · (1 − pnRank_i / max_j pnRank_j) ]
  ```
  where pnRank is the rank of the child's proof number (or disproof at AND nodes). Defaults C=√2, C_pn=1. MiniShogi results: PN-MCTS beats MCTS 86.0% (±4.30) at 0.1 s/move, 76.8% (±5.23) at 1 s, 67.2% (±5.82) at 2 s (250 games per cell). Quote: "PN-MCTS outperforms MCTS in LOA, Knightthrough, and MiniShogi. Though for MiniShogi the added benefit seems to diminish when the thinking time increases."
- **Bonanza (Hoki 2005–2013)** — alpha-beta + Bonanza-method evaluation tuning; champion of the 16th World Computer Shogi Championship in 2006 on its first-ever entry (per AI Factory's Spring 2006 newsletter: "Bonanza won the CSA tournament the first time it participated, something that had never happened before") and again at WCSC23 in 2013 (Hiroshi Yamashita, SHOGI-L post, May 6, 2013). Bitboards stored as three 32-bit unsigned ints. Pioneered KPP/KKP feature tuning from pro games.
- **Apery, YaneuraOu** — Stockfish-derived alpha-beta + NNUE; YaneuraOu's df-pn implementation is in `source/engine/tanuki-mate-engine/tanuki-mate-search.cpp` and is a reference for production-grade df-pn in C++.
- **Tacos (Hashimoto, Iida)** — alpha-beta with context killer heuristic; bronze at 11th Computer Olympiad 2006.
- **Sakuta & Iida (ICGA Journal 24(4), 2001)** — survey of AND/OR-tree search algorithms in shogi mating search; useful for understanding how Japanese researchers integrated PN family into engines.
- **Iida, Sakuta, Rollason (2002, "Computer shogi", *Artificial Intelligence* 134(1–2))** — the canonical survey of computer shogi before the NN era.

**Why alpha-beta dominated classically.** Hoki's Bonanza method showed that with KPP/KPPT-style millions-of-parameters evaluators tuned by MMTO from pro games, alpha-beta scales further than MCTS on the same CPU because (a) shogi's tactical density makes shallow traps fatal to MCTS and (b) the static evaluator is good enough that even shallow αβ extracts more signal per node than a random rollout. The ~130 Elo jump Stockfish announced upon releasing Stockfish 12 with NNUE in September 2020 ("The rating difference compared to Stockfish 11 is estimated to be about 130 Elo points" — Chess.com Stockfish 12 release, Sep 2020) — based on Yu Nasu's shogi-originated NNUE design (2018) — is the same pattern in reverse: better static eval > more search.

**Open-source classical MCTS shogi engines: effectively none.** This is genuinely a green-field implementation.

### 7. Practical implementation considerations for CPU-only desktops

**Bitboards.** Use a 128-bit board (two 64-bit lanes, or `__m128i` under SSE2/SSE4) with bits 0–80 for the 81 squares; bit 63 unused (a hack Apery and YaneuraOu both use, exploiting it for PEXT-based rook attacks). Per YaneuraOu source (`source/bitboard.h`):
```cpp
struct alignas(16) Bitboard {
#if defined(USE_SSE2)
  union { u64 p[2]; __m128i m; };
#else
  u64 p[2];
#endif
};
```
For sliding pieces (rook, bishop, lance, dragon, horse) use **magic bitboards** (Apery) or **PEXT-based attacks** (BMI2-capable CPUs). For drops, you need per-piece-type "empty squares allowed" bitboards excluding (a) the back rank(s) where the piece would be immobilized (e.g., pawn/lance on rank 1 from Black's side; knight on ranks 1–2), and (b) for pawns, columns already containing one of your own unpromoted pawns (the *nifu* rule).

**Move generation speed.** Grimbergen (ICGA Journal 30(1), 2007) reported bitboards gave 48.8% speedup vs attack tables in his Spear program. Modern shogi engines hit 1M–5M nps in alpha-beta; in MCTS with heavy playouts expect 1k–10k playouts/sec single-threaded on a 2024 desktop CPU.

**Tree memory.** Each MCTS node needs: move, visit count, win sum, child pointer/index, parent pointer, plus implicit-minimax value, plus proven-win/proven-loss flag — ~64 bytes. For a 1 GB tree you can hold ~16M nodes. Use an arena allocator; reuse the subtree rooted at the played move (sibling subtrees freed back to the arena).

**Transposition tables in MCTS.** Use a *DAG-aware* MCTS (Saffidine's UCD — UCT for Directed acyclic graphs, 2010) if you want to share statistics across transpositions. For shogi with sennichite, encode the path-history hash into the key per Kishimoto 2005 to avoid GHI.

**Parallelization on a desktop CPU.**
- **Root parallelization** (Soejima, Kishimoto, Watanabe 2010 style; Chaslot et al. 2008): N independent trees, vote at root. Almost no synchronization, but wastes simulations.
- **Tree parallelization with virtual loss** (Chaslot/Winands/Bouzy 2008): one shared tree, virtual loss steers threads apart. Standard for ≤16 cores.
- **Lock-free tree (Enzenberger & Müller, ACG 2010)**: each thread has a small free-list for new nodes; only after children are fully initialized do they get linked to the parent. Standard for >16 cores.
- For shogi specifically, **a hybrid**: tree parallelization for UCT, a separate dedicated thread or two running df-pn co-process. This is what dlshogi does and it's the right pattern even without NN.

**Time management.** Standard MCTS approach: allocate a base budget per move (`T_total / expected_moves_left`); extend the budget if the principal variation is unstable or if the second-best child's visit count is close to the best's (Pepels & Winands time-management studies for MCTS). For shogi-specific byoyomi (per-move overtime after main time), keep a guard band (e.g., return current best move when 90% of byoyomi consumed).

**Realistic strength expectations.** A well-engineered classical MCTS shogi engine, with df-pn mate co-process, hand-tuned KPP-style eval for rollout cutoffs, MCTS-Solver, MCTS-Minimax hybrids, and heavy playouts, will likely reach 1-dan to 3-dan amateur on standard desktop hardware (Sato et al. 2010 baseline + 15 years of MCTS enhancements). It will be 1000–1500 Elo *below* a current YaneuraOu / Apery / dlshogi running on the same hardware. Don't compete with Stockfish-of-shogi; compete with yourself.

### 8. Alpha-beta vs MCTS in shogi: when does each make sense?

**Pick alpha-beta + heavy eval (Bonanza/YaneuraOu pattern) if** you want maximum playing strength without an NN. The 30+ years of accumulated tricks (futility pruning, null move, LMR, singular extensions, KPPT eval) are mature, and shogi's tactical density rewards the deep narrow search of αβ over the wide shallow sampling of MCTS.

**Pick classical MCTS if** any of the following apply:
1. **You want anytime, smoothly-improving move quality** (MCTS's principal advantage — at any moment, it has a current best with a confidence estimate).
2. **You want clean parallelization** on many cores without the complexity of YBWC/ABDADA.
3. **You want positional / strategic exploration** — MCTS' bias toward broader sampling helps in long, slow openings and endgames where αβ horizon effects bite (Sato et al. note their MCTS solved certain opening/endgame positions that conventional methods missed).
4. **You're building this as a learning project** and the algorithmic interest of MCTS+df-pn+Score-Bounded+Implicit-Minimax is the point.

**Don't pick classical MCTS if** you're chasing tournament strength. The literature is consistent: pure classical MCTS doesn't catch alpha-beta in shogi.

## Recommendations

**Stage 1 (week 1–4): Get the rules right.**
- 128-bit bitboard (`__m128i` union with `u64[2]`), pseudo-legal + legal move gen for all 14 piece types including drops, promotion options, *nifu* (no two unpromoted pawns same column), *uchifuzume* (no checkmate by pawn drop), legality of last-rank drops.
- Zobrist hashing with pieces-in-hand included; SFEN parser; USI protocol.
- Test: perft to known node counts at depth ≤ 5 from the start position, and at least one mid-game tactical position.

**Stage 2 (week 5–8): df-pn mate solver as a standalone module.**
- Implement df-pn per Nagai 2002 with: TT replacement strategy (SmallTreeGC per Nagai 1999b/Kishimoto 2010 in the Hoki et al. 2013 parallel-dovetailing paper), `1+ε` trick (Pawlewicz & Lew), df-pn(r) GHI fix (Kishimoto 2005), df-pn+ heuristic init (Nagai & Imai 1999). Restrict OR-node move generation to checking moves only; AND-node to evasions only.
- Test against published tsume-shogi problem sets (the Shogi Tsume Paradise / 詰将棋パラダイス collections); aim for solving classic mate-in-9 to mate-in-17 problems in <1 s, and at least one Choju-mate (long mate) of >30 plies.

**Stage 3 (week 9–12): MCTS skeleton.**
- UCT with UCB1-TUNED (Auer, Cesa-Bianchi, Fischer 2002), FPU = parent's eval minus a small reduction (start at 0.1–0.2 and tune), virtual loss, tree-parallel with lock-free per Enzenberger & Müller 2010.
- MCTS-Solver: proven-win/proven-loss propagation in negamax sense per Winands/Björnsson/Saito 2008.
- Score-Bounded MCTS (Cazenave & Saffidine 2010) extended for sennichite: nodes get `[pessimistic, optimistic]` bounds; sennichite/jishogi nodes have `[0.5, 0.5]` for non-checking-side, `[0, 0]` for the perpetual-checker.

**Stage 4 (week 13–16): Playout policy and evaluation.**
- Hand-crafted move-priority list for rollouts: (1) `mate_in_1`, (2) escape from check, (3) capture by SEE>0, (4) promote-and-not-lose-tempo, (5) check, (6) drop near opponent king, (7) Bradley-Terry softmax over move features. Use ε-greedy (ε=0.1) with NST or PPA.
- KPP-lite static evaluator with piece-square, king-safety, and pawn-structure terms. Train piece values + simple KPP from pro game databases via MMTO (Hoki & Kaneko 2014) — this is its own subproject; if too ambitious, hand-tune piece values and a king-safety penalty and accept lower strength.
- Static eval cutoff at 60 rollout plies via sigmoid `1/(1+exp(-eval/600))`.

**Stage 5 (week 17–20): Tactical integration.**
- df-pn co-process: dedicated thread runs df-pn on every leaf where the side-to-move can give check, with a 50k-node budget; results written back to MCTS via a proven-win/loss flag.
- Decisive-moves check at every rollout step (mate-in-1 detection).
- 2-ply αβ at MCTS leaves for the prior bias (Baier & Winands MCTS-IP).
- Implicit-minimax-backup secondary value per Lanctot et al. 2014.

**Stage 6 (week 21+): Tuning and benchmarks.**
- Self-play tournaments against earlier versions (CLOP, SPSA, or simple bisection on UCB1's C, FPU reduction, ε, α-mix for implicit minimax).
- External benchmark: play vs YaneuraOu at fixed 100ms/move. Expect 2–10% score (you will lose almost every game). Useful diagnostic, not a competitive yardstick.
- Tactical benchmark: re-run on Sato et al.'s style of 98-problem test sets. Anything above their 49/98 with modern enhancements is real progress.

**Triggers that would change the plan:**
- If your df-pn solves 90%+ of test tsume problems but MCTS still loses to YaneuraOu at 1% — your bottleneck is the static evaluator, not the search. Switch focus to MMTO/KPP training.
- If your nps in heavy playouts is below 500/sec single-threaded — your bitboard or move-gen is wrong; profile and fix.
- If sennichite handling causes infinite loops or wildly wrong values — your path-history hash isn't being included in the TT key; fix per Kishimoto 2005.
- If PN-MCTS-style proof-number injection (per Doe et al. 2022) into UCB outperforms vanilla UCB1 in self-play at short time controls, scale up the C_pn weight; if not, drop it — the MiniShogi result suggests it should help most when your playouts/sec is low.

## Caveats

- **Pure MCTS-without-NN in shogi is a known dead end strength-wise.** Sato/Takahashi/Grimbergen 2010 is the clearest published evidence; multiple Japanese GPW papers in the 2007–2014 window reached the same conclusion and the field moved to alpha-beta+heavy-eval (Bonanza, Apery, YaneuraOu) and then to NN (dlshogi). Decide explicitly whether you want strength or algorithmic interest before starting.
- **The PN-MCTS MiniShogi result is on 5×5 shogi, not 9×9.** The 5×5 game is far less tactical, has fewer drops, and shorter games; expect substantially diluted gains on full shogi. Sato et al. 2010 remains the only published full-shogi MCTS data point.
- **Yokoyama & Kitsuregawa (PRICAI 2014) needed 30–50× more compute** to beat their baseline. Their Bayesian distribution propagation is conceptually elegant but the cost makes it impractical on a CPU-only desktop.
- **Japanese-language GPW (Game Programming Workshop) papers are scattered**; many are behind IPSJ paywalls or only available as conference handouts. The English-translated subset (via ICGA Journal, ACG/CG/CGW conference series, IEEE TCIAIG/CIG) covers the major algorithms but not every Japanese refinement. If you read Japanese, the IPSJ digital library and JST J-STAGE archives have additional df-pn and shogi MCTS papers (Kakinoki, Yamashita, Yoshizoe, etc.).
- **Some results may not transfer between games.** RAVE/AMAF was a large gain in Go (Gelly & Silver 2011 show roughly doubling the win rate vs GnuGo level 10 on 9×9 Go at 3,000 simulations); in shogi it can be neutral or negative because move identity is more context-dependent. Implicit minimax backups (Lanctot et al. 2014) helped in Kalah, Breakthrough, LoA, but no one has published the result on full shogi; the *expectation* is that they help (shogi is more like LoA than Go), but expect surprises.
- **df-pn has known failure modes**: the "double-counting problem" on highly correlated sibling values (typical tsume-shogi sacrifice/interpose sequences; Kakinoki 2005) and infinite loops on DCGs if Kishimoto's GHI fix is omitted. Implement weak PNS (Ueda et al. 2008) or df-pn(r) (Kishimoto 2005) from the start to dodge both.
- **Time-management for MCTS in shogi byoyomi is under-researched.** Most published MCTS time-management work targets Go's fixed/Canadian time controls; shogi-specific per-move byoyomi may need adaptation.
- **FPU reduction values are domain- and engine-dependent.** The 0.1–0.2 figure originates from a Leela Zero (Go) self-play experiment (leela-zero issue #696); LeelaChessZero (lc0) for chess defaults to `--fpu-reduction 1.20`, which is 6× larger. For shogi, neither value is validated — treat it as a hyperparameter to tune via self-play.