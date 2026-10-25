# Known limitations

This page lists behaviour that users of real-world documents will notice. Each item is deliberate or
deferred, not an undiscovered bug.

## Forms (AcroForm) and JavaScript

| Capability | Status |
|---|---|
| Show stored form-field values and check marks | **Supported** (drawn by PDFium's form-fill layer, render-only) |
| Edit / fill in form fields | **Not yet** (planned for phase 2) |
| PDF JavaScript (calculation, validation, formatting, show/hide) | **Never runs** (no JavaScript engine is built in, by design: threat model T1/T2) |
| XFA forms ("dynamic" forms, common in some government and bank PDFs) | **Not supported** |
| Form submission, URL/launch actions | **Not supported** (never executed) |

What this means in practice:

- A field is shown with the value **stored in the file**. If a form relies on JavaScript to compute a
  value (totals, dates, derived fields), that value is **not recomputed** and may look empty or stale.
  Nothing tells the user this happened.
- Forms that need JavaScript to reveal or enable parts of the page may look incomplete or inert.
- XFA-only documents usually render as a placeholder page ("Please wait... / update your viewer").
- Do not rely on this viewer to fill, calculate or validate official forms until phase 2 ships and
  this page says so.

Form-field values are drawn since the form-fill environment was added to the viewer's render path
(`PdfiumPage::drawFormFields`). Before that change, filled-in values were **not displayed at all**.
The environment installs no callbacks, forwards no input and sets no JavaScript platform.

## Redaction

See [REDACTION.md](REDACTION.md): output is rasterised, existing signatures are invalidated.
