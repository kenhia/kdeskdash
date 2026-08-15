# 028 — Re-apply the harness with `--stack cmake`

WI #1280 · proposal korg:1281 · batch 5 of the kprojects rollout (program korg:1277)

## Goal

`CLAUDE.md`'s managed block told every agent working in this repo:

> - Python managed by `uv`; lint/format with `ruff`; typecheck with `ty`
>   (astral toolchain)

kdeskdash is C built with CMake. There is no Python in it. Wrong guidance, in
the one region a repo is not allowed to hand-correct.

## Why it said python

Nothing regressed. The block's marker was the **retired bash installer's** —
`managed by kprojects/install.sh` — so this repo had never been through the
Python installer, and its block predated stack detection existing at all. The
old `install.sh` wrote the python stanza unconditionally (korg #699/#725). It
was simply never re-applied.

kdeskdash was missed by the #737 survey; a fleet-wide audit on 2026-08-15 found
**17 repos** still carrying the old block, six of them Rust or CMake repos being
told to use the Python toolchain. This sprint fixes the repo in hand rather than
waiting for 1278's systematic pass.

## What shipped

One command:

```sh
uvx --from git+https://github.com/kenhia/kprojects kproject-install --stack cmake .
```

No migration step. kprojects matches block markers on the `<!-- kproject:begin`
*prefix*, never the full line, precisely so an old-installer block is replaced in
place. The layout it wants (`sprints/`, `sprints/planning`, `sprints/review`,
`docs`, `.scratch`) already existed, so nothing moved.

- **Tooling stanza**: python → cmake, in both `CLAUDE.md` and
  `.github/copilot-instructions.md` (the default `--agent both` is correct here —
  both files exist).
- **Marker text**: updated to the current `managed by kprojects` wording, and the
  harness reference from a local path to the GitHub URL.
- **Workflow stanza**: gained the "mark each work item resolved as its work
  completes" rule, which came along with the current block.
- **`.gitignore`**: gained `build-*/`, `CMakeFiles/`, `CMakeCache.txt`.
- **Nothing else.** Everything outside the managed block is untouched.

### The justfile is byte-identical

The existing hand-written recipes are better than the template's and had to
survive. `_seed()` structurally cannot overwrite an existing justfile, but that
was verified rather than trusted — `cmp` against a pre-install copy, identical.

## This repo is why the cmake stack exists as shipped

Worth recording, because kprojects sprint 006 chose the fifth stack believing
kpidash was the fleet's only C repo, and #1260's research argued no generic C
gate could be meaningful because there was "no CI to mirror, no existing source
of truth to copy". There was one — here, written with no knowledge of the
template:

```
build:
    cmake -B build -DKD_VERSION="$(scripts/version.sh)"
    cmake --build build -j"$(nproc)"

check: build
    ctest --test-dir build --output-on-failure
```

Configure → build → ctest: the same triple kprojects now seeds, arrived at
independently. The `build/` + `build-pi/` split is also exactly the two-tree case
the template's `build-*/` ignore was written for.

## Folded in: `--no-tests=error`

The one substantive change beyond the block. Measured against this box's real
`ctest` 3.28.3 rather than assumed:

| command | exit |
|---|---|
| `ctest --test-dir b` with zero tests registered | **0** — prints "No tests were found!!!" and passes |
| `ctest --test-dir b --no-tests=error` | 8 |

So the gate passes loudest when there is least to check. 17 tests are registered
today (the WI estimated 18) so it does not bite, but a refactor that stopped
registering them — a guarded `if(BUILD_TESTING)`, a renamed target — would leave
`just check` green while checking nothing.

Added to **both** `check` and `test`, not just the gate the WI named. `test name`
filters with `-R test_{{ name }}`, so before this a typo'd name matched nothing
and exited 0: `just test golzz` *passed*. That is the same trap at the point
where it actually fires, since a mistyped test name is a routine event and an
emptied test suite is not.

The same latent hole is tracked for kpidash on #1279.

## Verified

- `just check` — 17/17 pass.
- `just test golz` — 1/1 passes; `just test nosuchtest` now exits 8 instead of 0.
- `cmp` on the justfile across the install — identical.
- `git diff` on `CLAUDE.md` and `.github/copilot-instructions.md` — confined to
  the managed block, same 16+/5− change in each.

No behaviour change to the panel; nothing to deploy.
