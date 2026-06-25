## Objective

Continuously repair the project until it builds successfully and all detectable issues are resolved.

Do not stop after a single fix.

---

## Mandatory Workflow

Before investigating any issue:

```bash
codegraph sync
```

Use CodeGraph to inspect:

* definitions
* references
* callers
* callees
* dependencies

Do not guess architecture when CodeGraph can provide the answer.

---

## Fix Loop

Repeat the following process:

1. Build project.
2. Collect all errors.
3. Identify root cause.
4. Run `codegraph sync`.
5. Inspect affected symbols.
6. Apply fixes.
7. Build again.
8. Repeat until successful.

---

## Rules

* Fix root causes instead of symptoms.
* Prefer minimal safe changes.
* Preserve existing functionality.
* Avoid unnecessary refactoring.
* Keep code style consistent with surrounding files.

---

## Validation

Before stopping:

* Build succeeds.
* No compiler errors remain.
* No linker errors remain.
* No runtime crashes introduced by recent changes.
* No critical diagnostics remain.

---

## Completion

Only stop when all validation checks pass.

Output:

SUCCESS

Include:

* files modified
* fixes applied
* remaining warnings (if any)
