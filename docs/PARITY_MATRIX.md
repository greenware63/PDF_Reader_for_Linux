# PDF Feature Parity Matrix

**Status counts:** Supported: 12 · Partial: 4 · Not supported: 7 · Unverified: 1 · Out of scope: 1

Statuses describe this repository at `b0a01ea`. Evidence points to source or test locations; where evidence describes an exposed interface, the note states the boundary rather than implying broader support.

| Feature | Category | Our status | Evidence | Notes |
| :--- | :--- | :--- | :--- | :--- |
| PDF page rendering, tiling, zoom, and view rotation | Viewing | Supported | `test_render.cpp:879`; `test_render.cpp:2037`; `test_layout.cpp:692` | Rotation is a viewing operation, not a persisted page edit. |
| Text selection, extraction, and Unicode/Persian search | Viewing | Supported | `test_render.cpp:1357`; `test_render.cpp:1631`; `test_widget.cpp:2023` | Search and extraction cover Unicode and right-to-left text. |
| Outlines/bookmarks and navigation | Viewing | Supported | `test_render.cpp:1892`; `test_widget.cpp:2261` | Includes hierarchical outline extraction and sidebar navigation. |
| Page thumbnails and labels | Viewing | Supported | `include/engine/IDocument.h:111`; `include/engine/IDocument.h:125`; `test_widget.cpp:2607` | Labels and thumbnail rendering are available in the viewer. |
| Printing | Viewing | Supported | `src/ui/MainWindow.cpp:1316`; `src/engine/RenderWorker.cpp:423` | Pages are rasterized for printing. |
| Text-markup annotations (highlight, underline, strikeout, squiggly) | Annotating | Supported | `test_widget.cpp:5107`; `include/engine/IPage.h:122` | Creation and undo/redo are covered. |
| Sticky notes, geometric shapes, and freehand ink | Annotating | Supported | `include/engine/IPage.h:145`; `test_render.cpp:3508`; `test_render.cpp:3711` | Supported annotation types; see the FreeText row for text-box limitations. |
| FreeText annotations | Annotating | Partial | `test_render.cpp:3962`; `test_render.cpp:4401`; `docs/KNOWN_LIMITATIONS.md:73` | Text boxes use constrained styling; existing boxes cannot be edited in place. |
| Stamps and file-attachment annotations | Annotating | Supported | `test_render.cpp:4594`; `test_render.cpp:4710`; `test_widget.cpp:6603` | Attachments are bounded and treated as inert data. |
| Deleting annotations and removing their saved data | Annotating | Partial | `include/engine/IPage.h:221`; `docs/KNOWN_LIMITATIONS.md:75`; `docs/KNOWN_LIMITATIONS.md:101` | Removal is supported, but incremental saves can retain deleted annotation objects and contents in prior bytes. |
| In-place editing of existing PDF page text or images | Editing | Not supported | `include/engine/IPage.h:31`; `include/engine/IPage.h:232` | The page interface exposes rendering, extraction, annotation, and form operations, not page-content text/image editing. |
| Interactive AcroForm editing | Forms | Partial | `test_widget.cpp:6840`; `test_widget.cpp:7272`; `docs/KNOWN_LIMITATIONS.md:11` | Common field types are editable; appearance-generation failures roll edits back, and password fields, JavaScript, and XFA are unsupported. |
| Flattened form export | Forms | Partial | `test_render.cpp:5529`; `test_widget.cpp:7778`; `docs/KNOWN_LIMITATIONS.md:12` | Export is available, but JavaScript/XFA-dependent fields remain unsupported. |
| PDF JavaScript and dynamic XFA forms | Forms | Not supported | `docs/KNOWN_LIMITATIONS.md:13`; `docs/KNOWN_LIMITATIONS.md:14` | Scripts are not run; dynamic XFA is not supported. |
| Exporting PDF content as standalone image, text, or table files | Converting | Not supported | `include/engine/IPage.h:62`; `include/engine/IPage.h:74`; `include/engine/IDocument.h:134` | Rendering and text extraction exist, but the exposed export interface copies page ranges to PDF rather than exporting these formats. |
| Optical character recognition | OCR | Not supported | `include/engine/IPage.h:74`; `include/engine/IDocument.h:83`; `docs/REDACTION.md:56` | The page interface provides text extraction, not OCR; re-OCR after raster redaction is the caller's responsibility. |
| Insert, delete, or reorder pages in a saved document | Pages | Not supported | `include/engine/IDocument.h:83`; `include/engine/IDocument.h:134`; `include/engine/IDocument.h:159` | Page-range PDF extraction is not an in-place page-organization operation. |
| Persist page rotation to the PDF | Pages | Not supported | `include/engine/IPage.h:36`; `include/engine/IPage.h:55`; `include/engine/IDocument.h:159` | Rotation is accepted for rendering; there is no page-rotation save operation in the interface. |
| Open password-protected PDFs | Security | Supported | `test_password_prompt.cpp:68`; `include/engine/IDocument.h:41` | Cancel, incorrect-password, and successful-unlock flows are exercised. |
| Incremental save of document changes | Security | Supported | `test_render.cpp:2953`; `include/engine/IDocument.h:159`; `src/engine/PdfiumDocument.cpp:768` | Incremental save preserves the original byte prefix and remains the default. |
| Isolated PDF worker with Linux sandboxing | Security | Supported | `test_worker_ipc.cpp:547`; `test_worker_ipc.cpp:1396`; `test_worker_ipc.cpp:1548`; `src/worker/main.cpp:220` | Linux Landlock, seccomp, resource limits, and fail-closed startup are tested; the sandbox is Linux-specific. |
| Burn-in raster redaction | Security | Supported | `test_widget.cpp:4158`; `tests/redaction/test_redaction.cpp:1`; `src/redact/Redaction.cpp:429` | Reconstructs image-only output; existing signatures are dropped. |
| Create or verify digital signatures | Signing | Not supported | `docs/REDACTION.md:6`; `docs/REDACTION.md:89`; `src/engine/PdfiumDocument.cpp:777` | Signature count can be read, but signing and signature verification are out of scope. |
| Compatibility with Acrobat, Foxit, and Okular | Other | Unverified | `docs/EXTERNAL_VIEWER_CHECKLIST.md:94`; `CMakeLists.txt:485` | Automated compatibility checks cover other engines; manual checks for these viewers have not been run. |
| AI-assisted PDF features | Other/AI | Out of scope | `docs/ROADMAP.md:7` | Per the roadmap decision, AI remains out of scope until core capabilities are complete. |

## Capabilities Outside the Vendor Feature List

The consumer-facing feature categories do not describe these implementation capabilities:

- **Sandboxed worker architecture:** PDFium parsing and rendering run in a separate worker; Linux isolation and confinement behavior are exercised by `test_worker_ipc.cpp:547` and `test_worker_ipc.cpp:1396`.
- **Bounded worker IPC and descriptor handling:** Descriptor lifecycle and rejection cases are covered by `test_worker_ipc.cpp:1571`; the protocol has hostile-frame checks in `test_worker_ipc.cpp:3642`.
- **Large-document and tile-oriented rendering:** The layout and rendering tests cover tile rendering and page geometry through `test_render.cpp:879` and `test_layout.cpp:548`.
- **Unicode and right-to-left support:** Persian text extraction/search and vector form appearances are covered by `test_render.cpp:1631` and `test_widget.cpp:7272`.
- **Sanitizing raster redaction:** Structural checks and hostile fixtures exercise redaction output in `tests/redaction/test_redaction.cpp:1`.
