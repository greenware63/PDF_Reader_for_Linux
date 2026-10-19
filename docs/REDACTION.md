# Burn-in Redaction Engine — Security Specification

Status: **normative for the code in `include/redact/` and `src/redact/`**.
Scope: Stage 1 of redaction ("burn-in"). Object-level redaction that preserves
text/vector output is explicitly **not** in this stage.
Out of scope here: digital signing, signature verification, the selection UI.

## 1. What the engine does

`pdf::redact::redactDocument()` takes PDF bytes, a set of rectangles per page, and
produces a **brand-new PDF** in which

1. every page is a single full-page raster image,
2. the pixels inside each redaction rectangle were overwritten with opaque black
   **in the bitmap, before the bitmap was handed to the PDF writer**, and
3. nothing from the source file is copied structurally — not one object, string,
   stream or byte.

The output is built from an empty `FPDF_CreateNewDocument()`. It is never produced
by editing, appending to, or "saving as" the source document. This matters: the
failure mode of every weak redactor is that the original content survives
somewhere in the file (hidden under an overlay, in an old revision, in an
unreferenced object, in a metadata stream). Here the original content has no path
into the output because the only data that crosses from source to output is a
**pixel array**, and the redacted pixels were destroyed in that array.

## 2. Guarantees (G)

These are what the code promises and what the test-suite tries to falsify. Each
guarantee names the mechanism and the test that attacks it.

| # | Guarantee | Mechanism |
|---|-----------|-----------|
| G1 | **No original page content behind a redaction box.** Every pixel inside every redaction rectangle (grown outward to whole pixels plus `edgeGrowPx`) is `0x000000` in the image stored in the output file. | Black is painted into the bitmap with `FPDFBitmap_FillRect` after rendering; the image is only then attached to the page. |
| G2 | **No text layer.** The output contains no text-showing operators, no fonts, no font programs, no ToUnicode maps, no `/ActualText`, no structure tree. Text extraction returns nothing. | Output pages contain only `q cm Do Q` / `gs`; there is nothing to extract. |
| G3 | **No annotations, widgets, form fields, JavaScript, actions, links.** | Not copied. Visible annotation *appearance* is rendered into the pixels (see L3) and then dropped as an object. |
| G4 | **No embedded files and no file-attachment annotations.** | Not copied. |
| G5 | **No source metadata.** No source `/Info` values, no XMP `/Metadata`, no `/PieceInfo`, no page thumbnails, no page labels, no named destinations, no `/Lang`, no `/ViewerPreferences`. | Not copied. |
| G6 | **No outlines / bookmarks.** | Not copied. |
| G7 | **No obsolete revisions or unreferenced objects.** Output has exactly one cross-reference section, no `/Prev`, and every object is reachable from the trailer. | New document written with `FPDF_NO_INCREMENTAL`. Source bytes are never appended to. |
| G8 | **Output is structurally minimal and allowlisted.** Only a fixed set of dictionary keys / operators appears (§6). Anything else is a failure, not a warning. | Built-in post-verification (`verifyOutput`) and the independent inspector in the tests. |
| G9 | **Fail closed.** If any step fails, validation fails, a limit is hit, or self-verification fails, the result contains **no output bytes**. | `Result::pdf` is only populated after verification succeeds. |
| G10 | **The output is not encrypted and not signed.** An encrypted input yields an unencrypted output; callers decide what to do with that. | Documented, surfaced in `Report::warnings`. |

The only data in the output that did not come from rendering is PDFium's own
writer boilerplate: `/Info << /Creator (PDFium) /CreationDate (D:…) >>`, a
random `/ID`, and PDFium-private `/FXE1`/`/FXX1` resource names. They contain no
source-derived information. They are listed in the allowlist and reported as
"engine-generated residue" (see L7).

## 3. Non-guarantees and limitations (L)

**L1 — The output is raster-only.** Text is no longer selectable, searchable,
copyable or readable by screen readers; accessibility tagging is gone; file size
grows (≈0.1–1 MB per page at 150 DPI instead of a few KB); fine detail is limited
by `dpi`. Re-OCR is the caller's responsibility. This is the price of G1/G2:
there is no text to leak because there is no text.

**L2 — Rasterization is lossy and resolution-bound.** Rendering is done once at
`dpi` (default 150, allowed 72–300). Thin lines, tiny text and subpixel detail are
approximated. Colour management is whatever PDFium's renderer does; output is
8-bit RGB.

**L3 — The engine redacts what PDFium *renders*, not what the file *contains*.**
Content that PDFium does not paint (optional-content layers that are off,
invisible text render mode, off-page content, hidden annotations, content
outside the crop box) is **dropped, not blacked out**: it does not reach the
output at all. This is safe for confidentiality but means the output differs
from the source for any viewer that would have shown that content (e.g. enabling
a layer).
The reverse also holds and is the **caller's responsibility**: content that *is*
visible but is not covered by a supplied rectangle remains visible in the pixels
(including very low-contrast, near-white, or tiny text). The engine cannot know
what the user meant to hide.

**L4 — Rectangles are geometry, not search.** The engine does not find text. A
rectangle that is too small, mis-positioned, or that misses anti-aliased glyph
edges will leave part of a glyph visible. `edgeGrowPx` (default 1 px) covers
anti-aliasing fringe at the rectangle boundary only. The selection UI is
responsible for good rectangles. Text that *continues beyond* a rectangle stays
visible beyond it.

**L5 — Pixel-level disclosure outside boxes is out of scope.** Everything that is
visible outside the boxes is, by definition, not redacted. Pixels inside the boxes
are uniformly black; their *size and position* are visible (a black bar over an
8-character word discloses roughly its length). Use larger bars if length
matters.

**L6 — Existing digital signatures are invalidated, by construction.** The output
is a different file. Every signature in the input (document signature, certification
signature, form signature, DocMDP, timestamps, DSS/LTV data) is gone, and the
original signature would not validate against the new bytes even if copied. The
engine reports `Report::sourceSignatureCount` and adds a warning when it is non-zero.
The UI **must** warn the user before redacting a signed document and must not
present the output as signed or certified. **No signing, and no verification of
the input's signatures, is done in this stage.** Whether a signed original should
be kept alongside the redacted copy is a product decision, not a property of this
engine.

**L7 — Residual engine-generated metadata.** See §2: `Creator (PDFium)` and a
`CreationDate` timestamp are written by PDFium's writer and cannot be removed
through PDFium's public API. The timestamp reveals *when the redaction ran*.
Callers needing to remove it must post-process with a separate tool; the
allowlist test will be updated if/when we do.

**L8 — Not a guarantee against the process or the filesystem.** The engine holds
the unredacted source and the unredacted bitmaps in process memory; they are not
securely zeroed by PDFium's allocator on free (the engine zeroes the bitmaps it
owns on its own paths but cannot zero PDFium-internal copies). Swap, crash dumps,
shared-memory tiles, thumbnails caches, recent-files lists and the **original file
on disk** are outside the engine. Securely deleting or replacing the original is a
UI/OS concern.

**L9 — Does not defend against a compromised PDFium.** The input is untrusted and
is parsed/rendered by PDFium. A memory-safety bug in PDFium could in principle
corrupt the output or leak. Run the engine inside the sandboxed worker
(`docs/THREAT_MODEL`). The tests run under ASan/UBSan but fuzzing is not proof of
absence.

**L10 — Forensic scope.** "Black pixels in the image" is the guarantee. The engine
does not defend against side channels such as file size, page count, page
dimensions, or the *shape* of the redaction mask.

**L11 — Lost semantics.** Hyperlinks, bookmarks, form interactivity, tags, page
labels, document language, layer structure, embedded fonts, colour profiles and
PDF/A conformance are not preserved. Page rotation is baked in (output `/Rotate` is 0).
Crop box becomes the page box.

**L13 — Form-fill environment widens the parsing surface.** Burning in form-field
appearances requires PDFium's form-fill environment (otherwise visible field content
silently disappears; this was found by the test-suite, see the test report). It runs in the
same process on untrusted input, with no JavaScript platform installed. Run it in the
sandboxed worker (L9). Without the environment the output would be safe but would drop
visible field content.

**L14 — Report counts are informational.** `sourceAttachmentCount` is PDFium's count of
*document-level* embedded files; it does **not** count file-attachment annotations (the
`attachments.pdf` fixture has one of each and reports 1). `sourceSignatureCount` is PDFium's
count of signature fields. Neither is used for a security decision: all attachments and
signatures are dropped regardless of what is counted.

**L12 — Self-verification is partial.** `verifyOutput` re-opens the output with
PDFium and scans the bytes with a small structural checker. That is the same
parser that produced the file; it can miss bugs PDFium shares with itself. The
independent checks live in the test-suite (separate parser, `qpdf`, `pdftotext`,
`pdftoppm`) and are only as strong as their fixtures (§8).

## 4. Threat model assumptions

Attacker model: the **recipient of the redacted output**, holding only the output
bytes, with arbitrary PDF tooling, unlimited time, and knowledge of the redactor's
algorithm.

Assumptions we rely on:

- A1. The caller supplies correct rectangles covering all sensitive visible content.
- A2. The caller is authorised to see the unredacted source (the engine does not
  protect the source from the caller).
- A3. PDFium's renderer is the source of truth for "what is visible" and is not
  malicious. Input PDFs are hostile: they may be malformed, enormous, recursive,
  or crafted to make hidden content render differently than other viewers do (a
  *viewer differential*, e.g. optional content states, JavaScript-driven
  visibility, or non-compliant parsing). The engine is conservative toward its
  own renderer; it cannot guarantee that another viewer would have shown the same
  thing.
- A4. The original file is handled separately by the user.
- A5. Output bytes are not modified between the engine and the recipient.

Explicitly **not** protected: an attacker who also has the original file; an
attacker who can observe the redaction process in memory; an attacker who can
infer content from the unredacted area.

## 5. Scope of sanitization (what each artefact class does)

| Artefact | Handling |
|----------|----------|
| Page content streams (text, vector, images) | Rendered to pixels; never copied |
| Annotations (all subtypes), popups, links | Visible appearances rendered (`FPDF_ANNOT`), objects not copied |
| AcroForm / widgets / field values / XFA | Widget appearances rendered through PDFium's form-fill environment (`FPDF_FFLDraw`; no JavaScript platform, highlight alpha 0); values and fields not copied; XFA ignored |
| JavaScript, OpenAction, AA, named actions | Not copied |
| Embedded files, file-attachment annotations, portfolios | Not copied |
| Document `/Info`, XMP, `/PieceInfo`, `/Metadata` on any object | Not copied; replaced by PDFium's generated `/Info` (L7) |
| Outlines/bookmarks, named destinations, page labels | Not copied |
| Thumbnails (`/Thumb`) | Not copied |
| Structure tree, `/ActualText`, `/Alt`, `/Lang` | Not copied |
| Optional content (layers) | Visible state rendered; configuration not copied |
| Obsolete revisions, free/unreferenced objects, orphaned streams | Impossible to copy: new document, `FPDF_NO_INCREMENTAL` |
| Encryption dictionary, permissions | Not copied (output unencrypted, G10) |
| Digital signatures, DSS/LTV, certifications | Not copied; reported (L6) |
| Linearization, hybrid xref, object streams from source | Not copied |

## 6. Output allowlist (what a valid output may contain)

Names allowed anywhere in a dictionary:
`Type Catalog Pages Page Kids Count Parent MediaBox Resources XObject Image Subtype Width Height
BitsPerComponent ColorSpace DeviceRGB DeviceGray Filter FlateDecode Length Contents Rotate ExtGState
ca CA BM Normal Info Creator CreationDate Producer ID Root Size FXE1 FXX1 ProcSet PDF ImageC`
(`Producer` is allowed only with the PDFium value, and is not currently emitted.)
Page content operators allowed: `q Q cm Do gs RG rg w J j`.
(The last five set graphics state only and paint nothing; they were added to this list after the first test run
showed PDFium's writer emits `0 0 0 RG 0 0 0 rg 1 w 0 J 0 j` at the top of every generated page. Painting
operators such as `re f S B sh BI` and every text operator remain forbidden. The page must contain exactly one `Do`.)
Image streams: `FlateDecode` only; `Width*Height*components` bytes after inflate.
Forbidden outright (checked separately as a tripwire): `Prev`, `Font`, `Annots`, `AcroForm`, `Names`,
`Outlines`, `Metadata`, `EmbeddedFiles`, `EmbeddedFile`, `JS`, `JavaScript`, `OpenAction`, `AA`,
`Dests`, `PageLabels`, `StructTreeRoot`, `OCProperties`, `Thumb`, `PieceInfo`, `Encrypt`, `Sig`,
`ByteRange`, `DSS`, `Perms`, `ObjStm`, `XRef`.

## 7. API contract (summary)

```
Result redactDocument(const uint8_t* data, size_t size, const std::string& password,
                      const std::vector<PageRedactions>& redactions, const Options& opts);
```

- Rectangles: PDF points, origin **top-left of the displayed page** (after `/Rotate`
  and crop box), i.e. the same space the viewer shows. They are clamped to the page;
  non-finite, inverted or zero-area rectangles are **rejected** (`InvalidRect`), never
  silently dropped. A fully off-page rectangle is rejected.
- At least one rectangle is required (`NothingToRedact`) so a no-op can never be
  mistaken for a redaction.
- Every page of the input appears in the output, in order. All are rasterized (§1).
- Limits (all enforced before allocation): max pages, max pixels per page, max total
  raster bytes, max output size, max input size, rectangles per page. Exceeding a limit
  is an error (`LimitExceeded`), not a downgrade.
- The engine is synchronous, has no Qt dependency and no UI dependency.
- Thread-safety: serialised by the process-wide PDFium mutex.

## 8. What the tests claim (see `docs/REDACTION_TEST_REPORT.md`)

Checks per fixture: independent parse + inflate + allowlist; decompressed-content canary
search **and** structural checks (a literal search is deliberately not accepted as proof);
PDFium and Poppler text extraction; pixel check inside every box in the *stored image*;
visual comparison outside boxes against an independent render of the source; residual
metadata/attachment checks; `qpdf --check`; adversarial and malformed input; negative
controls (naive overlay and plain rewrite must **fail** the same checks); and manual
mutation checks that show the tests fail when the engine is broken.
