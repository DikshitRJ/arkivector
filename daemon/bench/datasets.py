#!/usr/bin/env python3
"""
bench/datasets.py: Benchmark dataset generator / downloader for Arkivector HNSW recall & latency tests.
"""
import os
import sys
import numpy as np
from pathlib import Path

CACHE_DIR = Path(os.environ.get("ARKIVECTOR_BENCH_CACHE", Path(__file__).resolve().parent / "cache"))

def generate_synthetic_dataset(n_samples: int = 10000, n_queries: int = 100, dims: int = 128, intrinsic_dim: int = 16, seed: int = 42):
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    out_file = CACHE_DIR / f"synthetic_{n_samples}_{dims}d.npz"
    if out_file.exists():
        data = np.load(out_file)
        return data["vectors"], data["queries"]

    rng = np.random.default_rng(seed)
    proj = rng.standard_normal((dims, intrinsic_dim)).astype(np.float32)
    z_train = rng.standard_normal((n_samples, intrinsic_dim)).astype(np.float32)
    vectors = (z_train @ proj.T + 0.01 * rng.standard_normal((n_samples, dims)).astype(np.float32))

    z_test = rng.standard_normal((n_queries, intrinsic_dim)).astype(np.float32)
    queries = (z_test @ proj.T + 0.01 * rng.standard_normal((n_queries, dims)).astype(np.float32))

    # Normalize vectors
    norms = np.linalg.norm(vectors, axis=1, keepdims=True)
    norms[norms == 0] = 1.0
    vectors = vectors / norms

    q_norms = np.linalg.norm(queries, axis=1, keepdims=True)
    q_norms[q_norms == 0] = 1.0
    queries = queries / q_norms

    np.savez_compressed(out_file, vectors=vectors, queries=queries)
    return vectors, queries

if __name__ == "__main__":
    v, q = generate_synthetic_dataset()
    print(f"Generated synthetic dataset: vectors {v.shape}, queries {q.shape}")
