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

## Annotations

| Capability | Status |
|---|---|
| Sticky notes, text markup, rectangles, circles, freehand ink: add, undo/redo, save | **Supported** |
| Text boxes (`/FreeText`) in any script the installed fonts cover, with line breaks and wrapping | **Supported**, with the limits below |
| Editing a text box after it was added (its text, font, size or colour) | **Not yet** (delete it and add a new one) |
| Choosing the font, size or colour of a new text box | **Not yet** (12 pt, black, no frame) |
| Purging a deleted annotation's data from the saved file | **Not supported** (see below) |

Text boxes:

- The viewer lays the text out itself and stores it in the file as vector shapes cut from the fonts installed on
  the computer that creates the box. It therefore looks the same everywhere (checked in Poppler and MuPDF with
  Latin, Persian, mixed-direction, multi-line and wrapped text), and Persian and Arabic text is shaped and
  right-aligned. The text itself is also kept in the annotation's `/Contents`.
- Because the text is drawn as shapes it cannot be selected or searched on the page. An application that edits
  the box may rebuild its appearance from the stored default font (Helvetica) and lose non-Latin text there;
  Okular, Foxit and Acrobat have not been tested.
- A character that no installed font can draw as an outline (a colour emoji, a script with no font installed) is
  refused with a message that points to sticky notes, which keep any text. A text that is too long for a text box
  is refused the same way.
- Deleting a text box that came from another application and then undoing the deletion lays its text out again
  with this viewer's font, so its look can change.

Selecting and deleting:

- An unfilled rectangle or circle is selected by its outline, not by the empty space inside it, so a shape drawn
  around a note does not hide the note. A filled shape is selected anywhere inside it; whether it is filled is
  read from what its appearance draws.
- An annotation that has no `/NM` name in its file can be seen and hovered but not deleted.
- Deleting an annotation removes it from the page, but **not its data from the file**: the saved file still holds
  the old objects. A deleted sticky note's text was still found in the bytes of the saved file, although no
  annotation referred to it any more. Do not rely on deleting an annotation to remove sensitive text from a file
  you are going to share.

## Redaction

See [REDACTION.md](REDACTION.md): output is rasterised, existing signatures are invalidated.
