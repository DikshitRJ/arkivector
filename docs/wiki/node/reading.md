# Reading

## Get one entity

```ts
const e = await client.get("notes", "cat", { include: ["attributes", "document"] });
e.key; e.id; e.owner; e.creator; e.createdAtBlock; e.updatedAtBlock; e.expiresAtBlock;
e.attributes; e.document;
```

Options: `include`, `verify`, `minBlock`, `timeoutMs`. `ref` is an id or a key; a missing entity throws `NotFoundError`.

## Count

```ts
(await client.count("notes")).count;                                    // all live entities
(await client.count("notes", { filter: F.eq("topic", "pets") })).count; // 2
await client.count("notes", { where: "stars BETWEEN 2 AND 4" });        // { count, asOfBlock }
```

## Scroll

`scroll` is an async generator over every matching live entity, in key order:

```ts
for await (const entity of client.scroll("notes", { where: "stars >= 4", limit: 100 })) {
  console.log(entity.id, entity.attributes);
}
```

Manual paging:

```ts
let page = await client.scrollPage("notes", { limit: 100 });
while (true) {
  for (const e of page.entities) { /* ... */ }
  if (page.nextCursor === null) break;
  page = await client.scrollPage("notes", { limit: 100, cursor: page.nextCursor });
}
```
