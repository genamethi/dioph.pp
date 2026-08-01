# chains as a graded Hopf algebra (thread to return to)

Status: **open / promising, not pinned.** Raised 2026-07-22 while choosing the
`pp-graph` data representation. We chose to build the trie store first and let the
data guide, but the algebra decides how clean the store is, so it must be
revisited.

## The object

A chain's word is a composition of the maps `f_i : t -> t^{n_i} + C_i` (with the
`n=1` steps folding as translations `t -> t + 2^m`). So chains live in the Hopf
algebra of the **composition group** of these maps (the Faà-di-Bruno / ladder
side of Connes-Kreimer).

- **Coproduct = cut the composition** into `upper ⊗ lower`
  (`f_r∘…∘f_{k+1}) ⊗ (f_k∘…∘f_1`). For linear chains this is **deconcatenation**,
  and it is exactly the **parent-link trie**: walking down the links iterates the
  coproduct's lower leg. The store *is* the coproduct. Non-cocommutative, which
  is why `[3,2] ≠ [2,3]` (observed at 1e6: 105K vs 58K).
- **Antipode = compositional inverse** `W ↦ W⁻¹` (alternating sum over cuts, a
  Möbius inversion on the chain poset). This is why the **root is derivable for
  free**: `q = W⁻¹(p)`, and because every `f_i` is strictly increasing for `t>0`,
  `W` is monotone so `W(q)=p` has a unique positive solution (degree ≤ 26, one
  integer root). We therefore store `(node, word)` and derive the root, never
  store it.
- **Grading.** Length `r` (number of `n≥2` segments) is the additive CK-style
  grade; degree `∏ n_i` is a multiplicative filtration on top. The `n=1`
  translations are **grade-0** (touch neither). This is a translation group acting within
  a graded piece. 39% of words at 1e6 are degree-1 pure translations = the grade-0
  orbit.
- **Count** is a multiplicity on the *comodule* (`(node, word)` incidence),
  orthogonal to the algebra. Dropped from the core.

## Solid vs. to pin

Solid: monotone ⇒ unique derivable root; linear chains ⇒ deconcatenation/ladder
Hopf algebra; antipode = inverse = root recovery.

To pin:
1. The **exact coproduct on the `C`-translations**: how the shifts distribute
   across a cut. This is where composition is *not* free deconcatenation (a cut
   must carry the intermediate value). This one decides whether the store is a
   clean graded Hopf algebra or "only" a graded coalgebra.
2. Whether the **branching DAG** lifts to full Connes-Kreimer forests, or stays a
   comodule over the ladder Hopf algebra.

Payoff if it pins: word store = the graded Hopf algebra, trie = the coproduct,
antipode = the free root, injectivity (word↔Hermite, empirically confirmed at
1e6) = the algebra being connected/graded.
