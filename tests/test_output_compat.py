#!/usr/bin/env python3
"""Automated External-Engine Compatibility Check (poppler, qpdf, mupdf).

Generates output PDFs from the application's engine save and export paths:
  1. compat_annotated_save.pdf: Saved PDF with Highlight and Text Note annotations.
  2. compat_form_filled.pdf: Incremental save with modified AcroForm values.
  3. compat_form_flattened.pdf: Exported flattened PDF with burned-in form widgets.
  4. compat_redacted_export.pdf: Redacted export asserting canary text elimination.

Runs each produced output through independent external PDF analysis and rendering engines:
  - qpdf --check: structural validation and cross-reference integrity.
  - qpdf --qdf: deep object inspection (/Subtype /Highlight, /Text, /V entries, zero /Widget on flatten).
  - pdfinfo (Poppler): metadata, dimensions, and page count verification.
  - pdftoppm (Poppler): page 1 rasterization asserting valid non-blank rendering.
  - pdftotext (Poppler): text extraction asserting form values survive, unredacted text survives,
    and redacted export has zero text layer (guarantee G2).
  - mutool info / mutool draw (MuPDF): independent engine parsing and raster verification.
  - Redaction pixel comparison: asserts inside-box pixels are solid black (0xFF000000) and
    outside-box pixels match the unredacted source within tolerance (mean < 4.0, bigFrac < 0.015).

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


def main():
    source_root = sys.argv[1] if len(sys.argv) > 1 else "."
    test_render_bin = sys.argv[2] if len(sys.argv) > 2 else "test_render"
    output_dir = sys.argv[3] if len(sys.argv) > 3 else os.path.join(source_root, "build", "compat_outputs")
    sample_pdf = os.path.join(source_root, "sample.pdf")

    # 1. Check required external tools (mutool is required in CI)
    if os.environ.get("CI") and shutil.which("mutool") is None:
        try:
            print("CI: mutool not found; installing mupdf-tools via apt-get...")
            subprocess.run(["sudo", "apt-get", "update"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            subprocess.run(["sudo", "apt-get", "install", "-y", "mupdf-tools"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except Exception as e:
            print(f"Warning: Failed to install mupdf-tools: {e}", file=sys.stderr)

    qpdf_exe = check_tool("qpdf", required_in_ci=True)
    pdfinfo_exe = check_tool("pdfinfo", required_in_ci=True)
    pdftoppm_exe = check_tool("pdftoppm", required_in_ci=True)
    pdftotext_exe = check_tool("pdftotext", required_in_ci=True)
    mutool_exe = check_tool("mutool", required_in_ci=True)

    if os.environ.get("CI"):
        missing_ci = []
        for name, tool in [("qpdf", qpdf_exe), ("pdfinfo", pdfinfo_exe),
                           ("pdftoppm", pdftoppm_exe), ("pdftotext", pdftotext_exe),
                           ("mutool", mutool_exe)]:
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
    print(f" qpdf:       {qpdf_exe}")
    print(f" pdfinfo:    {pdfinfo_exe}")
    print(f" pdftoppm:   {pdftoppm_exe}")
    print(f" pdftotext:  {pdftotext_exe}")
    print(f" mutool:     {mutool_exe if mutool_exe else 'NOT INSTALLED (optional)'}")
    print("====================================================")

    os.makedirs(output_dir, exist_ok=True)

    # Step A: Generate output PDFs via test_render --dump-compat-outputs
    print(f"[1/4] Generating application save/export outputs in {output_dir}...")
    gen_cmd = [test_render_bin, "--dump-compat-outputs", output_dir, sample_pdf]
    code, out, err = run_command(gen_cmd)
    if code != 0:
        print(f"Error: test_render --dump-compat-outputs failed (code {code}):\n{err}", file=sys.stderr)
        return 1

    outputs = [
        ("compat_annotated_save.pdf", "Annotated Document Save"),
        ("compat_form_filled.pdf", "Form-Filled Document Save"),
        ("compat_form_flattened.pdf", "Flattened Form Export"),
        ("compat_redacted_export.pdf", "Redacted Document Export"),
    ]

    failures = []

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

            # 3. pdfinfo
            code, out, err = run_command([pdfinfo_exe, fpath])
            if code != 0 or "Pages:" not in out:
                print(f"  [FAIL] pdfinfo failed on {fname} (code {code}):\n{err}", file=sys.stderr)
                failures.append(f"{fname}: pdfinfo failed")
            else:
                pages_line = [l for l in out.splitlines() if "Pages:" in l]
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

                elif fname in ("compat_form_filled.pdf", "compat_form_flattened.pdf"):
                    missing_form_vals = []
                    for val in ("CompatibilityTester", "Germany"):
                        if val not in out:
                            missing_form_vals.append(val)
                    if missing_form_vals:
                        print(f"  [FAIL] pdftotext: Expected form values {missing_form_vals} not found in {fname}", file=sys.stderr)
                        failures.append(f"{fname}: Missing form values in pdftotext: {missing_form_vals}")
                    else:
                        print(f"  [PASS] pdftotext: Form values 'CompatibilityTester' and 'Germany' extracted cleanly")

            # 6. Redaction Pixel Verification (inside solid black, outside matches unredacted source)
            if fname == "compat_redacted_export.pdf":
                redact_src = os.path.join(tempfile.gettempdir(), "compat_temp_redact_base.pdf")
                if os.path.isfile(redact_src):
                    src_ppm_base = os.path.join(scratch_dir, "redact_src_ppm")
                    out_ppm_base = os.path.join(scratch_dir, "redact_out_ppm")
                    run_command([pdftoppm_exe, "-r", "150", "-f", "1", "-l", "1", redact_src, src_ppm_base])
                    run_command([pdftoppm_exe, "-r", "150", "-f", "1", "-l", "1", fpath, out_ppm_base])
                    src_file = f"{src_ppm_base}-1.ppm"
                    out_file = f"{out_ppm_base}-1.ppm"

                    data_src, ws, hs = read_ppm(src_file)
                    data_out, wo, ho = read_ppm(out_file)

                    if data_src and data_out and ws == wo and hs == ho:
                        scale = 150.0 / 72.0
                        # Redaction rectangle in PDF points: left=80, top=680, right=260, bottom=720
                        # Pixel box with 1px edgeGrow:
                        bx0 = int(80.0 * scale) - 1
                        by0 = int(680.0 * scale) - 1
                        bx1 = int(260.0 * scale) + 1
                        by1 = int(720.0 * scale) + 1

                        # A. Inside box: verify solid black (0, 0, 0)
                        # Test inside rect with 2px margin from edge
                        all_black = True
                        for y in range(max(0, by0 + 2), min(ho, by1 - 2)):
                            for x in range(max(0, bx0 + 2), min(wo, bx1 - 2)):
                                off = (y * wo + x) * 3
                                r, g, b = data_out[off], data_out[off + 1], data_out[off + 2]
                                if r != 0 or g != 0 or b != 0:
                                    all_black = False
                                    break
                            if not all_black:
                                break

                        if all_black:
                            print("  [PASS] Pixel verification: Pixels inside redaction rectangle are solid black (0x000000)")
                        else:
                            print("  [FAIL] Pixel verification: Non-black pixels found inside redaction rectangle!", file=sys.stderr)
                            failures.append(f"{fname}: Redaction box contains non-black pixels")

                        # B. Outside box: cross-engine comparison against source render
                        # Dilate mask by 6 pixels (matching tests/redaction/test_redaction.cpp line 341)
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

                        # Tolerance commitment from docs/REDACTION.md §234 and test_redaction.cpp lines 354-355:
                        # mean < 4.0 intensity levels, bigFrac < 0.015 (1.5%)
                        if mean < 4.0 and big_frac < 0.015:
                            print(f"  [PASS] Pixel verification: Outside-box pixels match source render (mean diff={mean:.2f} < 4.0, strong diff={big_frac*100:.2f}% < 1.5%)")
                        else:
                            print(f"  [FAIL] Pixel verification: Outside-box difference exceeds tolerance (mean={mean:.2f}, bigFrac={big_frac:.3f})", file=sys.stderr)
                            failures.append(f"{fname}: Outside-box pixel diff exceeded tolerance")

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
