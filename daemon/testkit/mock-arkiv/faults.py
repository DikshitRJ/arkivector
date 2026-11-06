"""Seeded fault injection for the RPC surface (never applied to /_mock endpoints)."""

from __future__ import annotations

import random
from typing import Any

from state import ControlError


class Faults:
    FIELDS = ("drop_ws_ratio", "http_429_ratio", "latency_ms", "fail_methods")

    def __init__(self, seed: int) -> None:
        self.reseed(seed)
        self.clear()

    def reseed(self, seed: int) -> None:
        self.rng = random.Random(seed ^ 0xFA117)

    def clear(self) -> None:
        self.drop_ws_ratio = 0.0
        self.http_429_ratio = 0.0
        self.latency_ms = 0
        self.fail_methods: dict[str, float] = {}  # method (or "*") -> failure probability

    def update(self, body: Any) -> None:
        if not isinstance(body, dict):
            raise ControlError("body must be an object")
        extra = set(body) - set(self.FIELDS)
        if extra:
            raise ControlError(f"unknown fault fields {sorted(extra)}")
        new: dict[str, Any] = {}
        for f in ("drop_ws_ratio", "http_429_ratio"):
            if f in body:
                v = body[f]
                if isinstance(v, bool) or not isinstance(v, (int, float)) or not 0 <= v <= 1:
                    raise ControlError(f"{f} must be a number in [0, 1]")
                new[f] = float(v)
        if "latency_ms" in body:
            v = body["latency_ms"]
            if isinstance(v, bool) or not isinstance(v, (int, float)) or not 0 <= v <= 60000:
                raise ControlError("latency_ms must be a number in [0, 60000]")
            new["latency_ms"] = v
        if "fail_methods" in body:
            fm = body["fail_methods"]
            if isinstance(fm, list) and all(isinstance(m, str) for m in fm):
                new["fail_methods"] = {m: 1.0 for m in fm}
            elif isinstance(fm, dict) and all(
                isinstance(k, str) and isinstance(v, (int, float)) and not isinstance(v, bool) and 0 <= v <= 1
                for k, v in fm.items()
            ):
                new["fail_methods"] = {k: float(v) for k, v in fm.items()}
            else:
                raise ControlError("fail_methods must be [method,...] or {method: ratio}")
        for k, v in new.items():
            setattr(self, k, v)

    def to_obj(self) -> dict[str, Any]:
        return {
            "drop_ws_ratio": self.drop_ws_ratio,
            "http_429_ratio": self.http_429_ratio,
            "latency_ms": self.latency_ms,
            "fail_methods": dict(self.fail_methods),
        }

    # Each roll consumes the seeded RNG only when the fault is enabled, so runs are reproducible.
    def roll(self, ratio: float) -> bool:
        return ratio > 0 and self.rng.random() < ratio

    def should_429(self) -> bool:
        return self.roll(self.http_429_ratio)

    def should_drop_ws(self) -> bool:
        return self.roll(self.drop_ws_ratio)

    def should_fail(self, method: str) -> bool:
        r = self.fail_methods.get(method, self.fail_methods.get("*", 0.0))
        return self.roll(r)
