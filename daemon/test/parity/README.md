# pgvector parity suite

Proves that the pgvector code copied into the daemon computes what real pgvector v0.8.7
(PostgreSQL 17) computes: distances for vector/halfvec/sparsevec (`<->`, `<#>`, `<=>`, `<+>`)
and bit (`<~>`, `<%>`), plus text input/output and error messages.

## Run

```bash
# from the repo root
cmake --preset dev -S daemon && cmake --build --preset dev -S daemon --target av_parity_cli   # or: (cd daemon && ...)
pip install -r daemon/test/parity/requirements.txt
docker compose -f daemon/test/parity/docker-compose.yml up -d --wait && pytest -q daemon/test/parity
```

CI note: the `daemon.yml` integration phase runs exactly the last two lines after building
`av_parity_cli`. Without Docker, without the database, or without the binary every test skips cleanly.

Environment: `PARITY_N` (pairs per type/operator, default 10000), `PARITY_SEED`, `PARITY_PG_DSN`,
`PARITY_PG_PASSWORD`, `AV_PARITY_CLI` (default `daemon/build/dev/test/av_parity_cli`).
`pytest -q` prints a summary with the bit-identical count per (type, operator).

## Pass criteria

- Results equal to 1e-6 relative, or (float32 only) within the worst-case float32 summation
  rounding bound `2*n*2^-24*sum|terms|`. Kernel tiers sum in a different order than the stock
  PostgreSQL build, so bit-identical results are expected only for `bit`, sparsevec `<#>` and
  most small inputs. The summary line reports how many results needed the rounding fallback.
- Error messages and text output must be byte-identical.
