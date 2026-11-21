# Known limitations

This page lists behaviour that users of real-world documents will notice. Each item is deliberate or
deferred, not an undiscovered bug.

## Forms (AcroForm) and JavaScript

| Capability | Status |
|---|---|
| Show stored form-field values and check marks | **Supported** (drawn by PDFium's form-fill environment) |
| Edit / fill in text fields, checkboxes, radio buttons, combo and list boxes | **Partial** (in-place Qt editors, Tab navigation, undo/redo; failed appearance generation rolls the edit back, see below) |
| Export flattened PDF | **Partial** (burns saved form appearances into static page graphics; JavaScript/XFA-dependent fields remain unsupported) |
| PDF JavaScript (calculation, validation, formatting, show/hide) | **Never runs** (no JavaScript engine is built in, by design: threat model T1/T2) |
| XFA forms ("dynamic" forms, common in some government and bank PDFs) | **Not supported** |
| Pushbuttons, ResetForm, form submission, URL/launch actions | **Inert / Not supported** (never executed) |
| Calculated / derived fields | **Not recomputed** (no JavaScript engine) |

### Form Field Appearance Generation

- **Explicit appearances**: Text edits are saved with a verified normal `/AP`; PDFium regenerates standard Latin, multiline, and comb appearances. Editable combos and PDFium readback failures use a minimal single-line fallback only for printable ASCII. If a valid appearance cannot be read back or built, the edit is rolled back instead of saving a value that may appear blank.
- **`/NeedAppearances`**: The pinned PDFium public API can set annotation appearances but does not expose a way to mutate the catalog's AcroForm `/NeedAppearances` entry. The viewer therefore does not rely on that flag; the explicit `/AP` is required. The flag may remain false.
- **Password fields**: PDFium's regenerated password appearance is masked. Password-field security and stored-value handling are tracked separately under T20-A; this change does not alter that behavior.
- **External viewers**: Automated Poppler and MuPDF checks cover saved appearances, flattened text, and widget-region ink. Manual compatibility with Adobe Acrobat, Foxit, and Okular remains unverified.

### Form Editing & Navigation

- **Editable field types**:
  - Single-line text fields (`QLineEdit`) and multi-line text fields (`QPlainTextEdit`) with password masking where requested by field flags.
  - Checkboxes and radio button groups: toggled directly on click or keyboard activation. Radio buttons in a group share mutual exclusion.
  - Dropdown combo boxes and editable combo boxes with custom typed text.
  - List boxes with single selection or multi-selection (`ExtendedSelection`).
- **Keyboard navigation**: Tab and Shift+Tab navigate forward and backward in document order through navigable fields across pages, scrolling the target field into view and activating its in-place editor. Escape cancels an active editor; Enter commits combo/list choices.
- **Undo / Redo and Crash Resilience**: Form mutations participate directly in the UI-owned `EditLog`. Undo (`QKeySequence::Undo`, typically `Ctrl+Z`) and Redo (`QKeySequence::Redo`, platform-dependent, e.g. `Ctrl+Y` or `Ctrl+Shift+Z`) revert field values, toggle states, choices, and their associated appearance streams. If the isolated worker crashes, the UI restarts it and replays the edit log automatically.
- **Flatten & Export**: "Export Flattened PDF..." burns all annotations and interactive form fields into static page contents (`FPDFPage_Flatten`) and strips interactive form structures (`/Annots`). Flattened export is strictly non-destructive: pages are imported into an isolated temporary document, so the user's active document and in-memory form edits remain interactive.

### Non-Latin and Persian Values

- For Persian, Arabic, and other non-Latin text values where standard PDF built-in fonts lack glyph coverage or HarfBuzz shaping, the viewer lays the text out with Qt and synthesizes custom vector appearance streams (`/AP`) directly on the field widget annotation. The text is shaped and joined (including Persian letters like گ and ی). For single-line fields whose right-to-left text is narrower than the field, the layout is right-aligned.
- Vector appearance generation and the field edit are refused if the widget is 4 pt or less in width or height, if the text requires more than 250,000 vector outline elements (`kMaxTextOutlineElements`), or if any character cannot be drawn by any installed system font.
- The raw Unicode text value is simultaneously stored in the field's `/V` entry.
- External viewers: third-party readers like Poppler and MuPDF render the synthesized vector appearance identically without requiring embedded fonts. However, if another PDF editor modifies the field value, it may rebuild the appearance from standard PDF fonts (e.g., Helvetica) and drop non-Latin glyphs. Compatibility with Adobe Acrobat, Foxit, and Okular is currently **untested**.

### Ingestion Caps & Limits

To prevent memory exhaustion and denial-of-service from malformed or hostile PDFs (threat model T1/T2):
- **Fields per page**: At most 1,000 form fields are enumerated per page.
- **Options per choice field**: At most 1,000 choices are loaded for a combo box or list box.
- **Value length cap**: A value already stored in the file that is longer than 32,768 UTF-16 code units (64 KiB wire bytes) is enumerated clipped to 32,768 units and causes the page's field list to be flagged as `truncated`. Typing or setting a value exceeding 32,768 code units via the UI or API is rejected.
- **Field names and option labels**: Capped at 1,024 characters each.
- **Field hierarchy nesting**: PDFium resolves inherited fully-qualified field names up to 31 nesting levels; deeper hierarchies return an unknown/empty name.
- **IPC response budget**: The worker's serialized per-page form field response is strictly capped at 16 MiB. If the field count or byte budget is exceeded, the worker truncates the list safely, sets `truncated = true`, and preserves worker session integrity without crashing.

### What Never Runs

- **JavaScript**: No JavaScript engine is embedded. Form validation, derived calculations, formatting scripts, and visibility toggling never execute. Forms that rely on JavaScript to calculate totals, fill dates, or enable sections will show only the values stored in the file or remain inert.
- **XFA**: Pure dynamic XFA forms are not supported and render only the static fallback page.
- **Pushbuttons and Actions**: Pushbuttons and `ResetForm` actions do not submit data, trigger network requests, or reset fields. SubmitForm and URL actions are blocked.

### Incremental Save and Byte Persistence

- When saving a modified form, the viewer uses PDF incremental updates (`FPDF_INCREMENTAL`) by default to preserve original file bytes and byte-prefix intact.
- **Old field values remain in the saved file bytes**: Just like deleted annotations, an incremental update appends a new revision to the end of the file. The original objects and their prior field values remain in the preceding byte stream.
- **Privacy notice**: Do not rely on editing form fields or replacing sensitive text (passwords, identity numbers, financial information) to sanitize a document before sharing. Older revisions remain intact in earlier file bytes. Exporting a flattened PDF creates a brand-new document without incremental revision history, purging superseded prior values; however, any active (un-cleared) field values are permanently baked into visible page content. To permanently eliminate sensitive text from a document, use the pixel redaction engine.

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
  with this viewer's font, so its look can change. If one of its characters cannot be drawn by any installed
  font, the deletion cannot be undone (a message says so and the box stays deleted until the file is reopened
  without saving).

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
