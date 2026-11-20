#!/usr/bin/env python3
"""CTest fixture determinism check (Threat T17).

Regenerates all hostile AcroForm fixtures via tests/fixtures/make_hostile_form_fixtures.py,
normalizes both committed and regenerated files via qpdf --qdf (ignoring compression differences
and trailer /ID), and verifies that normalized representations are byte-identical.
Ensures committed test fixtures cannot silently drift from the generator.
"""
import difflib
import filecmp
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile


def normalize_pdf(qpdf_exe, in_path, out_path):
    """Normalize a PDF via qpdf --qdf with uncompressed streams, no original object IDs,
    and strip the trailer line matching `^  /ID \\[` so that differing compression or ID
    generation does not cause false-positive drift failures.
    """
    tmp_qdf = out_path + ".raw.qdf"
    cmd = [
        qpdf_exe,
        "--qdf",
        "--object-streams=disable",
        "--no-original-object-ids",
        "--stream-data=uncompress",
        in_path,
        tmp_qdf,
    ]
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    # qpdf exit codes: 0 = ok, 3 = succeeded with warnings (output is still written), 2 = error.
    if res.returncode not in (0, 3):
        raise RuntimeError(f"qpdf normalization failed for {in_path} (exit code {res.returncode}):\n{res.stderr}")

    id_pattern = re.compile(r"^  /ID \[")
    with open(tmp_qdf, "r", encoding="latin-1", errors="replace") as fin, \
         open(out_path, "w", encoding="latin-1", newline="\n") as fout:
        for line in fin:
            if id_pattern.match(line):
                continue
            fout.write(line)

    try:
        os.remove(tmp_qdf)
    except OSError:
        pass


def compare_normalized(file_committed, file_generated, name):
    h_com = hashlib.sha256()
    with open(file_committed, "rb") as f:
        while True:
            chunk = f.read(65536)
            if not chunk:
                break
            h_com.update(chunk)

    h_gen = hashlib.sha256()
    with open(file_generated, "rb") as f:
        while True:
            chunk = f.read(65536)
            if not chunk:
                break
            h_gen.update(chunk)

    if h_com.digest() == h_gen.digest():
        return True

    print(f"Error: Normalized QDF mismatch for {name}:", file=sys.stderr)
    with open(file_committed, "r", encoding="latin-1", errors="replace") as f1, \
         open(file_generated, "r", encoding="latin-1", errors="replace") as f2:
        lines1 = f1.readlines()
        lines2 = f2.readlines()

    diff = difflib.unified_diff(lines1, lines2, fromfile=f"committed/{name}", tofile=f"regenerated/{name}", n=3)
    printed = 0
    for diff_line in diff:
        print(diff_line, end="", file=sys.stderr)
        printed += 1
        if printed >= 20:
            print("\n... [truncated remaining diff output] ...", file=sys.stderr)
            break
    return False


def main():
    source_root = sys.argv[1] if len(sys.argv) > 1 else "."
    fixtures_dir = os.path.join(source_root, "tests", "fixtures")
    generator_script = os.path.join(fixtures_dir, "make_hostile_form_fixtures.py")

    if not os.path.isfile(generator_script):
        print(f"Error: Generator script not found: {generator_script}", file=sys.stderr)
        return 1

    qpdf_exe = shutil.which("qpdf")
    if qpdf_exe is None:
        if os.environ.get("CI"):
            print("Error: 'qpdf' is required in CI environment but not found in PATH!", file=sys.stderr)
            return 1
        print("Note: 'qpdf' not found in PATH; skipping fixture regeneration check on this host.")
        return 0

    with tempfile.TemporaryDirectory() as tmpdir:
        regen_dir = os.path.join(tmpdir, "regen")
        norm_dir = os.path.join(tmpdir, "norm")
        os.makedirs(regen_dir, exist_ok=True)
        os.makedirs(norm_dir, exist_ok=True)

        print(f"Regenerating hostile form fixtures via {generator_script}...")
        cmd = [sys.executable, generator_script, regen_dir]
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if res.returncode != 0:
            print(f"Error: Generator failed with code {res.returncode}:\n{res.stderr}", file=sys.stderr)
            return 1

        # Read the fixture list from the generator's own registry so a new fixture can never be
        # forgotten here. Each entry is (file name, builder, compress). compress=False means the
        # generator writes the bytes itself, without qpdf (needed for content qpdf cannot represent,
        # such as integers beyond 64 bits). Those bytes are pure Python output, hence portable, and
        # are compared exactly. Everything qpdf produced is compared in normalized QDF form, because
        # its compressed bytes depend on the machine's zlib.
        import importlib.util
        sys.dont_write_bytecode = True  # do not leave __pycache__ in the source tree
        spec = importlib.util.spec_from_file_location("hostile_form_generator", generator_script)
        generator = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(generator)
        fixtures = [(entry[0], bool(entry[2])) for entry in generator.FIXTURES]
        if not fixtures:
            print("Error: generator registry FIXTURES is empty", file=sys.stderr)
            return 1

        mismatches = []
        for name, compressed_by_qpdf in fixtures:
            committed_pdf = os.path.join(fixtures_dir, name)
            generated_pdf = os.path.join(regen_dir, name)

            if not os.path.isfile(committed_pdf):
                print(f"Error: Committed fixture missing: {committed_pdf}", file=sys.stderr)
                mismatches.append(name)
                continue
            if not os.path.isfile(generated_pdf):
                print(f"Error: Generated fixture missing: {generated_pdf}", file=sys.stderr)
                mismatches.append(name)
                continue

            if not compressed_by_qpdf:
                if filecmp.cmp(committed_pdf, generated_pdf, shallow=False):
                    print(f"  OK (raw bytes identical, written without qpdf): {name}")
                else:
                    print(f"Error: raw byte mismatch for {name} (written without qpdf, so it must be exact)",
                          file=sys.stderr)
                    mismatches.append(name)
                continue

            norm_committed = os.path.join(norm_dir, name + ".committed.norm.qdf")
            norm_generated = os.path.join(norm_dir, name + ".generated.norm.qdf")

            try:
                normalize_pdf(qpdf_exe, committed_pdf, norm_committed)
                normalize_pdf(qpdf_exe, generated_pdf, norm_generated)
            except Exception as e:
                print(f"Error normalizing {name}: {e}", file=sys.stderr)
                mismatches.append(name)
                continue

            if not compare_normalized(norm_committed, norm_generated, name):
                mismatches.append(name)
            else:
                print(f"  OK (normalized QDF identical): {name}")

        if mismatches:
            print(f"FAILED: {len(mismatches)} fixture(s) drifted from generator output!", file=sys.stderr)
            return 1

        print("All fixtures verified byte-identical in normalized QDF representation.")
        return 0


if __name__ == "__main__":
    sys.exit(main())
