#!/usr/bin/env python3
"""
check_provenance.py: prove that code copied from pgvector (and PostgreSQL) is unmodified
apart from the ARKIVECTOR-marked deltas listed in PROVENANCE.md / STUBS.md.

    python3 -I daemon/tools/check_provenance.py [--pgvector pgvector/src] [--postgres <pg checkout>]

Checks:
  V  verbatim files are byte-identical to upstream
  F  function-level copies regenerate identically (tools/pgvector_copy.py --check)
  S  copy+shim files equal upstream after removing ARKIVECTOR regions:
       #ifndef ARKIVECTOR ... [#else <ours>] #endif   -> keep the upstream part
       #ifdef  ARKIVECTOR <ours> [#else ...] #endif   -> keep the #else part
     hnswbuild_mem.c: every copied function is byte-identical to hnswbuild.c's
  PostgreSQL verbatim files (only with --postgres)
Exit status 1 on any difference.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PGV = ROOT / "daemon" / "src" / "pgvector"
PGC = ROOT / "daemon" / "src" / "pgcompat"

VERBATIM = {  # local file -> upstream file (pgvector/src)
    "halfutils.c": "halfutils.c", "halfutils.h": "halfutils.h",
    "bitutils.c": "bitutils.c", "bitutils.h": "bitutils.h",
    "halfvec.h": "halfvec.h", "vector.h": "vector.h", "bitvec.h": "bitvec.h", "sparsevec.h": "sparsevec.h",
    "bitvec_kernels.c": "bitvec.c",
}
SHIM_FILES = {"hnsw.h": "hnsw.h", "hnswutils.c": "hnswutils.c"}
HNSWBUILD_FUNCS = ["AddDuplicateInMemory", "FindDuplicateInMemory", "AddElementInMemory",
                   "UpdateNeighborsInMemory", "UpdateGraphInMemory", "InsertTupleInMemory", "InitGraph",
                   "InitAllocator", "HnswSharedMemoryAlloc"]
PG_VERBATIM = {  # local (relative to pgcompat) -> PostgreSQL path
    "src/list.c": "src/backend/nodes/list.c",
    "src/pairingheap.c": "src/backend/lib/pairingheap.c",
    "src/pg_prng.c": "src/common/pg_prng.c",
    "src/pg_bitutils.c": "src/port/pg_bitutils.c",
    "src/f2s.c": "src/common/f2s.c",
    "src/digit_table.h": "src/common/digit_table.h",
    "src/ryu_common.h": "src/common/ryu_common.h",
    "include/nodes/pg_list.h": "src/include/nodes/pg_list.h",
    "include/lib/pairingheap.h": "src/include/lib/pairingheap.h",
    "include/lib/simplehash.h": "src/include/lib/simplehash.h",
    "include/utils/relptr.h": "src/include/utils/relptr.h",
    "include/common/pg_prng.h": "src/include/common/pg_prng.h",
    "include/common/int.h": "src/include/common/int.h",
    "include/common/hashfn.h": "src/include/common/hashfn.h",
    "include/common/shortest_dec.h": "src/include/common/shortest_dec.h",
    "include/port/pg_bitutils.h": "src/include/port/pg_bitutils.h",
}


def strip_arkivector(lines):
    out = []
    stack = []  # entries: (kind, keep_now) kind in {'ndef', 'def', 'other'}
    for line in lines:
        s = line.strip()
        if re.match(r"#\s*ifndef\s+ARKIVECTOR\b", s):
            stack.append(["ndef", True])
            continue
        if re.match(r"#\s*ifdef\s+ARKIVECTOR\b", s):
            stack.append(["def", False])
            continue
        if re.match(r"#\s*if", s):
            if all(k for _, k in stack):
                out.append(line)
            stack.append(["other", True])
            continue
        if re.match(r"#\s*else\b", s) and stack:
            kind = stack[-1][0]
            if kind in ("ndef", "def"):
                stack[-1][1] = kind == "def"
                continue
        if re.match(r"#\s*endif\b", s) and stack:
            kind, _ = stack.pop()
            if kind in ("ndef", "def"):
                continue
            if all(k for _, k in stack):
                out.append(line)
            continue
        if all(k for _, k in stack):
            out.append(line)
    return out


def find_func(lines, name):
    idx = [i for i, l in enumerate(lines) if re.match(r"^" + re.escape(name) + r"\(", l)]
    if len(idx) != 1:
        return None
    i = idx[0]
    start = i - 1
    while start > 0 and lines[start - 1].strip() and not lines[start - 1].startswith("}") \
            and not lines[start - 1].startswith("#"):
        start -= 1
    end = i
    while lines[end] != "}":
        end += 1
    return lines[start:end + 1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pgvector", default=str(ROOT / "pgvector" / "src"))
    ap.add_argument("--postgres", default=None)
    args = ap.parse_args()
    up = Path(args.pgvector)
    bad = 0

    for local, upstream in VERBATIM.items():
        if (PGV / local).read_bytes() != (up / upstream).read_bytes():
            print(f"V   DIFFERS {local} vs pgvector/src/{upstream}")
            bad = 1
        else:
            print(f"V   ok      {local}")

    r = subprocess.run([sys.executable, "-I", str(ROOT / "daemon" / "tools" / "pgvector_copy.py"), "--check",
                        "--src", str(up)], capture_output=True, text=True)
    print("F   " + ("ok      function-level copies" if r.returncode == 0 else "DIFFERS\n" + r.stdout))
    bad |= r.returncode != 0

    for local, upstream in SHIM_FILES.items():
        mine = strip_arkivector((PGV / local).read_text().split("\n"))
        theirs = (up / upstream).read_text().split("\n")
        if mine != theirs:
            import difflib
            print(f"S   DIFFERS {local} (outside ARKIVECTOR regions):")
            for d in list(difflib.unified_diff(theirs, mine, "upstream", local, lineterm="", n=1))[:40]:
                print("    " + d)
            bad = 1
        else:
            print(f"S   ok      {local}")

    mine = (PGV / "hnswbuild_mem.c").read_text().split("\n")
    theirs = (up / "hnswbuild.c").read_text().split("\n")
    for fn in HNSWBUILD_FUNCS:
        a, b = find_func(mine, fn), find_func(theirs, fn)
        if a is None or a != b:
            print(f"S   DIFFERS hnswbuild_mem.c:{fn}")
            bad = 1
    print("S   ok      hnswbuild_mem.c functions" if not bad else "", end="\n" if not bad else "")

    if args.postgres:
        pg = Path(args.postgres)
        vmine = (PGC / "src/varbit_io.c").read_text().split("\n")
        vtheirs = (pg / "src/backend/utils/adt/varbit.c").read_text().split("\n")
        for fn in ["bit_in", "varbit_out", "bit_out"]:
            a, b = find_func(vmine, fn), find_func(vtheirs, fn)
            if a is None or a != b:
                print(f"PG  DIFFERS varbit_io.c:{fn}")
                bad = 1
        for local, upstream in PG_VERBATIM.items():
            if (PGC / local).read_bytes() != (pg / upstream).read_bytes():
                print(f"PG  DIFFERS {local} vs {upstream}")
                bad = 1
            else:
                print(f"PG  ok      {local}")
    sys.exit(bad)


if __name__ == "__main__":
    main()
