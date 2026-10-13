# PDF Reader Workstation (Linux)

A fast, native PDF workstation for Linux designed with an Adobe Pro-grade feature set (forms, annotations, digital signatures, redaction) built using **C++20** and **Qt 6**, powered by **PDFium**.

## Architecture & Principles

- **Engine Foundation**: Uses Google's [PDFium](https://pdfium.googlesource.com/pdfium/) via prebuilt release binaries (`bblanchon/pdfium-binaries`), verified cryptographically with SHA256 checksums and integrated via CMake `FetchContent`.
- **Strict Engine Abstraction**: The viewport and UI layers interact exclusively through `IDocument` and `IPage` interfaces without exposing PDFium types.
- **Robust Object Lifetimes**: Pages hold shared ownership of the underlying document handle, preventing use-after-free even if the document is closed or reloaded while pages are retained.
- **In-Memory Lifetime Safety**: In-memory byte buffers are tied to the document handle's custom deleter, ensuring zero dangling pointer access during lifetime overlaps.
- **Non-Destructive Loading**: Failed document opens (corrupt file, wrong password, missing file) preserve any currently active document.
- **Scale-Preserving Clamping**: Ultra-high zoom renders are bounded to 8192 px and 16 Megapixels to prevent bad allocation / OOM, scaling effective device pixel ratios dynamically so page layout is preserved.

## Project Structure

```
├── include/
│   └── engine/
│       ├── IDocument.h          # Abstract document interface & DocumentError
│       └── IPage.h              # Abstract page interface
├── src/
│   ├── engine/
│   │   ├── PdfiumDocument.h/cpp # Engine implementation & safe handle management
│   │   └── PdfiumPage.h/cpp     # Page rendering logic with resolution clamping
│   └── ui/
│       ├── MainWindow.h/cpp     # Desktop main window & actions
│       └── PdfViewerWidget.h/cpp# Viewport widget with zoom and navigation
├── ci/
│   └── linux-ci.yml             # GitHub Actions CI workflow (Release + ASan/UBSan)
├── CMakeLists.txt               # CMake build configuration with CTest & FetchContent
├── LICENSE                      # MIT License
├── main.cpp                     # Application entry point
├── sample.pdf                   # Reference PDF for validation
└── test_render.cpp              # Automated 10-part CTest test suite
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

### Running the Test Suite via CTest

```bash
ctest --test-dir build --output-on-failure
```

The test runner validates 10 automated test cases:
1. Missing file handling (`DocumentError::FileNotFound`)
2. Null/empty memory buffer rejection
3. Garbage memory buffer rejection (`DocumentError::InvalidFormat`)
4. Standard PDF loading and page count
5. Non-destructive loading verification
6. Page point geometry and valid raster generation
7. Rendered glyph/graphic verification (non-white pixel check)
8. In-memory document buffer lifetime preservation across `doc->close()`
9. Physical pixel clamping ($\le 8192$ px, $\le 16$ MP) and logical dimension preservation
10. Document reload while retaining prior page handle references

## Continuous Integration

A GitHub Actions workflow is provided in [`ci/linux-ci.yml`](ci/linux-ci.yml) targeting `ubuntu-24.04`:
- Matrix testing for **Release** and **Sanitizer** (AddressSanitizer + UndefinedBehaviorSanitizer).
- Automated test execution under CTest.
*(To activate on GitHub, copy `ci/linux-ci.yml` to `.github/workflows/ci.yml`).*
