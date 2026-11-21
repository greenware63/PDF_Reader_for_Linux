# Roadmap

## Decisions

1. Page insert, delete, and reorder use a rewritten-copy path.
2. Incremental save remains the default.
3. AI stays out of scope until core capabilities are complete.
4. Work proceeds in this order: foundations → page organization/security → annotation/header/footer/Bates → OCR → digital signatures → edit text/images (narrow first) → conversion (images/text/tables first).

## Gate for Every Feature

Every feature must have a README status backed by tests, a hostile fixture, external compatibility coverage, mutation-tested tests, and a threat-model entry.

## Baseline and Verification

- Starting point: `main` at `b0a01ea`, recorded in `git log`.
- Baseline CI: [run 44908715725](https://github.com/greenware63/PDF_Reader_for_Linux/actions/runs/44908715725) completed successfully for commit `b0a01ea`.
- This T21 change is documentation-only. No build, test suite, mutation test, or external-viewer checklist was run for this change; the cited CI run is the pre-change baseline, not validation of this branch.
