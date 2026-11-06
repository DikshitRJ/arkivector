#!/usr/bin/env python3
"""
bench/report.py: Generates markdown tables from run_recall.py JSON results.
"""
import json
import sys

def format_report(results_json_path: str) -> str:
    with open(results_json_path, "r") as f:
        data = json.load(f)

    lines = [
        "# HNSW Recall & Latency Benchmark Report",
        "",
        "| `ef_search` | Recall@10 | p50 latency (ms) | p99 latency (ms) | QPS |",
        "| :---: | :---: | :---: | :---: | :---: |"
    ]
    for row in data:
        lines.append(f"| {row['ef_search']} | {row['recall_at_10']:.4f} | {row['p50_ms']:.2f} | {row['p99_ms']:.2f} | {row['qps']} |")

    return "\n".join(lines)

if __name__ == "__main__":
    path = sys.argv[1] if len(sys.argv) > 1 else "bench_results.json"
    print(format_report(path))
