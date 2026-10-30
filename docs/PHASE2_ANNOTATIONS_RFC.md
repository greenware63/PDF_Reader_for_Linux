# RFC: Phase 2 — Annotations, the save pipeline, and (later) interactive forms

Status: **PROPOSED ARCHITECTURE (Decisions recorded). Ready for Milestone M0 spike.**
Scope: standard PDF annotations first; AcroForm filling reuses the same edit and save machinery afterwards.
Out of scope (unchanged standing rules): digital *signing*, signature *validation* (a separate sandboxed verifier later), JavaScript, XFA, replacing PDFium, any change to the MIT licence, and anything that overwrites the user's original file without an explicit, confirmed action.

## 1. Why a design first

Everything shipped so far reads documents. Annotations are the first feature that **changes and persists** a user's file. That touches three security properties the current design relies on:

| Property today | Why it holds | What annotations put under pressure |
|---|---|---|
| The worker cannot write user files | Landlock allows writes only under `/dev/shm`; the worker no longer even knows the file path (descriptor passing, T1) | Something must produce the saved bytes; the worker is the only process that links PDFium |
| The UI never parses PDF | The UI links no PDFium (zero PDFium linkage) | Annotation geometry, hit testing and appearance all live in PDFium |
| Hostile worker output is untrusted | Bounded, typed, fuzzed IPC (T6) | Every new command and result is new attack surface |

## 2. What was measured before writing this (feasibility)

Facts from the **pinned** PDFium headers (chromium/8100, `fpdf_annot.h`, `fpdf_save.h`):

* `FPDFPage_CreateAnnot` supports creating: circle, fileattachment, freetext, highlight, ink, link, popup, square, squiggly, stamp, strikeout, text, underline.
* `FPDF_SaveAsCopy` accepts `FPDF_INCREMENTAL` and `FPDF_NO_INCREMENTAL` (mutually exclusive); there is `FPDFAnnot_SetAP` for explicit appearance streams.
* The existing redaction path already saves with `FPDF_NO_INCREMENTAL` and a progress heartbeat (`Redaction.cpp`); the existing page export returns bytes over IPC.

A quick experiment on `sample.pdf` with **another** PDFium build (pypdfium2 5.13.0, PDFium 153.0.7999.0 — *not* the pinned binary): create a Highlight, set rect, quad points, colour; save both ways.

| Observation | Incremental | Full rewrite |
|---|---|---|
| Save succeeds | yes | yes |
| Output starts with the exact original bytes | **yes** | no |
| `qpdf --check` | no errors | no errors |
| Highlight present in output | yes | yes |
| Size (original 12.5 KB) | 25.8 KB (re-emits page objects) | 13.4 KB |
| `/AP` appearance stream written for the new Highlight | **no** (annotation has `/C`, `/CA`, `/QuadPoints`, `/Rect` only) | not checked |

Consequences that shape the design: (a) incremental save really does preserve the original bytes as a prefix, which is the property that keeps existing signatures byte-range intact, so the UI can **verify** it instead of trusting the worker; (b) PDFium does not generate appearance streams for the annotations it creates, so interoperability with Acrobat/Foxit/Preview needs **our own `/AP`** (via `FPDFAnnot_SetAP`); (c) **milestone M0 must repeat this with the pinned binary** before any code depends on it.

## 3. Architecture

### 3.1 Who owns the edit state

The **UI process owns the authoritative edit log**: an ordered list of semantic commands (`AddAnnotation`, `ModifyAnnotation`, `DeleteAnnotation`, later `SetFormFieldValue`), each with a UI-assigned 64-bit id. The document the user sees is *original bytes + log*. Consequences:

* Undo/redo is a cursor in the log; it lives where the keyboard handling lives and needs no PDFium.
* A worker crash or watchdog kill loses nothing: the UI starts a new worker, reopens the document through a fresh descriptor and **replays the log**.
* The worker's PDFium document is a derived, disposable view.
* Annotation author is **blank by default** for user privacy (no automatic leakage of local `$USER` or hostname); a user-configurable author name will be provided in application settings.

Annotations created by us carry a standard `/NM` (annotation name) entry holding the id, so ids survive save/reload and map to PDFium objects without relying on array indices (which shift on delete).

Mouse gestures (drag a highlight, draw ink) are drawn as a UI-side overlay and sent as **one** command when the gesture ends; no IPC per mouse move.

### 3.2 Rendering

The worker applies commands to its PDFium document and renders with annotations enabled. The viewer tile cache is invalidated per affected page (existing per-page invalidation). To be checked in M1: that widget annotations are not drawn twice now that form fields are drawn through `FPDF_FFLDraw`.

### 3.3 Hit testing and reading existing annotations

`ListAnnotations(page)` and `HitTest(page, x, y)` return plain data (subtype, rect, contents, author, flags, our `/NM` id if any). The UI never receives annotation *objects*, only bounded POD. Links and URI actions are shown and activated by the UI after user confirmation; the worker never opens anything.

## 4. The save pipeline (the central decision)

Requirements: the worker keeps **no path-based write access**; output may exceed the 64 MiB IPC frame cap; a compromised worker must not be able to damage the user's file.

**Proposal: the UI hands the worker one writable descriptor to a UI-created temporary file; the worker streams the saved PDF into it; the UI validates, then commits atomically.**

1. UI creates a temp file in the destination directory (same filesystem; `QSaveFile`-style) and keeps ownership.
2. UI sends `SaveRequest{mode, logRevision}` carrying **exactly one** descriptor (`SCM_RIGHTS`) to that temp file, opened write-only.
3. The worker applies pending commands, calls `FPDF_SaveAsCopy` with a writer that `write()`s to the descriptor and emits a progress heartbeat from the writer callback (as redaction already does); closes the descriptor; replies `SaveResult{ok, bytesWritten}`.
4. The UI treats the file as **untrusted worker output** and validates before commit: size equals `bytesWritten` and is below a cap; starts with `%PDF-`; for incremental mode the first `originalSize` bytes are **byte-identical** to the original (hash compared); then re-open it in a **fresh** sandboxed worker and confirm it parses and has the expected page count and annotation count.
5. Only then `rename` over the chosen destination. Any failure deletes the temp file and leaves everything else untouched.

Why this holds up: Landlock restricts *opening*, not writing to an already-open descriptor, so the worker gains write access to exactly one temp file and nothing else; `RLIMIT_FSIZE` can additionally cap what it can write; a hostile worker can at worst produce a bad temp file, which step 4 rejects before it can replace anything.

Descriptor policy change (must be updated in `threat_model.md` and tests together): today only `OpenDocumentRequest` may carry a descriptor. After this RFC: `OpenDocumentRequest` (read-only descriptor) and `SaveRequest` (write-only descriptor), each exactly one, everything else still closed and dropped. Test 19's table-driven cases extend to `SaveRequest`.

**Defaults.**
* The default action is **Save As** to a new file.
* *Save* in place requires an explicit confirmation naming the file, is atomic, and is refused if the destination changed on disk since open (size/mtime/inode check).
* Under Flatpak, saves operate strictly via the **XDG Desktop File Chooser Portal** (`org.freedesktop.portal.FileChooser`), requiring no extra filesystem write permissions in the sandbox manifest.
* The standing rule that a redaction output never overwrites its source stays as is.

## 5. Incremental vs full save, and signatures

* **Incremental** (`FPDF_INCREMENTAL`) is the default whenever the source is not encrypted-in-a-way PDFium cannot append to (to be verified in M2, with a fallback to full + a warning).
* If the document has signatures (`FPDF_GetSignatureCount() > 0`) a **full rewrite is forbidden**: it would invalidate them. Only incremental save is offered, and the UI-side prefix check in step 4 is mandatory.
* We **do not** validate signatures or claim a signature is valid or invalid. Whether appended annotations are permitted by a certifying signature (DocMDP level) is not evaluated here; the UI says only that existing bytes are preserved and signature validity must be checked in a dedicated tool. Real verification remains the future sandboxed verifier.

## 6. IPC additions (all bounded, typed, fuzzed)

| Message | Direction | Carries | Hard limits (proposal) |
|---|---|---|---|
| `AddAnnotation` | UI→worker | id, page, subtype, rect, quad points, colour, opacity, border, text, author | quads ≤ 4096; text ≤ 64 KiB UTF-16; author ≤ 256 chars |
| `AddInkAnnotation` | UI→worker | id, page, strokes, colour, width | strokes ≤ 256; total points ≤ 100 000 |
| `ModifyAnnotation` / `DeleteAnnotation` | UI→worker | id (+ changed fields) | same field limits |
| `ListAnnotations` / `HitTest` | UI→worker | page (+ point) | reply ≤ 10 000 items, text clipped |
| `SaveRequest` (+1 fd) / `SaveProgress` / `SaveResult` | both | mode, revision / progress / result | one fd; `bytesWritten` verified by the UI |

Rules carried over from existing messages: every collection length is validated before allocation; reject NaN/inf/out-of-page geometry; reject unknown subtypes; the worker answers with failure messages, never crashes the connection. The existing `IpcFuzzTestSuite` gains one mutated-deserialisation corpus per new message; M1 also adds hostile-PDF corpus items (huge or cyclic `/Annots`, absurd quad counts, deep popup parent chains).

## 7. Threat-model additions (to be merged into `threat_model.md` with the code)

| New threat | Mitigation |
|---|---|
| Compromised worker writes junk or oversized output through the save descriptor | One fd only; `RLIMIT_FSIZE`; UI validates size, prefix, re-parse in a fresh worker before commit; atomic rename |
| Descriptor confusion (write fd used as read fd or vice versa) | Per-message-type descriptor policy, access mode checked by the worker with `fcntl(F_GETFL)` and by the UI when it creates it; tests extend Test 19 |
| Hostile annotation content (URI, JavaScript, launch, attachments) | JavaScript/XFA not built; URI/link actions activated only by the UI after confirmation; file attachments are a **separate, later milestone** with their own review (exfiltration/malware vector) |
| Resource exhaustion through `/Annots` | Count and size limits above; the watchdog heartbeat covers the save and replay |
| Edit loss on worker death | UI-owned log + replay; temp output removed on failure |

## 8. Redaction interplay (to be verified, not assumed)

Redaction currently re-rasterises pages and builds a new document. Before M2 we must **prove with a test** that annotation text and appearances under a redacted region cannot survive in the output (including `/Contents` of notes and popup text), and that a document with annotations can still be redacted. Until that test exists this RFC makes no claim.

## 9. Interoperability test matrix

Automated (CI already installs `qpdf` and `poppler-utils`):

* `qpdf --check` on every saved output; incremental outputs also checked for the byte-prefix property.
* Render the saved file with `pdftoppm` and compare the annotation region to our own render (tolerance-based pixel check) — this is the check that catches missing `/AP`.
* `pdftotext`/`qpdf --json` confirm annotation count, subtype, `/Contents`, `/NM` after a round trip; re-open in our reader and compare the list.

Manual, per release that changes annotations (checklist in the PR): open our output in Acrobat Reader, Okular, Foxit and a browser PDF viewer; edit/delete an annotation there; re-open in ours. Acrobat and Foxit are **stated as untested / unverified** until external platform verification can be arranged.

## 10. Milestones (each a small PR with its own tests)

| # | Content | Exit criteria |
|---|---|---|
| M0 | Spike with the **pinned** PDFium: create/set AP/save both ways for each target subtype; record results here | Table in §2 repeated for chromium/8100 and for every subtype; decide the `/AP` generation approach |
| M1 | **Read-only**: list, render, hit-test existing annotations; hostile-PDF fuzz corpus; no writes anywhere | Tests incl. double-draw check for widgets; fuzz additions |
| M2 | Save pipeline + **one** type (highlight) end to end: edit log, undo/redo, save descriptor, validation, atomic commit, crash replay (in-memory log replay; disk persistence journal deferred post-M2); warning dialog on unsaved edits; tests for each failure branch | Tests for each failure branch (bad prefix, truncated output, worker killed mid-save); threat model updated; redaction-interplay test |
| M3 | Underline, strikeout, squiggly, text note (+popup), ink, square, circle, freetext | Per-type round-trip tests in the matrix |
| M4 | Stamps (images) and file attachments | Separate threat review; image size/dimension limits |
| M5 | Interactive AcroForm filling on the same log and pipeline | Event dispatch into the form-fill environment, incremental save of values |

## 11. Decisions recorded by project owner

1. **Default save behaviour**: **Save As** by default to protect existing user files from accidental overwrites. In-place *Save* is supported only upon explicit user confirmation, executes atomically via temp-file commit, and aborts if file modification time or size changed externally.
2. **Annotation author**: **Blank by default** to preserve privacy and avoid leaking system usernames or environment details. An optional author name field will be available in user settings.
3. **Crash-recovery journal**: Defer persistent disk journaling to post-M2. Milestone M2 implements in-memory edit log crash replay (surviving background worker process terminations) and clear UI warnings before closing windows with unsaved changes.
4. **Flatpak permissions**: No filesystem write permissions will be added to the Flatpak manifest. All file saves in Flatpak will operate strictly through the **XDG Desktop File Chooser Portal** (`org.freedesktop.portal.FileChooser`).
5. **Acrobat & Foxit testing**: Compatibility with Acrobat and Foxit will be documented as **untested / unverified** until manual verification on Windows/macOS is performed. Automated CI tests rely on `qpdf`, `pdftoppm` render comparisons, and `poppler-utils`.
