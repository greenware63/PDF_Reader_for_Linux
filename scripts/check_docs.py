#!/usr/bin/env python3
"""Documentation checks for docs/*.md and README.md.

Validates that:
1. No line-number citations (*.cpp|h|py:NNN or #LNNN) appear in docs/*.md or README.md.
2. Quoted test titles/symbols paired with source/test files in docs/threat_model.md exist verbatim.
"""

import glob
import os
import re
import sys


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.dirname(script_dir)

    doc_files = sorted(glob.glob(os.path.join(repo_root, 'docs', '*.md')))
    readme_path = os.path.join(repo_root, 'README.md')
    if os.path.exists(readme_path):
        doc_files.append(readme_path)

    errors = 0

    line_pat = re.compile(r'([A-Za-z0-9_./-]+\.(?:cpp|h|py):\d+|#L\d+)')
    for doc_path in doc_files:
        rel_doc = os.path.relpath(doc_path, repo_root)
        with open(doc_path, 'r', encoding='utf-8', errors='ignore') as f:
            for lineno, line in enumerate(f, 1):
                for m in line_pat.finditer(line):
                    print(f"ERROR in {rel_doc}:{lineno}: Line-number citation forbidden: {m.group(0)}")
                    errors += 1

    # Verify `file.(cpp|h|py)` (`name`, ...) citations in docs/threat_model.md
    threat_model_path = os.path.join(repo_root, 'docs', 'threat_model.md')
    if os.path.exists(threat_model_path):
        with open(threat_model_path, 'r', encoding='utf-8') as f:
            tm_content = f.read()

        cmake_path = os.path.join(repo_root, 'CMakeLists.txt')
        cmake_text = ''
        if os.path.exists(cmake_path):
            with open(cmake_path, 'r', encoding='utf-8', errors='ignore') as cf:
                cmake_text = cf.read()

        # Match `file.ext` (`item1`, `item2`, ...)
        cite_group_pat = re.compile(r'`([A-Za-z0-9_./-]+\.(?:cpp|h|py))`\s*\(([^)]+)\)')
        for m in cite_group_pat.finditer(tm_content):
            cited_file = m.group(1)
            names_blob = m.group(2)
            full_path = os.path.normpath(os.path.join(repo_root, cited_file))
            if not os.path.exists(full_path):
                print(f"ERROR in docs/threat_model.md: Cited file not found: {cited_file}")
                errors += 1
                continue
            with open(full_path, 'r', encoding='utf-8', errors='ignore') as sf:
                src_text = sf.read()
            for sym in re.findall(r'`([^`]+)`', names_blob):
                # Strip trailing () for function citations if written as fn()
                lookup = sym[:-2] if sym.endswith('()') else sym
                is_ctest = bool(re.search(rf'add_test\s*\(\s*NAME\s+{re.escape(lookup)}\b', cmake_text))
                if lookup not in src_text and not is_ctest:
                    print(f"ERROR in docs/threat_model.md: Symbol or test \"{sym}\" not found verbatim in {cited_file}")
                    errors += 1

    print(f"Docs check errors: {errors}")
    if errors > 0:
        sys.exit(1)
    print("ALL DOCS CHECKS PASSED!")


if __name__ == '__main__':
    main()
