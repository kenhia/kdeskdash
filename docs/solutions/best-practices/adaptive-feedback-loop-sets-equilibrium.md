---
title: "An adaptive feedback loop sets the equilibrium, not your per-parameter tuning"
date: 2026-06-27
category: docs/solutions/best-practices
module: GoLZ machetes balance (src/modes/golz.c, src/golz.c, tools/golz_mc.c)
problem_type: best_practice
component: game_balance
severity: medium
applies_when:
  - A system has a self-adjusting control variable updated by fixed steps on
    win/loss (difficulty sliders, rate limiters, backoff, ELO-like ratings)
  - You are also hand-tuning other parameters to hit a target outcome ratio
  - You are about to monte-carlo a big parameter grid to "find the 50/50 settings"
related_components:
  - golz mode
  - monte-carlo harness
tags: [balance, control-loop, equilibrium, simulation, monte-carlo, tuning]
---

# An adaptive feedback loop sets the equilibrium, not your per-parameter tuning

## Context

GoLZ machetes added a self-balancing difficulty knob: the Humans win by lasting
`generations_to_win` generations, and after each game that threshold moves
**+3 on a Human win, −5 on a Zombie win** (floored at 100). Separately, two new
per-round knobs (`machete_percentage`, `human_kill_zombie`) were to be monte-carlo'd
to find ranges giving "an even chance to win."

The instinct was to sweep the machete knobs across a grid and read off the cell
where the Human/Zombie win ratio is ~50/50. We built that sweep — and it was
misleading.

## Guidance

**When a control variable self-adjusts by fixed win/loss steps, it — not your other
parameters — pins the steady-state outcome ratio. Solve the loop's fixed point
first; only then tune the rest, and tune it for the things the loop does *not*
control.**

At steady state the expected change of the adaptive variable is zero. For a rule
of `+a` on event A and `−b` on event B:

```
a·P(A) = b·P(B)   ⇒   P(A)/P(B) = b/a
```

For GoLZ's +3/−5 that is `P(Human)/P(Zombie) = 5/3`, i.e. the **decisive** Human
share converges to `5/8 = 62.5%` — *regardless* of the machete parameters. The
loop drags the threshold up or down until that ratio holds. The monte-carlo grid
confirmed it: every candidate machete range steady-stated at 62–66% decisive
Human, differing only in **tie rate** and **where the threshold settled**.

So the machete knobs were the wrong dial for the win ratio. What they *do* control
is everything outside the loop's grip:

- the **tie rate** (here, intrinsic board-stabilisation outcomes the loop ignores),
- the **equilibrium value** of the adaptive variable (we chose ranges that settle
  it right at the human-readable default of 250, so it barely drifts and the
  on-screen number stays meaningful),
- how fast the loop converges and whether it slams into a floor/ceiling.

And if the target ratio itself is wrong (62.5% ≠ "even"), the fix is the **step
asymmetry**, not the parameters: symmetric steps (`+n/−n`) give a 50/50 fixed
point. One-line change; no re-sweep.

## Why This Matters

- **A parameter sweep against a closed loop measures the transient, not the
  steady state.** Our fixed-gens grid (loop disabled) showed machete ≥ 30% →
  70–100% Human; with the loop live, all ranges collapsed to ~62.5%. Reporting the
  grid as "the balance" would have been wrong.
- **It redirects tuning effort.** Once you know the loop owns the ratio, you stop
  hunting for the 50/50 cell and start choosing parameters for tie rate,
  equilibrium placement, and convergence — the things that actually vary.
- **It surfaces hidden design intent.** "+3/−5" reads as "loser gets an easier
  game," but it silently encodes a 62.5%-favoured side. Making the fixed point
  explicit lets you keep it on purpose or flip to symmetric.

## How to Apply

1. Write the loop's update rule and set `E[Δ] = 0`; solve for the outcome ratio it
   forces. That number is your real balance target, set before any sweep.
2. Validate with an **end-to-end adaptive simulation** (run the live loop over many
   games), not just a fixed-parameter grid. See `tools/golz_mc.c --adaptive`.
3. Tune the remaining parameters for what the loop does *not* fix (tie rate,
   equilibrium value, convergence), and keep the loop's step constants in one
   obvious place so the fixed point is a deliberate, easily-changed choice.

## Related

- [Two-layer faction reuse](two-layer-faction-reuse.md) — the architecture the
  machete layer was added to.
- Implementation plan: [docs/plans/2026-06-27-001-feat-golz-machetes-plan.md](../../plans/2026-06-27-001-feat-golz-machetes-plan.md)
  (Monte-carlo results section).
