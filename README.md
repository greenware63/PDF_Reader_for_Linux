# PDF Reader Workstation (Linux)

A high-performance, native PDF workstation for Linux designed with an Adobe Pro-grade feature set (forms, annotations, digital signatures, redaction) built using **C++20** and **Qt 6**, powered by **PDFium**.

## Architecture & Principles

- **Engine Foundation**: Uses Google's [PDFium](https://pdfium.googlesource.com/pdfium/) via prebuilt release binaries (`bblanchon/pdfium-binaries`), verified cryptographically with SHA256 checksums and integrated via CMake `FetchContent`.
- **Pure Qt-Free Raster Core**: `PixelBuffer` represents rasters with explicit byte strides and custom `std::shared_ptr<const void>` buffer ownership, decoupling the rendering core from GUI toolkits.
- **Standalone Layout Engine (`pdf_layout`)**: `DocumentLayout` is compiled as a standalone static library with **zero Qt and zero PDFium dependencies**. It operates entirely in 64-bit device pixel space, providing continuous page layout, multi-spread view modes (Continuous, Single Page, Two-Page, Two-Page with Cover), arbitrary page rotation, O(log N) binary search for visible pages, gap/boundary detection, horizontal centering, 2D cursor zoom anchor pinning, and Qt-free visible tile enumeration.
- **Separate PDF Worker Process**: The `pdf-reader` executable links to `pdf_ipc`, not `pdf_engine` or PDFium. It launches `pdf_worker` and sends document, tile, text, outline, thumbnail, print, search, and burn-in redaction requests over a versioned, bounded socket protocol. PDFium calls and page caching remain serialized on the worker's `RenderWorker` thread.
- **Shared-Memory Tile Transport**: Rendered tile pixels are transferred through per-tile `QSharedMemory` segments (`0600` permissions, strict UUID naming `pdf-tile-<pid>-<rand>`); only the segment key and layout metadata travel in the socket packet. The UI validates segment bounds via `fstat` and copies pixels into an owned `PixelBuffer` using `pread` before acknowledging and unlinking the segment.
- **Bounded IPC Framing**: Packets validate magic (`0x50444657`), protocol version, message type, and a 64 MiB payload ceiling. Deserialization rejects trailing data, validates collection bounds, and enforces size caps before memory allocation.
- **Descriptor Passing over `SCM_RIGHTS` (Phase 4 / Threat T1)**: The UI process opens user PDFs read-only (`O_RDONLY | O_CLOEXEC | O_NOCTTY`) and passes the file descriptor to `pdf_worker` over UNIX domain sockets via `SCM_RIGHTS` ancillary control messages. The UI process closes its local descriptor immediately after transmission. The worker receives descriptors, marks them `FD_CLOEXEC`, associates each descriptor strictly to the packet currently being assembled (rejecting and closing stray or unexpected descriptors immediately), and loads documents via `loadFromDescriptor`.
- **Descriptor-Backed PDFium Loading**:
  - $\le 512\text{ MiB}$: Cloned into an immutable heap memory buffer (`loadFromBuffer`), guaranteeing defense against SIGBUS from background file truncation or mutations while open.
  - $> 512\text{ MiB}$: Streamed via `pread`-backed `FPDF_FILEACCESS` with `EINTR` retry.
  - Redaction pipelines duplicate opened file descriptors (`fcntl(F_DUPFD_CLOEXEC)`) to read source bytes directly without path-based re-opens.
- **Strict Process Sandboxing & Fail-Closed Enforcement**:
  - **Landlock ABI 1–5**: Confines `pdf_worker` to deny **both read and write** access to the filesystem. Arbitrary file path opens in the worker return `EACCES`. Only essential system runtime paths (fonts, fontconfig, locales, timezone, `/dev/shm`, and Flatpak host font mapping paths `/run/host/{fonts,user-fonts,local-fonts,font-dirs.xml}`) are allowlisted.
  - **Fail-Closed Policy**: Linux systems lacking Landlock support (pre-5.13 or kernels without `CONFIG_SECURITY_LANDLOCK=y`) abort startup immediately with an explicit diagnostic error rather than degrading security boundaries silently.
  - **Seccomp BPF**: Architecture validation (killing on x32 ABI or architecture mismatch), strict denial of `execve`/`execveat` (SIGSYS), blocking `connect`, `socketcall`, and `io_uring_*`, and restricting `socket` domain to `AF_UNIX`.
  - **Resource Limits & Process Isolation**: `PR_SET_NO_NEW_PRIVS`, `PR_SET_PDEATHSIG = SIGTERM`, `PR_SET_DUMPABLE = 0`, `RLIMIT_CORE = 0` (disabling core dumps/ptrace), `RLIMIT_AS = 4 GiB`, `RLIMIT_CPU = 7200s` (`SIGXCPU` -> `_exit(70)`), and `RLIMIT_NOFILE = 256`.
  - **Heartbeat Watchdog & Liveness**: The worker IPC thread answers Ping requests with lock-free progress snapshots (`WorkerOp::Idle`, `Tile`, `Search`, `Open`, `Thumbnail`, `Print`, `Redact`, `Extract`). The UI watchdog detects stalls without false-positive timeouts during long-running operations.
- **Unconstrained Virtual Tiling**: Virtual page dimensions at deep zoom are unconstrained (supporting rasters up to 17,850 × 25,260 px and beyond). Rendering is decomposed into a 512×512 tile grid via `renderTile()` with origin translation `(-x0, -y0)` and LCD sub-pixel font fringing disabled.
- **Scale-Preserving Whole-Page Clamping**: For single-buffer whole-page convenience renders, rasters are safely clamped to 8192 px and 16 Megapixels to prevent bad allocation / OOM, scaling effective device pixel ratios dynamically so page layout is preserved. Individual tile requests are guarded against requests exceeding 4096 px per side or 16 MP.
- **Robust Object Lifetimes**: Pages hold shared ownership of the underlying document handle, preventing use-after-free even if the document is closed or reloaded while pages are retained.
- **Non-Destructive Loading & Restoration**: Failed document opens (corrupt file, wrong password, missing file) preserve any currently active document and its render queue intact.

On Windows, the worker process is launched as a separate process over QLocalSocket, but sandbox and resource confinement (Landlock, seccomp, PR_SET_NO_NEW_PRIVS, rlimits) are Linux-specific. Windows builds should currently be considered unsupported for secure processing of untrusted or hostile PDF files.


## Security Hardening & Compiler Diagnostics

The build system configures strict diagnostics and defense-in-depth compile-time/runtime hardening:
- **Compiler Warnings**: `-Wall -Wextra -Wpedantic -Wconversion` enabled across all GCC and Clang builds. Implicit integer conversions and sign/narrowing issues are treated seriously and resolved in source code without suppressing warnings broadly.
- **Standard Library Assertions**: `_GLIBCXX_ASSERTIONS` enabled on Linux/UNIX builds for bounds checking in standard library containers.
- **Stack Protection**: `-fstack-protector-strong` enabled on Linux builds for stack buffer overflow protection.
- **Source Fortification**: `_FORTIFY_SOURCE=3` configured for glibc Release and optimized builds (`Release`, `RelWithDebInfo`, `MinSizeRel`), guarded so non-glibc or unoptimized debug builds remain unaffected.
- **Binary Hardening Checks in CI**: Asserts Position Independent Executable (`PIE`), full Read-Only Relocations (`GNU_RELRO`), and Immediate Binding (`BIND_NOW` / `-z now`) on both `pdf-reader` and `pdf_worker` binaries.

## Project Structure

```
├── ci/
│   ├── annotate-failed-tests   # CI failure reporting script
│   ├── linux-ci.yml            # Reference GitHub Actions workflow
│   └── tsan_suppressions.txt   # ThreadSanitizer suppressions
├── docs/
│   ├── KNOWN_LIMITATIONS.md    # Unsupported features and specification disclosures
│   ├── REDACTION.md            # Burn-in pixel redaction specification and guarantees
│   ├── REDACTION_TEST_REPORT.md# Empirical redaction verification report
│   └── threat_model.md         # Comprehensive process isolation threat model (T1–T12)
├── include/
│   ├── cache/
│   │   └── TileCache.h         # In-memory tile cache with LRU eviction and failure backoff
│   ├── engine/
│   │   ├── IDocument.h         # Abstract document interface & DocumentError
│   │   ├── IPage.h             # Abstract page interface, PageText, and PixelRect
│   │   ├── PixelBuffer.h       # Pure C++ raster abstraction with custom owner
│   │   └── RenderWorker.h      # Serialized background render thread & request queue
│   ├── ipc/
│   │   ├── IpcMessages.h       # Typed worker request/response serialization
│   │   ├── IpcProtocol.h       # Versioned, bounded packet framing
│   │   ├── SharedTile.h        # Shared memory tile transport helpers
│   │   └── WorkerClient.h      # UI-side worker process lifecycle and IPC client
│   └── layout/
│       ├── DocumentLayout.h    # 64-bit continuous/multi-spread layout, binary search & tile enumeration
│       └── ScrollMapper.h      # Qt-free 64-bit to 32-bit scrollbar mapping & resize remapping
├── packaging/
│   └── flatpak/                # Flatpak manifest (org.greenware.PDFReader.yaml) & AppStream metadata
├── resources/                  # Application icons, desktop entry, and SVG assets
├── scripts/
│   └── test_flatpak_sandbox.sh # Flatpak sandbox isolation verification script
├── src/
│   ├── cache/
│   │   └── TileCache.cpp       # Thread-confined tile caching & recovery tombstones
│   ├── engine/
│   │   ├── PdfiumDocument.h/cpp# Engine implementation, handle management, and descriptor loading
│   │   ├── PdfiumPage.h/cpp    # Page rendering, unconstrained tiles, and text extraction
│   │   ├── RenderWorker.cpp    # Serialized worker thread, priority queue & document lifecycle
│   │   └── SearchText.h/cpp    # Unicode/Persian-normalized text search and snippet extraction
│   ├── ipc/
│   │   ├── IpcMessages.cpp     # IPC serializers and deserializers
│   │   ├── SharedTile.cpp      # Shared memory segment management
│   │   └── WorkerClient.cpp    # Worker process lifecycle, SCM_RIGHTS descriptor passing, and client IPC
│   ├── layout/
│   │   └── DocumentLayout.cpp  # Qt-free layout calculations, view modes, and 2D anchors
│   ├── redact/
│   │   └── Redaction.h/cpp     # Burn-in pixel redaction, PDF reconstruction, and sanitization
│   ├── ui/
│   │   ├── ColorMode.h         # Viewport color modes (Normal, Inverted, Sepia, High Contrast)
│   │   ├── FindBar.h/cpp       # Find bar with match count, navigation, and RTL support
│   │   ├── ImageUtils.h        # Zero-copy wrapper from PixelBuffer to QImage
│   │   ├── MainWindow.h/cpp    # Desktop main window, actions, tabs, and session state
│   │   ├── PdfViewerWidget.h/cpp# Viewport widget with zoom, navigation, selection, and redaction
│   │   ├── RedactionFileSafety.h# Redaction output file collision and overwrite guards
│   │   ├── SearchResultsWidget.h/cpp# Sidebar search results list with snippets and jump
│   │   └── ThumbnailListWidget.h/cpp# Sidebar page thumbnails and page label navigation
│   └── worker/
│       ├── main.cpp            # Headless worker entry point, Landlock, seccomp, and rlimits
│       └── WorkerServer.h/cpp  # Socket server, SCM_RIGHTS FD reception, and RenderWorker bridge
├── tests/
│   ├── fixtures/               # Test fixtures (sample PDFs, encrypted documents, unicode fixtures, redaction inputs)
│   │   ├── make_unicode_fixtures.py # Python script generating CJK and embedded unicode test fixtures
│   │   ├── unicode_embedded.pdf     # Embedded Latin, Cyrillic, Greek, and Persian glyph fixture
│   │   └── unicode_system_font.pdf  # Non-embedded CID Japanese CJK system font test fixture
│   ├── fuzz/
│   │   └── fuzz_ipc_messages.cpp # Mutated IPC deserialization fuzz test
│   ├── redaction/
│   │   ├── PdfInspector.h      # Independent PDF structure and stream inspector
│   │   └── test_redaction.cpp  # End-to-end redaction engine test suite
│   ├── support/
│   │   └── TestIsolation.h     # Headless QSettings temporary file isolation helper
│   └── ui/
│       ├── test_redaction_file_safety.cpp # Redaction save guards unit tests
│       └── test_redaction_rotation.cpp    # Pixel-level end-to-end redaction rotation tests
├── CMakeLists.txt              # Modular CMake build configuration with CTest & FetchContent
├── LICENSE                     # MIT License
├── main.cpp                    # Application entry point
├── sample.pdf                  # Reference PDF for validation
├── test_layout.cpp             # Layout engine test suite
├── test_password_prompt.cpp    # Password prompt and encrypted document test suite
├── test_render.cpp             # Engine rendering and document loading test suite
├── test_tile_cache.cpp         # In-memory tile cache test suite
├── test_widget.cpp             # Headless viewport widget & integration test suite
└── test_worker_ipc.cpp         # Worker IPC, process lifecycle, and sandbox isolation test suite
```

## Known Limitations

Form filling, PDF JavaScript and XFA forms are not supported: stored field values are displayed, but nothing is
computed, validated or editable. See [docs/KNOWN_LIMITATIONS.md](docs/KNOWN_LIMITATIONS.md).

## Building

### Requirements
- CMake 3.24+
- C++20 compliant compiler: Tested and validated on **GCC 13+** (e.g. Ubuntu 24.04 / MinGW GCC 15) and **Clang 16+**. *(Older toolchains such as GCC 11 or Clang 14 are not claimed or supported without verification of required C++20 standard library features).*
- Qt 6 (Core, Gui, Widgets, Network, PrintSupport)

### Build Commands

```bash
# Configure and build in Release mode
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Configure with test fault-injection hooks enabled (for unit testing watchdog stalls/rlimits)
cmake -B build-test -DCMAKE_BUILD_TYPE=Release -DENABLE_TEST_HOOKS=ON
cmake --build build-test
```

### Running Automated Test Suites via CTest

```bash
ctest --test-dir build --output-on-failure
```

#### 1. Engine Test Suite (`EngineRenderTestSuite` - 25 Tests):
1. Missing file handling (`DocumentError::FileNotFound`)
2. Null/empty memory buffer rejection
3. Garbage memory buffer rejection (`DocumentError::InvalidFormat`)
4. Standard PDF loading and page count
5. Non-destructive loading verification (failed open preserves active document)
6. Page point geometry and valid raster generation
7. Rendered glyph/graphic verification (non-white pixel check)
8. In-memory document buffer lifetime preservation across `doc->close()`
9. Size cap enforcement (8192 px / 16 MP whole-page clamp, 4096 px / 16 MP tile rejection)
10. Document reload while retaining prior page handle references
11. `IDocument::pageSizePoints` cross-check against layout and engine dimensions to exact pixel
12. Complex multi-feature fixture tile stitching & 37px grid-shift analysis (axial gradients, clips, alpha transparency; verifies 0 visible seams across standard and shifted grids)
13. Extreme zoom sub-pixel edge straddling (15× zoom × 2× DPR = 17,850 × 25,260 px, ~450 MP virtual space; edge transition verified at exact local pixel)
14. `PageText` extraction, character bounding boxes, hit-testing, and range selection
15. Supplementary Unicode code points (>0xFFFF) and surrogate pair extraction
16. Fallback text reconstruction across merged surrogate pairs in `PdfiumPage`
17. Persian RTL text extraction, bidirectional highlight box merging, and background search
18. Persian/Arabic normalization, whitespace folding, and bounded search results
19. Document Outline / Bookmarks hierarchical extraction and cycle bounding
20. PDF page labels with and without `/PageLabels` dictionaries
21. Rotated whole-page, thumbnail, and tile rendering with asymmetric corner-color fixture (0, 90, 180, 270 degrees)
22. Redaction source pinning, sync-open registration, and large-file change guard
23. Form field (widget) appearances and visible annotations rendering
24. Search result snippets generation, bounding, ellipsis handling, and whitespace collapsing
25. Descriptor-backed document loading (`loadFromDescriptor`) and `pread` streaming path

#### 2. Document Layout Test Suite (`DocumentLayoutTestSuite` - 12 Tests):
1. Empty document zero-state validation
2. Untrusted dimension hardening (rejects NaN, Inf, negatives; clamps 1e9 pt MediaBox without overflow)
3. Horizontal centering relative to scrollable content width in both narrow and wide viewports
4. Boundary edge cases (above document, below document, inside page gaps, exact page edges)
5. Property test: 1,000 random viewport queries cross-checked against linear brute-force scan
6. Visible tile partition & coverage invariant across random viewports
7. Physical page coordinate pinning (PDF points under cursor preserved across zoom)
8. 64-bit coordinate verification exceeding $2^{31}$ (4.21 billion pixels without integer overflow)
9. ScrollMapper bidirectional coordinate mapping & 1,000 randomized property tests
10. Compressed-scroll resize remapping edge case (> $10^9$ slider range)
11. View modes (Continuous, Single Page, Two-Page, Two-Page with Cover): row structure, no overlap, visible-page search vs. brute force
12. Arbitrary page rotation in `DocumentLayout` (90, 180, 270 degrees)

#### 3. Tile Cache Test Suite (`TileCacheTestSuite` - 13 Tests):
1. Default parameters and byte budget configuration
2. LRU eviction order and byte accounting
3. Pinned tile protection during memory pressure
4. Invalidation of document entries and orphan pin pruning
5. DPR quantization and NaN/Inf clamping
6. Scale quantization (milli-units rounding)
7. Failure marking and tombstone accounting (256 bytes)
8. Buffer eviction behavior on invalid buffer input
9. Key uniqueness across document IDs, scale, and DPR
10. Cache clear operation
11. Failure tombstone recovery, exponential backoff, and explicit clearing
12. Deterministic failure backoff intervals strictly growing across retries
13. Permanent failure after `MAX_RETRY_ATTEMPTS` lifted by `resetFailureHistory()`

#### 4. Viewport Widget Test Suite (`ViewportWidgetTestSuite` - 45 Tests):
1. Failed open document preservation and continued tile rendering
2. Multi-page navigation when scroll position clamps at bottom
3. DPR injection (1.0, 1.5, 2.0) and tile seam continuity
4. 2D cursor zoom anchor precision (tolerance $\le 1.5$ px)
5. Document swap cache invalidation
6. Clean teardown on `closeDocument()`
7. Focused fit-width regression test (0 horizontal scrollbar range when vertical scrollbar appears)
8. Document scroll coordinate preservation across window resizes
9. RenderWorker cancellation and request generations
10. Document replacement under pending render requests
11. Clean widget destruction under background render load
12. Tile failure recovery and explicit retry triggers
13. Tile delivery during continuous smooth scrolling (no starvation)
14. In-flight tile deduplication on worker thread
15. Worker page cache LRU bound on 200-page document
16. `openDocumentAsync` does not block the UI thread
17. Failed asynchronous open preserves current document and rendering
18. Back-to-back asynchronous opens resolve in a consistent state
19. `closeDocument` while an asynchronous open is in flight
20. `RenderWorker::openDocumentAsync` contract (success, failure, `waitForIdle`)
21. Zoom fallback (scaled previous-zoom tiles displayed while new tiles render)
22. Prefetch ring (off-screen neighboring tiles rendered after visible tiles)
23. Asynchronous `PageText` extraction arrival
24. Mouse drag text selection and hover cursor feedback
25. `copySelection` API and Ctrl+C clipboard copy
26. Cross-page mouse drag selection and multi-page text extraction
27. Supplementary Unicode character selection and bounding boxes
28. Text selection box alignment on rotated/cropped pages
29. `goToPage` discards obsolete queued tiles
30. Extracted-text cache capacity bounding
31. Text extraction response handling for corrupt/unloadable pages
32. Full-text search (Ctrl+F), FindBar, match cycling, and Persian search
33. Document Outline / Bookmarks Sidebar and TreeView navigation
34. Worker crash survival and recovery banner
35. Thumbnails & Page Labels Sidebar, worker thumbnail rendering, and viewport sync
36. Multi-document tabs and recent files session management
37. Native printing rasterization over IPC and headless PDF export
38. Dark mode toggle, `#181818` viewport color, inverted pixels, and reversibility
39. Reading position memory persistence across documents/sessions in `QSettings` (DPI-independent fractional offset and fingerprint recovery)
40. Comprehensive keyboard navigation (arrows, Page Up/Down, Home/End, Ctrl+Home/End)
41. View modes, fit page, and navigation per mode
42. Page rotation in `PdfViewerWidget` and `MainWindow`
43. Reading color modes (Normal, Inverted, Sepia, High Contrast)
44. Burn-in redaction UI mode, mark staging, 1.5 pt selection margin, viewport rendering, and `MainWindow` binding
45. Sidebar Search results panel with hit snippets and click-to-jump

#### 5. Worker IPC & Process Isolation Test Suite (`WorkerIpcTestSuite` - 28 Tests):
- **0S**: SearchMatch snippet serialization, bounds checking, and hostile snippet rejection
- **0**: Shared-tile transport security, hostile segment name rejection, and shrunken segment SIGBUS defense
- **1**: Worker process spawn, versioned protocol negotiation, and Ping/Pong handshake
- **2**: Document loading over IPC with descriptor passing
- **3**: Tile rasterization and shared-memory tile buffer transfer
- **4**: Asynchronous page text extraction over IPC
- **5 & 5.1**: Document outline and page thumbnail rendering over IPC
- **6**: Clean worker process exit on socket disconnect
- **7**: Crash resilience (abrupt worker termination caught cleanly without crashing UI)
- **8**: Deterministic worker isolation escape probe (`--isolation-selftest` verifying write confinement to `/tmp`, read confinement asserting `read_outside: BLOCKED`, seccomp D-Bus/TCP denial, and foreign `/dev/shm` access)
- **9A**: Heartbeat watchdog progress snapshot verification during simulated engine hang
- **9B**: Watchdog re-arming across document close/reopen and live stall timeout detection
- **9C**: Progressing long workloads survive without false watchdog timeouts
- **9D**: Workload validation asserting `VmPeak` $< 4\text{ GiB}$ under 4 GiB `RLIMIT_AS`
- **9E**: Worker responds to heartbeats while `waitForIdle()` is pending
- **10**: Explicit abort and diagnostic handling when resource limits fail (`--test-fail-rlimit`)
- **11**: Burn-in redaction request/response execution through sandboxed worker IPC
- **12**: Rejection of malformed `RedactDocumentRequest` frames
- **13**: Redaction immunity to source file mutation/deletion after opening
- **14**: Redaction progress heartbeats keep watchdog and client silence limit alive
- **15**: Valid document descriptor passing over `SCM_RIGHTS` and rendering
- **16**: Worker Landlock read confinement blocks arbitrary file path opens
- **17**: Unexpected descriptor hygiene (unsolicited descriptors closed immediately without leaks)
- **18**: Worker startup strictly fails closed when Landlock is unavailable (`--test-fail-landlock`)
- **19A–19I**: Outside-worker observable descriptor lifecycle across normal open, rejection, and error paths via pipe `POLLERR` inspection (strict association to the packet carrying the descriptor's first byte, rejection/closure on non-Open requests, stray descriptor closure, queued packet boundary preservation, multi-descriptor rejection, malformed payload hygiene, invalid header closure, and client disconnect mid-packet)
- **20a–20b**: Non-Latin text rendering inside the confined worker across embedded glyphs (`unicode_embedded.pdf` - Cyrillic, Greek, Persian) and system CJK CID fonts via fontconfig through Landlock read allowlist (`unicode_system_font.pdf` - Japanese)
- **21a–21b**: Live font-root probing (`--isolation-selftest --test-probe-read`) verifying the confined worker can read all font roots reported by fontconfig, enforcing Flatpak host font accessibility under `/run/host/fonts`, and reporting skips when host fonts are absent

#### 6. Specialized Test Suites:
- **`PasswordPromptTestSuite`**: Automated headless testing of encrypted PDF handling, wrong password rejection, and correct password unlocking.
- **`RedactionRotationTestSuite`**: End-to-end pixel-level redaction verification with pdftoppm witness across rotated pages and multi-page fixtures.
- **`RedactionFileSafetyTestSuite`**: Source file overwrite prevention and atomic all-or-nothing save guards.
- **`RedactionTestSuite`**: Comprehensive redaction engine tests verifying structural sanitization, complete metadata elimination, and independent parser inspection.
- **`IpcFuzzTestSuite`**: 1,000+ randomized mutation fuzz tests verifying parser resilience against malformed IPC packets.

### Flatpak Sandbox Verification

To verify Landlock confinement and host font mapping inside the official Flatpak container environment on Linux/WSL2:
```bash
./scripts/test_flatpak_sandbox.sh
```

## Continuous Integration

A GitHub Actions workflow is provided in [`.github/workflows/ci.yml`](.github/workflows/ci.yml) targeting `ubuntu-24.04`:
- **Release Matrix Build**: Release build with fault-injection test hooks (`ENABLE_TEST_HOOKS=ON`) executing all automated CTest test suites with offscreen platform (`QT_QPA_PLATFORM=offscreen`).
- **Sanitizer Matrix Build (ASan / UBSan)**: `RelWithDebInfo` build with AddressSanitizer and UndefinedBehaviorSanitizer (`detect_leaks=1:abort_on_error=1`).
- **ThreadSanitizer Matrix Build (TSan)**: Multi-threaded race detection across single-process layout, cache, and engine suites.
- **Shipping Configuration Job (`ENABLE_TEST_HOOKS=OFF`)**: Compiles with shipping defaults, asserts fault-injection hooks and `--test-*` flags are stripped from the worker binary, verifies binary hardening flags (`PIE`, full `RELRO`, `BIND_NOW`), and runs CTest.
