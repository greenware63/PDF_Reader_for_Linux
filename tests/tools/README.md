# Test tools for the M5 (AcroForms) work

Developer tools. Nothing here is built by CMake or run by CI: they need Python packages (and, for the probes in
`tests/spikes/`, the PDFium headers), and wiring them in belongs with the code that uses them.

| File | What it is for |
|---|---|
| `check_persian_appearance.py` | Checks that the stored appearance of a Persian text field or text box is correct and portable, by drawing it in up to three independent readers and comparing it with an independent shaping of the expected text |
| `selftest_check_persian_appearance.py` | Proves the checker can fail: builds good and deliberately broken appearances and requires the right check to fail for each |
| `../fixtures/make_hostile_form_fixtures.py` | Generates the AcroForm fixtures (hostile and well-formed) in `../fixtures/`; see its docstring for what each file targets |
| `../fixtures/persian_freetext_production_sample.pdf` | Real viewer output (five Persian FreeText boxes); the golden positive for the checker |
| `../spikes/m5_fixture_probe.cpp` | Lists a PDF's form fields with PDFium without any cap and reports the naive payload, time and memory, to show a fixture really is hostile |
| `../spikes/m5_freetext_persian_probe.cpp` | Provenance of the golden sample |

## The Persian checker

```
python3 -I check_persian_appearance.py FILE.pdf --field NAME --expect "TEXT"
python3 -I check_persian_appearance.py FILE.pdf --annot-name NM --expect "TEXT"
python3 -I check_persian_appearance.py FILE.pdf --all-freetext
```

Exit 0: every check passed (skips and warnings do not fail). Exit 1: a check failed. Exit 2: usage error or a missing
required package. Checks: `value`, `portable` (outlines only, no fonts), `render`, `agree` (readers draw the same
picture), `joined` (letters are joined as the script requires), `reference` (matches an independent shaping).

What it caught on the pinned PDFium's own appearance for the typed value "سلام دنیا" (all four of these fail):
a text appearance in three non-embedded fonts (`portable`), MuPDF and Poppler drawing different pictures
(`agree`, 0.28), 6 to 7 letter runs where the script requires 4 (`joined`), and a 0.55 match with the reference (`reference`).
On the viewer's own output it passes everything: the five FreeText boxes of the golden sample, and the Persian form field
written by the merged slice 4 (five different strings, all three readers; `reference` 0.96 to 1.00, `agree` 1.000).
A digits-only date matches the reference less closely (0.92 to 0.96) than letters do (0.99 to 1.00), so a text made only of
digits has the smallest margin above the 0.88 threshold.

Needs: `pikepdf numpy scipy pillow` (Pillow built with raqm, which the pip wheels are, plus `libfribidi0`), and
optionally `pymupdf`, `pypdfium2` and Poppler's `pdftoppm` as the readers. An installed font with the Persian letters is
needed for `reference` (`fonts-dejavu-core` is enough). Limits: single-line text, dark ink on a lighter background, pages
without `/Rotate`. Acrobat, Foxit and Okular are not among the readers.

## Wiring it in later

* The self-test exits 77 when a dependency is missing, so CTest can skip it:
  `add_test(NAME PersianCheckerSelfTest COMMAND python3 -I ${CMAKE_SOURCE_DIR}/tests/tools/selftest_check_persian_appearance.py)`
  with `set_tests_properties(PersianCheckerSelfTest PROPERTIES SKIP_RETURN_CODE 77)`.
* A C++ test for the Persian appearance (slice 4) saves a PDF through the viewer and runs the checker on it, requiring exit 0.
  A green CI run does not show whether a skipped test ran, so require the font and the packages there instead of skipping.
