# Prime Lattice Research: Roadmap

A structured guide to the study of the prime lattice defined by $p = 2^m + q^n$, organized as a research program. Each section motivates *why* the topic matters, marks the status of results (proven, open, speculative), and points toward concrete next steps.

---

## Part I: The Problem and Its Structure

### 1.1 The Fundamental Equation

We study the directed graph where an edge exists from $q \to P$ if there exist $m, n > 0$ such that $P = 2^m + q^n$. The value $k(P)$ denotes the in-degree of the prime $P$ (the number of parents).

A node is a prime (or prime power). Each edge carries the data $\{m, q, n\}$. For $r = 1$ this gives a planar DAG. The grading $r > 1$ (replacing $p$ with $p^r$) introduces the Universal Power Web, which is non-planar.

**Why this equation:** Fixing one base to 2 avoids the combinatorial explosion of general two-term prime power partitions. The structure is rich enough to exhibit non-trivial topology (non-planarity at $r > 1$, covering-system obstructions) while remaining computationally tractable up to billions of primes.

### 1.2 Node Classification

| Object Type | Property | $k(P)$ | Structural Role |
|---|---|---|---|
| Initial Object | $P = 2^m + q^n$ is unsatisfiable | 0 | Source; no ancestors |
| Simple Node | Unique parentage | 1 | Standard successor; often involves $q=3$ |
| Resonant Node | Multiple parents (e.g., $k=16$) | $\ge 2$ | Confluence of arithmetic coincidences |
| Terminal Sink | Prime powers $p^r$ ($r > 1$) | $\to 0$ | Genealogy dies out |

**Initial Objects (Sources, $k=0$):** ~17.3% of primes. Locked by Covering Systems: the small primes $\{3, 5, 7, \dots\}$ act as modular obstructions that collectively ensure no remainder $P - 2^m$ is a prime power. Every ancestral chain terminates at a $k=0$ prime.

**Terminal Objects (Sinks):** As $r$ increases, the Pillai Gap between $p^r$ and the nearest $2^m$ or $q^n$ grows. Prime powers become too sparse ($\sim X^{1/n}$) to bridge the gap, so $k(p^r) \to 0$.

### 1.3 Degree Notions for the Graded Web

- **Fiber Degree** $k_s(p^r)$: In-degree for a fixed power $p^r$ and parent-exponent $s$.
- **Local Degree** $k(p^r) = \sum_n k_n(p^r)$: Total in-degree at a specific power.
- **Total Degree** $K(p) = \sum_r k(p^r)$: Aggregate in-degree across all powers.

### 1.4 Empirical Landscape

*Status: Established computationally across ~1.17 billion primes.*

- $k$ distribution is approximately Poisson-shaped but with algebraic structure.
- At most 2 solutions $(m, n)$ per $(p, q)$ pair (S-unit equation bound; Evertse 1984). Only $p=11, q=3$ achieves exactly 2 (verified across 602M pairs).
- The backbone $\{3, 5, 7\}$ with multiplicative orders $\{2, 4, 3\}$ and period $\text{lcm}(2,3,4) = 12$ governs the obstruction structure.
- The mod-105 weighted average over $\mathbb{Z}/12\mathbb{Z}$ gap counts predicts the 17.6% obstruction rate exactly.

### 1.5 The Catalan Spine

*Status: Proven (Mihailescu 2002).*

The unique solution $3^2 - 2^3 = 1$ (Catalan's Conjecture) provides the only instance where a prime power and a power of 2 are consecutive. This creates a unique root for the $k=1$ spine at $P=3$. In cyclotomic fields, the torsion of the units is rigid enough that only this one "distance-1" solution exists, constraining how $k=1$ chains can form.

---

## Part II: The Planarity Question

*Central question: Can the $r=1$ lattice contain a $K_{3,3}$ minor?*

This is a concrete, falsifiable question. The proof architecture proceeds in phases, each eliminating a class of potential $K_{3,3}$ configurations. The phases are ordered by increasing sophistication; the earliest phases do the most work.

### Phase 1: The Constant-Power Collapse

*Status: Proven. Elementary.*

**Binary Uniqueness:** $2^a - 2^b = 2^c - 2^d$ with $a > b, c > d$ implies $a=c, b=d$. This is just uniqueness of binary representation.

**The $n=1$ Contradiction:** If two ancestors $(q_1, q_2)$ both connect to two descendants $(p_1, p_2)$ using constant exponent $n=1$:
$$p_1 - p_2 = 2^{m_{11}} - 2^{m_{21}} = 2^{m_{12}} - 2^{m_{22}}$$
Binary uniqueness forces the powers of 2 to match, canceling them and leaving $q_1 = q_2$. Contradiction.

**Consequence (Fluctuation Mandate):** Ancestors sharing multiple descendants must use varying exponents. At least one edge per shared pair must use $n \ge 2$, which limits eligible ancestors to $q \le \sqrt{p}$.

### Phase 2: The Multiplicative Order Jump

*Status: Proven. Elementary number theory (coprimality + Euler's totient).*

This is the core engine. It shows that attempting to replicate a shared-ancestor configuration forces double-exponential separation.

**Setup:** Suppose $p$ is shared by ancestors $q_1, q_2$ with binary spacing:
$$2^m - 2^{m'} = q_2^b - q_1^a = \Delta$$

To build $K_{3,3}$, a second descendant $p'$ must share these ancestors with the same spacing. Binary uniqueness forces:
$$q_1^a(q_1^{a'-a} - 1) = q_2^b(q_2^{b'-b} - 1)$$

Since $\gcd(q_1, q_2) = 1$, the term $q_1^a$ must divide $(q_2^{b'-b} - 1)$:
$$q_2^{b'-b} \equiv 1 \pmod{q_1^a}$$

The multiplicative order gives $b' - b \ge \phi(q_1^a) \ge q_1^{a-1}(q_1 - 1)$, so:
$$p' \approx q_2^{b'} > q_2^{q_1^{a-1}(q_1-1)}$$

**Example:** For $q_1^a \approx 1000$, the next alignment requires $b' > 1000$, giving $p' \approx 5^{1000}$. This is a double-exponential ejection from any finite search region.

### Phase 3: Closing the Gap (THE OPEN PROBLEM)

*Status: Open. This is where the proof program needs new ideas.*

Phase 2 proves that a second shared descendant $p'$ must be astronomically larger than $p$. Phase 3 must prove that solutions at such sizes are impossible. This requires an *effective upper bound* $B(q_1, q_2)$ such that no shared descendant can exist above $B$.

#### Why Baker bounds are insufficient

Baker's theorem (via Matveev 2000) gives bounds on linear forms in logarithms. Applied to the Pillai equation $|q_2^b - q_1^a - (2^m - 2^{m'})| = 0$, it yields a computable $B(q_1, q_2)$. The problem is the *size* of $B$:

- The general Matveev constant is $C \sim 10^7$ in the exponent.
- For $(q_1, q_2) = (3, 5)$: $B(3,5) \sim 2^{10^7}$, a number with millions of digits.
- Even the sharper Laurent-Mignotte-Nesterenko two-logarithm specialization only reduces this to roughly $2^{100}$.

These bounds are mathematically valid but computationally useless. They leave a gap between the search frontier (~$2^{37}$) and the provable bound (~$2^{10^7}$) that cannot be bridged by brute force.

#### More tractable approaches to explore

**1. Specific-base Pillai results.** For the base pair $(2, 3)$, de Weger (1989) completely solved $|2^m - 3^n| = c$ for all $|c| \le 100$. Stroeker-Tijdeman and Scott-Styer (2006) extended this to other small base pairs. The spacings $\Delta = 2^{m'}(2^d - 1)$ in the Phase 2 argument are Mersenne-structured. It may be possible to resolve the finitely many relevant Pillai equations for small $q_1, q_2$ directly, rather than invoking general Baker bounds.

**2. Covering-system / modular approach.** The covering-system analysis (Part IV) already provides algebraic proofs for obstruction classes. A similar strategy for the planarity question: rather than bounding the size of solutions, show that the required configurations are *algebraically impossible* in the relevant residue classes. This bypasses transcendence theory entirely and works within the specific arithmetic of the problem.

**3. Primitive divisor theory.** Zsygmondy's theorem (1892) guarantees that $a^n - b^n$ has a prime divisor not dividing $a^k - b^k$ for any $k < n$ (with finitely many exceptions). Applied to the Mersenne structure $2^d - 1$, this constrains which primes can divide the spacing $\Delta$, potentially ruling out configurations that Phase 2 doesn't reach.

**4. Computational extension + tightening analysis.** The scaling analysis (Section 2.7) shows that the mathematical vise tightens as $p$ grows: the ancestor pool grows exponentially faster than the slot count. Rather than proving an absolute bound, it may suffice to push the computational frontier to $2^{50}$ or $2^{64}$ and combine with the tightening rate to establish impossibility beyond the frontier.

**5. Hybrid approach.** Use Baker bounds only to show finiteness (which they can do), then resolve the finite residual computationally. The difficulty is that "finite" in Baker's sense means $< 2^{10^7}$, which is still too large. But specific-base results (approach 1) or modular arguments (approach 2) may dramatically reduce the residual.

### Phase 4: The 2-Adic Vice

*Status: Proven, conditional on Phase 3. Closes asymmetric loopholes.*

This ensures no configurations escape by mixing a massive power of 2 with a massive prime power.

**Mersenne Factorization:** The binary gap factors as $2^{m'}(2^{m-m'} - 1) = q_2^b - q_1^a$, forcing $v_2(q_2^b - q_1^a) = m'$.

**2-adic Baker Bound (Yu Kunrui 2007):** $v_2(q_2^b - q_1^a) \le C \cdot \log(\max(a, b))$, giving $m' \le C \cdot \log(\log p)$.

**Asymmetric Squeeze:** Large $m'$ ($\approx \log p$) violates the 2-adic bound. Small $m'$ with large prime powers triggers the Archimedean case.

### The Three-Pronged Vise (Exhaustive Case Analysis)

*Status: The case analysis is complete; each case reduces to Phase 3.*

For a shared descendant $p = 2^m + q_1^a = 2^{m'} + q_2^b$ with $m > m'$:

**Scenario 1 (Both $m, m'$ large):** $q_1^a, q_2^b$ are small, so $v_2(q_2^b - q_1^a) = m' \approx \log p$. Violates 2-adic Baker bound. *Impossible.*

**Scenario 2 (Both $m, m'$ small):** $q_1^a \approx q_2^b \approx p$, so $q_2^b - q_1^a$ is a small constant. Pillai's conjecture (proven for fixed bases via Baker's method): two prime powers cannot maintain a bounded gap as they grow. *Impossible for large $p$.*

**Scenario 3 ($m$ large, $m'$ small):** $2^m - q_2^b = 2^{m'} - q_1^a = \text{small constant}$. Baker bounds $|2^m - q_2^b|$ away from any fixed constant. *Bounded and finite.*

No safe zone exists: large $m$ triggers the 2-adic bound; small $m$ triggers the Archimedean bound.

### 2.7 Scaling Analysis

*Status: Empirically established. Quantifies the tightening.*

The $\sqrt{p}$ bound on ancestors (from Phase 1) is dataset-relative. With $p_\text{max} \approx 2^{37.1}$, the ancestor ceiling was $q \le 2^{18.55} \approx 2^{19}$. At $p_\text{max} = 2^{40}$, it becomes $q \le 2^{20}$.

As $p$ scales from $2^{37}$ to $2^{40}$:
- $2^m$ slot count grows from 37 to 40 (8.1% increase).
- Eligible ancestor pool roughly triples.
- Intersection probability drops geometrically because the pool grows exponentially faster than slots.

| Rule | Condition | Consequence |
|---|---|---|
| $n=1$ Collapse | $2^a - 2^b$ uniquely determined | Constant-exponent connections banned |
| Fluctuation Mandate | Exponents must vary | Ancestors limited to $q \le \sqrt{p}$ |
| Capacity Squeeze | $k(p) \le \lfloor \log_2 p \rfloor$ | High $k$ needs massive $p$, diluting shared parentage |
| Mersenne Lock | $2^{m'}(2^{m-m'} - 1) = q_2^b - q_1^a$ | Diophantine gaps are Mersenne-structured |

---

## Part III: The Obstruction Question

*Central question: Why are ~17.3% of primes unreachable? What is the mechanism?*

### 3.1 Covering Systems

*Status: Proven for 22 unconditional obstruction classes. The covering-system characterization is empirically complete for all tested primes.*

An obstructed prime $p$ ($k=0$) means: for every $m \in [1, \lfloor \log_2 p \rfloor]$, the remainder $p - 2^m$ has at least two distinct prime factors (never a prime power).

**The propagation rule:** If $q \mid p - 2^k$ and $\text{ord}(2, q) \mid (m - k)$, then $q \mid p - 2^m$. Each small prime $q$ "poisons" an arithmetic progression of $m$-values with common difference $\text{ord}(2, q)$.

An obstructed prime is one where these progressions cover all valid $m$:
$$[1, \lfloor \log_2 p \rfloor] \subseteq \bigcup_i \{k_i + j \cdot \text{ord}(2, q_i) : j \ge 0\}$$

**The backbone** $\{3, 5, 7\}$ with orders $\{2, 4, 3\}$:
- $q=3$: covers one parity (6/12 positions mod 12)
- $q=5$: covers one mod-4 class (3/12 positions)
- $q=7$: covers one mod-3 class (4/12 positions, or 0 if inactive)
- Period: $\text{lcm}(2, 3, 4) = 12$

### 3.2 The Mod-255255 Analysis

*Status: Computed across 6.4 billion primes.*

Extending to $\{3, 5, 7, 11, 13, 17\}$ with modulus $255255 = 3 \cdot 5 \cdot 7 \cdot 11 \cdot 13 \cdot 17$ gives 92,160 residue groups. Residual gap distribution:

| Gaps remaining | Primes | Obstruction rate |
|---|---|---|
| 0 | 3.40B | 21.96% |
| 1 | 150M | 42.48% (highest) |
| 2 | 976M | 20.37% |
| 6 | 300M | 1.29% (lowest) |

The 1-gap class has the highest rate: when only one $m$ escapes the covering, there's only one chance at a prime power remainder.

**22 Unconditional Obstruction Classes:** For 22 residue classes mod 255255 (1.53M primes total), every prime is obstructed. All 22 have $q=3$ and $q=11$ covering opposite parities, so $\{3, 11\}$ alone cover all of $\mathbb{Z}/12\mathbb{Z}$. The jump from ~97% to 100% is sharp---no near-100% groups exist.

### 3.3 Subgroup Exclusion Proof

*Status: Proven for the 22 classes.*

At single-coverage positions (always covered by $q=3$ alone), the proof shows $p - 2^m \neq 3^n$ by exhibiting a blocking prime $\ell$ where:
$$(p - 2^m) \bmod \ell \notin \langle 3 \rangle \subset (\mathbb{Z}/\ell\mathbb{Z})^*$$

Since $3^n \bmod \ell \in \langle 3 \rangle$ for all $n$, this is a contradiction. The blocking primes needed: $\{13, 37, 41, 61, 67, 73, 181, 193\}$. Most positions are blocked by $\ell = 13$ alone (where $\text{ord}(3, 13) = 3$, excluding 75% of residues).

### 3.4 The Local-Global Question

*Status: Open. This is the next frontier.*

**Structurally obstructed primes** have their obstruction forced by congruence conditions alone (the covering system + subgroup exclusion). **Mysteriously obstructed primes** are locally unobstructed at every $\ell$ but globally obstructed---their obstruction has a non-local origin.

**Open questions:**
- What fraction of obstructed primes are structurally explained by covering systems?
- Do any obstructed primes survive all local checks? (These would be analogous to Grunwald-Wang failures.)
- Does the bimodality in obstruction rates (continuous 1-90%, gap, then sharp 100%) reflect a categorical distinction between probabilistic and algebraically forced regimes?

### 3.5 Power Residue Symbols and $\ell$-adic Analysis

*Status: Framework established, implementation in progress.*

The question "Is $p - 2^m$ an $n$-th power?" decomposes $\ell$-adically into:
1. $v_\ell(p - 2^m) \equiv 0 \pmod{n}$ (valuation divisible by $n$)
2. The unit part $(p - 2^m)/\ell^{v_\ell}$ is an $n$-th power in $\mathbb{Z}_\ell^*$

Part (2) depends on the group structure of $(\mathbb{Z}/\ell\mathbb{Z})^*$: the unit $u$ is an $n$-th power iff $u^{(\ell-1)/\gcd(n, \ell-1)} \equiv 1 \pmod{\ell}$.

**Key insight:** The informative filtration primes are exponent-dependent:
- $n=2$ (squares): every odd $\ell$ works
- $n=3$ (cubes): need $\ell \equiv 1 \pmod{3}$, so $\ell = 7, 13, 19, 31, \dots$
- $n=5$: need $\ell \equiv 1 \pmod{5}$, so $\ell = 11, 31, 41, \dots$

The prime $\ell = 3$ can *never* detect a cube obstruction because $|(\mathbb{Z}/3\mathbb{Z})^*| = 2$.

### 3.6 Euler Product and Distribution

The $k(P)$ distribution is shaped by the global pressure of the Euler Product:

- **Covering Systems as Siphons:** Small primes siphon off most potential solutions, creating $k=0$ obstructions.
- **Zipf structure:** The "word length" of integer factorization follows Zipf's Law; a hit ($k \ge 1$) is a rare reduction in descriptive complexity.
- **Resonance:** A $k=16$ prime is a confluence where additive pressure overcomes covering-system obstructions at sixteen points simultaneously.
- **Baker circle visualization:** Projecting $\theta(n) = \text{frac}(\log_2(p - q^n))$ onto the unit circle shows how close $\theta$ approaches 0 (the solution point). Baker's theorem constrains the minimum approach distance.

---

## Part IV: Algebraic Geometry Framework

*Status: Speculative / interpretive. This section maps the problem onto arithmetic geometry language as a candidate formalization. None of this is proven to apply rigorously; it is a research direction, not an established framework.*

**Why pursue this:** The covering-system analysis (Part III) shows that local congruence conditions explain the obstruction structure. Arithmetic geometry provides a systematic language for local-global phenomena---the same local-global interplay governing the $k=0$ primes. If the formalization succeeds, it would connect the problem to deep structural results (etale cohomology, Brauer-Manin obstruction) that could resolve open questions.

### 4.1 The Relative Perspective

Treat the set of primes as a base space $Y = \text{Spec } \mathbb{Z}$. The equation $p = 2^m + q^n$ defines (at least conceptually) a relative scheme $\mathcal{X} \to Y$:

- Each prime $p$ is a closed point in $Y$.
- The fiber $\mathcal{X}_p$ contains all solutions $(m, q, n)$ for that prime.
- $k(P)$ is the "number of rational points" in the fiber.
- Initial Objects ($k=0$) are points where $\mathcal{X}$ has empty fiber.
- The covering system is a Global Obstruction: $\pi: \mathcal{X} \to Y$ fails to be surjective onto $k=0$ points.

**What this buys:** Framing $k=0$ as "empty fiber" and the covering system as "obstruction sheaf" connects to the Brauer-Manin obstruction (a standard tool for explaining failures of the local-global principle in Diophantine geometry).

### 4.2 The Tate Module and $\ell$-adic Tower

The $\ell$-adic analysis from Section 3.5 can be organized as an inverse system. Define $T_\ell = \varprojlim (\text{Congruence Classes mod } \ell^\nu)$, tracking the survival of a potential solution across the $\ell$-adic tower.

- **Local success at level $\nu$:** $P - 2^m$ is an $n$-th power residue in $\mathbb{Z}/\ell^\nu\mathbb{Z}$.
- **Torsion:** If the module of potential solutions vanishes at finite level $\nu$, the prime is locally obstructed at $\ell$.
- **Obstruction depth:** The level $\nu$ where lifting first fails. This is directly computable via Hensel lifting in SageMath's $p$-adic rings.

**What this buys:** A systematic way to organize the power-residue-symbol computations and detect obstructions at increasing precision. The "Tate module" language is aspirational---there is no actual abelian variety here---but the projective system of congruence conditions is real and computable.

### 4.3 The Kummer Sequence and Etale Cohomology

The Kummer sequence $1 \to \mu_n \to \mathbb{G}_m \xrightarrow{[n]} \mathbb{G}_m \to 1$ connects $n$-th roots of unity to the $n$-th power map. In principle:

- $H^1_\text{et}(Y, \mu_n)$ classifies obstructions to finding global $n$-th roots.
- When $k(P) \ge 1$, a section of $\mathbb{G}_m$ can be "lifted" through the etale cover $[n]$.
- When $k(P) = 0$, the cohomological obstruction is non-zero.

**What this would buy:** If the $k=0$ obstruction can be expressed as a non-trivial class in $H^1_\text{et}$, it would connect the covering-system structure to the broader machinery of arithmetic duality and potentially allow computation of obstruction rates from cohomological invariants.

### 4.4 Grunwald-Wang and Ghost Sections

The Grunwald-Wang Theorem describes specific failures of the local-global principle for $n$-th powers, particularly at the prime 2 for $n$ divisible by 8.

**Relevance:** If an obstructed prime passes all local power-residue checks (locally unobstructed at every $\ell$) but remains globally obstructed, this is a Grunwald-Wang-type phenomenon. Detecting such primes would be strong evidence that the scheme-theoretic formalization captures real structure.

### 4.5 Cyclotomic Fields and Covering Systems

The covering system $\{3, 5, 7, \dots\}$ connects to cyclotomic fields:

- The covering primes generate the modular torsion; they define forbidden zones on the unit circle ($e^{2\pi i x}$).
- Each partition $q^n = P - 2^m$ can be viewed as an element in $\mathbb{Q}(\zeta_n)$.
- The class group complexity of cyclotomic fields grows with $n$, eventually preventing $k \ge 1$ alignments.

### 4.6 Distinguished Open Subsets and Localization

In $\text{Spec } \mathbb{Z}$, the distinguished open subset $D(f)$ inverts the prime factors of $f$. If the Covering System uses $S = \{3, 5, 7\}$, the natural open set is $D(105)$, where obstructions from 3, 5, 7 are "turned off."

The structure sheaf $\mathcal{O}_Y$ assigns $\mathbb{Z}_f$ to $D(f)$. The prime powers $q^n$ are sections of $\mathbb{G}_m$ over $Y$. The $k(P)$ counts measure the space of sections of a sheaf $\mathcal{F}$ satisfying $s = P - 2^m$.

### 4.7 Gluing: From Local to Global

Local affine pieces ($\text{Spec } \mathbb{Z}_p$) are glued along the generic point ($\mathbb{Q}$) to form the general scheme $\mathcal{X}$. Evaluating the Frobenius action on the cohomology of the glued pieces measures the "curvature" of the prime landscape: high curvature gives $k=0$; aligned curvature gives high $k$.

### 4.8 Research Program for Formalization

1. **Compute obstruction depth** at exponent-appropriate primes for a sample of obstructed primes (SageMath $p$-adic rings).
2. **Search for locally-unobstructed-but-globally-obstructed primes.** If none exist, the covering system may be the complete obstruction mechanism. If they exist, the scheme-theoretic framework gains immediate justification.
3. **Express the covering system as a sheaf cohomology class.** Can the mod-255255 obstruction structure be recovered from $H^1_\text{et}$?
4. **Connect the Frobenius action to $k$ values.** For $k=16$ primes, check whether the $\ell$-adic valuations $v_\ell(P - 2^{m_i})$ share structure suggesting a common symmetry.

---

## Part V: Computational Architecture

### 5.1 Bit-Weight Filtration

*Key principle: Do not bound $r$ globally. Do not bound $m$ globally. Bound their product.*

The bit-length of $p^r$ is $W(p^r) = r \cdot \lfloor \log_2 p \rfloor$. Set a global Maximum Bit-Weight $M_\text{max}$. Each prime gets:
$$r_\text{max}(p) = \left\lfloor \frac{M_\text{max}}{\log_2 p} \right\rfloor$$

Nodes with $W > M_\text{max}$ do not exist in the current topological space. The complex $\mathcal{X}_{\le M_\text{max}}$ is mathematically complete up to that weight.

**Implementation:**
1. Pick $M_\text{max}$ (e.g., 64 for u64, 128 for lightweight arbitrary precision).
2. For each prime $p$: if $r_\text{max} < 1$, skip. Otherwise loop $r$ from 1 to $r_\text{max}$.
3. Apply the same horizon to ancestors: $s \cdot \log_2 q \le M_\text{max}$.

Small primes get deep fibers ($r = 1, 2, 3, 4, 5$); massive primes get shallow fibers ($r=1$ only).

### 5.2 Data Structure Architectures

The graph is large but exponentially sparse. Two complementary approaches:

**Stalk-and-Fiber Model** (Document / Object-Oriented): Base prime $p$ is the primary key; powers are the fiber. Good for backward traversal (finding parents).

```json
{
  "base_prime": 137438953481,
  "is_universal_source": false,
  "total_degree_D": 3,
  "power_fiber": {
    "1": {
      "k_degree": 1,
      "incoming_edges": [
        {"parent_q": 3, "parent_s": 2, "m_offset": 37}
      ]
    },
    "2": {
      "k_degree": 2,
      "incoming_edges": [
        {"parent_q": 5, "parent_s": 1, "m_offset": 14},
        {"parent_q": 11, "parent_s": 3, "m_offset": 8}
      ]
    },
    "3": { "k_degree": 0, "incoming_edges": [] }
  }
}
```

**Edge-First Columnar Model** (Relational / Vectorized): Edges are first-class citizens. Good for statistical sweeps and bi-directional querying.

| Child_p | Child_r | Parent_q | Parent_s | offset_m |
|---|---|---|---|---|
| 137438953481 | 1 | 3 | 2 | 37 |
| 137438953481 | 2 | 5 | 1 | 14 |
| 41 | 1 | 3 | 2 | 5 |
| 41 | 1 | 5 | 2 | 4 |

**Optimizations:**
- $k=0$ primes in a separate Bitset/Bloom Filter.
- Never store $p^r$ or $q^s$ if they exceed 64 bits. Store generators; compute dynamically.
- Typing: $p, q$: u64. $m, r, s$: u8 or u16.

### 5.3 Parquet/Polars/DuckDB Schema

Never store evaluated integers in Parquet. Store only generators: `child_p` (u64), `child_r` (u16), `parent_q` (u64), `parent_s` (u16), `offset_m` (u16). Rug constructs massive integers in CPU cache/RAM for arithmetic checks, then drops them.

**DuckDB partitioning:** Primary by `child_r`, secondary by `parent_s`. Total Degree $D(p)$ in a separate materialized view. Query flat planar slices (`WHERE child_r = 1 AND parent_s = 1`) without scanning skew edges.

### 5.4 Rust Plugin: Mersenne Pre-Filter

Before Rug allocates arbitrary-precision memory:
1. Compute $\Delta = 2^m - 2^{m'}$.
2. If $\Delta$ is odd, reject.
3. $v_2(\Delta)$ via `trailing_zeros()`.
4. If $v_2(\Delta)$ exceeds expected bounds, reject.

### 5.5 TDA Memory Management

Persistent homology is inherently global---a hole can span any partition boundary. The Vietoris-Rips combinatorial explosion ($2^n$ simplices for $n$-cliques) is the main threat.

**Smart Sampling:**
- *Witness Complex:* Pick Landmarks ($k \ge 5$ and $k=0$ nodes). A simplex is included only if a Witness exists close to all vertices. Reduces 100K nodes to ~1K complex.
- *Sparse Rips (Sheehy):* Metric hierarchy that deletes redundant nodes. Guarantees polynomial edges.

**Distributed (Mayer-Vietoris):** Chunk into overlapping regions. Workers compute local persistence; master glues via spectral sequences.

**GPU (RTX 3080):**
- Wins: parallel metric generation, Ripser-GPU/PHAT matrix reduction.
- Fails: don't build simplicial complexes in VRAM.

**Optimal pipeline:** Farthest Point Sampling (Polars) → GPU metric evaluation → Witness Complex (Rust) → Dimension cap at 2 ($H_0$, $H_1$ only).

### 5.6 Topology Crate Comparison

| Feature | cova-space | amari-topology |
|---|---|---|
| Best for | Sheaf theory, categorical modeling | Morse theory, formal verification |
| Architecture fit | Brauer-Manin obstruction as sheaf cohomology | Morse collapse to save RAM |
| Data handling | Flexible cell complexes | Strict, verified simplicial complexes |
| Pipeline fit | Easier dynamic Polars piping | Phantom Type boilerplate required |

**amari-topology** has built-in Discrete Morse Theory: use grading $r$ or offset $m$ as a Morse function to collapse the complex before computing homology. Also consider **lophat** for lockfree persistent homology.

---

## Research Priorities (Ordered)

### Tier 1: Immediate

1. **Extend covering-system analysis to planarity.** Can the modular machinery that proves obstruction classes also rule out $K_{3,3}$ configurations? The Phase 2 coprimality argument is already modular in nature; the covering-system framework may close the gap without transcendence theory.

2. **Search for locally-unobstructed-but-globally-obstructed primes.** This is the key test for whether the covering system is the complete obstruction mechanism or whether deeper structure exists.

3. **Compute power residue symbols** at exponent-appropriate primes for a sample of obstructed primes. Compare local obstruction rates vs naive $v_\ell$ approach.

### Tier 2: Medium-term

4. **Resolve specific-base Pillai equations** for the spacing values $\Delta$ arising in Phase 2. For $(q_1, q_2) \in \{(3,5), (3,7), (5,7)\}$, the relevant equations may be directly solvable using existing literature (de Weger, Scott-Styer).

5. **Exploit primitive divisor theory** (Zsygmondy/Bang) to constrain the Mersenne structure $2^d - 1$ in the spacing argument.

6. **Implement the graded web** ($r > 1$) in the Parquet schema and hunt for $K_{3,3}$ across the non-planar extension.

### Tier 3: Longer-term

7. **Express covering-system obstruction as sheaf cohomology.** If successful, this connects the problem to arithmetic duality and potentially computes obstruction rates from cohomological invariants.

8. **Investigate the bimodality** in obstruction rates (continuous 1-90% → gap → sharp 100%). Does this reflect a categorical boundary between probabilistic and algebraically forced regimes?

9. **$q$-adic tower / Iwasawa-style analysis.** Study the limit behavior of obstruction depth as $\ell^\nu \to \infty$.

---

## Glossary

**$\ell$-adic valuation ($v_\ell$):** The number of times a prime $\ell$ divides an integer. $v_2(24) = 3$ because $24 = 2^3 \cdot 3$.

**$K_{3,3}$:** The complete bipartite graph on $3+3$ vertices. By Kuratowski's theorem, containing a $K_{3,3}$ minor implies non-planarity.

**Ancestor / Parent ($q$):** A prime such that $p = 2^m + q^n$ for some $m, n$.

**Archimedean:** Relating to the standard absolute value on $\mathbb{R}$ (as opposed to $p$-adic valuations). Archimedean bounds concern physical sizes.

**Baker's Theorem:** Effective lower bounds on $|\Lambda|$ for linear forms $\Lambda = b_1 \log \alpha_1 + \dots$ in logarithms of algebraic numbers. Prevents exponential curves from getting arbitrarily close. Matveev (2000) is the standard modern reference.

**Betti number ($H_n$):** Counts $n$-dimensional holes. $H_0$ = components, $H_1$ = loops, $H_2$ = voids.

**Bit-Weight Filtration:** Filtration by $W(p^r) = r \cdot \lfloor \log_2 p \rfloor$ with ceiling $M_\text{max}$.

**Capacity Squeeze:** $k(p) \le \lfloor \log_2 p \rfloor$ because $m$ ranges over $\{1, \dots, \lfloor \log_2 p \rfloor\}$.

**Catalan Point:** The unique solution $3^2 - 2^3 = 1$ (Mihailescu 2002).

**Confluence Point:** A prime with high $k$ (e.g., $k=16$).

**Covering System:** A collection of arithmetic progressions $\{a_i \pmod{n_i}\}$ covering every integer. Here, small primes generate progressions of "poisoned" $m$-values via their multiplicative orders $\text{ord}(2, q)$.

**Cyclotomic Field ($\mathbb{Q}(\zeta_n)$):** $\mathbb{Q}$ adjoined with a primitive $n$-th root of unity.

**DAG:** Directed Acyclic Graph. The prime lattice is a DAG because $p^r > q$.

**Descendant:** A prime $p$ writable as $p = 2^m + q^n$.

**Distinguished Open Subset ($D(f)$):** In $\text{Spec } \mathbb{Z}$, the primes not dividing $f$.

**Double-Exponential Ejection:** Coprime factorization forces next alignment exponent to exceed the previous prime power: $p' > q_2^{q_1^a}$.

**Etale Cohomology ($H^1_\text{et}$):** Algebraic-geometric analog of singular cohomology. $H^1_\text{et}(Y, \mu_n)$ classifies obstructions to global $n$-th roots.

**Evertse's Theorem (1984):** An S-unit equation in two variables has at most $3 \cdot 7^{|S|+1}$ solutions. Applied here: at most 2 solutions $(m,n)$ per $(p,q)$.

**Fiber ($\mathcal{X}_p$):** The set of solutions $(m, q, n)$ above a specific prime $p$.

**Fluctuation Mandate:** Ancestors sharing descendants must use varying exponents (consequence of binary uniqueness).

**Frobenius Element ($\text{Frob}_p$):** Canonical generator of the Galois group of a finite field extension.

**Ghost Section:** A section existing locally at every $\ell$ but not globally. Detected via Grunwald-Wang.

**Grunwald-Wang Theorem:** Describes failures of the local-global principle for $n$-th powers, particularly for $n$ divisible by 8 at the prime 2.

**Kummer Sequence:** $1 \to \mu_n \to \mathbb{G}_m \xrightarrow{[n]} \mathbb{G}_m \to 1$.

**Laurent-Mignotte-Nesterenko (1995):** Sharper Baker-type bounds specialized to two logarithms.

**Mersenne Lock:** The factorization $2^{m'}(2^{m-m'} - 1) = q_2^b - q_1^a$.

**Multiplicative Order ($\text{ord}(a, m)$):** Smallest $k > 0$ with $a^k \equiv 1 \pmod{m}$.

**Obstruction Depth:** The level $\nu$ where Hensel lifting of $P - 2^m = q^n$ first fails in $\mathbb{Z}/\ell^\nu\mathbb{Z}$.

**Persistent Homology:** Tracks topological features across a filtration parameter, recording birth and death of each feature.

**Pillai's Conjecture:** $|a^x - b^y| \to \infty$ for fixed $a, b$. Proven for fixed bases via Baker's method.

**Power Residue Symbol:** For unit $u \in \mathbb{Z}_\ell^*$, $u$ is an $n$-th power iff $u^{(\ell-1)/\gcd(n,\ell-1)} \equiv 1 \pmod{\ell}$.

**Primitive Divisor (Zsygmondy 1892):** $a^n - b^n$ has a prime divisor not dividing $a^k - b^k$ for $k < n$, with finitely many exceptions.

**Relative Scheme ($\pi: \mathcal{X} \to Y$):** A scheme $\mathcal{X}$ with morphism to base $Y$. Fibers $\mathcal{X}_p$ form the family of interest.

**Scott-Styer (2006):** Complete solutions to $|p^a - q^b| = c$ for specific small base pairs.

**Shorey-Tijdeman (1986):** *Exponential Diophantine Equations*. Systematic application of Baker's method to equations involving exponentials. Source of the "Shorey-Tijdeman constants" (effective but impractically large).

**Skew Edge:** An edge where child and parent use different power gradings ($r \neq s$).

**Structure Sheaf ($\mathcal{O}_Y$):** Assigns $\mathbb{Z}_f$ to each open set $D(f)$.

**Tate Module ($T_\ell$):** Inverse limit $\varprojlim$ of $\ell$-power torsion. Here used (aspirationally) for the projective system of congruence conditions.

**Terminal Object (Sink):** $p^r$ with $r > 1$ where $k(p^r) = 0$.

**Total Degree ($K(p)$):** $\sum_r k(p^r)$.

**Universal Power Web:** The full graph including all $p^r$ for $r \ge 1$.

**Vietoris-Rips Complex:** Simplicial complex connecting all points within distance $\epsilon$; combinatorially explosive ($2^n$ simplices for $n$-cliques).

**Vojta Conjectures:** Predict finiteness of rational points on varieties of "general type."

**Witness Complex:** Sparse VR approximation using Landmark/Witness decomposition.

**Yu Kunrui (2007):** $p$-adic analog of Baker's theorem. Bounds $v_p(q_2^b - q_1^a) \le C \cdot \log(\max(a,b))$.

**de Weger (1989):** Complete solution of $|2^m - 3^n| = c$ for $|c| \le 100$.
