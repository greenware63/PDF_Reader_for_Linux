#!/usr/bin/env python3
"""Automated External-Engine Compatibility Check (poppler, qpdf, mupdf).

Generates output PDFs from the application's engine save and export paths:
  1. compat_annotated_save.pdf: Saved PDF with Highlight and Text Note annotations.
  2. compat_form_filled.pdf: Incremental save with modified AcroForm values.
  3. compat_form_flattened.pdf: Exported flattened PDF with burned-in form widgets.
  4. compat_redacted_export.pdf: Redacted export asserting canary text elimination.

Runs each produced output through independent external PDF analysis and rendering engines:
  - qpdf --check: structural validation and cross-reference integrity.
  - pdfinfo (Poppler): metadata, dimensions, and page count verification.
  - pdftoppm (Poppler): page 1 rasterization asserting valid non-blank rendering.
  - pdftotext (Poppler): text extraction asserting redacted text does not appear and unredacted text does.
  - mutool info / mutool draw (MuPDF): independent engine parsing and raster verification when mupdf-tools is available.

Behavior:
  - In CI (CI=1): Fails closed (exit 1) if essential external tools (qpdf, poppler-utils) are missing.
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


def verify_ppm_non_blank(ppm_path):
    """Assert PPM file contains non-blank pixels (ink present)."""
    try:
        with open(ppm_path, "rb") as f:
            header = f.readline().decode("latin-1", errors="ignore").strip()
            if header not in ("P6", "P5"):
                return False
            # Read dimensions
            line = f.readline().decode("latin-1", errors="ignore").strip()
            while line.startswith("#"):
                line = f.readline().decode("latin-1", errors="ignore").strip()
            parts = line.split()
            if len(parts) >= 2:
                w, h = int(parts[0]), int(parts[1])
            else:
                w, h = 100, 100
            # Read maxval
            f.readline()
            data = f.read()
            if len(data) == 0:
                return False
            # Check for non-white/non-empty pixels
            # In P6 (RGB), 255 255 255 is pure white. If every byte is 255, it's blank.
            has_ink = any(b < 240 for b in data)
            return has_ink
    except Exception as e:
        print(f"Warning: Could not parse PPM {ppm_path}: {e}", file=sys.stderr)
        return False


def main():
    source_root = sys.argv[1] if len(sys.argv) > 1 else "."
    test_render_bin = sys.argv[2] if len(sys.argv) > 2 else "test_render"
    sample_pdf = os.path.join(source_root, "sample.pdf")

    # 1. Check required external tools
    qpdf_exe = check_tool("qpdf", required_in_ci=True)
    pdfinfo_exe = check_tool("pdfinfo", required_in_ci=True)
    pdftoppm_exe = check_tool("pdftoppm", required_in_ci=True)
    pdftotext_exe = check_tool("pdftotext", required_in_ci=True)
    mutool_exe = check_tool("mutool", required_in_ci=False)  # Optional second engine

    if os.environ.get("CI"):
        missing_ci = []
        for name, tool in [("qpdf", qpdf_exe), ("pdfinfo", pdfinfo_exe),
                           ("pdftoppm", pdftoppm_exe), ("pdftotext", pdftotext_exe)]:
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
    print(f" qpdf:      {qpdf_exe}")
    print(f" pdfinfo:   {pdfinfo_exe}")
    print(f" pdftoppm:  {pdftoppm_exe}")
    print(f" pdftotext: {pdftotext_exe}")
    print(f" mutool:    {mutool_exe if mutool_exe else 'NOT INSTALLED (optional)'}")
    print("====================================================")

    with tempfile.TemporaryDirectory() as tmpdir:
        # Step A: Generate output PDFs via test_render --dump-compat-outputs
        print(f"[1/4] Generating application save/export outputs in {tmpdir}...")
        gen_cmd = [test_render_bin, "--dump-compat-outputs", tmpdir, sample_pdf]
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

        # Step B: Run external tools on each output
        for fname, desc in outputs:
            fpath = os.path.join(tmpdir, fname)
            if not os.path.isfile(fpath) or os.path.getsize(fpath) == 0:
                print(f"Error: Expected output {fname} was not produced or is empty", file=sys.stderr)
                failures.append(f"{fname}: Missing output file")
                continue

            print(f"\n[Verifying {fname}] - {desc} ({os.path.getsize(fpath)} bytes)")

            # 1. qpdf --check
            # exit 0 = clean, exit 3 = warning (e.g. unreferenced streams / minor warnings)
            code, out, err = run_command([qpdf_exe, "--check", fpath])
            if code not in (0, 3):
                print(f"  [FAIL] qpdf --check returned error code {code}:\n{err}", file=sys.stderr)
                failures.append(f"{fname}: qpdf check failed")
            else:
                warn_note = " (with acceptable warnings)" if code == 3 else ""
                print(f"  [PASS] qpdf --check: Structure valid{warn_note}")

            # 2. pdfinfo
            code, out, err = run_command([pdfinfo_exe, fpath])
            if code != 0 or "Pages:" not in out:
                print(f"  [FAIL] pdfinfo failed on {fname} (code {code}):\n{err}", file=sys.stderr)
                failures.append(f"{fname}: pdfinfo failed")
            else:
                pages_line = [l for l in out.splitlines() if "Pages:" in l]
                print(f"  [PASS] pdfinfo: Readable by Poppler ({pages_line[0].strip() if pages_line else 'Pages verified'})")

            # 3. pdftoppm -r 50 (Page 1 non-blank render assertion)
            ppm_base = os.path.join(tmpdir, f"{fname}_ppm")
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

            # 4. pdftotext (Text assertions)
            code, out, err = run_command([pdftotext_exe, fpath, "-"])
            if code != 0:
                print(f"  [FAIL] pdftotext failed on {fname} (code {code}):\n{err}", file=sys.stderr)
                failures.append(f"{fname}: pdftotext execution failed")
            else:
                if fname == "compat_redacted_export.pdf":
                    # Assert redacted text does NOT appear
                    if "Classified Canary Secret" in out:
                        print("  [FAIL] pdftotext: Redacted secret found in text extraction!", file=sys.stderr)
                        failures.append(f"{fname}: Redacted secret leaked into pdftotext")
                    else:
                        print("  [PASS] pdftotext: Redacted text eliminated as expected")
                elif fname == "compat_annotated_save.pdf":
                    # Assert unredacted base document text survives
                    if "External Compatibility Base Document" in out:
                        print("  [PASS] pdftotext: Underlying text extracted cleanly")
                    else:
                        print("  [FAIL] pdftotext: Base text missing in annotated file", file=sys.stderr)
                        failures.append(f"{fname}: Base text missing in pdftotext")

            # 5. mutool info & draw (if mupdf-tools is installed)
            if mutool_exe:
                code_info, out_info, err_info = run_command([mutool_exe, "info", fpath])
                if code_info != 0:
                    print(f"  [FAIL] mutool info failed on {fname} (code {code_info}):\n{err_info}", file=sys.stderr)
                    failures.append(f"{fname}: mutool info failed")
                else:
                    mutool_png = os.path.join(tmpdir, f"{fname}_mutool.png")
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
