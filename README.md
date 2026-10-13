# PDF Reader Workstation (Linux)

A fast, native PDF workstation for Linux designed with an Adobe Pro-grade feature set (forms, annotations, digital signatures, redaction) built using **C++20** and **Qt 6**, powered by **PDFium**.

## Architecture & Principles

- **Proven Engine**: Uses Google's [PDFium](https://pdfium.googlesource.com/pdfium/) via prebuilt release binaries (`bblanchon/pdfium-binaries`), managed automatically by CMake `FetchContent`.
- **Engine Abstraction Layer**: Viewport and UI interact strictly through an abstract `IDocument` and `IPage` interface.
- **Process Isolation**: Ready for sandboxed worker processes per open document.
- **HiDPI-Ready Zero-Copy Rendering**: Renders directly into `QImage` buffers at native screen DPR (`devicePixelRatio`).

## Project Structure

```
├── include/
│   └── engine/
│       ├── IDocument.h       # Abstract document interface
│       └── IPage.h           # Abstract page interface
├── src/
│   ├── engine/
│   │   ├── PdfiumDocument.h/cpp # PDFium engine implementation
│   │   └── PdfiumPage.h/cpp     # PDFium page rendering logic
│   └── ui/
│       ├── MainWindow.h/cpp     # Main application window & actions
│       └── PdfViewerWidget.h/cpp# Viewport widget with zoom and navigation
├── CMakeLists.txt            # CMake configuration with Qt 6 & PDFium FetchContent
├── main.cpp                  # Application entry point
├── sample.pdf                # Sample PDF for testing
└── test_render.cpp           # Headless rendering verification test
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

### Running Tests

```bash
cmake --build build --target test_render
./build/test_render sample.pdf
```
