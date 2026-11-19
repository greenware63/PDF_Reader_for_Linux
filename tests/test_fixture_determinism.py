#!/usr/bin/env python3
"""CTest fixture determinism check (Threat T17).

Regenerates all hostile AcroForm fixtures into a temporary directory
and byte-compares them against the committed fixtures in tests/fixtures/.
Ensures committed test binaries cannot silently drift from the generator.
"""
import filecmp
import os
import shutil
import subprocess
import sys
import tempfile


def main():
    source_root = sys.argv[1] if len(sys.argv) > 1 else "."
    fixtures_dir = os.path.join(source_root, "tests", "fixtures")
    generator_script = os.path.join(fixtures_dir, "make_hostile_form_fixtures.py")

    if not os.path.isfile(generator_script):
        print(f"Error: Generator script not found: {generator_script}", file=sys.stderr)
        return 1

    # Check if qpdf is available; if not on this machine, report and return 0 (skip)
    if shutil.which("qpdf") is None:
        print("Note: 'qpdf' not found in PATH; skipping fixture regeneration check on this host.")
        return 0

    with tempfile.TemporaryDirectory() as tmpdir:
        print(f"Regenerating hostile form fixtures into {tmpdir}...")
        cmd = [sys.executable, generator_script, tmpdir]
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if res.returncode != 0:
            print(f"Error: Generator failed with code {res.returncode}:\n{res.stderr}", file=sys.stderr)
            return 1

        fixtures = [
            "hostile_form_many_widgets.pdf",
            "hostile_form_big_values.pdf",
            "hostile_form_slow_load_MANUAL.pdf",
            "hostile_form_oversize_values.pdf",
            "hostile_form_huge_options.pdf",
            "hostile_form_deep_hierarchy.pdf",
            "hostile_form_long_names.pdf",
            "hostile_form_cycles_and_malformed.pdf",
            "forms_identity_and_topology.pdf",
        ]

        mismatches = []
        for name in fixtures:
            committed = os.path.join(fixtures_dir, name)
            generated = os.path.join(tmpdir, name)
            if not os.path.isfile(committed):
                print(f"Error: Committed fixture missing: {committed}", file=sys.stderr)
                mismatches.append(name)
                continue
            if not os.path.isfile(generated):
                print(f"Error: Generated fixture missing: {generated}", file=sys.stderr)
                mismatches.append(name)
                continue
            if not filecmp.cmp(committed, generated, shallow=False):
                print(f"Error: Mismatch: {name} differs from generator output", file=sys.stderr)
                mismatches.append(name)
            else:
                print(f"  OK (byte-identical): {name}")

        if mismatches:
            print(f"FAILED: {len(mismatches)} fixture(s) drifted from generator output!", file=sys.stderr)
            return 1

        print("All fixtures verified byte-identical to generator output.")
        return 0


if __name__ == "__main__":
    sys.exit(main())
