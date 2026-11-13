# Get an entity

`GET /v1/collections/{name}/entities/{ref}` reads one live entity by its id or Arkiv key.

```bash
curl -s "$AV/v1/collections/notes/entities/cat?include=attributes,document"
```

```json
{
  "as_of_block": 15,
  "entity": {
    "key": "0xc59528cfc47a6a19b62d660a46e4ff652f0af203d5b8b40b60ff26fb6a18c1b6",
    "id": "cat",
    "owner": "0xf39fd6e51aad88f6f4ce6ab8827279cfffb92266",
    "creator": "0xf39fd6e51aad88f6f4ce6ab8827279cfffb92266",
    "created_at_block": 15,
    "updated_at_block": 15,
    "expires_at_block": 302415,
    "attributes": { "stars": { "type": "i32", "value": 5 }, "topic": { "type": "str", "value": "pets" } },
    "document": "Cats purr."
  }
}
```

## Parameters

| Parameter | Default | Meaning |
| :--- | :--- | :--- |
| `ref` (path) | | An Arkiv key (`0x` + 64 hex) or your `id`, percent-encoded |
| `include` | `attributes` | Comma list of `attributes`, `document`, `vector` (adds base64 `vector_bytes`) |
| `min_block`, `timeout_ms` | | Wait until this block is indexed ([Consistency](consistency.md)) |
| `verify` | `false` | Re-read the entity from the chain and report `verified` |

## Entity fields

| Field | Meaning |
| :--- | :--- |
| `key` | Arkiv entity key |
| `id` | Your id (`av.id`), or `null` |
| `owner`, `creator` | Current owner and creator addresses |
| `created_at_block`, `updated_at_block`, `expires_at_block` | Arkiv block numbers |
| `attributes` | Your attributes as typed values (reserved `arkiv.schema` and `av.*` are never included) |
| `document` | Present with `include=document` |
| `vector_bytes` | Present with `include=vector` (base64 [VectorBytes](vectors.md#vectorbytes)) |

Only entities that are live at `as_of_block` are returned. Expired, deleted or quarantined entities give `404 entity_not_found`. If two live entities share an id (possible when other wallets write directly), the most recently updated wins.
