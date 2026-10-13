# PDF Reader Workstation (Linux)

A high-performance, native PDF workstation for Linux designed with an Adobe Pro-grade feature set (forms, annotations, digital signatures, redaction) built using **C++20** and **Qt 6**, powered by **PDFium**.

## Architecture & Principles

- **Engine Foundation**: Uses Google's [PDFium](https://pdfium.googlesource.com/pdfium/) via prebuilt release binaries (`bblanchon/pdfium-binaries`), verified cryptographically with SHA256 checksums and integrated via CMake `FetchContent`.
- **Pure Qt-Free Raster Core**: `PixelBuffer` represents rasters with explicit byte strides and custom `std::shared_ptr<const void>` buffer ownership, decoupling the rendering core from GUI toolkits and preparing for zero-copy Linux `memfd_create` IPC in Phase 3.
- **Standalone Layout Engine (`pdf_layout`)**: `DocumentLayout` is compiled as a standalone static library with **zero Qt and zero PDFium dependencies**. It operates entirely in 64-bit device pixel space, providing continuous page layout, O(log N) binary search for visible pages, gap/boundary detection, horizontal centering, 2D cursor zoom anchor pinning, and Qt-free visible tile enumeration.
- **Strict Engine Abstraction (`pdf_engine`)**: The viewport and UI layers interact exclusively through `IDocument`, `IPage`, and `PixelBuffer` interfaces without exposing PDFium types.
- **Single-Threaded PDFium Concurrency Model**: Conforms strictly to PDFium's single-threaded API contract. PDFium calls are serialized to a single dedicated engine thread, with future multi-core parallelism reserved for Phase 3 process-level isolation.
- **Unconstrained Virtual Tiling**: Virtual page dimensions at deep zoom are unconstrained (supporting rasters up to 17,850 × 25,260 px and beyond). Rendering is decomposed into a 512×512 tile grid via `renderTile()` with origin translation `(-x0, -y0)` and LCD sub-pixel font fringing disabled.
- **Scale-Preserving Whole-Page Clamping**: For single-buffer whole-page convenience renders, rasters are safely clamped to 8192 px and 16 Megapixels to prevent bad allocation / OOM, scaling effective device pixel ratios dynamically so page layout is preserved. Individual tile requests are guarded against requests exceeding 4096 px per side or 16 MP.
- **Robust Object Lifetimes**: Pages hold shared ownership of the underlying document handle, preventing use-after-free even if the document is closed or reloaded while pages are retained.
- **In-Memory Lifetime Safety**: In-memory byte buffers are tied to the document handle's custom deleter, ensuring zero dangling pointer access during lifetime overlaps.
- **Non-Destructive Loading**: Failed document opens (corrupt file, wrong password, missing file) preserve any currently active document.

## Project Structure

```
├── include/
│   ├── engine/
│   │   ├── IDocument.h          # Abstract document interface & DocumentError
│   │   ├── IPage.h              # Abstract page interface & PixelRect
│   │   └── PixelBuffer.h        # Pure C++ raster abstraction with custom owner
│   └── layout/
│       └── DocumentLayout.h     # 64-bit continuous layout, binary search & tile enumeration
├── src/
│   ├── engine/
│   │   ├── PdfiumDocument.h/cpp # Engine implementation & safe handle management
│   │   └── PdfiumPage.h/cpp     # Page rendering, unconstrained tiles & clamped whole-page
│   ├── layout/
│   │   └── DocumentLayout.cpp   # Qt-free layout calculations, 2D anchors & tile grids
│   └── ui/
│       ├── ImageUtils.h         # Zero-copy wrapper from PixelBuffer to QImage
│       ├── MainWindow.h/cpp     # Desktop main window & actions
│       └── PdfViewerWidget.h/cpp# Viewport widget with zoom and navigation
├── ci/
│   └── linux-ci.yml             # GitHub Actions CI workflow (Release + ASan/UBSan)
├── CMakeLists.txt               # Modular CMake build configuration with CTest & FetchContent
├── LICENSE                      # MIT License
├── main.cpp                     # Application entry point
├── sample.pdf                   # Reference PDF for validation
├── test_render.cpp              # Automated 13-part engine & tile CTest test suite
└── test_layout.cpp              # Automated 8-part layout & property CTest test suite
```

## Building

### Requirements
- CMake 3.24+
- C++20 compliant compiler (GCC 13+, Clang 16+, MSVC)
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

#### Engine Test Suite (`EngineRenderTestSuite` - 13 Tests):
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

#### Document Layout Test Suite (`DocumentLayoutTestSuite` - 8 Tests):
1. Empty document zero-state validation
2. Untrusted dimension hardening (rejects NaN, Inf, negatives; clamps 1e9 pt MediaBox without overflow)
3. Horizontal centering relative to scrollable content width in both narrow and wide viewports
4. Boundary edge cases (above document, below document, inside page gaps, exact page edges)
5. Property test: 1,000 random viewport queries cross-checked against linear brute-force scan
6. Visible tile partition & coverage invariant across random viewports
7. Physical page coordinate pinning (PDF points under cursor preserved across zoom)
8. 64-bit coordinate verification exceeding $2^{31}$ (4.21 billion pixels without integer overflow)

## Continuous Integration

A GitHub Actions workflow is provided in [`ci/linux-ci.yml`](ci/linux-ci.yml) targeting `ubuntu-24.04`:
- Matrix testing for **Release** and **Sanitizer** (AddressSanitizer + UndefinedBehaviorSanitizer).
- Automated test execution under CTest.
*(To activate on GitHub, copy `ci/linux-ci.yml` to `.github/workflows/ci.yml`).*
