# Reading

Point reads, counts and scans that don't need a query vector.

## Get one entity

```python
e = client.get("notes", "cat", include=["attributes", "document"])
e.key, e.id, e.owner, e.creator
e.created_at_block, e.updated_at_block, e.expires_at_block
e.attributes, e.document
```

```python
client.get(collection, ref, include=("attributes",), verify=False, min_block=None, timeout_ms=None) -> Entity
```

`ref` is an id or a key. A missing, expired or deleted entity raises `NotFoundError` (`entity_not_found`).

## Count

```python
client.count("notes").count                              # all live entities
client.count("notes", filter=F.eq("topic", "pets")).count  # 2
client.count("notes", where="stars BETWEEN 2 AND 4")      # CountResult(count=..., as_of_block=...)
```

## Scroll

`scroll` is a generator that pages through every matching live entity in key order:

```python
for entity in client.scroll("notes", where="stars >= 4", limit=100):
    print(entity.id, entity.attributes)
```

For manual paging use `scroll_page`:

```python
page = client.scroll_page("notes", limit=100)
while True:
    for e in page.entities:
        ...
    if page.next_cursor is None:
        break
    page = client.scroll_page("notes", limit=100, cursor=page.next_cursor)
```
