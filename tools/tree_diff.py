"""Compare two extracted folder trees; print differences like `diff -rq`.

    python tree_diff.py <before> <after>      exit code 0 = identical, 1 = differences
"""
import filecmp
import os
import sys


def walk(a: str, b: str, rel: str = "") -> list[str]:
    out = []
    cmp = filecmp.dircmp(os.path.join(a, rel), os.path.join(b, rel))
    for name in cmp.left_only:
        out.append(f"Only in before: {os.path.join(rel, name)}")
    for name in cmp.right_only:
        out.append(f"Only in after: {os.path.join(rel, name)}")
    for name in cmp.common_files:
        pa, pb = os.path.join(a, rel, name), os.path.join(b, rel, name)
        if not filecmp.cmp(pa, pb, shallow=False):
            out.append(f"Files differ: {os.path.join(rel, name)}")
    for name in cmp.common_dirs:
        out += walk(a, b, os.path.join(rel, name))
    return out


if __name__ == "__main__":
    diffs = walk(sys.argv[1], sys.argv[2])
    for d in diffs:
        print(d)
    sys.exit(1 if diffs else 0)
