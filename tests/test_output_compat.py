#!/usr/bin/env python3
"""Automated External-Engine Compatibility Check (poppler, qpdf, mupdf).

Generates output PDFs from the application's engine save and export paths:
  1. compat_annotated_save.pdf: Saved PDF with Highlight and Text Note annotations.
  2. compat_form_filled.pdf: Incremental save with modified AcroForm values.
  3. compat_form_flattened.pdf: Exported flattened PDF with burned-in form widgets.
  4. compat_form_appearances_filled.pdf: Incremental save with Latin/Persian text appearances.
  5. compat_form_appearances_flattened.pdf: Flattened export preserving Latin/Persian text appearances.
  6. compat_redacted_export.pdf: Redacted export asserting canary text elimination.
  7. compat_organized_pages.pdf: Reorganized subset/reordered/rotated PDF.
  8. compat_organized_signed.pdf: Reorganized signed PDF verifying signature invalidation.

Runs each produced output through independent external PDF analysis and rendering engines:
  - qpdf --check: structural validation and cross-reference integrity.
  - qpdf --qdf: deep object inspection (/Subtype /Highlight, /Text, /V entries, zero /Widget on flatten).
  - pdfinfo (Poppler): metadata, dimensions, and page count verification.
  - pdftoppm (Poppler): page 1 rasterization asserting valid non-blank rendering.
  - pdftotext (Poppler): text extraction asserting form values survive, unredacted text survives,
    organized page markers match, and redacted export has zero text layer (guarantee G2).
  - pdfsig (Poppler): verifies organized signed output reports a non-valid signature.
  - mutool info / mutool draw (MuPDF): independent engine parsing and raster verification.
  - Redaction pixel comparison (Poppler and MuPDF): asserts inside-box pixels are solid black (0x000000)
    and outside-box pixels match the unredacted source within tolerance (mean < 4.0, bigFrac < 0.015).

Behavior:
  - In CI (CI=1): Fails closed (exit 1) if required external tools (qpdf, poppler-utils, mupdf-tools) are missing.
  - Local workstation: Gracefully reports missing tools and skips if environment lacks external dependencies.
"""
import os
import shutil
import subprocess
import sys
import tempfile


def run_command(cmd, desc=""):
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    return res.returncode, res.stdout, res.stderr


def check_tool(name, required_in_ci=True):
    path = shutil.which(name)
    if path is None:
        if required_in_ci and os.environ.get("CI"):
            print(f"Error: Required tool '{name}' not found in CI PATH!", file=sys.stderr)
            return None
        return None
    return path


def read_ppm(ppm_path):
    """Read P6 PPM file and return (bytes_data, width, height)."""
    try:
        with open(ppm_path, "rb") as f:
            header = f.readline().decode("latin-1", errors="ignore").strip()
            if header not in ("P6", "P5"):
                return None, 0, 0
            line = f.readline().decode("latin-1", errors="ignore").strip()
            while line.startswith("#") or not line:
                line = f.readline().decode("latin-1", errors="ignore").strip()
            parts = line.split()
            while len(parts) < 2:
                next_line = f.readline().decode("latin-1", errors="ignore").strip()
                if not next_line.startswith("#"):
                    parts.extend(next_line.split())
            w, h = int(parts[0]), int(parts[1])
            maxval_line = f.readline().decode("latin-1", errors="ignore").strip()
            while maxval_line.startswith("#") or not maxval_line:
                maxval_line = f.readline().decode("latin-1", errors="ignore").strip()
            data = f.read()
            return data, w, h
    except Exception as e:
        print(f"Warning: Could not read PPM {ppm_path}: {e}", file=sys.stderr)
        return None, 0, 0


def verify_ppm_non_blank(ppm_path):
    """Assert PPM file contains non-blank pixels (ink present)."""
    data, w, h = read_ppm(ppm_path)
    if not data or len(data) == 0:
        return False
    # In P6 (RGB), 255 255 255 is pure white. If every byte is 255, it's blank.
    return any(b < 240 for b in data)


def verify_redaction_pixels(engine_label, src_file, out_file, fname, failures):
    """Verify inside-box pixels are solid black (0x000000) and outside-box pixels match source."""
    data_src, ws, hs = read_ppm(src_file)
    data_out, wo, ho = read_ppm(out_file)

    if not data_src:
        print(f"  [FAIL] Pixel verification ({engine_label}): Unreadable source PPM image at {src_file}", file=sys.stderr)
        failures.append(f"{fname}: Unreadable {engine_label} source PPM image")
        return False
    if not data_out:
        print(f"  [FAIL] Pixel verification ({engine_label}): Unreadable output PPM image at {out_file}", file=sys.stderr)
        failures.append(f"{fname}: Unreadable {engine_label} output PPM image")
        return False
    if ws != wo or hs != ho:
        print(f"  [FAIL] Pixel verification ({engine_label}): Dimension mismatch: source {ws}x{hs} vs output {wo}x{ho}", file=sys.stderr)
        failures.append(f"{fname}: {engine_label} dimension mismatch: source {ws}x{hs} vs output {wo}x{ho}")
        return False

    scale = 150.0 / 72.0
    # Redaction rectangle in PDF points: left=80, top=680, right=260, bottom=720
    # Pixel box with 1px edgeGrow:
    bx0 = int(80.0 * scale) - 1
    by0 = int(680.0 * scale) - 1
    bx1 = int(260.0 * scale) + 1
    by1 = int(720.0 * scale) + 1

    all_black = True
    non_black_count = 0
    total_inside = 0
    for y in range(max(0, by0 + 2), min(ho, by1 - 2)):
        for x in range(max(0, bx0 + 2), min(wo, bx1 - 2)):
            total_inside += 1
            off = (y * wo + x) * 3
            r, g, b = data_out[off], data_out[off + 1], data_out[off + 2]
            if r != 0 or g != 0 or b != 0:
                all_black = False
                non_black_count += 1

    if all_black:
        print(f"  [PASS] Pixel verification ({engine_label}): Pixels inside redaction rectangle are solid black (0x000000) ({total_inside} px inspected)")
    else:
        print(f"  [FAIL] Pixel verification ({engine_label}): {non_black_count}/{total_inside} non-black pixels found inside redaction rectangle!", file=sys.stderr)
        failures.append(f"{fname}: {engine_label} redaction box contains non-black pixels ({non_black_count}/{total_inside})")

    # Outside box: dilate mask by 6 pixels (matching tests/redaction/test_redaction.cpp)
    mask_x0 = max(0, bx0 - 6)
    mask_y0 = max(0, by0 - 6)
    mask_x1 = min(wo, bx1 + 6)
    mask_y1 = min(ho, by1 + 6)

    sum_d = 0
    cnt = 0
    big = 0
    for y in range(ho):
        in_y = (mask_y0 <= y < mask_y1)
        for x in range(wo):
            if in_y and (mask_x0 <= x < mask_x1):
                continue
            off = (y * wo + x) * 3
            ra, ga, ba = data_src[off], data_src[off + 1], data_src[off + 2]
            rb, gb, bb = data_out[off], data_out[off + 1], data_out[off + 2]
            d = (abs(int(ra) - int(rb)) + abs(int(ga) - int(gb)) + abs(int(ba) - int(bb))) // 3
            sum_d += d
            cnt += 1
            if d > 96:
                big += 1

    mean = (sum_d / float(cnt)) if cnt > 0 else 0.0
    big_frac = (big / float(cnt)) if cnt > 0 else 0.0

    if mean < 4.0 and big_frac < 0.015:
        print(f"  [PASS] Pixel verification ({engine_label}): Outside-box pixels match source render (mean diff={mean:.2f} < 4.0, strong diff={big_frac*100:.2f}% < 1.5%)")
    else:
        print(f"  [FAIL] Pixel verification ({engine_label}): Outside-box difference exceeds tolerance (mean={mean:.2f}, bigFrac={big_frac:.3f})", file=sys.stderr)
        failures.append(f"{fname}: {engine_label} outside-box pixel diff exceeded tolerance (mean={mean:.2f}, bigFrac={big_frac:.3f})")

    return True


def main():
    source_root = sys.argv[1] if len(sys.argv) > 1 else "."
    test_render_bin = sys.argv[2] if len(sys.argv) > 2 else "test_render"
    output_dir = sys.argv[3] if len(sys.argv) > 3 else os.path.join(source_root, "build", "compat_outputs")
    work_dir = sys.argv[4] if len(sys.argv) > 4 else os.path.join(os.path.dirname(os.path.abspath(output_dir)), "compat_work")
    sample_pdf = os.path.join(source_root, "sample.pdf")

    # 1. Check required external tools (qpdf, poppler-utils, mutool required in CI)
    qpdf_exe = check_tool("qpdf", required_in_ci=True)
    pdfinfo_exe = check_tool("pdfinfo", required_in_ci=True)
    pdftoppm_exe = check_tool("pdftoppm", required_in_ci=True)
    pdftotext_exe = check_tool("pdftotext", required_in_ci=True)
    pdfsig_exe = check_tool("pdfsig", required_in_ci=True)
    mutool_exe = check_tool("mutool", required_in_ci=True)

    if os.environ.get("CI"):
        missing_ci = []
        for name, tool in [("qpdf", qpdf_exe), ("pdfinfo", pdfinfo_exe),
                           ("pdftoppm", pdftoppm_exe), ("pdftotext", pdftotext_exe),
                           ("pdfsig", pdfsig_exe), ("mutool", mutool_exe)]:
            if tool is None:
                missing_ci.append(name)
        if missing_ci:
            print(f"FAILED: CI requires external tools: {', '.join(missing_ci)}", file=sys.stderr)
            return 1

    if not all([qpdf_exe, pdfinfo_exe, pdftoppm_exe, pdftotext_exe]):
        print("Note: External tools (qpdf or poppler-utils) missing in local environment; skipping compatibility suite.")
        return 0

    print("====================================================")
    print(" EXTERNAL ENGINE COMPATIBILITY TEST SUITE          ")
    print(f" Output Dir: {output_dir}")
    print(f" Work Dir:   {work_dir}")
    print(f" qpdf:       {qpdf_exe}")
    print(f" pdfinfo:    {pdfinfo_exe}")
    print(f" pdftoppm:   {pdftoppm_exe}")
    print(f" pdftotext:  {pdftotext_exe}")
    print(f" mutool:     {mutool_exe if mutool_exe else 'NOT INSTALLED (optional)'}")
    print("====================================================")

    os.makedirs(output_dir, exist_ok=True)
    os.makedirs(work_dir, exist_ok=True)

    # Step A: Generate output PDFs via test_render --dump-compat-outputs
    print(f"[1/4] Generating application save/export outputs in {output_dir} (work: {work_dir})...")
    gen_cmd = [test_render_bin, "--dump-compat-outputs", output_dir, sample_pdf, work_dir]
    code, out, err = run_command(gen_cmd)
    if code != 0:
        print(f"Error: test_render --dump-compat-outputs failed (code {code}):\n{err}", file=sys.stderr)
        return 1

    outputs = [
        ("compat_annotated_save.pdf", "Annotated Document Save"),
        ("compat_form_filled.pdf", "Form-Filled Document Save"),
        ("compat_form_flattened.pdf", "Flattened Form Export"),
        ("compat_form_appearances_filled.pdf", "Latin Text Appearance Save"),
        ("compat_form_appearances_flattened.pdf", "Latin Text Appearance Flattened Export"),
        ("compat_redacted_export.pdf", "Redacted Document Export"),
        ("compat_organized_pages.pdf", "Organized Pages Subset/Reorder/Rotate"),
        ("compat_organized_signed.pdf", "Organized Signed Document (Invalidated Signature)"),
    ]

    failures = []
    redaction_pixel_verified_poppler = False
    redaction_pixel_verified_mupdf = False
    form_value_assertions = 0

    with tempfile.TemporaryDirectory() as scratch_dir:
        # Step B: Run external tools and deepened assertions on each output
        for fname, desc in outputs:
            fpath = os.path.join(output_dir, fname)
            if not os.path.isfile(fpath) or os.path.getsize(fpath) == 0:
                print(f"Error: Expected output {fname} was not produced or is empty", file=sys.stderr)
                failures.append(f"{fname}: Missing output file")
                continue

            print(f"\n[Verifying {fname}] - {desc} ({os.path.getsize(fpath)} bytes)")

            # 1. qpdf --check
            code, out, err = run_command([qpdf_exe, "--check", fpath])
            if code not in (0, 3):
                print(f"  [FAIL] qpdf --check returned error code {code}:\n{err}", file=sys.stderr)
                failures.append(f"{fname}: qpdf check failed")
            else:
                warn_note = " (with acceptable warnings)" if code == 3 else ""
                print(f"  [PASS] qpdf --check: Structure valid{warn_note}")

            # 2. qpdf --qdf object stream inspection
            qdf_out = os.path.join(scratch_dir, f"{fname}.qdf")
            code, out, err = run_command([qpdf_exe, "--qdf", "--object-streams=disable", fpath, qdf_out])
            qdf_bytes = b""
            if code in (0, 3) and os.path.isfile(qdf_out):
                with open(qdf_out, "rb") as qf:
                    qdf_bytes = qf.read()

            if fname == "compat_annotated_save.pdf":
                if b"/Subtype /Highlight" not in qdf_bytes:
                    print("  [FAIL] qdf inspection: /Subtype /Highlight missing from saved PDF", file=sys.stderr)
                    failures.append(f"{fname}: Missing /Subtype /Highlight in qdf")
                elif b"/Subtype /Text" not in qdf_bytes:
                    print("  [FAIL] qdf inspection: /Subtype /Text missing from saved PDF", file=sys.stderr)
                    failures.append(f"{fname}: Missing /Subtype /Text in qdf")
                else:
                    print("  [PASS] qdf inspection: /Subtype /Highlight and /Subtype /Text verified in object graph")

            elif fname == "compat_form_filled.pdf":
                if b"CompatibilityTester" not in qdf_bytes or b"/V" not in qdf_bytes:
                    print("  [FAIL] qdf inspection: /V (CompatibilityTester) missing from form fields", file=sys.stderr)
                    failures.append(f"{fname}: /V CompatibilityTester missing in qdf")
                elif b"Germany" not in qdf_bytes:
                    print("  [FAIL] qdf inspection: /V (Germany) missing from form choice field", file=sys.stderr)
                    failures.append(f"{fname}: /V Germany missing in qdf")
                else:
                    print("  [PASS] qdf inspection: Form values 'CompatibilityTester' and 'Germany' stored in /V entries")

            elif fname == "compat_form_flattened.pdf":
                if b"/Subtype /Widget" in qdf_bytes or b"/Widget" in qdf_bytes:
                    print("  [FAIL] qdf inspection: Flattened document still contains /Widget annotations!", file=sys.stderr)
                    failures.append(f"{fname}: Flattened document contains /Widget objects")
                else:
                    print("  [PASS] qdf inspection: Zero /Widget objects confirmed in flattened export")

            elif fname == "compat_form_appearances_filled.pdf":
                expected_appearance_text = (
                    b"LatinAppearanceName",
                    b"ExternalPersianField",
                    b"First external line",
                    b"Second external line",
                )
                missing = [value.decode("ascii") for value in expected_appearance_text
                           if qdf_bytes.count(value) < 2]
                if b"/AP" not in qdf_bytes or missing:
                    print(f"  [FAIL] qdf inspection: Saved /AP text missing or incomplete: {missing}", file=sys.stderr)
                    failures.append(f"{fname}: /AP text missing for {missing}")
                else:
                    print("  [PASS] qdf inspection: Saved /AP streams contain name, multiline notes, and persian_name text")

            elif fname == "compat_form_appearances_flattened.pdf":
                if b"/Subtype /Widget" in qdf_bytes or b"/Widget" in qdf_bytes:
                    print("  [FAIL] qdf inspection: Flattened appearance export still contains /Widget annotations!", file=sys.stderr)
                    failures.append(f"{fname}: Flattened document contains /Widget objects")
                elif not all(value in qdf_bytes for value in (
                    b"LatinAppearanceName", b"ExternalPersianField",
                    b"First external line", b"Second external line",
                )):
                    print("  [FAIL] qdf inspection: Flattened page streams are missing form text", file=sys.stderr)
                    failures.append(f"{fname}: Flattened page streams missing form text")
                else:
                    print("  [PASS] qdf inspection: Flattened page streams contain all three form values")

            # 3. pdfinfo
            code, out, err = run_command([pdfinfo_exe, fpath])
            if code != 0 or "Pages:" not in out:
                print(f"  [FAIL] pdfinfo failed on {fname} (code {code}):\n{err}", file=sys.stderr)
                failures.append(f"{fname}: pdfinfo failed")
            else:
                pages_line = [l for l in out.splitlines() if "Pages:" in l]
                if fname == "compat_organized_pages.pdf" and "Pages:          3" not in out and "Pages: 3" not in " ".join(out.split()):
                    print(f"  [FAIL] pdfinfo: Expected 3 pages in {fname}, got:\n{out}", file=sys.stderr)
                    failures.append(f"{fname}: expected 3 pages")
                else:
                    print(f"  [PASS] pdfinfo: Readable by Poppler ({pages_line[0].strip() if pages_line else 'Pages verified'})")

            # 4. pdftoppm -r 50 (Page 1 non-blank render assertion)
            ppm_base = os.path.join(scratch_dir, f"{fname}_ppm")
            code, out, err = run_command([pdftoppm_exe, "-r", "50", "-f", "1", "-l", "1", fpath, ppm_base])
            ppm_file = f"{ppm_base}-1.ppm"
            if code != 0 or not os.path.isfile(ppm_file):
                print(f"  [FAIL] pdftoppm failed on {fname} (code {code}):\n{err}", file=sys.stderr)
                failures.append(f"{fname}: pdftoppm rasterization failed")
            else:
                if verify_ppm_non_blank(ppm_file):
                    print(f"  [PASS] pdftoppm: Rendered page 1 successfully (non-blank ink detected)")
                else:
                    print(f"  [FAIL] pdftoppm: Page 1 rendered completely blank!", file=sys.stderr)
                    failures.append(f"{fname}: pdftoppm rendered blank page")

            # 5. pdftotext (Text assertions)
            code, out, err = run_command([pdftotext_exe, fpath, "-"])
            if code != 0:
                print(f"  [FAIL] pdftotext failed on {fname} (code {code}):\n{err}", file=sys.stderr)
                failures.append(f"{fname}: pdftotext execution failed")
            else:
                if fname == "compat_redacted_export.pdf":
                    clean_text = out.replace("\x0c", "").strip()
                    if clean_text != "":
                        print(f"  [FAIL] pdftotext: Redacted export has unexpected text layer: {clean_text!r}", file=sys.stderr)
                        failures.append(f"{fname}: Text layer not eliminated (guarantee G2 violation)")
                    elif "Classified Canary Secret" in out:
                        print("  [FAIL] pdftotext: Redacted secret found in text extraction!", file=sys.stderr)
                        failures.append(f"{fname}: Redacted secret leaked into pdftotext")
                    else:
                        print("  [PASS] pdftotext: Guarantee G2 verified (zero text layer, canary eliminated)")

                elif fname == "compat_annotated_save.pdf":
                    if "External Compatibility Base Document" in out:
                        print("  [PASS] pdftotext: Underlying base text survived incremental save")
                    else:
                        print("  [FAIL] pdftotext: Base text missing in annotated file", file=sys.stderr)
                        failures.append(f"{fname}: Base text missing in pdftotext")

                elif fname == "compat_organized_pages.pdf":
                    if all(m in out for m in ("PAGE 4", "PAGE 2", "PAGE 1")) and "PAGE 3" not in out:
                        print("  [PASS] pdftotext: Organized pages contain PAGE 4, PAGE 2, PAGE 1 (PAGE 3 deleted)")
                    else:
                        print(f"  [FAIL] pdftotext: Unexpected page markers in {fname}: {out!r}", file=sys.stderr)
                        failures.append(f"{fname}: Unexpected page markers in pdftotext")

                elif fname in ("compat_form_filled.pdf", "compat_form_flattened.pdf"):
                    for val in ("CompatibilityTester", "Germany"):
                        form_value_assertions += 1
                        present = (val in out)
                        if not present:
                            print(f"  [FAIL] pdftotext: Expected form value '{val}' not found in {fname}", file=sys.stderr)
                            failures.append(f"{fname}: Missing form value '{val}' in pdftotext")
                        else:
                            print(f"  [PASS] pdftotext: Form value '{val}' verified in {fname}")

                elif fname in ("compat_form_appearances_filled.pdf", "compat_form_appearances_flattened.pdf"):
                    normalized_text = " ".join(out.split())
                    for val in ("LatinAppearanceName", "ExternalPersianField",
                                "First external line", "Second external line"):
                        form_value_assertions += 1
                        if val not in normalized_text:
                            print(f"  [FAIL] pdftotext: Expected form value '{val}' not found in {fname}", file=sys.stderr)
                            failures.append(f"{fname}: Missing form value '{val}' in pdftotext")
                        else:
                            print(f"  [PASS] pdftotext: Form value '{val}' verified in {fname}")

            if fname == "compat_organized_signed.pdf" and pdfsig_exe:
                _, sig_out, sig_err = run_command([pdfsig_exe, fpath])
                combined_sig = sig_out + "\n" + sig_err
                if "Signature #1:" not in combined_sig or "Signature Validation:" not in combined_sig:
                    print(f"  [FAIL] pdfsig: Could not parse Signature #1 / Signature Validation:\n{combined_sig}", file=sys.stderr)
                    failures.append(f"{fname}: pdfsig did not report Signature #1 / Signature Validation")
                elif "Signature is Valid" in combined_sig:
                    print(f"  [FAIL] pdfsig: Expected invalidated signature after rewrite, got:\n{combined_sig}", file=sys.stderr)
                    failures.append(f"{fname}: Signature unexpectedly valid after rewrite")
                else:
                    print("  [PASS] pdfsig: Signature #1 reported with non-valid status after non-incremental rewrite")

            if fname in ("compat_form_appearances_filled.pdf", "compat_form_appearances_flattened.pdf"):
                for page_number, label, left, bottom, right, top in (
                    (1, "name", 72.0, 690.0, 300.0, 714.0),
                    (1, "persian_name", 72.0, 480.0, 300.0, 504.0),
                    (2, "notes", 72.0, 500.0, 300.0, 590.0),
                ):
                    rect_base = os.path.join(scratch_dir, f"{fname}_{label}_{page_number}")
                    code, _, err = run_command([
                        pdftoppm_exe, "-singlefile", "-r", "150", "-f", str(page_number), "-l", str(page_number),
                        fpath, rect_base,
                    ])
                    ppm_file = rect_base + ".ppm"
                    if code != 0 or not os.path.isfile(ppm_file):
                        print(f"  [FAIL] pdftoppm rectangle render failed for {label} on page {page_number}: {err}",
                              file=sys.stderr)
                        failures.append(f"{fname}: failed rectangle raster for {label}")
                        continue
                    pixels, width, height = read_ppm(ppm_file)
                    if not pixels:
                        print(f"  [FAIL] Could not read {label} rectangle render", file=sys.stderr)
                        failures.append(f"{fname}: unreadable rectangle raster for {label}")
                        continue
                    scale = 150.0 / 72.0
                    x0 = max(0, int(left * scale) + 2)
                    x1 = min(width, int(right * scale) - 2)
                    y0 = max(0, int((792.0 - top) * scale) + 2)
                    y1 = min(height, int((792.0 - bottom) * scale) - 2)
                    ink = 0
                    for y in range(y0, y1):
                        for x in range(x0, x1):
                            offset = (y * width + x) * 3
                            if min(pixels[offset], pixels[offset + 1], pixels[offset + 2]) < 220:
                                ink += 1
                    if ink < 10:
                        print(f"  [FAIL] External-engine rectangle check: {label} has only {ink} ink pixels",
                              file=sys.stderr)
                        failures.append(f"{fname}: no visible ink in {label} widget rectangle")
                    else:
                        print(f"  [PASS] External-engine rectangle check: {label} has {ink} ink pixels")

            # 6. Redaction Pixel Verification (inside solid black, outside matches unredacted source)
            if fname == "compat_redacted_export.pdf":
                redact_src = os.path.join(work_dir, "compat_redacted_source.pdf")
                if not os.path.isfile(redact_src):
                    print(f"  [FAIL] Pixel verification: Redaction source file missing at {redact_src}", file=sys.stderr)
                    failures.append(f"{fname}: Redaction source missing ({redact_src})")
                else:
                    src_ppm_base = os.path.join(scratch_dir, "redact_src_ppm")
                    out_ppm_base = os.path.join(scratch_dir, "redact_out_ppm")
                    c1, _, e1 = run_command([pdftoppm_exe, "-r", "150", "-f", "1", "-l", "1", redact_src, src_ppm_base])
                    c2, _, e2 = run_command([pdftoppm_exe, "-r", "150", "-f", "1", "-l", "1", fpath, out_ppm_base])
                    src_file = f"{src_ppm_base}-1.ppm"
                    out_file = f"{out_ppm_base}-1.ppm"

                    if c1 != 0 or c2 != 0:
                        print(f"  [FAIL] Pixel verification (Poppler): pdftoppm rasterization failed (src={c1}, out={c2}):\n{e1}\n{e2}", file=sys.stderr)
                        failures.append(f"{fname}: pdftoppm failed during pixel verification")
                    else:
                        if verify_redaction_pixels("Poppler", src_file, out_file, fname, failures):
                            redaction_pixel_verified_poppler = True

                    if mutool_exe:
                        src_mu_ppm = os.path.join(scratch_dir, "redact_src_mutool.ppm")
                        out_mu_ppm = os.path.join(scratch_dir, "redact_out_mutool.ppm")
                        m1, _, me1 = run_command([mutool_exe, "draw", "-r", "150", "-F", "ppm", "-o", src_mu_ppm, redact_src, "1"])
                        m2, _, me2 = run_command([mutool_exe, "draw", "-r", "150", "-F", "ppm", "-o", out_mu_ppm, fpath, "1"])
                        if m1 != 0 or m2 != 0:
                            print(f"  [FAIL] Pixel verification (MuPDF): mutool draw failed (src={m1}, out={m2}):\n{me1}\n{me2}", file=sys.stderr)
                            failures.append(f"{fname}: mutool draw failed during pixel verification")
                        else:
                            if verify_redaction_pixels("MuPDF", src_mu_ppm, out_mu_ppm, fname, failures):
                                redaction_pixel_verified_mupdf = True

            # 7. mutool info & draw (when installed, required in CI)
            if mutool_exe:
                code_info, out_info, err_info = run_command([mutool_exe, "info", fpath])
                if code_info != 0:
                    print(f"  [FAIL] mutool info failed on {fname} (code {code_info}):\n{err_info}", file=sys.stderr)
                    failures.append(f"{fname}: mutool info failed")
                else:
                    mutool_png = os.path.join(scratch_dir, f"{fname}_mutool.png")
                    code_draw, out_draw, err_draw = run_command([mutool_exe, "draw", "-o", mutool_png, fpath, "1"])
                    if code_draw != 0 or not os.path.isfile(mutool_png) or os.path.getsize(mutool_png) == 0:
                        print(f"  [FAIL] mutool draw failed on {fname} (code {code_draw}):\n{err_draw}", file=sys.stderr)
                        failures.append(f"{fname}: mutool draw failed")
                    else:
                        print(f"  [PASS] mutool (MuPDF): Document parsed and rasterized page 1 to PNG ({os.path.getsize(mutool_png)} bytes)")

        # 3. Regression guards: ensure redaction pixel verification & form-value assertions actually ran
        if not redaction_pixel_verified_poppler:
            print("  [FAIL] Regression guard: Poppler redaction pixel verification did not run!", file=sys.stderr)
            failures.append("compat_redacted_export.pdf: Poppler redaction pixel verification did not run")
        if mutool_exe and not redaction_pixel_verified_mupdf:
            print("  [FAIL] Regression guard: MuPDF redaction pixel verification did not run!", file=sys.stderr)
            failures.append("compat_redacted_export.pdf: MuPDF redaction pixel verification did not run")
        expected_form_value_assertions = 12
        if form_value_assertions != expected_form_value_assertions:
            print(f"  [FAIL] Regression guard: expected {expected_form_value_assertions} form-value assertions, ran {form_value_assertions}!", file=sys.stderr)
            failures.append(f"form_value_assertions mismatch: expected {expected_form_value_assertions}, ran {form_value_assertions}")
        else:
            print(f"  [PASS] Vacuity guard: {form_value_assertions}/{expected_form_value_assertions} form-value assertions executed")

        if failures:
            print("\n====================================================")
            print(f" FAILED: {len(failures)} compatibility check(s) failed:")
            for f in failures:
                print(f"   - {f}")
            print("====================================================")
            return 1

        print("\n====================================================")
        print(" ALL EXTERNAL-ENGINE COMPATIBILITY CHECKS PASSED!   ")
        print("====================================================")
        return 0


if __name__ == "__main__":
    sys.exit(main())
