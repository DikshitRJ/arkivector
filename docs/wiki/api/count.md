# Count

`POST /v1/collections/{name}/count` returns the exact number of live entities, optionally only those matching a filter.

```bash
curl -s $AV/v1/collections/notes/count -H 'content-type: application/json' \
  -d "{\"where\": \"topic = str('pets')\"}"
```

```json
{"as_of_block": 22, "count": 2}
```

```bash
curl -s $AV/v1/collections/notes/count -H 'content-type: application/json' \
  -d '{"filter": {"between": {"stars": [2, 4]}}}'
```

| Field | Meaning |
| :--- | :--- |
| `filter` or `where` | Optional [filter](filters.md); without one, every live entity is counted |
| `consistency` | [Consistency](consistency.md) |

Counts are computed from the local attribute index, so they cost no chain calls.
