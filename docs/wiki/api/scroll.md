# Scroll

`POST /v1/collections/{name}/scroll` pages through live entities that match an optional filter, in ascending **key** order. Use it to export a collection or walk everything that matches a filter.

```bash
curl -s $AV/v1/collections/notes/scroll -H 'content-type: application/json' -d '{"limit": 2}'
```

```json
{
  "as_of_block": 15,
  "entities": [
    { "key": "0x34754cd9…fe0f", "id": "car", "owner": "0xf39f…2266", "creator": "0xf39f…2266",
      "created_at_block": 15, "updated_at_block": 15, "expires_at_block": 302415,
      "attributes": { "stars": { "type": "i32", "value": 3 }, "topic": { "type": "str", "value": "vehicles" } },
      "document": null },
    { "key": "0xc59528cf…c1b6", "id": "cat", "...": "..." }
  ],
  "next_cursor": "eyJr…"
}
```

Pass `next_cursor` back as `cursor` for the next page; `null` means the end:

```bash
curl -s $AV/v1/collections/notes/scroll -H 'content-type: application/json' \
  -d '{"limit": 2, "cursor": "eyJr…"}'
```

| Field | Default | Meaning |
| :--- | :--- | :--- |
| `filter` or `where` | none | [Filter](filters.md) |
| `limit` | 100 | 1–1000 entities per page |
| `cursor` | | Opaque cursor from the previous page (valid for 1 hour; don't parse it) |
| `include` | `["attributes"]` | `attributes`, `document`, `vector` |
| `consistency` | | [Consistency](consistency.md) |

Each page reflects its own `as_of_block`. Because ordering is by immutable key, an entity is never returned twice, and one that exists for the whole scroll is never skipped. The [Python](../python/reading.md) and [Node](../node/reading.md) SDKs wrap this in an iterator.
