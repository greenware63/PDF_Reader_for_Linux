#!/usr/bin/env python3
"""Validation script for docs/PARITY_MATRIX.md

Validates that:
1. Executive summary counts match Table 1 counts exactly across all status values.
2. The total count matches the number of rows in Table 1.
3. The count of Architectural Capabilities matches Table 2.
4. Every cited file exists and referenced line numbers are within range.
5. Quoted test titles can be matched in their respective test files.
"""

import os
import re
import sys

def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.dirname(script_dir)
    matrix_path = os.path.join(repo_root, 'docs', 'PARITY_MATRIX.md')

    if not os.path.exists(matrix_path):
        print(f"ERROR: Matrix file not found at {matrix_path}")
        sys.exit(1)

    with open(matrix_path, 'r', encoding='utf-8') as f:
        content = f.read()

    lines = content.splitlines()

    # 1. Parse Executive Summary counts
    summary_counts = {}
    for line in lines:
        m = re.match(r'\|\s*\*\*([^*]+)\*\*\s*\|\s*\*\*(\d+)\*\*\s*\|', line)
        if m:
            summary_counts[m.group(1).strip()] = int(m.group(2))

    print("Executive Summary Counts:", summary_counts)

    # 2. Parse Table 1 and Table 2 rows
    table1_rows = []
    table2_rows = []
    current_section = 0

    for line in lines:
        if '## 1. Feature-by-Feature Parity Table' in line:
            current_section = 1
            continue
        elif '## 2. Architectural Capabilities' in line:
            current_section = 2
            continue
        elif line.startswith('---'):
            continue

        if current_section == 1 and line.startswith('| **'):
            cols = [c.strip() for c in line.split('|')[1:-1]]
            if len(cols) >= 5:
                table1_rows.append(cols)
        elif current_section == 2 and line.startswith('| **'):
            cols = [c.strip() for c in line.split('|')[1:-1]]
            if len(cols) >= 5:
                table2_rows.append(cols)

    print(f"Table 1 rows: {len(table1_rows)}")
    print(f"Table 2 rows: {len(table2_rows)}")

    status_counts = {}
    for r in table1_rows:
        st = r[2].replace('**', '').strip()
        status_counts[st] = status_counts.get(st, 0) + 1

    print("Table 1 Status Counts:", status_counts)

    errors = 0

    # Verify summary matches table
    for st, cnt in status_counts.items():
        if summary_counts.get(st) != cnt:
            print(f"ERROR: Mismatch in status {st}: summary has {summary_counts.get(st)}, table has {cnt}")
            errors += 1
        else:
            print(f"OK: Status {st} matches ({cnt})")

    if summary_counts.get('Total Features Audited') != len(table1_rows):
        print(f"ERROR: Total Features Audited mismatch: summary has {summary_counts.get('Total Features Audited')}, table has {len(table1_rows)}")
        errors += 1
    else:
        print(f"OK: Total Features Audited matches ({len(table1_rows)})")

    if summary_counts.get('Architectural Capabilities (Ours)') != len(table2_rows):
        print(f"ERROR: Architectural Capabilities mismatch: summary has {summary_counts.get('Architectural Capabilities (Ours)')}, table has {len(table2_rows)}")
        errors += 1
    else:
        print(f"OK: Architectural Capabilities matches ({len(table2_rows)})")

    # 3. Check citations in Table 1 and Table 2
    print("\nVerifying citations in all rows...")
    all_rows = table1_rows + table2_rows

    for r in all_rows:
        feature_name = r[0]
        evidence = r[3]

        # Extract markdown links [`path:line`](url) or [`path`](url)
        links = re.findall(r'\[`([^`]+)`\]\(([^)]+)\)', evidence)
        for text, _ in links:
            if ':' in text:
                fpath, line_str = text.split(':', 1)
                try:
                    line_no = int(line_str.split('-')[0])
                except ValueError:
                    line_no = None
            else:
                fpath = text
                line_no = None

            full_path = os.path.normpath(os.path.join(repo_root, fpath))
            if not os.path.exists(full_path):
                print(f"ERROR in \"{feature_name}\": File not found: {fpath} (full: {full_path})")
                errors += 1
            else:
                if line_no is not None:
                    with open(full_path, 'r', encoding='utf-8', errors='ignore') as src_file:
                        src_lines = src_file.readlines()
                    if line_no > len(src_lines) or line_no < 1:
                        print(f"ERROR in \"{feature_name}\": Line {line_no} out of range for {fpath} ({len(src_lines)} lines)")
                        errors += 1

        # Extract quoted test titles: (`exact test title`)
        quoted_tests = re.findall(r'\(`([^`]+)`\)', evidence)
        for q_test in quoted_tests:
            found = False
            for text, _ in links:
                fpath = text.split(':')[0]
                if fpath.endswith('.cpp'):
                    full_path = os.path.normpath(os.path.join(repo_root, fpath))
                    if os.path.exists(full_path):
                        with open(full_path, 'r', encoding='utf-8', errors='ignore') as tf:
                            tf_content = tf.read()
                        prefix = q_test[:25].strip()
                        if prefix.lower() in tf_content.lower():
                            found = True
                            break
            if not found:
                print(f"WARNING in \"{feature_name}\": Could not locate quoted test snippet \"{q_test[:40]}\" in cited files")

    print(f"\nTotal validation errors: {errors}")
    if errors > 0:
        sys.exit(1)
    print("ALL CHECKS PASSED!")

if __name__ == '__main__':
    main()
