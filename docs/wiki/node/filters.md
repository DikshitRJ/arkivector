# Filters

`F` builds the JSON filter tree ([Filters API](../api/filters.md)):

```ts
import { F, T } from "@dikshitrj/arkivector";

F.eq("topic", "pets");
F.ne("topic", "pets");
F.lt("stars", 3); F.lte("stars", 3); F.gt("stars", 3); F.gte("stars", 3);
F.between("stars", 2, 4);                       // inclusive
F.startswith("region", "eu-");                  // str only
F.exists("archived");
F.typeof("score", "dec");

F.and(F.gte("stars", 4), F.eq("published", true));
F.or(F.eq("topic", "pets"), F.eq("topic", "birds"));
F.not(F.exists("archived"));

F.eq("$creator", T.addr("0x1111111111111111111111111111111111111111"));
F.gte("price", T.dec("4.5"));
```

`F.and` and `F.or` also take an array. Plain values are typed from the collection's declared attributes; use [`T`](types.md#typed-values) for undeclared ones.

Or pass a `where` string: `{ where: "stars >= 4 AND topic = str('pets')" }`. Strings need `str('…')`. Use `filter` or `where`, not both.
