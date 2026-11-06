#!/usr/bin/env python3
"""
pgvector_copy.py: regenerate the function-level (mode F) copies in daemon/src/pgvector
from an upstream pgvector source tree, byte-for-byte per item.

    python3 -I daemon/tools/pgvector_copy.py [--src pgvector/src] [--check]

Each destination file = a provenance header + include lines + upstream items, in
manifest order. An item is either
    ("func", NAME)                       a top-level function definition, with the
                                         comment / PG_FUNCTION_INFO_V1 lines directly above it
    ("block", FILE_LINE_REGEX, NLINES)   NLINES upstream lines starting at the first line
                                         that matches (for macro blocks)
Item text is copied verbatim. --check exits 1 if any destination differs from what
would be generated (used by CI to prove the copies are unmodified).
"""
import argparse
import re
import sys
from pathlib import Path

TAG = "v0.8.7"
COMMIT = "f37c13f68b57d2c3472b2214fbcff699d6d34876"

COMMON_TAIL = ["#if PG_VERSION_NUM >= 160000", '#include "varatt.h"', "#endif", ""]

MANIFEST = {
    "vector_io.c": {
        "upstream": "vector.c",
        "includes": ["postgres.h", "<math.h>", "fmgr.h", "port.h", "common/shortest_dec.h",
                     "parser/scansup.h", "utils/float.h", "vector.h"],
        "items": [
            ("func", "CheckExpectedDim"), ("func", "CheckDim"), ("func", "CheckElement"),
            ("func", "InitVector"),
            ("block", r"^#if PG_VERSION_NUM >= 170000$", 16, r"^#define vector_isspace"),
            ("func", "vector_in"),
            ("block", r"^#define AppendChar", 2, None),
            ("func", "vector_out"),
        ],
    },
    "vector_kernels.c": {
        "upstream": "vector.c",
        "tiered": True,
        "includes": ["postgres.h", "<math.h>", "fmgr.h", "halfvec.h", "vector.h"],
        "items": [
            ("block", r"^#if defined\(USE_TARGET_CLONES\) && !defined\(__FMA__\)$", 5, None),
            ("func", "CheckDims"),
            ("func", "VectorL2SquaredDistance"), ("func", "l2_distance"), ("func", "vector_l2_squared_distance"),
            ("func", "VectorInnerProduct"), ("func", "inner_product"), ("func", "vector_negative_inner_product"),
            ("func", "VectorCosineSimilarity"), ("func", "cosine_distance"),
            ("func", "VectorL1Distance"), ("func", "l1_distance"),
        ],
    },
    "vector_funcs.c": {
        "upstream": "vector.c",
        "includes": ["utils/float.h", "postgres.h", "<math.h>", "bitvec.h", "fmgr.h", "halfutils.h", "halfvec.h",
                     "utils/varbit.h", "vector.h"],
        "items": [
            ("func", "CheckExpectedDim"), ("func", "CheckDim"),
            ("func", "vector_dims"), ("func", "vector_norm"), ("func", "l2_normalize"),
            ("func", "binary_quantize"), ("func", "subvector"), ("func", "halfvec_to_vector"),
        ],
    },
    "halfvec_io.c": {
        "upstream": "halfvec.c",
        "includes": ["postgres.h", "<math.h>", "common/shortest_dec.h", "fmgr.h", "halfutils.h", "halfvec.h",
                     "port.h", "parser/scansup.h", "utils/float.h", "vector.h"],
        "items": [
            ("func", "CheckExpectedDim"), ("func", "CheckDim"), ("func", "CheckElement"),
            ("func", "InitHalfVector"),
            ("block", r"^#if PG_VERSION_NUM >= 170000$", 16, r"^#define halfvec_isspace"),
            ("func", "halfvec_in"),
            ("block", r"^#define AppendChar", 2, None),
            ("func", "halfvec_out"),
        ],
    },
    "halfvec_kernels.c": {
        "upstream": "halfvec.c",
        "tiered": True,
        "includes": ["postgres.h", "<math.h>", "fmgr.h", "halfutils.h", "halfvec.h", "vector.h"],
        "items": [
            ("func", "CheckDims"),
            ("func", "halfvec_l2_distance"), ("func", "halfvec_l2_squared_distance"),
            ("func", "halfvec_inner_product"), ("func", "halfvec_negative_inner_product"),
            ("func", "halfvec_cosine_distance"), ("func", "halfvec_l1_distance"),
        ],
    },
    "halfvec_funcs.c": {
        "upstream": "halfvec.c",
        "includes": ["utils/float.h", "postgres.h", "<math.h>", "bitvec.h", "fmgr.h", "halfutils.h", "halfvec.h",
                     "utils/varbit.h", "vector.h"],
        "items": [
            ("func", "CheckExpectedDim"), ("func", "CheckDim"), ("func", "CheckElement"),
            ("func", "vector_to_halfvec"), ("func", "halfvec_vector_dims"), ("func", "halfvec_l2_norm"),
            ("func", "halfvec_l2_normalize"), ("func", "halfvec_binary_quantize"), ("func", "halfvec_subvector"),
        ],
    },
    "sparsevec_io.c": {
        "upstream": "sparsevec.c",
        "includes": ["postgres.h", "<limits.h>", "<math.h>", "common/shortest_dec.h", "fmgr.h", "port.h",
                     "parser/scansup.h", "sparsevec.h", "utils/builtins.h", "utils/float.h", "vector.h"],
        "items": [
            ("block", r"^#if PG_VERSION_NUM >= 190000$", 7, r"^#define palloc_array_checked"),
            ("block", r"^typedef struct SparseInputElement$", 5, None),
            ("func", "CheckExpectedDim"), ("func", "CheckDim"), ("func", "CheckNnz"), ("func", "CheckIndex"),
            ("func", "CheckElement"), ("func", "InitSparseVector"),
            ("func", "AppendElement"), ("func", "FinishAppend"),
            ("block", r"^#if PG_VERSION_NUM >= 170000$", 16, r"^#define sparsevec_isspace"),
            ("func", "CompareIndices"),
            ("func", "sparsevec_in"),
            ("block", r"^#define AppendChar", 3, None),
            ("func", "sparsevec_out"),
        ],
    },
    "sparsevec_kernels.c": {
        "upstream": "sparsevec.c",
        "tiered": True,
        "includes": ["postgres.h", "<math.h>", "fmgr.h", "sparsevec.h", "vector.h"],
        "items": [
            ("func", "CheckDims"),
            ("func", "SparsevecL2SquaredDistance"), ("func", "sparsevec_l2_distance"),
            ("func", "sparsevec_l2_squared_distance"),
            ("func", "SparsevecInnerProduct"), ("func", "sparsevec_inner_product"),
            ("func", "sparsevec_negative_inner_product"),
            ("func", "sparsevec_cosine_distance"), ("func", "sparsevec_l1_distance"),
        ],
    },
    "sparsevec_funcs.c": {
        "upstream": "sparsevec.c",
        "includes": ["utils/float.h", "postgres.h", "<math.h>", "fmgr.h", "sparsevec.h", "vector.h"],
        "items": [
            ("func", "CheckElement"), ("func", "AppendElement"), ("func", "FinishAppend"),
            ("func", "sparsevec_vector_dims"), ("func", "sparsevec_l2_norm"), ("func", "sparsevec_l2_normalize"),
        ],
    },
}


def find_func(lines, name, src):
    idx = [i for i, l in enumerate(lines) if re.match(r"^" + re.escape(name) + r"\(", l)]
    if len(idx) != 1:
        sys.exit(f"{src}: function {name}: found {len(idx)} definitions")
    i = idx[0]
    start = i - 1
    while start > 0 and lines[start - 1].strip() and not lines[start - 1].startswith("}") \
            and not lines[start - 1].startswith("#"):
        start -= 1
    end = i
    while lines[end] != "}":
        end += 1
    return lines[start:end + 1]


def find_block(lines, regex, n, anchor, src):
    for i, l in enumerate(lines):
        if re.match(regex, l) and (anchor is None or re.match(anchor, lines[i + 1])):
            return lines[i:i + n]
    sys.exit(f"{src}: block {regex!r} not found")


def generate(src_dir: Path, name: str, spec: dict) -> str:
    src = spec["upstream"]
    lines = (src_dir / src).read_text().split("\n")
    funcs = [it[1] for it in spec["items"] if it[0] == "func"]
    out = [
        "/*",
        f" * Copied from pgvector {TAG} src/{src} (commit {COMMIT[:7]}), mode F.",
        " * Generated by daemon/tools/pgvector_copy.py: function bodies are byte-identical",
        " * to upstream. Functions: " + ", ".join(funcs) + ".",
    ]
    if spec.get("tiered"):
        out.append(" * Compiled once per x86-64 kernel tier with -include av_tier.h (symbol renaming).")
    out += [" * Do not edit; regenerate. See PROVENANCE.md.", " */"]
    for inc in spec["includes"]:
        out.append(f"#include {inc}" if inc.startswith("<") else f'#include "{inc}"')
    out.append("")
    out += COMMON_TAIL
    for item in spec["items"]:
        if item[0] == "func":
            out += find_func(lines, item[1], src)
        else:
            out += find_block(lines, item[1], item[2], item[3], src)
        out.append("")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    root = Path(__file__).resolve().parents[2]
    ap.add_argument("--src", default=str(root / "pgvector" / "src"))
    ap.add_argument("--dst", default=str(root / "daemon" / "src" / "pgvector"))
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    bad = 0
    for name, spec in MANIFEST.items():
        text = generate(Path(args.src), name, spec)
        dst = Path(args.dst) / name
        if args.check:
            if not dst.exists() or dst.read_text() != text:
                print(f"DIFFERS: {dst}")
                bad = 1
        else:
            dst.write_text(text)
            print(f"wrote {dst}")
    sys.exit(bad)


if __name__ == "__main__":
    main()
