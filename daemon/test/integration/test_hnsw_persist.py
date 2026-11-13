"""HNSW persistence, background rebuilds and expiry bands (GitHub issue #11).

- a restart loads the saved graph instead of rebuilding it, and indexes only what changed since
  the save (including after a crash, from an older file);
- a corrupt or truncated graph file is rejected with a warning and rebuilt in the background,
  never a crash, and searches stay correct meanwhile;
- a rebuild runs on the worker thread: block ingestion and new writes continue while it runs,
  and the adopted graph catches up with them;
- expired bands are released whole, live_entities and search results never include expired rows,
  and an extended entity survives the drop of its old band.
"""

from __future__ import annotations

import random
import signal
import time
from pathlib import Path
from typing import Any

import pytest
from conftest import DEFAULT_OWNER, DaemonClient, DaemonFactory, MockArkiv
from test_hnsw import DIM, entity, gaussian_vectors, search

pytestmark = pytest.mark.hnsw

N = 3000


def create_collection(d: DaemonClient, name: str) -> None:
    col = {
        "name": name,
        "schema_tag": f"arkivector.v1.test.{name}",
        "column": {"type": "vector", "dimensions": DIM},
        "index": {"method": "hnsw", "opclass": "vector_l2_ops", "with": {"m": 16, "ef_construction": 64}},
        "writers": [DEFAULT_OWNER],
        "membership": "creator",
        "attributes": {"category": "str"},
    }
    status, body = d.post("/v1/collections", col)
    assert status in (200, 201), body


def seed(mock: MockArkiv, name: str, vecs: list[list[float]], cat: str = "a", **op: Any) -> list[str]:
    schema = f"arkivector.v1.test.{name}"
    keys: list[str] = []
    for i in range(0, len(vecs), 1000):
        items = []
        for v in vecs[i : i + 1000]:
            ent = entity(schema, v, cat)
            ent.update(op)
            items.append(ent)
        created = mock.ctl("POST", "/entities", items)["created"]
        keys += [k for r in created for k in r["keys"]]
    mock.mine(1)
    return keys


def stats(d: DaemonClient, name: str) -> dict[str, Any]:
    status, body = d.get(f"/v1/collections/{name}")
    assert status == 200, body
    return body


def wait_live(d: DaemonClient, mock: MockArkiv, name: str, live: int, timeout: float = 120) -> None:
    head = mock.head()
    deadline = time.time() + timeout
    body: Any = None
    while time.time() < deadline:
        body = stats(d, name)
        if body["sync"]["as_of_block"] >= head and body["stats"]["live_entities"] == live:
            return
        time.sleep(0.1)
    raise TimeoutError(f"{name}: expected {live} live entities: {body}")


def hnsw_state(d: DaemonClient, name: str) -> dict[str, Any]:
    status, body = d.get(f"/_debug/collections/{name}/hnsw")
    assert status == 200, body
    return body


def wait_until(pred: Any, timeout: float = 60, what: str = "condition") -> Any:
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        last = pred()
        if last:
            return last
        time.sleep(0.1)
    raise TimeoutError(f"timed out waiting for {what}")


def graceful_stop(d: DaemonClient) -> None:
    d.proc.send_signal(signal.SIGTERM)
    d.proc.wait(timeout=30)


def graph_files(d: DaemonClient, name: str) -> list[Path]:
    return sorted((d.data_dir / "segments" / name).glob("*.hnsw"))


def top_keys(d: DaemonClient, name: str, q: list[float], **extra: Any) -> list[str]:
    return [h["key"] for h in search(d, name, q, **extra)["results"]]


def recall(d: DaemonClient, name: str, queries: list[list[float]]) -> float:
    total = 0.0
    for q in queries:
        approx = search(d, name, q)
        assert approx["plan"]["strategy"] == "hnsw", approx["plan"]
        exact = {h["key"] for h in search(d, name, q, exact=True)["results"]}
        total += len(exact & {h["key"] for h in approx["results"]}) / 10.0
    return total / len(queries)


# --------------------------------------------------------------------------- restart


def test_restart_loads_graph_without_rebuild(mock_arkiv: MockArkiv, daemon_factory: DaemonFactory) -> None:
    rng = random.Random(21)
    d1 = daemon_factory.start()
    wd = d1.workdir  # type: ignore[attr-defined]
    create_collection(d1, "persist")
    seed(mock_arkiv, "persist", gaussian_vectors(rng, N))
    wait_live(d1, mock_arkiv, "persist", N)
    queries = gaussian_vectors(rng, 10)
    before = [top_keys(d1, "persist", q) for q in queries]
    assert recall(d1, "persist", queries) >= 0.9

    graceful_stop(d1)
    files = graph_files(d1, "persist")
    assert len(files) == 1 and files[0].stat().st_size > 1000, files

    d2 = daemon_factory.start(workdir=wd)
    st = hnsw_state(d2, "persist")
    assert st["loads"] == 1 and st["load_failures"] == 0, st
    assert st["builds"] == 0 and st["builds_running"] == 0, st  # no full rebuild
    assert st["bands"][0]["source"] == "loaded" and st["bands"][0]["graph"] == N and st["bands"][0]["tail"] == 0, st
    assert stats(d2, "persist")["sync"]["state"] == "live"
    # the very same graph: identical approximate results
    assert [top_keys(d2, "persist", q) for q in queries] == before

    # writes after the restart go into the loaded graph; then a crash (no shutdown save) and a
    # restart from the older file: only the delta is indexed, still no full rebuild
    extra = gaussian_vectors(rng, 200)
    seed(mock_arkiv, "persist", extra, cat="b")
    wait_live(d2, mock_arkiv, "persist", N + 200)
    d2.proc.send_signal(signal.SIGKILL)
    d2.proc.wait(timeout=10)

    d3 = daemon_factory.start(workdir=wd)
    st = hnsw_state(d3, "persist")
    assert st["loads"] == 1 and st["builds"] == 0 and st["builds_running"] == 0, st
    assert st["bands"][0]["graph"] == N + 200 and st["bands"][0]["tail"] == 0, st
    assert "vectors added since the save" in daemon_factory.log(d3)
    assert recall(d3, "persist", queries) >= 0.9
    # the new entities are reachable through the graph
    hit = search(d3, "persist", extra[7], filter={"eq": {"category": "b"}})
    assert hit["results"][0]["distance"] == pytest.approx(0.0, abs=1e-5)


# --------------------------------------------------------------------------- corrupt files


@pytest.mark.parametrize("damage", ["flip", "truncate", "garbage"])
def test_corrupt_graph_file_falls_back_to_rebuild(mock_arkiv: MockArkiv, daemon_factory: DaemonFactory, damage: str) -> None:
    rng = random.Random(22)
    d1 = daemon_factory.start()
    wd = d1.workdir  # type: ignore[attr-defined]
    create_collection(d1, "corrupt")
    seed(mock_arkiv, "corrupt", gaussian_vectors(rng, N))
    wait_live(d1, mock_arkiv, "corrupt", N)
    graceful_stop(d1)
    (f,) = graph_files(d1, "corrupt")
    data = bytearray(f.read_bytes())
    if damage == "flip":
        for off in range(4096, len(data), len(data) // 50):
            data[off] ^= 0x5A
    elif damage == "truncate":
        data = data[: len(data) // 2]
    else:
        data = bytearray(b"AVHNSW01" + bytes(rng.getrandbits(8) for _ in range(5000)))
    f.write_bytes(bytes(data))

    d2 = daemon_factory.start(workdir=wd)
    assert d2.proc.poll() is None
    st = hnsw_state(d2, "corrupt")
    assert st["load_failures"] == 1 and st["loads"] == 0, st
    assert "rejected" in daemon_factory.log(d2)
    # searches are correct while the band is rebuilt (exact over the tail), then through the new graph
    q = gaussian_vectors(rng, 1)[0]
    exact = top_keys(d2, "corrupt", q, exact=True)
    assert top_keys(d2, "corrupt", q)[:3] == exact[:3]
    mock_arkiv.ctl("POST", "/auto_mine", {"interval_ms": 200})  # publishes adopt the finished build
    st = wait_until(lambda: (s := hnsw_state(d2, "corrupt"))["builds"] >= 1 and s, what="background rebuild")
    assert st["bands"][0]["source"] == "built" and st["bands"][0]["graph"] == N, st
    assert recall(d2, "corrupt", gaussian_vectors(rng, 5)) >= 0.9
    # the rebuilt band was written back and loads cleanly next time
    graceful_stop(d2)
    d3 = daemon_factory.start(workdir=wd)
    st = hnsw_state(d3, "corrupt")
    assert st["loads"] == 1 and st["load_failures"] == 0, st


def test_incompatible_graph_file_is_rebuilt(mock_arkiv: MockArkiv, daemon_factory: DaemonFactory) -> None:
    """A file from another band layout (here: [hnsw] band_blocks changed) is not reused."""
    rng = random.Random(23)
    d1 = daemon_factory.start()
    wd = d1.workdir  # type: ignore[attr-defined]
    create_collection(d1, "compat")
    seed(mock_arkiv, "compat", gaussian_vectors(rng, 500), min_lifetime=100000)
    wait_live(d1, mock_arkiv, "compat", 500)
    graceful_stop(d1)
    d2 = daemon_factory.start(workdir=wd, extra_toml="[hnsw]\nband_blocks = 0\n")
    st = hnsw_state(d2, "compat")
    assert st["loads"] == 0, st
    assert st["bands"][0]["source"] in ("pending", "built"), st
    assert stats(d2, "compat")["stats"]["live_entities"] == 500


# --------------------------------------------------------------------------- background rebuild


def test_rebuild_never_blocks_ingestion(mock_arkiv: MockArkiv, daemon_factory: DaemonFactory) -> None:
    rng = random.Random(24)
    d = daemon_factory.start(env={"AV_TEST_HNSW_BUILD_DELAY_MS": "4000"})
    create_collection(d, "bg")
    seed(mock_arkiv, "bg", gaussian_vectors(rng, N))
    wait_live(d, mock_arkiv, "bg", N)

    status, _ = d.post("/_debug/collections/bg/hnsw/rebuild")
    assert status == 202
    mock_arkiv.mine(1)  # the applier schedules the requested rebuild at its next publish
    wait_until(lambda: hnsw_state(d, "bg")["builds_running"] == 1, what="rebuild start")
    start_block = stats(d, "bg")["sync"]["as_of_block"]

    # while the build runs (>= 4 s): blocks keep being applied and new entities become searchable
    extra = gaussian_vectors(rng, 50)
    seed(mock_arkiv, "bg", extra, cat="new")
    mock_arkiv.mine(5)
    wait_live(d, mock_arkiv, "bg", N + 50, timeout=3)
    assert hnsw_state(d, "bg")["builds_running"] == 1, "the rebuild should still be running"
    assert stats(d, "bg")["sync"]["as_of_block"] >= start_block + 6
    hit = search(d, "bg", extra[3], filter={"eq": {"category": "new"}})
    assert hit["results"][0]["distance"] == pytest.approx(0.0, abs=1e-5)
    approx = search(d, "bg", extra[9])
    assert approx["plan"]["strategy"] == "hnsw" and approx["results"][0]["distance"] == pytest.approx(0.0, abs=1e-5)

    # the adopted graph caught up with the writes made during the build
    mock_arkiv.ctl("POST", "/auto_mine", {"interval_ms": 200})
    st = wait_until(lambda: (s := hnsw_state(d, "bg"))["builds"] >= 1 and s["builds_running"] == 0 and s, what="adoption")
    assert st["bands"][0]["source"] == "built" and st["bands"][0]["graph"] == N + 50 and st["bands"][0]["tail"] == 0, st
    assert "+50 caught up" in daemon_factory.log(d)
    approx = search(d, "bg", extra[11])
    assert approx["plan"]["strategy"] == "hnsw" and approx["results"][0]["distance"] == pytest.approx(0.0, abs=1e-5)


# --------------------------------------------------------------------------- expiry bands


def test_expired_bands_are_released(mock_arkiv: MockArkiv, daemon_factory: DaemonFactory) -> None:
    rng = random.Random(25)
    d = daemon_factory.start(extra_toml="[hnsw]\nband_blocks = 20\n")
    create_collection(d, "bands")
    short = gaussian_vectors(rng, 1000)
    long = gaussian_vectors(rng, N)
    short_keys = seed(mock_arkiv, "bands", short, cat="short", expires_at=50)  # band 2: blocks [40, 60)
    seed(mock_arkiv, "bands", long, cat="long", expires_at=5000)  # band 250
    wait_live(d, mock_arkiv, "bands", N + 1000)
    st = hnsw_state(d, "bands")
    assert [b["band"] for b in st["bands"]] == [2, 250], st
    assert stats(d, "bands")["stats"]["segments"] == 3  # flat + two bands

    # extend one short-lived entity past its band: it moves to band 250 and must survive the drop
    survivor = short_keys[17]
    mock_arkiv.ctl("POST", f"/entities/{survivor}/extend", {"expires_at": 5000})
    mock_arkiv.mine(1)
    wait_until(lambda: stats(d, "bands")["sync"]["as_of_block"] >= mock_arkiv.head(), what="extend synced")

    # before expiry the short entities are found through the graph
    approx = search(d, "bands", short[5])
    assert approx["plan"]["strategy"] == "hnsw" and approx["results"][0]["key"] == short_keys[5]

    mock_arkiv.mine(60 - mock_arkiv.head())  # head 60: everything of band 2 has expired (expires_at 50)
    wait_live(d, mock_arkiv, "bands", N + 1)
    st = wait_until(lambda: (s := hnsw_state(d, "bands"))["bands_dropped"] >= 1 and s, what="band drop")
    assert [b["band"] for b in st["bands"]] == [250], st
    assert stats(d, "bands")["stats"]["segments"] == 2
    expired = set(short_keys) - {survivor}
    for q in short[:20]:
        for mode in ({}, {"exact": True}):
            body = search(d, "bands", q, **mode)
            assert not expired & {h["key"] for h in body["results"]}, body
    approx = search(d, "bands", short[17])
    assert approx["plan"]["strategy"] == "hnsw"
    assert approx["results"][0]["key"] == survivor
    assert approx["results"][0]["distance"] == pytest.approx(0.0, abs=1e-5)
    status, body = d.post("/v1/collections/bands/count", {})
    assert status == 200 and body["count"] == N + 1
    # the dropped band's file is gone after the next save
    graceful_stop(d)
    names = [p.name for p in graph_files(d, "bands")]
    assert names == [f"{250:020d}.hnsw"], names
