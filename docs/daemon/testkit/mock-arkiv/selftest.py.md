# daemon/testkit/mock-arkiv/selftest.py

## Purpose

Quick self-check of the mock. It starts `server.py` as a subprocess and checks its endpoints. It uses only the standard library.

## Usage

```
python selftest.py     # use a venv that has requirements.txt installed
```

## Behavior

The script starts the server on a free port, reads `READY <port>`, sends requests with `urllib`, and checks the answers. Only the first lines of the file were read for this document. The list of checks is not documented here yet.

## Ownership and thread safety

Test script. Single-threaded.

## Testing

This file is the test.
