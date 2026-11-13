# Collections

```python
col = client.create_collection(
    "notes",                                              # name
    ("halfvec", 768),                                     # column: (type, dimensions)
    {"method": "hnsw", "opclass": "halfvec_cosine_ops",   # index
     "with": {"m": 16, "ef_construction": 64}},
    writers=["0xYourDaemonWalletAddress"],
    attributes={"topic": "str", "stars": "i32"},
    default_ttl={"days": 7},
    model="openai/text-embedding-3-small",               # optional note
)
col.name, col.column.type, col.stats.live_entities, col.sync.state
```

| Argument | Meaning |
| :--- | :--- |
| `column` | `("halfvec", 768)`, `ColumnDef("vector", 1536)` or `{"type": ..., "dimensions": ...}` |
| `index` | `{"method": "hnsw" or "none", "opclass": ..., "with": {...}}` or `IndexDef(...)` |
| `writers` | Addresses whose entities belong to the collection. **Include your daemon's wallet** |
| `membership` | `"creator"` (default), `"owner"` or `"open"` |
| `attributes` | Declared attribute types (`bool i32 u64 u256 dec str addr key bytes`) |
| `default_ttl` | `{"days": 7}` or `Ttl.days(7)` |
| `schema_tag`, `model`, `mode`, `publish_manifest` | See [Collections API](../api/collections.md#request-fields) |

```python
client.list_collections()                  # [Collection]
client.get_collection("notes")             # Collection
client.follow_collection(manifest_key="0x…", name="their-notes")
client.delete_collection("notes")          # local index only; on-chain data stays
```

Opclasses by column type are listed in the [Collections API](../api/collections.md#operator-classes).
