# Redaction Engine — Test Report

Companion to `docs/REDACTION.md`. Scope: Stage 1 burn-in engine (`src/redact/`).
Not covered here: signing, signature verification, selection UI (all out of scope by decision).

## 1. Result

| Run | Result |
|-----|--------|
| `RedactionTestSuite`, Release, Ubuntu 24.04, Qt 6.4.2, PDFium chromium/8100 | **1679 checks, 0 failed** (~16 s) |
| Same suite, `-fsanitize=address,undefined -fno-sanitize-recover=undefined`, leak detection on | **1679 checks, 0 failed, no sanitizer reports** |
| Full CTest (7 suites) Release, `ENABLE_TEST_HOOKS=ON` | 7/7 pass |
| Mutation checks (engine deliberately broken, §5) | **12/12 caught** |

External witnesses used when present: `qpdf` (check + `--qdf` expansion), Poppler `pdftotext`,
`pdftoppm`, `pdfsig`. The suite prints which were found and **skips them silently-but-visibly**
(listed on the first output line) if missing; on a machine without them those checks do not run.

Not run: Windows, TSan (the multi-process suites cannot run under TSan; the redaction suite uses
threads only in one small test), a fuzzing campaign beyond the 600 seeded mutations below.

## 2. How the evidence is organised

The code under test and the checkers are kept apart on purpose:

- `src/redact/Redaction.cpp` — the engine, including its own fail-closed self-verification.
- `tests/redaction/PdfInspector.h` — a **separate** parser: its own `N G obj` scanner, zlib
  inflate, own copy of the allowlist, own page→image mapping. It shares no code with the engine and
  does not use PDFium. If the two disagree, the test fails.
- `tests/redaction/make_fixtures.py` — deterministic generator for the adversarial PDFs (committed
  under `tests/fixtures/redaction/`, with a `manifest.txt` of expectations).
- `tests/redaction/test_redaction.cpp` — the suite.

A literal-string search is never accepted as proof. It exists in the suite only as one detector
among five, and §4 shows it failing to notice a failed redaction.

## 3. What each group of tests attacks

**Fixtures (22 manifest entries, 12 documents + variants).** For every successful run the suite checks:

1. *Independent structure*: allowlist of every dictionary name in every object and the trailer;
   forbidden-name tripwire; content-stream operators restricted to `q Q cm Do gs RG rg w J j` with exactly
   one `Do`; every stream is FlateDecode and inflates; exactly one `startxref` and one `%%EOF`; no object
   streams; each page maps to exactly one 8-bit DeviceRGB image with no mask and `w*h*3` decoded bytes.
2. *Canaries*: searched in the raw output, in the **decompressed** objects, and in `qpdf --qdf`
   expansion; plus forbidden constructs (`/Font /Annots /AcroForm /EmbeddedFile /JavaScript /Outlines
   /Metadata /StructTreeRoot /Prev`, `Tj`, `TJ`, `BT`) in that expansion.
3. *Text extraction*: PDFium and Poppler `pdftotext` both return nothing.
4. *Pixels of the stored image*: the inspector inflates the image stream itself and requires every pixel
   in every reported box to be `0,0,0`; and that the **requested** rectangle, recomputed independently
   with inward rounding, lies inside black.
5. *Residual metadata/attachments/signatures*: only PDFium's `/Creator (PDFium)` + `/CreationDate`
   remain in `/Info`; `pdfsig` sees no signature; report counts match the fixture; a signature-invalidation
   warning is present when the source had one.
6. *Visual*: content that must survive is still there (ink counts, colour counts, including the burned-in
   widget and annotation appearances); content that was redacted is gone (colour absent anywhere on the
   page); and for 3 pages per document the output is compared with Poppler's render of the source outside the
   boxes (mean difference < 4/255, < 1.5 % of pixels differing by > 96/255).

| Fixture | Attack / property |
|---------|------------------|
| `basic.pdf` | text under box, 2 pages, unredacted second page must stay visible |
| `hidden_content.pdf` | OCG layer switched off, white-on-white, invisible render mode, off-page text, clipped text, text inside a Form XObject under the box |
| `annots_forms.pdf` | FreeText/Text/Popup/Link/hidden annotations, AcroForm field value + default value, widget appearance that must be burned in |
| `attachments.pdf` | name-tree embedded file, `/AF`, FileAttachment annotation, canary in filename/description/body |
| `metadata_all.pdf` | Info (incl. custom key), XMP (catalog and page), PieceInfo, Thumb, PageLabels, outlines, named dests, OpenAction JS, structure tree with `/ActualText`, `/Lang`, custom catalog key |
| `metadata_objstm.pdf` | same, but inside compressed object streams (invisible to raw grep) |
| `incremental.pdf` | superseded content stream and an orphan object left in an earlier revision |
| `multifilter.pdf` | content behind `ASCIIHex+Flate` and `ASCII85+Flate` |
| `geometry.pdf` | `/Rotate` 0/90/180/270 and a `CropBox` offset; red square must go, blue square must stay |
| `images.pdf` | image with SMask under the box; distinct colour must be absent afterwards |
| `signed.pdf` | signature field + `DocMDP` (structure only — **not cryptographically valid**) |
| `multipage.pdf` | 30 pages, boxes on pages 0/17/29 only; the other 27 pages must be untouched |
| `encrypted_aes256.pdf` (+ no/bad password) | `PasswordRequired`, `BadPassword`, success; output must be unencrypted |
| `trunc`, `garbage`, `empty_pdf_header`, `pagetree_loop`, `form_recursion`, `huge_page`, `lying_length` | hostile/malformed input: refuse (no bytes) or produce a verified output |

**API abuse (≈ 70 checks).** Null/zero input; NaN/±Inf/inverted/zero-area/huge/off-page rectangles;
negative/out-of-range/`INT_MAX` page indices; bad dpi and `edgeGrowPx`; every `Limits` field; valid edge cases
(partially off-page, whole page, tiny rectangle, overlapping, same page twice, dpi 72/300, grow 0/16);
edge-grow arithmetic (`148..302` px for a rectangle at exactly 150..300 px with grow 2); re-redacting an
output; pixel determinism across runs; four concurrent redactions.

**Mutation fuzz.** 5 fixtures × 120 seeded random mutations of the input bytes (1–4 byte flips/overwrites/
deletions) = 600 runs (470 accepted, 130 refused). Invariant: failure returns no bytes; success must pass
the inspector and have black boxes.

## 4. Negative controls — the checks must be able to fail

Two weak redactors are run through the same detectors:

- **Naive overlay**: draw a black rectangle over the content with PDFium, save.
- **Plain rewrite**: `FPDF_SaveAsCopy(FPDF_NO_INCREMENTAL)` of the whole source, no overlay (the "full rewrite" case).

Detector results (1 = flags the problem), from the actual run:

| Source | Method | literal grep | decompressed search | allowlist | PDFium text | pdftotext |
|---|---|---|---|---|---|---|
| basic | naive overlay | **0** | 1 | 1 | 1 | 1 |
| basic | plain rewrite | **0** | 1 | 1 | 1 | 1 |
| incremental | naive overlay | **0** | 1 | 1 | 1 | 1 |
| hidden_content | naive overlay | 1 | 1 | 1 | 1 | 1 |
| annots_forms | naive overlay | 1 | 1 | 1 | **0** | 1 |
| attachments, metadata_all, metadata_objstm | naive overlay | 1 | 1 | 1 | 1 | 1 |

Reading it: for `basic` and `incremental` a literal grep **passes a failed redaction** while the secret
is recoverable from the decoded streams — this is asserted by the suite, not just observed. The engine's own output
trips **none** of the five detectors on any of these sources.
(`annots_forms`/PDFium-text = 0 is expected: annotation text is not page text; `pdftotext` and the decoded search
still find it.)

**Detector teeth** (so the checkers themselves are tested):

- enlarging a claimed box over non-black pixels is rejected by both `verifyRedactedOutput` and the inspector;
- injecting `/Annots /Names /Outlines /OpenAction /Metadata /Prev` into the catalog is rejected by both;
- appending a second revision after `%%EOF` is rejected by both;
- re-compressing a content stream with a smuggled `BT … Tj ET` is rejected by the inspector, the canary is
  found by decoded search, and the same canary is invisible to a raw grep.

## 5. Mutation checks (are the tests actually sensitive?)

Each row: the engine source was patched, the suite rebuilt and run; "caught" = suite exits non-zero. Engine
self-verification was disabled in several mutants so the **independent** test layer had to catch the bug alone.

| # | Mutation | Self-check | Caught by (first failing check) |
|---|---|---|---|
| M1 | skip painting the black box | on | engine VerificationFailed → ~43 failures |
| M2 | skip painting **and** disable self-check | off | inspector: stored-image box not black (80) |
| M3 | box under-covers by 1 px on the right | off | edge-grow arithmetic, tiny-rect (6) — **not** by the fixtures, see §6 |
| M4 | `edgeGrowPx` ignored | on | edge-grow arithmetic, tiny-rect |
| M5 | y axis flipped | off | requested-rectangle-black check (36) |
| M6 | page rotation ignored | on | ink/visual checks (30) |
| M7 | annotations not rendered | on | annotation ink check |
| M8 | wrong scale for boxes (96 dpi) | on | arithmetic + requested-rect (37) |
| M9 | self-check pixel loop disabled | — | detector-teeth test only (as intended) |
| M10 | rectangles on pages > 0 dropped | on | requested-rect on page 1 (15) |
| M11 | page 0 copied from the source with `FPDF_ImportPages` (vector leak) | off | inspector / pixel / allowlist (21) |
| M12 | form widgets not drawn | on | green widget colour check |

## 6. Findings the tests produced during development

These were real, and each was fixed or documented:

1. **Bug — output size limit bypass.** With a small `maxOutputBytes`, PDFium's `FPDF_SaveAsCopy` returned success
   after a short write; the engine then reported a verification failure instead of `LimitExceeded`. Fixed: the writer's
   overflow flag is authoritative.
2. **Bug — form fields silently dropped.** `FPDF_ANNOT` does not draw widget appearances; visible field content
   outside the boxes vanished. Fixed with a form-fill environment + `FPDF_FFLDraw` (documented as L13: bigger parsing surface).
3. **Spec amended after observation.** PDFium's writer emits `0 0 0 RG 0 0 0 rg 1 w 0 J 0 j` in every page. The
   allowlist was widened by those five state-only operators (painting and text operators remain forbidden).
4. **Wrong expectation of mine.** `FPDFDoc_GetAttachmentCount` counts only document-level attachments, not
   FileAttachment annotations (documented as L14; both are dropped regardless).
5. Test bugs (fixture colour leaking into text; `pdftoppm` has no `-ppm` flag; `/Type/Catalog` has no space).

## 7. What the tests do NOT prove

- **Not a proof of security.** They show that on these fixtures and mutations the stated guarantees hold and that the
  checks fail when the guarantees are broken. They cannot show absence of every leak path in PDFium or in unseen PDFs.
- **Fixtures are synthetic.** Hand-built, small, English text. No real-world corpus (scanned books, CJK/RTL fonts, huge
  transparency groups, malformed-but-common producer output, PDF 2.0, XFA, portfolios, 3D/rich media, linearized files).
- **One renderer defines "visible".** PDFium decides what is painted. Poppler is used as a cross-check on geometry and
  overall appearance only (tolerance-based), not as an authority on every construct. Viewer-differential attacks (content visible in
  Acrobat/others but not in PDFium) are **not** tested.
- **The engine's self-check is not independent** (same PDFium parser). The independent layer is the test-suite; it is not
  part of the shipped engine.
- **The "stored image is black" check relies on my inflate and on PDFium's writer.** If a future PDFium wrote the image with a
  different filter or colour space, the allowlist would fail closed, but a pixel-correct *and* otherwise-leaking output of an
  unforeseen shape would need a human reviewing the allowlist.
- **Rectangle correctness is the caller's problem (L4).** The suite proves a requested rectangle is black, not that it covered
  the sensitive text. Glyph overhang outside a rectangle, near-white text, and content slightly outside the box are not detected.
- **Residual metadata is present.** `/Creator (PDFium)` and a `/CreationDate` timestamp (time of redaction) are in every output (L7).
- **Memory and process remnants (L8).** Not tested; PDFium's internal copies are not zeroed.
- **No test of signatures beyond structure.** `signed.pdf` is a structural stand-in; the engine only counts and warns. Real signed PDFs,
  certification signatures, timestamps and LTV data were not used, and verification is explicitly not implemented.
- **Limits are tested on boundaries, not at scale.** No 1000-page / hundreds-of-MB documents; peak memory and run time at the default
  limits (64 MP per page, 2000 pages, 2×10⁹ pixels total) were not measured. A caller should apply its own timeouts.
- **Rasterized-output limitations (L1/L2) are properties of the design,** not tested, apart from verifying that no text layer exists.
- **Only Linux x86-64 was exercised;** Windows builds do not build the suite (needs zlib and POSIX `popen`).
- **Randomness is seeded,** so the mutation fuzz is a regression test, not a discovery campaign.

Also observed but unrelated to this change: on pristine `origin/main` in this environment `ViewportWidgetTestSuite` failed 3/3
in one batch ("In-flight tile was queued a second time"), but passed in other runs, including on this branch. It is timing-dependent
and pre-existing.

## 8. Running it

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build test_redaction
build/test_redaction tests/fixtures/redaction          # or: ctest -R Redaction
# sanitizers
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=undefined" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
# regenerate the fixtures (committed; only needed when changing them)
python3 -I tests/redaction/make_fixtures.py
```
