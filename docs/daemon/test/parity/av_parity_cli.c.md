# daemon/test/parity/av_parity_cli.c

## Purpose

Test helper for the pgvector parity suite. It reads JSON lines on stdin. Each line names an operation and its inputs. It writes one JSON line with the result.

## Input and output

- Distance: `{"op":"<->","type":"vector","a":"[1,2]","b":"[3,4]"}` gives `{"ok":true,"d":"..."}`. The distance is printed with `%.17g`, as a string.
- Text: `{"op":"in","type":"sparsevec","a":"{1:1.5}/3"}` gives `{"ok":true,"text":"..."}`.
- Failure: `{"ok":false,"error":"<pgvector message>"}`.

## Ownership and thread safety

A single-threaded process. It uses one arena per input line.

## Testing

The parity suite runs it against the pgvector `pgvector/pgvector:0.8.7-pg17` image. The result is in `src/pgvector/PROVENANCE.md`, "Parity and kernel results".
