# Filters

`F` builds the JSON filter tree ([Filters API](../api/filters.md)). Combine with `&` (and), `|` (or) and `~` (not):

```python
from arkivector import F, T

F.eq("topic", "pets")
F.ne("topic", "pets")
F.lt("stars", 3); F.lte("stars", 3); F.gt("stars", 3); F.gte("stars", 3)
F.between("stars", 2, 4)                       # inclusive
F.startswith("region", "eu-")                  # str only
F.exists("archived")
F.typeof("score", "dec")

F.gte("stars", 4) & F.eq("topic", "pets")      # F.and_(a, b)
F.eq("topic", "pets") | F.eq("topic", "birds") # F.or_(a, b)
~F.exists("archived")                          # F.not_(a)

F.eq("$creator", T.addr("0x1111111111111111111111111111111111111111"))
F.gte("price", T.dec("4.5"))
```

Plain values are typed from the collection's declared attributes; use [`T`](types.md#typed-values) for undeclared ones or system fields.

The same filters as strings, in Arkiv's query language:

```python
client.search("notes", q, where="stars >= 4 AND topic = str('pets')")
```

Strings in `where` need `str('…')`. Pass either `filter=` or `where=`, not both.
