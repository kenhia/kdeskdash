---
title: Mode files cannot include their pure core with a bare "name.h"
date: 2026-07-21
category: docs/solutions/best-practices
module: modes with a same-named pure core (golz, calc)
problem_type: best_practice
severity: medium
applies_when:
  - Adding a mode src/modes/X.c whose pure core is src/X.c / src/X.h
  - The mode header src/modes/X.h shares the core header's basename
tags: [c, preprocessor, include-path, cmake, pure-core]
---

# Mode files cannot include their pure core with a bare `"name.h"`

## Context

The pure-core discipline gives many modes a same-named pair: `src/golz.h` (core)
and `src/modes/golz.h` (mode), `src/calc.h` and `src/modes/calc.h`. GCC resolves
a quote include by searching **the including file's directory first**, before any
`-I` path. So inside `src/modes/calc.c`, `#include "calc.h"` finds
`src/modes/calc.h` — the mode's own header, already included and guard-skipped —
and the core header never arrives. The symptom is `unknown type name 'calc_t'`
with the include line sitting right there looking correct.

The trap is camouflaged by precedent: `src/modes/golz.c` does exactly this
(`#include "golz.h"`) and builds — but only because `redis.h`, included two lines
later, happens to include the core `golz.h` itself from `src/`, where the bare
name resolves to the core. The golz include is a guard-skipped no-op; the core
arrives transitively, by luck. Copying that pattern into a mode with no such
transitive path (calc) breaks immediately.

## Guidance

From a mode file, include the core header by a path that cannot hit the modes
directory:

```c
#include "modes/calc.h"   /* the mode's own header */
#include "../calc.h"      /* the pure core — NOT "calc.h", which resolves
                             to this directory's mode header */
```

Verification when in doubt: `gcc -E -H src/modes/X.c ... | grep X.h` shows which
file each include actually opened.

## Related

- `src/modes/calc.c` — canonical fixed example with the explanatory comment
- `src/modes/golz.c` — the accidental-transitive case (works, don't copy)
