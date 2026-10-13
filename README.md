# PDF Reader Workstation (Linux)

A fast, native PDF workstation for Linux designed with an Adobe Pro-grade feature set (forms, annotations, digital signatures, redaction) built using **C++20** and **Qt 6**, powered by **PDFium**.

## Architecture & Principles

- **Proven Engine**: Uses Google's [PDFium](https://pdfium.googlesource.com/pdfium/) via prebuilt release binaries (`bblanchon/pdfium-binaries`), verified cryptographically with SHA256 checksums and fetched via CMake `FetchContent`.
- **Strict Engine Abstraction**: The UI layer communicates exclusively through `IDocument` and `IPage` interfaces without exposing PDFium types.
- **Safe Object Lifetimes**: Pages hold shared ownership of the underlying document handle, preventing use-after-free even if the document is closed while pages are retained.
- **Non-destructive Loading**: Loading a corrupt or password-protected document does not destroy the currently open document.
- **Prepared for Multi-Process Sandboxing**: The interface is designed to support IPC separation with a sandboxed document worker process (`memfd_create` + `SCM_RIGHTS`).

## Project Structure

```
├── include/
│   └── engine/
│       ├── IDocument.h       # Abstract document interface & DocumentError
│       └── IPage.h           # Abstract page interface
├── src/
│   ├── engine/
│   │   ├── PdfiumDocument.h/cpp # PDFium engine implementation & safe handle management
│   │   └── PdfiumPage.h/cpp     # PDFium page rendering logic with resolution clamping
│   └── ui/
│       ├── MainWindow.h/cpp     # Desktop main window & actions
│       └── PdfViewerWidget.h/cpp# Viewport widget with zoom and navigation
├── CMakeLists.txt            # CMake configuration with CTest & FetchContent
├── LICENSE                   # MIT License
├── main.cpp                  # Application entry point
├── sample.pdf                # Reference PDF for validation
└── test_render.cpp           # Automated CTest test suite
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

### Running Tests via CTest

```bash
ctest --test-dir build --output-on-failure
```
