"""Text I/O parity: '<literal>'::T::text and input error messages, PostgreSQL vs. ours."""

from __future__ import annotations

import pytest

from test_distances import pg_cast  # noqa: F401  (shared helper)

VALID = [
    ("vector", "[1,2,3]"),
    ("vector", " [ 1 , 2.5e3 ,-0 ] "),
    ("vector", "[1e-5,1.5E+2,0.1]"),
    ("vector", "[3.4028235e38,-3.4028235e38,1e-45]"),
    ("vector", "[0.1,0.2,0.30000001]"),
    ("vector", "[16777217]"),
    ("halfvec", "[1,2,3]"),
    ("halfvec", "[0.1,65504,-65504,6e-8]"),
    ("halfvec", " [ 1e-3 , 2.5 ] "),
    ("sparsevec", "{1:1.5}/3"),
    ("sparsevec", "{1:1,3:2}/5"),
    ("sparsevec", "{}/4"),
    ("sparsevec", " { 2 : 0.5 , 7 : -3 } / 10 "),
    ("sparsevec", "{1:0,2:3}/3"),
    ("sparsevec", "{1:1}/1000000000"),
    ("bit", "101"),
    ("bit", "0"),
    ("bit", "0000000011111111"),
]

INVALID = [
    # (type, literal, why)
    ("vector", "[NaN]", "nan"),
    ("vector", "[1,nan]", "nan"),
    ("vector", "[Infinity]", "inf"),
    ("vector", "[1,-Infinity]", "-inf"),
    ("vector", "[inf]", "inf short"),
    ("vector", "[]", "empty"),
    ("vector", "", "empty string"),
    ("vector", "[", "unterminated"),
    ("vector", "1,2", "no brackets"),
    ("vector", "[1,2", "no closing"),
    ("vector", "[1,,2]", "double comma"),
    ("vector", "[1,2]x", "junk after"),
    ("vector", "[a]", "non numeric"),
    ("vector", "[1e40]", "float32 overflow"),
    ("vector", "[" + ",".join(["1"] * 16001) + "]", "dims > 16000"),
    ("halfvec", "[NaN]", "nan"),
    ("halfvec", "[Infinity]", "inf"),
    ("halfvec", "[]", "empty"),
    ("halfvec", "[70000]", "half overflow"),
    ("halfvec", "[" + ",".join(["1"] * 16001) + "]", "dims > 16000"),
    ("halfvec", "1,2", "no brackets"),
    ("sparsevec", "{0:1}/3", "index 0 (1-based)"),
    ("sparsevec", "{4:1}/3", "index > dims"),
    ("sparsevec", "{2:1,1:1}/3", "unsorted"),
    ("sparsevec", "{1:1,1:2}/3", "duplicate"),
    ("sparsevec", "{}/0", "zero dims"),
    ("sparsevec", "{1:1}/0", "zero dims with element"),
    ("sparsevec", "{1:1}/1000000001", "dims too large"),
    ("sparsevec", "{1:NaN}/3", "nan"),
    ("sparsevec", "{1:Infinity}/3", "inf"),
    ("sparsevec", "{1:1}", "missing dims"),
    ("sparsevec", "{1:1/3", "unterminated"),
    ("sparsevec", "", "empty string"),
    ("sparsevec", "{1}/3", "missing colon"),
    ("sparsevec", "{-1:1}/3", "negative index"),
    ("sparsevec", "{1:1}/3x", "junk after"),
    ("bit", "102", "invalid digit 2"),
    ("bit", "abc", "invalid digit a"),
    ("bit", "1 0", "space"),
    ("bit", "-1", "sign"),
]


def pg_out(pg, t: str, lit: str) -> str:
    return pg.execute(f"SELECT %s::{pg_cast(t) if t == 'bit' else t}::text", (lit,)).fetchone()[0]


@pytest.mark.parametrize("t,lit", VALID, ids=[f"{t}:{lit[:30]}" for t, lit in VALID])
def test_roundtrip_matches(pg, cli, t, lit):
    expected = pg_out(pg, t, lit)
    (r,) = cli.run([{"op": "in", "type": t, "a": lit}])
    assert r["ok"] is True, r
    assert r["text"] == expected


@pytest.mark.parametrize("t,lit,why", INVALID, ids=[f"{t}:{why}" for t, _, why in INVALID])
def test_invalid_input_error_matches(pg, cli, t, lit, why):
    import psycopg

    try:
        got_pg = pg_out(pg, t, lit)
    except psycopg.Error as exc:
        pg_msg = exc.diag.message_primary
    else:
        pg_msg = None
    (r,) = cli.run([{"op": "in", "type": t, "a": lit}])
    if pg_msg is None:
        assert r["ok"] is True and r["text"] == got_pg, f"PG accepted {lit[:60]!r} -> {got_pg!r}; ours: {r}"
    else:
        assert r["ok"] is False, f"PG rejected {lit[:60]!r} ({pg_msg}) but we accepted: {r}"
        assert r["error"] == pg_msg


def test_pg_rejections_are_actually_exercised(pg):
    """Guard: most INVALID cases really are errors in PostgreSQL (the table is not stale)."""
    import psycopg

    rejected = 0
    for t, lit, _ in INVALID:
        try:
            pg_out(pg, t, lit)
        except psycopg.Error:
            rejected += 1
    assert rejected >= len(INVALID) - 3
