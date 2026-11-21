# RFC: T22 — Viewer Responsibility Refactor

Status: **PROPOSED ARCHITECTURE. No implementation or behavior change is included in T22.**
Scope: behavior-preserving decomposition of the viewer, window, and their large widget test source.
Non-goals: new features, behavior changes, UI redesign, replacing PDFium, changing the worker trust boundary, or altering public compatibility APIs.

## 1. Why a design first

The viewer already has distinct responsibilities, but their state and control flow are concentrated in two Qt classes and one end-to-end test source. Moving code without first recording ownership risks changing event ordering, painting, asynchronous worker callbacks, undo history, or public Qt connections.

The architectural invariant is unchanged: **the UI process never links to or calls PDFium**. UI-side code talks to the existing `pdf::ipc::WorkerClient`; PDFium remains in `pdf_worker`. The refactor must not introduce engine objects or PDFium headers into UI collaborators.

## 2. Measured baseline

Baseline: main tip **`b0a01ea`** (`Fix Latin AcroForm appearance persistence (#59)`), whose full commit is `b0a01ead73c52fb43e2b1092c4f7628b091effa5`. The preceding history includes **`3da1312`** (T19 feature-status and external-engine compatibility test) and **`6414a86`** (hostile AcroForm test coverage).

| File at `b0a01ea` | Git blob bytes | Lines |
|---|---:|---:|
| `src/ui/PdfViewerWidget.cpp` | 193,394 | 4,658 |
| `src/ui/PdfViewerWidget.h` | 25,218 | 496 |
| `src/ui/MainWindow.cpp` | 76,563 | 2,023 |
| `src/ui/MainWindow.h` | 7,666 | 210 |
| `test_widget.cpp` | 396,802 | 8,311 |

Measurement method: Git blob byte counts are read from `git ls-tree -l b0a01ea -- <path>` and can be verified with `git cat-file -s b0a01ea:<path>`. Line counts are `len(Path(path).read_bytes().splitlines())` on the checked-out baseline; they describe source line ranges, not blob byte size. Responsibility ranges below are inclusive, 1-based source line numbers from `rg -n` on that same checkout. They are source ownership ranges, not non-overlapping allocation: event handlers and paint composition intentionally cross-cut features.

For a checkout-independent unchanged-file check, take the working-tree bytes, replace each CRLF pair (`b"\r\n"`) with LF (`b"\n"`) without other transformations, and pass those normalized bytes to `git hash-object --no-filters --stdin`. Compare that resulting object ID with the baseline path's object ID (`git rev-parse b0a01ea:<path>` or the object ID from `git ls-tree`). This comparison is independent of CRLF checkout conversion only when the normalized bytes produce the exact same Git object ID; equal line counts alone do not establish byte identity.

The base commit's Linux CI run **44908715725** completed with conclusion `success` for `b0a01ea` (`Linux CI (Release & Sanitizers)`). This establishes the starting baseline only; every refactor PR must run CI again.

## 3. Responsibility map

### 3.1 `PdfViewerWidget`

`PdfViewerWidget` is both the compatibility facade and a `QAbstractScrollArea`: it owns event dispatch and paint composition, exposes the current viewer API, and coordinates worker callbacks. The current declaration is `src/ui/PdfViewerWidget.h`.

| Responsibility and measured implementation range | Current state owned by the widget | Existing public API and Qt signals |
|---|---|---|
| Document lifecycle, worker setup, async open and recovery entry points: `PdfViewerWidget.cpp:77-437`, with worker construction/connections at `77-169` | `m_renderWorker`; open queue/path/password (`m_loading`, `m_pendingOpenId`, `m_pendingOpenPath`, `m_pendingOpenPassword`, `m_hasQueuedOpen`, `m_queuedOpenPath`, `m_queuedOpenPassword`); document identity and metadata (`m_hasDocument`, `m_pageCount`, `m_currentDocumentId`, `m_currentGeneration`, `m_currentFilePath`, `m_currentPassword`, `m_sourceFileSize`, `m_sourceFileModified`); outline/page labels and worker error | `openDocument`, `openDocumentAsync`, `closeDocument`, document/loading getters; `documentLoaded`, `loadingChanged`, `loadError`, `documentOpenFailed`, `outlineReady`, `pageLabelsReady`, `editsRecovered` |
| Layout, navigation, zoom, rotation and scroll mapping: `PdfViewerWidget.cpp:543-672`, `707-932`, `1002-1149` | `m_layout`, `m_scrollMapper`; page/scale/rotation and navigation state (`m_currentPage`, `m_scale`, `m_rotation`, `m_navigating`, `m_pageFlipAccumulator`, `m_wheelDeltaAccumulator`); 64-bit scroll and resize anchors (`m_docScrollX`, `m_docScrollY`, `m_lastVpWidthDevice`, `m_lastVpHeightDevice`) | `setZoom`, zoom presets, `setViewMode`, `goToPage`, `nextPage`, `previousPage`, rotation and reading-position APIs; `pageChanged`, `zoomChanged`, `viewModeChanged`, `rotationChanged` |
| Tile scheduling, render results, fallback rendering and viewport paint: `PdfViewerWidget.cpp:77-199`, `933-945`, `1150-1308`, `2081-2343` | `m_renderWorker`, `m_cache`, `m_inFlightKeys`, `m_fallbackScales`, `m_fallbackTilesDrawn`, `m_prefetchEnabled`, `m_prefetchRequestsIssued`, `m_transformedPixmapCache`, document/generation identity | `retryFailedTiles`, `waitForRendering`, `renderWorker`, `cache`, `layout`, render observability getters, `setPrefetchEnabled`; `thumbnailReady`, `thumbnailFailed` and worker/lifecycle signals |
| Page text, selection, hit mapping, clipboard and selection paint: `PdfViewerWidget.cpp:954-1001`, `1309-1416`, plus input dispatch at `1649-2080` | `m_pageTextCache`, `m_selectionStart`, `m_selectionEnd`, `m_isSelecting`; nested selection position type | `hasSelection`, `selectedText`, `clearSelection`, `copySelection`, `getPageText`; `selectionChanged` |
| Search, match navigation, highlights and worker progress: `PdfViewerWidget.cpp:1417-1640` | `m_currentSearchId`, `m_searchQuery`, `m_searchMatchCase`, `m_searchStartPage`, `m_searchMatches`, `m_currentMatchIndex` | `findText`, `findNext`, `findPrevious`, `goToSearchMatch`, `clearSearch`, match getters; `searchResultsChanged`, `searchProgressChanged`, `searchFinished` |
| Annotation creation/edit/deletion, placement, hit testing, tooltips and annotation menus: `PdfViewerWidget.cpp:2565-3290`, with input/context dispatch at `1649-2080` and `3291-3460` | `m_editLog`; `m_hoverHitTestPending`; ink drawing mode/color/width and in-progress stroke (`m_inkDrawingMode`, `m_inkColorArgb`, `m_inkBorderWidth`, `m_isDrawingInk`, `m_inkCurrentPage`, `m_currentInkStroke`) | `markupSelection`, annotation add/delete/modify methods, `placeOnPage`, `annotationTooltipHtml`; annotation edits use the shared `pdf::EditLog`; no annotation-specific widget signal currently exists |
| Redaction staging, selection-to-mark conversion and overlay: `PdfViewerWidget.cpp:2344-2564`, with pointer dispatch at `1649-2080` | `m_redactionMode`, `m_stagedRedactions`, `m_isRedacting`, `m_redactStartPage`, `m_redactStartDocPos`, `m_redactCurrentDocPos` | `setRedactionMode`, `clearStagedRedactions`, `addStagedRedaction`, `markSelectionForRedaction`, redaction getters; `redactionModeChanged`, `redactionMarksChanged` |
| Edit history, undo/redo, worker-failure replay and save validation/commit: `PdfViewerWidget.cpp:3461-3963` | The single authoritative `m_editLog`; `m_saveInProgress`, `m_editInProgress`, `m_recovering`, `m_preserveEditLogOnOpen`, `m_recoveryPath`; test-only save hooks under `PDF_WORKER_TEST_HOOKS` | `editLog`, `undoEdit`, `redoEdit`, `saveDocument`, `saveDocumentAs`; `editsRecovered`; `pdf::EditLog::dirtyChanged` is connected by `MainWindow` |
| Form-field cache, worker requests, hit testing, native editors, edits and tab navigation: `PdfViewerWidget.cpp:3964-4658` | `m_formFieldsCache` and its `PageFormFieldCache`; `m_activeFormEditor`, active page/name/control/type and initial value/choices | Form cache/request/edit/hit-test/editor/navigation APIs; `formFieldsLoaded`, `formFieldEdited` |
| Print/export: `PdfViewerWidget.cpp:673-705`; window-side file dialogs and orchestration: `MainWindow.cpp:1245-1447` | Uses the same `m_renderWorker`; the window owns user-facing file/export actions | `renderPageForPrint`, `extractPagesPdf`, `exportFlattenedPagesPdf`; `MainWindow::exportDocumentToPdf`, `exportFlattenedPdf`, `printDocument`, `exportAsPdf`, `exportFlattenedPdfAction` |
| Other appearance and outlines: `PdfViewerWidget.cpp:438-542`, `1641-1648`; paint overlays and Qt event overrides cross-cut the ranges above | `m_colorMode`, `m_darkMode`, `m_outline`, `m_pageLabels`, `m_dprOverride` | Color/theme, outline and page-label getters/setters; `colorModeChanged`, `darkModeChanged`, `outlineReady`, `pageLabelsReady`; protected Qt paint, resize, wheel, mouse, keyboard, context-menu, viewport and event-filter overrides |

Public API also includes test-observation accessors (`renderWorker`, `cache`, `layout`, `editLog`, tile/text counters and device-pixel-ratio injection). These are compatibility constraints, not permission to move behavior into test-only implementations.

### 3.2 `MainWindow`

`MainWindow` owns application-window composition and binds the viewer's signals to actions, status and sidebar widgets. It declares no signals of its own; its public slots and public methods are used by Qt connections and tests.

| Responsibility and measured implementation range | Current state owned in `MainWindow.h` | Existing API/slots and connections |
|---|---|---|
| UI composition, viewer wiring and actions: `MainWindow.cpp:270-797` | Find bar, tab widget, viewer pointer, status labels, password dialog/input, sidebar/dock/tabs/outline/search/thumbnail widgets; color/view actions; redaction/save/edit/annotation actions | `setupUi`, `setupActions`, `createViewerTab`, `connectViewer`; connects viewer load/page/zoom/search/selection/redaction/ink and edit-log dirty signals |
| Color/view settings, reading position, tabs and active-tab routing: `MainWindow.cpp:798-1134` | `m_colorMode`, `m_darkMode`, `m_viewMode`, `m_docTabs`, `m_viewer`, `m_sidebarUserHidden` | Public tab/open methods, `currentViewer`, `viewerAt`, `tabCount`, `setColorMode`, `setDarkMode`, `setViewMode`, reading-position methods; private `onCurrentTabChanged` and tab signal connections |
| Recent files and open workflows: `MainWindow.cpp:1135-1244` | `m_recentFiles`, `m_recentFilesMenu`, password prompt state | `openFile`, `openFileInNewTab`, dialogs, recent-file APIs, password-dialog helpers; viewer load/failure/loading slots |
| Print, export and PDF output: `MainWindow.cpp:1245-1447`, with save slots at `1856-1881` | Active-view selection plus save controls; the worker remains owned by the viewer | Export and print public methods/slots; `saveDocument`, `saveDocumentAs` |
| Loaded-document/status, outline and redaction workflows: `MainWindow.cpp:1448-1855` | Status labels, outline model/view, redact toolbar/actions/status, `m_redactionBusy` | Viewer lifecycle/outline/redaction slots; `toggleRedactionMode`, `markSelectionForRedaction`, `clearRedactionMarks`, `applyAndSaveRedactions` |
| Annotation commands, undo/redo and dirty action state: `MainWindow.cpp:1882-2023` | Annotation and undo/redo `QAction*` fields | Public annotation command slots, `undoEdit`, `redoEdit`, private `onEditLogDirtyChanged`, `updateStickyNoteAction` |
| Close and unsaved-change handling: `MainWindow.cpp:206-269` | Per-window widgets and active document tab | `closeEvent` override; tab-closing and save confirmation flow |

The public compatibility surface includes `openFile`, tab creation/selection/close methods, `currentViewer`/`viewerAt`, export methods, color/view/reading-position methods, widget getters, recent-file methods, and all declared `public Q_SLOTS` in `MainWindow.h`. Keep these names, signatures, Qt meta-object visibility, and action object names stable during extraction.

### 3.3 `test_widget.cpp`

The test is one executable with a single `main` and sequential checks, not a set of independently registered test functions. Its fixture-generation helpers occupy `test_widget.cpp:70-455`. The full suite occupies `456-8311` and contains **63 numbered checks**:

| Measured range | Current coverage |
|---|---|
| `481-1514` (Tests 1-22) | Failed-open preservation; navigation/zoom/resize; DPR and tile seams; worker lifecycle, cancellation, failure, caching, prefetch and asynchronous opens |
| `1515-2022` (Tests 23-31) | Text extraction/cache; selection, copy, cross-page and Unicode behavior; text/page geometry |
| `2023-2505` (Tests 32-33) | Search, FindBar, match cycling, Persian search, outline/sidebar navigation |
| `2506-4456` (Tests 34-45) | Crash recovery, thumbnails/page labels, tabs/recent files, print/export, color modes, reading position, keyboard/view/rotation, redaction and search-results panel |
| `4457-6840` (Tests 46-59) | Annotation save integrity, undo/redo, recovery, markup, notes, shapes, ink, FreeText, tooltips, placement, stamps and attachments |
| `6841-8311` (Tests 60-63) | AcroForm interaction, appearance, undo/redo, save/reload and flattened export |

`test_widget` is configured in `CMakeLists.txt:309-340` and is run by the normal CTest CI suite. This file is a legacy integration/characterization suite spanning viewer, worker client, and MainWindow behavior; keep the entire existing file byte-for-byte unchanged in every migration pass. Add any missing characterization in a separate focused test source/target rather than splitting, reformatting, or weakening this suite.

## 4. Proposed split and interfaces

Keep `ui::PdfViewerWidget` as the stable `QAbstractScrollArea` facade in `src/ui/PdfViewerWidget.h/.cpp`. Keep `ui::MainWindow` as the stable `QMainWindow` facade in `src/ui/MainWindow.h/.cpp`. New collaborators should be private implementation details (prefer private headers or declarations and `.cpp` files), created and destroyed with their owning facade. Do not expose a new public API merely to facilitate extraction.

| Proposed implementation unit | Responsibility and interface boundary |
|---|---|
| `ViewerLayoutController` (`ViewerLayoutController.h/.cpp`) | Owns page navigation, zoom/fit, rotation, viewport geometry, and 64-bit scroll mapping. Receives viewport dimensions and user navigation/zoom intent; reports resulting page/zoom/layout state to the facade. `PdfViewerWidget` continues translating Qt events and emitting the existing page/zoom/view/rotation signals. |
| `ViewerTileController` (`ViewerTileController.h/.cpp`) | Owns visible/prefetch tile request decisions, in-flight tracking, retry/failure handling, and fallback-scale selection. Borrows the existing `WorkerClient` and `TileCache`; returns ready-to-paint tile data to `PdfViewerWidget`. Does not paint widgets or own an engine/PDFium object. |
| `ViewerSelectionController` (`ViewerSelectionController.h/.cpp`) | Owns page-text cache policy, selection endpoints, text extraction/copy and selection geometry. Accepts pointer/keyboard-derived positions and a page-text provider backed by `WorkerClient`; the facade retains event dispatch and paints returned selection geometry. |
| `ViewerSearchController` (`ViewerSearchController.h/.cpp`) | Owns query, match-cycle and search-id state, starts/cancels worker searches, and provides match/highlight geometry. The facade forwards existing `searchResultsChanged`, `searchProgressChanged`, and `searchFinished` signals unchanged. |
| `ViewerAnnotationController` (`ViewerAnnotationController.h/.cpp`) | Owns annotation commands, placement, tooltip/hit-test policy, and annotation interaction state. Borrows `WorkerClient` and records every mutation through the same viewer edit-history owner. |
| `ViewerRedactionController` (`ViewerRedactionController.h/.cpp`) | Owns redaction-mode gestures, staged canonical-page marks and overlay geometry. It submits only the existing typed worker operations; the MainWindow facade retains the current apply/save action and confirmation behavior. |
| `ViewerFormController` (`ViewerFormController.h/.cpp`) | Owns bounded form-field page caches, hit testing, editor widgets/event filters and semantic field commands. Borrows `WorkerClient`; all field edits go through the shared edit-history owner. |
| `ViewerEditController` (`ViewerEditController.h/.cpp`) | Owns the sole `pdf::EditLog` instance, undo/redo dispatch, edit/recovery state and replay orchestration. Annotation and form collaborators call this owner rather than maintaining parallel histories. |
| `ViewerSaveController` (`ViewerSaveController.h/.cpp`) | Owns the existing save transaction/validation/commit steps and print/export requests, using the same viewer `WorkerClient` and edit-log revision. It cannot open paths in the worker except through the existing typed descriptor-based protocol. `PdfViewerWidget` keeps the public save/export methods and forwards results. |
| `MainWindow` private collaborators (initially `ReadingPositionStore`, then narrowly scoped tab/action helpers only where extraction is independent) | Move persistence and separable action wiring without changing `MainWindow`'s public methods, slots, widget getters, Qt object names, close flow, or active-tab routing. Keep dialogs and presentation decisions in the window facade unless a characterization test proves a helper boundary safe. |

Collaborators that need engine work receive a non-owning reference to the facade's existing `WorkerClient`; do not create a second worker client per responsibility. The worker remains the only process linked to PDFium. UI collaborators exchange bounded Qt/POD values and retain UI-thread ownership. Keep `Q_OBJECT`, signal/slot signatures, Qt event overrides, and connection context/lifetime semantics on the existing facades; forward signals rather than replacing them with new public collaborator signals.

## 5. Migration sequence

Each numbered pass is a separate, behavior-preserving PR based on the previous green pass. Keep the existing `test_widget.cpp` byte-for-byte identical and run the complete CTest suite/CI on every pass; a targeted suite is not a substitute.

| Pass | Work and order | Main risk and rollback |
|---|---|---|
| 0. Characterize | Add only missing focused characterization cases in a separate test source/target. Capture signal order, event outcomes, tile requests, active-tab behavior, edit-log state, save/reload and crash-replay results before moving code. Run and record the full existing widget suite unchanged. | Risk: tests encode an accidental detail rather than a contract. Keep characterization narrowly tied to observed current behavior. Roll back the added test-only PR independently. |
| 1. Layout and navigation | Extract geometry, scroll mapping, page navigation, fit/zoom and rotation into `ViewerLayoutController`; leave Qt event dispatch and public facade methods in place. | Risk: scroll-anchor, resize and page-flip rounding. Revert the extraction PR; no persisted data or protocol changes. |
| 2. Tile/render client | Extract tile request selection, in-flight tracking, fallback and retry orchestration into `ViewerTileController`; keep paint composition and `WorkerClient` ownership in the facade at first. | Risk: asynchronous ordering, request generations, shutdown and paint invalidation. Revert the extraction PR; preserve the old request/callback path until CI is green. |
| 3. Selection and search | Move text-cache/selection state and then search state into separate collaborators, sharing the existing worker provider and retaining the existing Qt event routing and signals. | Risk: cross-page Unicode selection, cache bounds and stale search replies. Revert each extraction independently; keep the pre-move characterization tests. |
| 4. Annotation and redaction interaction | Extract annotation command/placement/interaction state and redaction gesture/staging/overlay state. Keep window-level action and apply/save orchestration unchanged. | Risk: coordinate transforms, pointer-event precedence, staged marks and tooltip timing. Revert the affected extraction without changing annotation IPC or saved files. |
| 5. Forms, edit history and persistence | Extract form UI/cache first, then move the one edit-log owner and finally save/recovery/export orchestration. Preserve replay ordering, transaction validation, test hooks and shared log revision across annotations and forms. | Highest risk: undo symmetry, worker crash recovery and save atomicity. Keep these as separately revertible changes; no protocol or on-disk format changes. |
| 6. MainWindow decomposition | Extract reading-position storage and only then independently separable tab/action orchestration. Keep all existing facade slots/getters, action names, dialog choices, signal connections and close/unsaved-edit behavior. | Risk: Qt meta-object discovery and active-view routing. Revert helper extraction independently; retain the MainWindow facade as the sole compatibility surface. |

Every implementation pass must avoid broad formatting, renaming, or unrelated cleanup so a revert remains local and reviewable. If a pass cannot keep the full suite green, revert that pass rather than weakening a characterization or acceptance test.

## 6. Compatibility and safety constraints

* **`PdfViewerWidget` API:** retain the header path, class name/base class, constructor/destructor, every public method/signature, test-observation accessor, protected Qt override, signal, and `private Q_SLOTS` entry. Preserve QObject lifetime, thread affinity, signal order, and event acceptance/propagation.
* **`MainWindow` API:** retain the header path, class name/base class, public methods, `public Q_SLOTS`, widget getters, close override, action object names, and signal connections. Keep tab-specific viewer routing and unsaved-change behavior.
* **Tests/build targets:** keep `test_widget` target and its complete existing source unchanged in every pass. Preserve its direct compilation of `MainWindow.cpp` and `PdfViewerWidget.cpp` or update only the target's source list when extracting implementation files; do not remove coverage or substitute a smaller test.
* **Worker boundary:** UI code continues to use `pdf::ipc::WorkerClient` and bounded typed IPC. No UI target or new UI collaborator links PDFium or calls PDFium. Worker ownership, lifecycle, generation filtering, cancellation and failure recovery stay behavior-equivalent.
* **Edit ownership:** one UI-owned `pdf::EditLog` remains authoritative across annotations, forms, undo/redo, worker recovery, and saving. No collaborator may silently create a parallel stack or bypass the log for a user-visible mutation.
* **No product change:** no features, UI redesign, altered defaults, changed file/save semantics, protocol changes, or format changes are part of the refactor.

## 7. Acceptance criteria

1. The source split is measurable using the exact byte/line method in section 2. At completion, `PdfViewerWidget.cpp` is at most **1,600 lines** (from 4,658), `PdfViewerWidget.h` at most **250 lines** (from 496), and `MainWindow.cpp` at most **1,200 lines** (from 2,023). Report both final measurements and the same method; do not claim a target from byte size unless it is measured.
2. The original root-level `test_widget.cpp` remains byte-for-byte identical to `b0a01ea` throughout every pass: its Git blob is 396,802 bytes and the LF-normalized `git hash-object --no-filters --stdin` object ID for the working file must equal `git rev-parse b0a01ea:test_widget.cpp`. Its measured source count is 8,311 lines, but line-count equality alone is not the identity check. The complete `test_widget` CTest executes and passes on each implementation PR. Any additional characterization lives outside that file.
3. Linux CI is green for each implementation PR (Release, ASan/UBSan, TSan where configured, and shipping-hooks-off build). The already-green base run **44908715725** is reported only as baseline evidence, not as evidence for later PRs.
4. Existing public C++/Qt compatibility surfaces, signal/slot delivery, event behavior, and test hooks remain intact. No UI code links PDFium and no engine access bypasses `WorkerClient`.
5. There are no feature, behavior, UI, IPC, or persisted-format changes. Each extraction is isolated enough to revert without reverting later unrelated work.

## 8. Not verified by this RFC

T22 changes documentation only. No source was moved; no characterization test was added; no local build, CTest, or sanitizer suite was run. The measured ranges, file sizes and baseline CI result above are verified against `b0a01ea`; any CI on this docs-only PR validates the documentation diff, not implementation behavior. The proposed collaborator boundaries, completion targets and migration ordering remain design decisions to validate incrementally.
