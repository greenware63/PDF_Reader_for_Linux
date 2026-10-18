# PDF Reader Workstation (Linux)

A high-performance, native PDF workstation for Linux designed with an Adobe Pro-grade feature set (forms, annotations, digital signatures, redaction) built using **C++20** and **Qt 6**, powered by **PDFium**.

## Architecture & Principles

- **Engine Foundation**: Uses Google's [PDFium](https://pdfium.googlesource.com/pdfium/) via prebuilt release binaries (`bblanchon/pdfium-binaries`), verified cryptographically with SHA256 checksums and integrated via CMake `FetchContent`.
- **Pure Qt-Free Raster Core**: `PixelBuffer` represents rasters with explicit byte strides and custom `std::shared_ptr<const void>` buffer ownership, decoupling the rendering core from GUI toolkits.
- **Standalone Layout Engine (`pdf_layout`)**: `DocumentLayout` is compiled as a standalone static library with **zero Qt and zero PDFium dependencies**. It operates entirely in 64-bit device pixel space, providing continuous page layout, O(log N) binary search for visible pages, gap/boundary detection, horizontal centering, 2D cursor zoom anchor pinning, and Qt-free visible tile enumeration.
- **Separate PDF Worker Process**: The `pdf-reader` executable links to `pdf_ipc`, not `pdf_engine` or PDFium. It launches `pdf_worker` and sends document, tile, text, outline, and search requests over a versioned `QLocalSocket` protocol. PDFium calls and page caching remain serialized on the worker's `RenderWorker` thread.
- **Shared-Memory Tile Transport**: Rendered tile pixels are transferred through per-tile `QSharedMemory` segments; only the segment key and layout metadata travel in the socket packet. The UI copies pixels into an owned `PixelBuffer` before acknowledging and releasing the segment.
- **Bounded IPC Framing**: Packets validate magic, protocol version, message type, and a 64 MiB payload ceiling. Deserialization rejects trailing data and bounds collection sizes before allocation.
- **Unconstrained Virtual Tiling**: Virtual page dimensions at deep zoom are unconstrained (supporting rasters up to 17,850 × 25,260 px and beyond). Rendering is decomposed into a 512×512 tile grid via `renderTile()` with origin translation `(-x0, -y0)` and LCD sub-pixel font fringing disabled.
- **Scale-Preserving Whole-Page Clamping**: For single-buffer whole-page convenience renders, rasters are safely clamped to 8192 px and 16 Megapixels to prevent bad allocation / OOM, scaling effective device pixel ratios dynamically so page layout is preserved. Individual tile requests are guarded against requests exceeding 4096 px per side or 16 MP.
- **Robust Object Lifetimes**: Pages hold shared ownership of the underlying document handle, preventing use-after-free even if the document is closed or reloaded while pages are retained.
- **In-Memory Lifetime Safety**: In-memory byte buffers are tied to the document handle's custom deleter, ensuring zero dangling pointer access during lifetime overlaps.
- **Non-Destructive Loading & Restoration**: Failed document opens (corrupt file, wrong password, missing file) preserve any currently active document and its render queue intact.

On Linux, the worker enables `PR_SET_PDEATHSIG` and requires `PR_SET_NO_NEW_PRIVS` plus Landlock. Reads remain unrestricted so the worker can open PDFs selected from arbitrary locations. Filesystem writes and creation/removal operations are denied by default, with a scoped `/dev/shm` allowance for the shared-memory tile transport. The IPC socket uses Linux's abstract namespace, so it does not need a filesystem-write exception. This is filesystem write confinement, not a complete sandbox; seccomp and read-path confinement are not implemented. If required setup fails or the kernel lacks Landlock, the worker exits instead of running with weaker isolation.

On Windows, the worker process is launched as a separate process over QLocalSocket, but sandbox and resource confinement (Landlock, PR_SET_NO_NEW_PRIVS, rlimits) are Linux-specific. Windows builds should currently be considered unsupported for secure processing of untrusted or hostile PDF files.


## Security Hardening & Compiler Diagnostics

The build system configures strict diagnostics and defense-in-depth compile-time/runtime hardening:
- **Compiler Warnings**: `-Wall -Wextra -Wpedantic -Wconversion` enabled across all GCC and Clang builds. Implicit integer conversions and sign/narrowing issues are treated seriously and resolved in source code without suppressing warnings broadly.
- **Standard Library Assertions**: `_GLIBCXX_ASSERTIONS` enabled on Linux/UNIX builds for bounds checking in standard library containers.
- **Stack Protection**: `-fstack-protector-strong` enabled on Linux builds for stack buffer overflow protection.
- **Source Fortification**: `_FORTIFY_SOURCE=3` configured for glibc Release and optimized builds (`Release`, `RelWithDebInfo`, `MinSizeRel`), guarded so non-glibc or unoptimized debug builds remain unaffected.

## Project Structure

```
├── include/
│   ├── cache/
│   │   └── TileCache.h         # In-memory tile cache with LRU eviction and failure backoff
│   ├── engine/
│   │   ├── IDocument.h         # Abstract document interface & DocumentError
│   │   ├── IPage.h             # Abstract page interface & PixelRect
│   │   ├── PixelBuffer.h       # Pure C++ raster abstraction with custom owner
│   │   └── RenderWorker.h      # Serialized background render thread & request queue
│   └── layout/
│       ├── DocumentLayout.h    # 64-bit continuous layout, binary search & tile enumeration
│       └── ScrollMapper.h      # Qt-free 64-bit to 32-bit scrollbar mapping & resize remapping
│   └── ipc/
│       ├── IpcMessages.h       # Typed worker request/response serialization
│       └── IpcProtocol.h       # Versioned, bounded packet framing
├── src/
│   ├── cache/
│   │   └── TileCache.cpp       # Thread-confined tile caching & recovery tombstones
│   ├── engine/
│   │   ├── PdfiumDocument.h/cpp# Engine implementation & safe handle management
│   │   ├── PdfiumPage.h/cpp    # Page rendering, unconstrained tiles & clamped whole-page
│   │   └── RenderWorker.cpp    # Serialized worker thread, priority queue & document lifecycle
│   ├── layout/
│   │   └── DocumentLayout.cpp  # Qt-free layout calculations, 2D anchors & tile grids
│   ├── ipc/
│   │   ├── IpcMessages.cpp     # IPC serializers
│   │   └── WorkerClient.cpp    # Worker process lifecycle and UI-side IPC client
│   ├── worker/
│   │   ├── main.cpp            # Headless PDF worker entry point
│   │   └── WorkerServer.cpp    # Socket server and RenderWorker bridge
│   └── ui/
│       ├── ImageUtils.h        # Zero-copy wrapper from PixelBuffer to QImage
│       ├── MainWindow.h/cpp    # Desktop main window & actions
│       └── PdfViewerWidget.h/cpp# Viewport widget with zoom, navigation & async tile rendering
├── ci/
│   └── linux-ci.yml            # GitHub Actions CI workflow (Release + ASan/UBSan)
├── CMakeLists.txt              # Modular CMake build configuration with CTest & FetchContent
├── LICENSE                     # MIT License
├── main.cpp                    # Application entry point
├── sample.pdf                  # Reference PDF for validation
├── test_render.cpp             # Automated 13-part engine & tile CTest test suite
├── test_layout.cpp             # Automated 10-part layout & property CTest test suite
├── test_tile_cache.cpp         # Automated 11-part tile cache CTest test suite
└── test_widget.cpp             # Automated 12-part headless viewport widget CTest test suite
```

## Building

### Requirements
- CMake 3.24+
- C++20 compliant compiler: Tested and validated on **GCC 13+** (e.g. Ubuntu 24.04 / MinGW GCC 15) and **Clang 16+**. *(Older toolchains such as GCC 11 or Clang 14 are not claimed or supported without verification of required C++20 standard library features).*
- Qt 6 (Core, Gui, Widgets)

### Build Commands

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Running Automated Test Suites via CTest

```bash
ctest --test-dir build --output-on-failure
```

#### 1. Engine Test Suite (`EngineRenderTestSuite` - 13 Tests):
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

#### 2. Document Layout Test Suite (`DocumentLayoutTestSuite` - 10 Tests):
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

#### 3. Tile Cache Test Suite (`TileCacheTestSuite` - 11 Tests):
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
11. Failure tombstone recovery, exponential backoff, and invalidation triggers

#### 4. Viewport Widget Test Suite (`ViewportWidgetTestSuite` - 12 Tests):
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

## Continuous Integration

A GitHub Actions workflow is provided in [`ci/linux-ci.yml`](ci/linux-ci.yml) targeting `ubuntu-24.04`:
- Matrix testing for **Release** and **Sanitizer** (AddressSanitizer + UndefinedBehaviorSanitizer).
- Automated test execution under CTest with `QT_QPA_PLATFORM=offscreen`.
*(To activate on GitHub, copy `ci/linux-ci.yml` to `.github/workflows/ci.yml`).*
