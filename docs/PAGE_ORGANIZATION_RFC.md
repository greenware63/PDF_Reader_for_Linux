# RFC: Page Organization (Phase 2)

Status: **PROPOSED ARCHITECTURE. No product code or behavior change is included in T43.**
Scope: Disk-persisted page manipulation (delete, reorder/move, insert/merge, split/extract, replace, rotate, crop) built on the existing sandboxed `pdf_worker` + `WorkerClient` architecture.

---

## 1. Spike Verification Summary (`tests/spikes/page_ops_spike.cpp`)

All seven candidate PDFium APIs were compiled and executed against pinned PDFium (`chromium/8100`) in `tests/spikes/page_ops_spike.cpp`, with every output validated by `qpdf --check`, `pdfinfo`, `pdftoppm`, `mutool info`, and `pdfsig`.

| API / Operation | Availability (`chromium/8100`) | Observed Behavior on Fixtures |
|---|---|---|
| `FPDFPage_Delete` | **Available** | Removes page from `/Pages /Kids`. **Outlines** & **named destinations** pointing to the deleted page become dangling (`FPDFDest_GetDestPageIndex == -1`), while later pages shift indices. **`/Link` annotations** on surviving pages pointing to the deleted page also dangle (`-1`). **`/PageLabels`** number tree is index-based and does **not** shift. **`/AcroForm /Fields`** retains orphan field references even though the deleted page's widget is no longer reachable (`Form: AcroForm` in `pdfinfo`, `formWidgetsAtProbe` drops from 2 to 1). |
| `FPDF_MovePages` | **Available** | Reorders pages in-place. **Outlines**, **named destinations**, and **page-bound `/Link`s / widgets** follow their target page objects automatically. **`/PageLabels`** is index-based and does **not** follow moved pages. |
| `FPDF_ImportPagesByIndex` | **Available** | Copies selected pages into a destination `FPDF_DOCUMENT`. **Drops** `/Outlines`, `/Names /Dests`, and `/PageLabels` completely (`outlines=[]`, `namedDests=0`, `labels=[,,,]`). Copies page `/Widget` annotations **without** the catalog `/AcroForm` dictionary (`pdfinfo` reports `Form: none`; `pdftoppm` emits `Syntax Error: Missing 'Tf' operator in field's DA string`). |
| `FPDF_ImportPages` | **Available** | Same behavior as `FPDF_ImportPagesByIndex` using a 1-based page range string (e.g. `"1-2"`). |
| `FPDFPage_SetRotation` | **Available** | Updates page `/Rotate` entry (`0/1/2/3` -> `0/90/180/270` deg). Preserves outlines, named destinations, links, labels, and AcroForm. Compatible with both `FPDF_SaveAsCopy(..., 0)` and `FPDF_INCREMENTAL`. |
| `FPDFPage_SetCropBox` | **Available** | Updates page `/CropBox` entry. Preserves outlines, named destinations, links, labels, and AcroForm. Compatible with both `FPDF_SaveAsCopy(..., 0)` and `FPDF_INCREMENTAL`. |
| `FPDF_SaveAsCopy` (non-incremental `flags = 0` to fd) | **Available** | Writes a valid rewritten PDF (`qpdf --check` exit 0, `mutool info` exit 0, `pdftoppm` exit 0) through a POSIX file descriptor (`FdWriter` / `writeBlockToFd`). **Signed documents:** rewrites xref/objects and breaks the original `/ByteRange` (`pdfsig` reports validation failure). **Encrypted documents:** importing/rewriting from an unlocked source strips encryption (`Encrypted: no`), writing plaintext unless explicitly re-encrypted or blocked/warned. |

---

## 2. Architectural Decisions & Recommendations

### Decision 1: Architecture of the Rewritten Copy
**Recommendation:** Execute all structural page operations (delete, reorder, insert/merge, replace, split) on a **fresh scratch `FPDF_DOCUMENT` opened in `pdf_worker`**, never mutating the live `m_workerDocument` in-place, and write the output through a **UI-opened write file descriptor** (`SCM_RIGHTS`, matching `SaveDocumentRequest` / `saveAsCopy`), committed atomically via `QSaveFile` on the UI side.
- **Unsaved edits handling:** Require a **clean `EditLog`** (or prompt the user to Save / Discard / Cancel before launching Organize Pages), matching the existing `MainWindow::applyAndSaveRedactions` pattern. Replaying pending annotation/form edits onto a structurally reindexed scratch document is error-prone because page indices shift under delete/reorder/insert; requiring a clean `EditLog` avoids index-remapping bugs and keeps `EditLog` undo history unambiguous.

### Decision 2: IPC and Bounds
**Recommendation:** Keep `extractPages` bytes-over-IPC **only** for small contiguous extractions capped strictly by existing IPC frame bounds (e.g., $\le 16\text{ MiB}$ / $\le 100$ pages); route general **page-organization rewrites** through a descriptor-based request (`OrganizePagesRequest` with an `O_WRONLY` output fd passed via `SCM_RIGHTS`, plus progress heartbeats like `SaveDocumentRequest`).
- **Declarative output spec:** Send a declarative list of `PageSpec { sourceIndex, rotationDegrees }` entries rather than an imperative sequence of move/delete operations. Reordering a large document as individual moves would hit operation-count caps and suffer from index-shift pitfalls; a declarative spec is idempotent and makes validation straightforward (non-empty, $\le \min(\text{pageCount}, 10{,}000)$, unique in-range indices, rotation $\in \{0, 90, 180, 270\}$).
- **Bounds:** Enforce strict caps at engine, IPC deserializer, and worker:
  - Maximum resulting page count (`kMaxOrganizedPages`): `10,000`.
  - Secondary import source size / fd validation: same `RLIMIT_AS` / `512 MiB` streaming limits as `OpenDocumentRequest`, with `O_RDONLY` fd check via `fcntl(F_GETFL) & O_ACCMODE`.

### Decision 3: UI Model
**Recommendation:** Ship a **modal "Organize Pages" dialog** in Slice 1 that previews thumbnails, stages a batch of page operations (delete, reorder, rotate, insert from file), and writes to a **new output file** (via `QFileDialog::getSaveFileName`, with overwrite guard or explicit save-as).
- Live in-tab editing (mutating the open tab's page tree with full `EditLog` undo/redo across structural page shifts) requires cross-document `EditLog` page-index remapping, tile-cache invalidation, and selection/form-cache rebuilding; deferring live in-tab structural editing keeps Slice 1 localized, safe, and easy to verify.
- **Dependency on T20 (T37/T38):** Because page indices shift across organized outputs, per-document UI state (staged redaction marks, active redaction drag, and in-progress ink strokes) must not survive reopening an organized output. T37 and T38 (`PdfViewerWidget::applyOpenedDocument()`) already enforce this reset on document open.

### Decision 4: What Survives (Outlines, Destinations, Links, Forms, Labels, Structure Tree)
Based directly on the spike measurements:
- **In-document delete (`FPDFPage_Delete`) & reorder (`FPDF_MovePages`):**
  - **Outlines & Named Destinations:** Surviving targets follow moved pages automatically, **except** targets pointing at deleted pages become dangling (`pageIndex == -1`). Inspection of the pinned PDFium public headers (`fpdf_doc.h`, `fpdfview.h`) confirms PDFium only exposes read-only bookmark, page-label, and named-destination APIs (`FPDFBookmark_*`, `FPDF_GetPageLabel`, `FPDF_GetNamedDest*`), with no public API to edit or prune them. Therefore, `organizePages` **warns with counts** (`danglingOutlineCount`, `danglingLinkCount`) rather than pruning.
  - **`/Link` annotations:** Links on surviving pages pointing to deleted pages become dangling (`pageIndex == -1`). Do **not** silently rewrite arbitrary link dictionaries; report `danglingLinkCount` so callers can warn the user.
  - **`/PageLabels`:** Index-based in PDF; neither `FPDFPage_Delete` nor `FPDF_MovePages` updates `/PageLabels`. Document as known limitation.
  - **AcroForm:** `FPDF_MovePages` preserves forms; `FPDFPage_Delete` leaves orphan `/AcroForm /Fields` entries referencing removed widgets. Documented in known limitations.
  - **Structure tree (`/StructTreeRoot`):** PDFium page deletion/import does not maintain tagged PDF structure trees; structure tags may become incomplete or dropped on import.
- **Cross-document import (`FPDF_ImportPagesByIndex`):**
  - Outlines, named destinations, `/PageLabels`, and `/AcroForm` catalog dictionaries are **dropped** by PDFium (leaving headless `/Widget` annotations if source pages had form fields, as observed by `pdftoppm`). Therefore, cross-document merge/insert must refuse importing pages containing `/Widget` annotations unless flattened first.

### Decision 5: Permissions, Signed, and Encrypted Documents
**Recommendation:**
- **Permission flags:** Honor owner restrictions via `FPDF_GetSecurityHandlerRevision` and `FPDF_GetDocPermissions`. Unencrypted documents (`secRev == -1`) are allowed. For security handler revision 3 or later (`secRev >= 3`), require the **assemble** bit (ISO 32000-1 Table 22, bit 11 = `1 << 10`). For revision 2 (`secRev < 3`), require the **modify** bit (bit 4 = `1 << 3`). Return `PermissionDenied` before any scratch work when restricted.
- **Signed documents:** Check `signatureCount() > 0` before any rewrite and require `allowInvalidateSignatures == true` (`SignedNotAcknowledged` otherwise). Warn the user explicitly that non-incremental save invalidates all existing digital signatures (`pdfsig` validation fails). Keep `FPDF_INCREMENTAL` as the strict default for all non-structural edits.
- **Encrypted documents:** Require `allowDecrypt == true` (`EncryptedNotAcknowledged` otherwise) when `secRev != -1`, because exporting reorganized pages through a fresh document strips encryption (`Encrypted: no`). Warn the user explicitly before exporting plaintext.

### Decision 6: Cross-Document Import (Merge and Insert)
**Recommendation:** Never allow `pdf_worker` to open a user path directly (Landlock read confinement forbids it). For insert/merge, the UI opens the secondary PDF read-only and passes an **`O_RDONLY` file descriptor via `SCM_RIGHTS`** alongside the existing write descriptor, reusing the same strict descriptor-typing checks in `WorkerServer` (`fcntl(F_GETFL) & O_ACCMODE == O_RDONLY`).
- **Security implications:** A second untrusted PDF doubles parser attack surface inside the same worker process (malformed page trees, deep recursion, `huge_page` allocations, password-protected secondaries). Enforce `RLIMIT_AS`, watchdog progress heartbeats during import, and run hostile-fixture tests (`pagetree_loop.pdf`, `lying_length.pdf`, `form_recursion.pdf`, `huge_page.pdf`) against the import path.

### Decision 7: Crop and Rotate
**Recommendation:** Keep **in-place persistent Rotate and Crop** (`FPDFPage_SetRotation`, `FPDFPage_SetCropBox`) **separate** from the full non-incremental structural rewrite path when used as standalone edits, because the spike proved both are **compatible with `FPDF_INCREMENTAL`** (`op_rot_crop_inc.pdf` passes `qpdf --check`, `pdfinfo`, `pdftoppm`, and `mutool` while preserving outlines, links, labels, and forms). Within the "Organize Pages" modal flow (when combined with delete/reorder/insert), applying rotation on the rewritten copy is fine; standalone "Rotate Page on Disk" / "Crop Page" actions should use incremental save (unless signed, which still warns).

### Decision 8: Quality Gates and Proposed Slice Plan
Every slice must satisfy the five gates in [`docs/ROADMAP.md`](ROADMAP.md):
1. `README.md` (and `docs/PARITY_MATRIX.md`) status row backed by exact test titles.
2. Hostile fixture coverage (bad indices, duplicate move indices, out-of-range crop/rotate, `pagetree_loop.pdf`, `lying_length.pdf`, oversized op counts).
3. External engine validation (`qpdf --check`, `pdfinfo`, `pdftoppm`, `mutool info`, `pdfsig`).
4. Mutation-tested assertions.
5. `docs/threat_model.md` entry covering descriptor typing, second-document import, signature invalidation, and encryption stripping.

#### Proposed Slice Plan
- **Slice 1a (T44 — Engine only):** `PageOrganizationTypes.h` (`PageSpec`, `OrganizeOptions`, `OrganizeStatus`, `OrganizeResult`) + `IDocument::organizePages` / `PdfiumDocument::organizePages` on a scratch copy from retained immutable source bytes, with validation, permission/signature/encryption/streamed guards, dangling outline/link counts, unit/hostile/mutation tests, and `ExternalEngineCompatTest` coverage.
- **Slice 1b (T45 — IPC, worker, and client, implemented in protocol v15):** Descriptor-based `OrganizePagesRequest` / `OrganizePagesResponse` in `IpcMessages` (carrying `catalogEntriesDropped` and `danglingOutlineCount` / `danglingLinkCount`), `WorkerServer` shared writable-descriptor exception, `RenderWorker::requestOrganizePages` with `WorkerOp::Organize` progress heartbeats, `WorkerClient::organizePagesSync`, IPC/hygiene/watchdog tests (`Test 44`), and `fuzz_ipc_messages` coverage.
- **Slice 2 (Cross-document Insert / Merge / Split, ~350–450 LOC):** Secondary `O_RDONLY` fd import via `FPDF_ImportPagesByIndex`, encryption-stripping & form-import guards, multi-output split, hostile secondary-PDF tests.
- **Slice 3 (Modal "Organize Pages" UI, ~400–500 LOC):** Thumbnail grid dialog in `MainWindow` / `src/ui/`, drag/button reorder, delete, rotate, insert-from-file, clean-`EditLog` & signature/encryption prompts, atomic `QSaveFile` commit, and `ViewportWidgetTestSuite` / `ViewerCharacterizationTestSuite` coverage.
- **Slice 4 (Persistent Crop + Incremental Rotate, ~250–350 LOC):** `FPDFPage_SetCropBox` and incremental-save single-page rotate/crop actions with external-tool verification.

---

## 3. Resolved Notes & Open Questions
- **Dangling outlines/links on `FPDFPage_Delete` (resolved):** PDFium public headers provide no bookmark/label/named-dest editing APIs, so Slice 1a reports `danglingOutlineCount` and `danglingLinkCount` instead of pruning.
- **Orphan `/AcroForm` widgets on cross-document import (resolved):** Slice 2 will refuse importing pages containing `/Widget` annotations unless the source pages are flattened first.
