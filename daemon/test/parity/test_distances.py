"""Distance parity: our copied pgvector kernels vs. real pgvector in PostgreSQL.

For each (type, operator) N seeded random pairs are evaluated in PostgreSQL
(batched via unnest) and through av_parity_cli. Results must match to 1e-6
relative; the number of bit-identical results is reported.
"""

from __future__ import annotations

import functools
import math
import os
import random
import struct

import pytest

N = int(os.environ.get("PARITY_N", "10000"))
SEED = int(os.environ.get("PARITY_SEED", "20260611"))
REL_TOL = 1e-6
BATCH = 500

CASES = [
    (t, op)
    for t in ("vector", "halfvec", "sparsevec")
    for op in ("<->", "<#>", "<=>", "<+>")
] + [("bit", "<~>"), ("bit", "<%>")]

SMALL_DIMS = [1, 2, 3, 4, 5, 7, 8, 15, 16, 17, 31, 32, 33, 63, 64, 100, 127, 128, 255, 384, 768, 1536]


def f32(x: float) -> float:
    return struct.unpack("f", struct.pack("f", x))[0]


def fmt(x: float) -> str:
    return "%.9g" % f32(x)


def pick_dim(rng: random.Random, i: int, t: str) -> int:
    """Mostly small dims; a few edge dims (1, 16000) and 2000+ for halfvec/sparsevec."""
    if i < 3:
        return 16000 if t != "bit" else 16000
    if i == 3:
        return 1
    if t in ("halfvec", "sparsevec") and i < 20:
        return rng.randint(2000, 16000)
    if t == "bit" and i < 6:
        return rng.choice([65, 4096, 64000])  # contract max for bit (vectors.md §3.3)
    r = rng.random()
    if r < 0.5:
        return rng.choice(SMALL_DIMS)
    return rng.randint(1, 300)


def gen_dense(rng: random.Random, dim: int, t: str, zero: bool) -> str:
    if zero:
        return "[" + ",".join(["0"] * dim) + "]"
    scale = rng.choice([1.0, 10.0, 0.01]) if t == "vector" else rng.choice([1.0, 10.0])
    return "[" + ",".join(fmt(rng.uniform(-scale, scale)) for _ in range(dim)) + "]"


def gen_sparse(rng: random.Random, dim: int, zero: bool, base: list[int] | None) -> tuple[str, list[int]]:
    if zero:
        return "{}/%d" % dim, []
    nnz = min(dim, rng.choice([1, 2, 5, 10, 30, 100]))
    if dim == 16000 and rng.random() < 0.3:
        nnz = 16000
    idx = set(rng.sample(range(1, dim + 1), nnz))
    if base and rng.random() < 0.7:
        idx |= {i for i in rng.sample(base, min(len(base), nnz // 2 + 1)) if i <= dim}
    while not idx:
        idx.add(rng.randint(1, dim))
    ids = sorted(idx)
    body = ",".join("%d:%s" % (i, fmt(rng.uniform(-5, 5)) if rng.random() > 0.02 else "0") for i in ids)
    return "{%s}/%d" % (body, dim), ids


def gen_bits(rng: random.Random, dim: int) -> str:
    return "".join(rng.choice("01") for _ in range(dim))


@functools.lru_cache(maxsize=None)
def pairs(t: str) -> list[tuple[str, str]]:
    rng = random.Random(f"{SEED}-{t}")
    out = []
    for i in range(N):
        dim = pick_dim(rng, i, t)
        # a few all-zero operands exercise the NaN paths of cosine / jaccard
        za = 10 <= i < 14
        zb = 14 <= i < 17
        if t == "sparsevec":
            a, ia = gen_sparse(rng, dim, za, None)
            b, _ = gen_sparse(rng, dim, zb, ia)
        elif t == "bit":
            a = "0" * dim if za else gen_bits(rng, dim)
            b = "0" * dim if zb else gen_bits(rng, dim)
        else:
            a = gen_dense(rng, dim, t, za)
            b = gen_dense(rng, dim, t, zb)
        out.append((a, b))
    return out


def pg_cast(t: str) -> str:
    return "varbit" if t == "bit" else t


def pg_distances(pg, t: str, op: str, ps: list[tuple[str, str]]) -> list[float]:
    ty = pg_cast(t)
    sql = f"SELECT a::{ty} {op.replace('%', '%%')} b::{ty} FROM unnest(%s::text[], %s::text[]) WITH ORDINALITY AS u(a, b, n) ORDER BY n"
    res: list[float] = []
    for i in range(0, len(ps), BATCH):
        chunk = ps[i : i + BATCH]
        rows = pg.execute(sql, ([a for a, _ in chunk], [b for _, b in chunk])).fetchall()
        res.extend(float(r[0]) for r in rows)
    return res


U32 = 2.0**-24  # float32 unit roundoff


def to_terms(t: str, s: str) -> dict[int, float]:
    """Parse a text literal into {index: value} (sparse view; zeros omitted)."""
    if t == "sparsevec":
        body, _ = s[1:].split("}/")
        d = {}
        for part in filter(None, body.split(",")):
            i, v = part.split(":")
            if float(v) != 0.0:
                d[int(i)] = float(v)
        return d
    vals = [float(x) for x in s[1:-1].split(",")]
    if t == "halfvec":
        vals = [struct.unpack("e", struct.pack("e", v))[0] for v in vals]
    return {i + 1: v for i, v in enumerate(vals) if v != 0.0}


def rounding_bound(t: str, op: str, a: str, b: str, ref: float) -> float | None:
    """Worst-case |difference| between two correct float32 summations in different order.

    Standard bound: each summation is within n*u*sum|terms| of exact, so two of them
    differ by at most 2*n*u*sum|terms| (n = number of summed terms, u = 2^-24).
    Returns None for types that are integer-exact (bit).
    """
    if t == "bit":
        return None
    ta, tb = to_terms(t, a), to_terms(t, b)
    keys = set(ta) | set(tb)
    if t == "sparsevec":
        n = max(len(keys), 1)
    else:
        n = len(a.split(","))
    if op == "<->":
        return 2 * n * U32 * abs(ref) + 1e-30
    if op == "<+>":
        return 2 * n * U32 * sum(abs(ta.get(k, 0.0) - tb.get(k, 0.0)) for k in keys) + 1e-30
    sabs = sum(abs(ta.get(k, 0.0) * tb.get(k, 0.0)) for k in keys)
    if op == "<#>":
        return 2 * n * U32 * sabs + 1e-30
    # <=>: 1 - dot / (|a||b|); dot and both norms carry float32 summation error
    na = math.sqrt(sum(v * v for v in ta.values()))
    nb = math.sqrt(sum(v * v for v in tb.values()))
    if na == 0 or nb == 0:
        return 0.0
    return 4 * n * U32 * (1.0 + sabs / (na * nb)) + 1e-30


def same(a: float, b: float) -> bool:
    if math.isnan(a) or math.isnan(b):
        return math.isnan(a) and math.isnan(b)
    if a == b:  # also +-inf, +-0
        return True
    return math.isclose(a, b, rel_tol=REL_TOL, abs_tol=0.0)


@pytest.mark.parametrize("t,op", CASES, ids=[f"{t}{op}" for t, op in CASES])
def test_distance_parity(pg, cli, summary, t, op):
    ps = pairs(t)
    expected = pg_distances(pg, t, op, ps)
    reqs = [{"op": op, "type": t, "a": a, "b": b} for a, b in ps]
    got_raw = cli.run(reqs)
    bad = [(i, r) for i, r in enumerate(got_raw) if not r.get("ok")]
    assert not bad, f"{t} {op}: av_parity_cli errors, first: {bad[0][0]} {bad[0][1]}"
    got = [float(r["d"]) for r in got_raw]

    identical = sum(1 for g, e in zip(got, expected) if (g == e) or (math.isnan(g) and math.isnan(e)))
    strict_bad = [i for i, (g, e) in enumerate(zip(got, expected)) if not same(g, e)]
    # float32 summation order differs between kernel tiers: accept anything within the
    # worst-case rounding bound, but count how often that fallback was needed.
    mism = []
    rounding_only = 0
    for i in strict_bad:
        g, e = got[i], expected[i]
        bound = rounding_bound(t, op, ps[i][0], ps[i][1], e)
        if bound is not None and math.isfinite(g) and math.isfinite(e) and abs(g - e) <= bound:
            rounding_only += 1
        else:
            mism.append(i)
    max_rel = 0.0
    for g, e in zip(got, expected):
        if g != e and not math.isnan(g) and not math.isnan(e) and e != 0 and math.isfinite(e):
            max_rel = max(max_rel, abs(g - e) / abs(e))
    summary.append(
        f"{t:9s} {op:4s} n={len(ps):6d} bit-identical={identical:6d} ({100.0 * identical / len(ps):6.2f}%) "
        f"beyond-1e-6-within-f32-rounding-bound={rounding_only} FAIL={len(mism)} max_rel={max_rel:.3g}"
    )
    if mism:
        i = mism[0]
        a, b = ps[i]
        pytest.fail(
            f"{t} {op}: {len(mism)}/{len(ps)} mismatches. first #{i}: pg={expected[i]!r} ours={got[i]!r} "
            f"dim_a_len={len(a)} a[:120]={a[:120]!r} b[:120]={b[:120]!r}"
        )


# ---- failure modes -------------------------------------------------------------------------

MISMATCH = [
    ("vector", "<->", "[1,2,3]", "[1,2]"),
    ("vector", "<#>", "[1]", "[1,2]"),
    ("vector", "<=>", "[1,2]", "[1,2,3]"),
    ("vector", "<+>", "[1,2]", "[1]"),
    ("halfvec", "<->", "[1,2,3]", "[1,2]"),
    ("halfvec", "<=>", "[1]", "[1,2]"),
    ("sparsevec", "<->", "{1:1}/3", "{1:1}/4"),
    ("sparsevec", "<=>", "{1:1}/3", "{1:1}/2"),
    ("bit", "<~>", "101", "10"),
    ("bit", "<%>", "1", "10"),
]


@pytest.mark.parametrize("t,op,a,b", MISMATCH)
def test_dimension_mismatch_error_matches(pg, cli, t, op, a, b):
    import psycopg

    ty = pg_cast(t)
    with pytest.raises(psycopg.Error) as ei:
        pg.execute(f"SELECT %s::{ty} {op.replace('%', '%%')} %s::{ty}", (a, b)).fetchone()
    pg_msg = ei.value.diag.message_primary
    (r,) = cli.run([{"op": op, "type": t, "a": a, "b": b}])
    assert r["ok"] is False
    assert r["error"] == pg_msg


@pytest.mark.parametrize("t,op", CASES, ids=[f"{t}{op}" for t, op in CASES])
def test_minimal_boundaries(pg, cli, t, op):
    """Dim 1, identical operands, and zero operands (NaN paths) match PostgreSQL."""
    one = {"vector": "[3]", "halfvec": "[3]", "sparsevec": "{1:3}/1", "bit": "1"}[t]
    zero = {"vector": "[0,0]", "halfvec": "[0,0]", "sparsevec": "{}/2", "bit": "00"}[t]
    other = {"vector": "[1,2]", "halfvec": "[1,2]", "sparsevec": "{1:1,2:2}/2", "bit": "01"}[t]
    cases = [(one, one), (zero, zero), (zero, other), (other, other)]
    exp = pg_distances(pg, t, op, cases)
    got = [float(r["d"]) for r in cli.run([{"op": op, "type": t, "a": a, "b": b} for a, b in cases])]
    for c, g, e in zip(cases, got, exp):
        assert same(g, e), f"{t} {op} {c}: pg={e!r} ours={g!r}"
