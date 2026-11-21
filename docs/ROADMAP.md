# Project Roadmap: Building Toward Full PDF Reader Pro Parity

This document outlines the architectural decisions, phased sequence, and quality gates for achieving full feature parity with professional PDF workstation standards on native Linux.

---

## 1. Architectural Decisions

1. **Page Organization Save Pipeline**:
   - Page insert, delete, and reorder use a separate **"save as rewritten copy"** path.
   - **Incremental save** (`FPDF_INCREMENTAL`) remains the default for all other edits (annotations, form field filling, markup) to preserve original file bytes, digital signatures, and revision history.

2. **Scope Boundaries**:
   - **AI features are out of scope until the core is done**. All development focus remains strictly dedicated to document fidelity, rendering performance, standards compliance, and kernel-isolated security.

---

## 2. Implementation Order

Development proceeds through sequential, gate-enforced phases:

1. **Foundations**:
   - Linux test environment (containerized CI runtime / package parity)
   - Parity matrix tracking ([`docs/PARITY_MATRIX.md`](PARITY_MATRIX.md))
   - Viewer widget modular refactoring ([`docs/VIEWER_REFACTOR_RFC.md`](VIEWER_REFACTOR_RFC.md))
2. **Page Organization & Security**:
   - Page manipulation (insert, delete, split, replace, combine, reorder, crop, merge)
   - Permission passwords and permission flags
   - Dynamic watermarking
3. **Annotation Completeness & Document Identification**:
   - Full annotation toolset completeness (measurement tools, link insertion, threaded annotation replies)
   - Running headers and footers
   - Bates numbering
4. **OCR (Optical Character Recognition)**:
   - Sandboxed Tesseract 5 / Leptonica helper process
   - Invisible searchable text layer generation (`3 Tr` render mode) on scanned raster PDFs
5. **Digital Signatures**:
   - Dedicated sandboxed signature verification worker (OpenSSL / NSS)
   - PAdES signature validation and cryptographic timestamping
   - Digital signature application
6. **Content Editing (Deliberately Narrow First Version)**:
   - Targeted object-level text stream editing and replacement
   - Embedded image replacement and transformation
7. **Document Conversion**:
   - Extraction of structured text, tabular data, and image XObjects
   - Image-to-PDF generation
   - Structured export to standard document formats (images, text, and tables first)

---

## 3. Mandatory Quality Gates for Every Feature

Every feature landed on `main` must strictly satisfy the five engineering gates:

1. **README Status Row**: A dedicated row in the `README.md` Feature Status table backed by verifiable test references.
2. **Hostile Fixture Suite**: Dedicated hostile/malformed fixtures added to the test suite for any new parser, deserializer, or engine entry point to prevent denial-of-service, memory bombs, or crashes.
3. **External Engine Compatibility Check**: Validation against third-party command-line PDF tools (`qpdf --check`, `pdftoppm`, `pdfinfo`, `pdftotext`, `mutool`) ensuring outputs conform strictly to ISO 32000-1 specifications.
4. **Mutation-Tested Test Assertions**: Tests must be verified via deliberate negative mutations (confirming assertions fail when behaviors are omitted or corrupted).
5. **Threat Model Entry**: An explicit threat analysis entry in [`docs/threat_model.md`](threat_model.md) documenting security properties, attack surfaces, sandboxing constraints, and mitigation status.
