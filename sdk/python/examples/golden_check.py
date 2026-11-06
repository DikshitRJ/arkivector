"""Golden check script for Arkivector Python SDK (VectorBytes v1 contract)."""

import json
import os
import sys
from pathlib import Path

# Ensure sdk/python/src is in sys.path
SCRIPT_DIR = Path(__file__).resolve().parent
SDK_SRC = SCRIPT_DIR.parent / "src"
if str(SDK_SRC) not in sys.path:
    sys.path.insert(0, str(SDK_SRC))

from arkivector import (  # noqa: E402
    Bit,
    HalfVector,
    SparseVector,
    Vector,
    VectorBytesError,
    vectorbytes_decode_any,
)


def find_repo_root() -> Path:
    env_root = os.environ.get("ARKIVECTOR_REPO")
    if env_root:
        p = Path(env_root).resolve()
        if (p / "spec" / "golden" / "vectorbytes.json").exists():
            return p

    cur = SCRIPT_DIR
    for _ in range(5):
        if (cur / "spec" / "golden" / "vectorbytes.json").exists():
            return cur
        cur = cur.parent

    raise FileNotFoundError(
        "Could not find repository root containing spec/golden/vectorbytes.json"
    )


def run_offline_golden_check() -> int:
    repo_root = find_repo_root()
    golden_file = repo_root / "spec" / "golden" / "vectorbytes.json"

    with open(golden_file, "r", encoding="utf-8") as f:
        data = json.load(f)

    cases = data.get("cases", [])
    total = len(cases)
    passed = 0
    failures = []

    for case in cases:
        name = case["name"]
        is_valid = case["valid"]
        hex_str = case["hex"]
        raw_bytes = bytes.fromhex(hex_str)

        if is_valid:
            expected_type = case["type"]
            expected_dim = case["dim"]
            normalized = case.get("normalized", False)

            try:
                # 1. Decode test
                decoded = vectorbytes_decode_any(raw_bytes)
                if decoded.dim != expected_dim:
                    raise AssertionError(f"dim mismatch: {decoded.dim} != {expected_dim}")

                if expected_type == "vector":
                    if not isinstance(decoded, Vector):
                        raise AssertionError(
                            f"Expected Vector instance, got {type(decoded).__name__}"
                        )
                    if decoded.to_list() != case["values"]:
                        raise AssertionError(
                            f"values mismatch: {decoded.to_list()} != {case['values']}"
                        )
                    # Round-trip encode test
                    vec = Vector(case["values"], normalized=normalized)
                    enc_hex = vec.to_vectorbytes().hex()
                    if enc_hex != hex_str:
                        raise AssertionError(
                            f"encoded hex mismatch:\n  got: {enc_hex}\n  exp: {hex_str}"
                        )

                elif expected_type == "halfvec":
                    if not isinstance(decoded, HalfVector):
                        raise AssertionError(
                            f"Expected HalfVector instance, got {type(decoded).__name__}"
                        )
                    if decoded.to_list() != case["values"]:
                        raise AssertionError(
                            f"values mismatch: {decoded.to_list()} != {case['values']}"
                        )
                    # Round-trip encode test (use input_values if present for rounding tests)
                    input_vals = case.get("input_values", case["values"])
                    hvec = HalfVector(input_vals, normalized=normalized)
                    enc_hex = hvec.to_vectorbytes().hex()
                    if enc_hex != hex_str:
                        raise AssertionError(
                            f"encoded hex mismatch:\n  got: {enc_hex}\n  exp: {hex_str}"
                        )

                elif expected_type == "bit":
                    if not isinstance(decoded, Bit):
                        raise AssertionError(f"Expected Bit instance, got {type(decoded).__name__}")
                    if decoded.to_text() != case["bits"]:
                        raise AssertionError(
                            f"bits mismatch: {decoded.to_text()} != {case['bits']}"
                        )
                    # Round-trip encode test
                    bvec = Bit(case["bits"], normalized=normalized)
                    enc_hex = bvec.to_vectorbytes().hex()
                    if enc_hex != hex_str:
                        raise AssertionError(
                            f"encoded hex mismatch:\n  got: {enc_hex}\n  exp: {hex_str}"
                        )

                elif expected_type == "sparsevec":
                    if not isinstance(decoded, SparseVector):
                        raise AssertionError(
                            f"Expected SparseVector instance, got {type(decoded).__name__}"
                        )
                    if decoded.indices() != case["indices"]:
                        raise AssertionError(
                            f"indices mismatch: {decoded.indices()} != {case['indices']}"
                        )
                    if decoded.values() != case["values"]:
                        raise AssertionError(
                            f"values mismatch: {decoded.values()} != {case['values']}"
                        )
                    if "text" in case and decoded.to_text() != case["text"]:
                        raise AssertionError(
                            f"text mismatch: {decoded.to_text()!r} != {case['text']!r}"
                        )
                    # Round-trip encode test
                    svec = SparseVector(
                        case["dim"],
                        case["indices"],
                        case["values"],
                        normalized=normalized,
                    )
                    enc_hex = svec.to_vectorbytes().hex()
                    if enc_hex != hex_str:
                        raise AssertionError(
                            f"encoded hex mismatch:\n  got: {enc_hex}\n  exp: {hex_str}"
                        )

                else:
                    raise AssertionError(f"Unknown type in valid golden case: {expected_type}")

                print(f"PASS {name}")
                passed += 1

            except Exception as e:
                print(f"FAIL {name}: {e}")
                failures.append((name, str(e)))

        else:
            # Invalid case
            expected_error = case["error"]
            try:
                vectorbytes_decode_any(raw_bytes)
                print(f"FAIL {name}: expected {expected_error}, but decode succeeded")
                failures.append((name, f"expected {expected_error}, but decode succeeded"))
            except VectorBytesError as e:
                if e.code == expected_error:
                    print(f"PASS {name}")
                    passed += 1
                else:
                    print(f"FAIL {name}: expected {expected_error}, got {e.code}")
                    failures.append((name, f"expected {expected_error}, got {e.code}"))
            except Exception as e:
                msg = f"expected VectorBytesError({expected_error}), got {type(e).__name__}: {e}"
                print(f"FAIL {name}: {msg}")
                failures.append((name, msg))

    print()
    print(f"Summary: {passed}/{total} PASS")

    if failures:
        return 1
    return 0


def main() -> None:
    code = run_offline_golden_check()
    sys.exit(code)


if __name__ == "__main__":
    main()
