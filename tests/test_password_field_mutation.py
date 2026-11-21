#!/usr/bin/env python3
"""T20-A mutation test: prove the password regression fails when the guard is removed.

The script temporarily removes only the Password-field guard from PdfiumPage.cpp,
runs the hostile form suite, expects that suite to fail, restores the source, and
rebuilds test_hostile_forms.  It leaves no alternate implementation behind.
"""

import subprocess
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} SOURCE_ROOT BUILD_DIR", file=sys.stderr)
        return 2

    source_root = Path(sys.argv[1]).resolve()
    build_dir = Path(sys.argv[2]).resolve()
    source = source_root / "src" / "engine" / "PdfiumPage.cpp"
    original = source.read_text(encoding="utf-8")

    guarded = """    if (flags & FormFieldFlag::Password) {
        FPDFPage_CloseAnnot(annot);
        return false;
    }
"""
    if guarded not in original:
        print("T20-A mutation setup failed: Password guard not found", file=sys.stderr)
        return 2

    mutated = original.replace(guarded, "", 1)
    try:
        source.write_text(mutated, encoding="utf-8")
        build = subprocess.run(
            ["cmake", "--build", str(build_dir), "--target", "test_hostile_forms"],
            text=True,
        )
        if build.returncode != 0:
            print("T20-A mutation build failed", file=sys.stderr)
            return 1

        mutant_run = subprocess.run(
            ["ctest", "--test-dir", str(build_dir), "-R", "^HostileFormTestSuite$", "--output-on-failure"],
            text=True,
        )
        if mutant_run.returncode == 0:
            print("MUTATION ESCAPED: password regression passed with Password guard removed",
                  file=sys.stderr)
            return 1
        print("T20-A mutation caught: hostile form suite failed with guard removed")
        return 0
    finally:
        source.write_text(original, encoding="utf-8")
        subprocess.run(
            ["cmake", "--build", str(build_dir), "--target", "test_hostile_forms"],
            text=True,
        )


if __name__ == "__main__":
    raise SystemExit(main())
