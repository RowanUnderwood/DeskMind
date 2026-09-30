"""Check that a folder in the Tandy C: image has only plain 8.3 entries (no long-name entries).

    python check_83.py <image> <FOLDER>      exit code 0 = clean
"""
import struct
import sys

img, folder = sys.argv[1], sys.argv[2].upper()
base = 32256                                   # FAT16 partition at sector 63
f = open(img, "rb")
f.seek(base)
bs = f.read(512)
bps, spc, res, nfats, rootn, _, _, spf = struct.unpack_from("<HBHBHHBH", bs, 11)
root_start = base + (res + nfats * spf) * bps
data_start = root_start + rootn * 32


def read_entries(off, n):
    f.seek(off)
    raw = f.read(n * 32)
    return [raw[i:i + 32] for i in range(0, len(raw), 32)]


bad = 0
root = read_entries(root_start, rootn)
cluster = None
for i, e in enumerate(root):
    if e[0] == 0:
        break
    if e[0:11] == folder.ljust(11).encode("cp437") and e[11] & 0x10:
        cluster = struct.unpack_from("<H", e, 26)[0]
        if i and root[i - 1][11] == 0x0F and root[i - 1][0] != 0xE5:
            print(f"long-name entry in front of {folder}")
            bad += 1
if cluster is None:
    sys.exit(f"{folder} not found in the root directory")

for e in read_entries(data_start + (cluster - 2) * spc * bps, spc * bps // 32):
    if e[0] == 0:
        break
    if e[0] != 0xE5 and e[11] == 0x0F:
        bad += 1
print(f"{folder}: {bad} long-name entries")
sys.exit(1 if bad else 0)
