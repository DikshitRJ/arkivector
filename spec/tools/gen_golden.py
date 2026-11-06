#!/usr/bin/env python3
"""Generate the Arkivector contract golden files (stdlib only, deterministic).

Outputs:
  spec/golden/vectorbytes.json   - VectorBytes v1 encode/decode cases (valid + invalid)
  spec/fixtures/demo-dataset.json - shared demo collection + queries with expected exact results

Run from repo root:  python3 -I spec/tools/gen_golden.py
Never edit the outputs by hand. Changing them is a contract change (see spec/changes/README.md).
"""
import base64, hashlib, json, math, struct, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
T_VECTOR, T_HALFVEC, T_BIT, T_SPARSEVEC = 1, 2, 3, 4
VERSION = 1

def hdr(t, flags=0, version=VERSION):
    return b"AV" + bytes([(version << 4) | t, flags])

def half_round(x):
    return struct.unpack("<e", struct.pack("<e", x))[0]

def enc_vector(vals, normalized=False):
    return hdr(T_VECTOR, 1 if normalized else 0) + struct.pack("<hh", len(vals), 0) + struct.pack(f"<{len(vals)}f", *vals)

def enc_halfvec(vals, normalized=False):
    return hdr(T_HALFVEC, 1 if normalized else 0) + struct.pack("<hh", len(vals), 0) + struct.pack(f"<{len(vals)}e", *vals)

def enc_bit(bits):
    n = len(bits)
    data = bytearray((n + 7) // 8)
    for i, c in enumerate(bits):
        if c == "1":
            data[i // 8] |= 0x80 >> (i % 8)          # MSB-first, PostgreSQL VarBit order
    return hdr(T_BIT) + struct.pack("<i", n) + bytes(data)

def enc_sparse(dim, indices, values):
    nnz = len(indices)
    return (hdr(T_SPARSEVEC) + struct.pack("<iii", dim, nnz, 0)
            + struct.pack(f"<{nnz}i", *indices) + struct.pack(f"<{nnz}f", *values))

def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]

def case(name, blob, **decoded):
    return {"name": name, "valid": True, "hex": blob.hex(), "base64": base64.b64encode(blob).decode(),
            "length": len(blob), **decoded}

def bad(name, blob, error):
    return {"name": name, "valid": False, "hex": blob.hex(), "base64": base64.b64encode(blob).decode(),
            "length": len(blob), "error": error}

def vectorbytes_cases():
    c = []
    v = [1.0, 2.0, 3.0]
    c.append(case("vector_dim3", enc_vector(v), type="vector", dim=3, normalized=False, values=v))
    v = [0.5, 0.5, 0.5, 0.5]
    c.append(case("vector_dim4_normalized_flag", enc_vector(v, True), type="vector", dim=4, normalized=True, values=v))
    c.append(case("vector_dim1_zero", enc_vector([0.0]), type="vector", dim=1, normalized=False, values=[0.0]))
    v = [f32(0.1), f32(-0.2), f32(1e-3)]
    c.append(case("vector_float32_rounding", enc_vector(v), type="vector", dim=3, normalized=False, values=v,
                  note="values are the float32 roundings of 0.1, -0.2, 0.001"))
    v = [1.5, -2.0, 0.25]
    c.append(case("halfvec_dim3", enc_halfvec(v), type="halfvec", dim=3, normalized=False, values=v))
    raw = [0.1, -0.333, 2.5, 100.0, -0.0001, 0.75, 65504.0, -1.0]
    v = [half_round(x) for x in raw]
    c.append(case("halfvec_dim8_rounding", enc_halfvec(v), type="halfvec", dim=8, normalized=False, values=v,
                  input_values=raw, note="encoders must round float input to nearest binary16 (round-half-even)"))
    c.append(case("bit_10", enc_bit("1011000011"), type="bit", dim=10, bits="1011000011"))
    c.append(case("bit_8", enc_bit("11111111"), type="bit", dim=8, bits="11111111"))
    c.append(case("sparsevec_dim10", enc_sparse(10, [0, 3, 9], [1.5, -2.0, 0.5]), type="sparsevec", dim=10,
                  indices=[0, 3, 9], values=[1.5, -2.0, 0.5], text="{1:1.5,4:-2,10:0.5}/10",
                  note="indices are 0-based on the wire; pgvector text format is 1-based"))
    c.append(case("sparsevec_empty", enc_sparse(5, [], []), type="sparsevec", dim=5, indices=[], values=[]))

    good = enc_vector([1.0, 2.0, 3.0])
    c.append(bad("too_short", good[:6], "VB_TOO_SHORT"))
    c.append(bad("bad_magic", b"AX" + good[2:], "VB_BAD_MAGIC"))
    c.append(bad("bad_version", b"AV" + bytes([0x21, 0]) + good[4:], "VB_BAD_VERSION"))
    c.append(bad("unknown_type", b"AV" + bytes([0x15, 0]) + good[4:], "VB_UNKNOWN_TYPE"))
    c.append(bad("reserved_flag_bits", b"AV" + bytes([0x11, 0x02]) + good[4:], "VB_BAD_FLAGS"))
    c.append(bad("unused_nonzero", good[:6] + struct.pack("<h", 1) + good[8:], "VB_UNUSED_NONZERO"))
    c.append(bad("dim_zero", hdr(T_VECTOR) + struct.pack("<hh", 0, 0), "VB_DIM_OUT_OF_RANGE"))
    c.append(bad("dim_too_large", hdr(T_VECTOR) + struct.pack("<hh", 16001, 0), "VB_DIM_OUT_OF_RANGE"))
    c.append(bad("length_short_payload", good[:-4], "VB_LENGTH_MISMATCH"))
    c.append(bad("length_trailing_bytes", good + b"\x00", "VB_LENGTH_MISMATCH"))
    c.append(bad("non_finite_nan", enc_vector([1.0, float("nan"), 3.0]), "VB_NON_FINITE"))
    c.append(bad("non_finite_inf_half", hdr(T_HALFVEC) + struct.pack("<hh", 1, 0) + b"\x00\x7c", "VB_NON_FINITE"))
    c.append(bad("bit_dim_too_large", hdr(T_BIT) + struct.pack("<i", 64001), "VB_DIM_OUT_OF_RANGE"))
    c.append(bad("bit_padding_nonzero", hdr(T_BIT) + struct.pack("<i", 10) + bytes([0xB0, 0xFF]), "VB_BIT_PADDING"))
    c.append(bad("sparse_unsorted", enc_sparse(10, [3, 1], [1.0, 2.0]), "VB_SPARSE_UNSORTED"))
    c.append(bad("sparse_duplicate_index", enc_sparse(10, [3, 3], [1.0, 2.0]), "VB_SPARSE_UNSORTED"))
    c.append(bad("sparse_index_out_of_range", enc_sparse(10, [0, 10], [1.0, 2.0]), "VB_SPARSE_INDEX_OUT_OF_RANGE"))
    c.append(bad("sparse_zero_value", enc_sparse(10, [0, 4], [1.0, 0.0]), "VB_SPARSE_ZERO_VALUE"))
    c.append(bad("sparse_nnz_gt_dim", hdr(T_SPARSEVEC) + struct.pack("<iii", 1, 2, 0), "VB_NNZ_OUT_OF_RANGE"))
    return c

# ---------------- demo dataset ----------------
class LCG:
    def __init__(self, seed): self.s = seed & 0xFFFFFFFF
    def next(self):
        self.s = (1664525 * self.s + 1013904223) & 0xFFFFFFFF
        return self.s / 2**32

CATS = ["finance", "news", "science", "sports"]
DIM, N = 8, 200

def l2norm(v):
    n = math.sqrt(sum(x * x for x in v))
    return [x / n for x in v]

def cosdist(a, b):
    a, b = l2norm(a), l2norm(b)
    return 1.0 - sum(x * y for x, y in zip(a, b))

def build_dataset(seed):
    r = LCG(seed)
    ents = []
    for i in range(N):
        raw = [r.next() * 2 - 1 for _ in range(DIM)]
        vec = [half_round(x) for x in raw]
        if all(x == 0 for x in vec):
            vec[0] = 0.5
        cat = CATS[int(r.next() * len(CATS))]
        score_cents = int(r.next() * 501)
        published = r.next() < 0.6
        rank = int(r.next() * 1000) - 500
        eid = f"doc-{i:03d}"
        key = "0x" + hashlib.sha256(eid.encode()).hexdigest()
        ents.append({
            "id": eid, "key": key, "vector": vec,
            "vector_bytes": base64.b64encode(enc_halfvec(vec)).decode(),
            "document": f"Demo document {i} about {cat}.",
            "attributes": {
                "category": {"type": "str", "value": cat},
                "score": {"type": "dec", "value": f"{score_cents // 100}.{score_cents % 100:02d}"},
                "published": {"type": "bool", "value": published},
                "rank": {"type": "i32", "value": rank},
            },
            "owner": "0x1111111111111111111111111111111111111111",
            "creator": "0x1111111111111111111111111111111111111111",
            "expires_at_block": 10_000_000,
        })
    return ents

def pred(ent, f):
    a = ent["attributes"]
    if f is None: return True
    if f == "finance": return a["category"]["value"] == "finance"
    if f == "hi_pub":
        return float(a["score"]["value"]) >= 4.0 and a["published"]["value"] is True
    if f == "rank_range": return -100 <= a["rank"]["value"] <= 100
    raise ValueError(f)

QUERIES = [
    {"name": "unfiltered_k5", "k": 5, "f": None, "request_extra": {}},
    {"name": "json_filter_finance_k5", "k": 5, "f": "finance",
     "request_extra": {"filter": {"eq": {"category": "finance"}}}},
    {"name": "where_score_published_k3", "k": 3, "f": "hi_pub",
     "request_extra": {"where": "score >= dec(4.0) AND published = true"}},
    {"name": "json_filter_between_k4", "k": 4, "f": "rank_range",
     "request_extra": {"filter": {"between": {"rank": [{"type": "i32", "value": -100}, {"type": "i32", "value": 100}]}}}},
]

def try_seed(seed):
    ents = build_dataset(seed)
    r = LCG(seed ^ 0xA5A5A5A5)
    out = []
    for q in QUERIES:
        qv = [half_round(r.next() * 2 - 1) for _ in range(DIM)]
        cand = [e for e in ents if pred(e, q["f"])]
        scored = sorted(((cosdist(qv, e["vector"]), e["id"]) for e in cand))
        k = q["k"]
        if len(scored) <= k: return None
        top = scored[: k + 1]
        gaps = [top[i + 1][0] - top[i][0] for i in range(k)]
        if min(gaps) < 5e-3: return None
        out.append({
            "name": q["name"],
            "request": {"vector": qv, "k": k, "exact": True, "include": ["attributes"], **q["request_extra"]},
            "expected_ids": [i for _, i in top[:k]],
            "expected_distances": [round(d, 6) for d, _ in top[:k]],
            "distance_tolerance": 2e-3,
            "expected_count_matching_filter": len(cand),
        })
    return ents, out

def main():
    vb = {"spec_version": (ROOT / "spec/VERSION").read_text().strip(), "format_version": VERSION,
          "types": {"vector": T_VECTOR, "halfvec": T_HALFVEC, "bit": T_BIT, "sparsevec": T_SPARSEVEC},
          "cases": vectorbytes_cases()}
    (ROOT / "spec/golden/vectorbytes.json").write_text(json.dumps(vb, indent=2) + "\n")

    for seed in range(1, 10_000):
        res = try_seed(seed)
        if res:
            ents, qs = res
            break
    else:
        sys.exit("no seed with clean gaps")
    ds = {
        "spec_version": vb["spec_version"], "seed": seed,
        "collection": {
            "name": "demo.docs", "schema_tag": "arkivector.v1.demo.docs",
            "column": {"type": "halfvec", "dimensions": DIM},
            "index": {"method": "hnsw", "opclass": "halfvec_cosine_ops", "with": {"m": 16, "ef_construction": 64}},
            "writers": ["0x1111111111111111111111111111111111111111"], "membership": "creator",
            "attributes": {"category": "str", "score": "dec", "published": "bool", "rank": "i32"},
        },
        "entities": ents, "queries": qs,
        "notes": ["Distances are cosine distances computed in double precision on binary16-rounded inputs.",
                  "Daemon results with exact=true must match expected_ids in order and distances within distance_tolerance."],
    }
    (ROOT / "spec/fixtures/demo-dataset.json").write_text(json.dumps(ds, indent=1) + "\n")
    print(f"wrote {len(vb['cases'])} vectorbytes cases; demo dataset seed={seed}, {len(ents)} entities, {len(qs)} queries")

if __name__ == "__main__":
    main()
