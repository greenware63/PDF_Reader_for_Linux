#!/usr/bin/env python3
"""Documentation checks for docs/*.md and README.md.

Validates that:
1. No line-number citations (*.ext:NNN or #LNNN) appear in docs/*.md or README.md.
2. Backtick-aware paired citations (`file.(cpp|h|py)` (`sym1`, `sym2`, ...)) in
   docs/threat_model.md exist verbatim in the cited file (or as a CTest name).
3. Shape-based check: every backtick span in docs/*.md matching a test-title shape
   appears verbatim in a test source (test_*.cpp, tests/**/*.cpp, tests/**/*.py)
   or as a CTest name in CMakeLists.txt.
4. Vacuity guard: prints verified citation counts per doc and fails if
   docs/threat_model.md has zero verified citations.
"""

import glob
import os
import re
import sys


TEST_TITLE_SHAPE_RE = re.compile(
    r'^(?:\[TEST\s|TEST\s|Test\s+\d+:|Group\s+\d+\.\d+:|\[PART\s+\d+\])'
)

LINE_CITE_RE = re.compile(
    r'([A-Za-z0-9_./-]+\.(?:cpp|h|hpp|py|txt|md|yml|yaml|sh|cmake):\d+|#L\d+)'
)


def extract_paren_group(text, open_idx):
    """Return content inside (...) starting at open_idx, ignoring parens inside backticks."""
    depth = 0
    in_backtick = False
    i = open_idx
    n = len(text)
    while i < n:
        ch = text[i]
        if ch == '`':
            in_backtick = not in_backtick
        elif not in_backtick:
            if ch == '(':
                depth += 1
            elif ch == ')':
                depth -= 1
                if depth == 0:
                    return text[open_idx + 1:i]
        i += 1
    return None


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.dirname(script_dir)

    doc_files = sorted(glob.glob(os.path.join(repo_root, 'docs', '*.md')))
    readme_path = os.path.join(repo_root, 'README.md')
    all_scan_files = list(doc_files)
    if os.path.exists(readme_path):
        all_scan_files.append(readme_path)

    errors = 0
    verified_counts = {}

    # 1. Line-number citation scan across docs/*.md and README.md
    for doc_path in all_scan_files:
        rel_doc = os.path.relpath(doc_path, repo_root).replace('\\', '/')
        with open(doc_path, 'r', encoding='utf-8', errors='ignore') as f:
            for lineno, line in enumerate(f, 1):
                for m in LINE_CITE_RE.finditer(line):
                    print(f"ERROR in {rel_doc}:{lineno}: Line-number citation forbidden: {m.group(0)}")
                    errors += 1

    # Load CMakeLists.txt for CTest names
    cmake_path = os.path.join(repo_root, 'CMakeLists.txt')
    cmake_text = ''
    if os.path.exists(cmake_path):
        with open(cmake_path, 'r', encoding='utf-8', errors='ignore') as cf:
            cmake_text = cf.read()

    def is_ctest_name(name):
        return bool(re.search(rf'add_test\s*\(\s*NAME\s+{re.escape(name)}\b', cmake_text))

    # 2. Backtick-aware paired citation check in docs/threat_model.md
    threat_model_path = os.path.join(repo_root, 'docs', 'threat_model.md')
    if os.path.exists(threat_model_path):
        rel_tm = 'docs/threat_model.md'
        verified_counts.setdefault(rel_tm, 0)
        with open(threat_model_path, 'r', encoding='utf-8') as f:
            tm_content = f.read()

        file_open_re = re.compile(r'`([A-Za-z0-9_./-]+\.(?:cpp|h|py))`\s*\(')
        for m in file_open_re.finditer(tm_content):
            cited_file = m.group(1)
            open_idx = m.end() - 1
            names_blob = extract_paren_group(tm_content, open_idx)
            if names_blob is None:
                print(f"ERROR in {rel_tm}: Unclosed parenthesis after `{cited_file}`")
                errors += 1
                continue

            full_path = os.path.normpath(os.path.join(repo_root, cited_file))
            if not os.path.exists(full_path):
                print(f"ERROR in {rel_tm}: Cited file not found: {cited_file}")
                errors += 1
                continue

            with open(full_path, 'r', encoding='utf-8', errors='ignore') as sf:
                src_text = sf.read()

            for sym in re.findall(r'`([^`]+)`', names_blob):
                lookup = sym[:-2] if sym.endswith('()') else sym
                if lookup not in src_text and not is_ctest_name(lookup):
                    print(f"ERROR in {rel_tm}: Symbol or test \"{sym}\" not found verbatim in {cited_file}")
                    errors += 1
                else:
                    verified_counts[rel_tm] += 1

    # 3. Shape-based test-title check across docs/*.md
    test_sources = []
    for pattern in [
        os.path.join(repo_root, 'test_*.cpp'),
        os.path.join(repo_root, 'tests', '**', '*.cpp'),
        os.path.join(repo_root, 'tests', '**', '*.py'),
    ]:
        test_sources.extend(glob.glob(pattern, recursive=True))

    test_corpus = []
    for ts in sorted(set(test_sources)):
        with open(ts, 'r', encoding='utf-8', errors='ignore') as tf:
            test_corpus.append(tf.read())
    combined_test_text = '\n'.join(test_corpus)

    fenced_block_re = re.compile(r'^```.*?^```', re.MULTILINE | re.DOTALL)
    inline_span_re = re.compile(r'(?<!`)`([^`\n]+)`(?!`)')

    for doc_path in doc_files:
        rel_doc = os.path.relpath(doc_path, repo_root).replace('\\', '/')
        verified_counts.setdefault(rel_doc, 0)
        with open(doc_path, 'r', encoding='utf-8', errors='ignore') as f:
            doc_text = fenced_block_re.sub('', f.read())
        for span in inline_span_re.findall(doc_text):
            if TEST_TITLE_SHAPE_RE.match(span):
                if span not in combined_test_text and not is_ctest_name(span):
                    print(f"ERROR in {rel_doc}: Test title \"{span}\" not found verbatim in any test source")
                    errors += 1
                else:
                    verified_counts[rel_doc] += 1

    # 4. Vacuity guard & per-doc summary
    print("Verified citation counts per doc:")
    for doc_name in sorted(verified_counts):
        print(f"  {doc_name}: {verified_counts[doc_name]}")

    if verified_counts.get('docs/threat_model.md', 0) == 0:
        print("ERROR: Vacuity guard failed — 0 verified citations in docs/threat_model.md")
        errors += 1

    print(f"Docs check errors: {errors}")
    if errors > 0:
        sys.exit(1)
    print("ALL DOCS CHECKS PASSED!")


if __name__ == '__main__':
    main()
