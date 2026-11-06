# daemon/src/pgcompat/include/storage/itemptr.h

## Purpose

Stub (PostgreSQL layout). Declares `ItemPointerData`, a 6-byte heap TID. The HNSW element stores it. Declares our helpers `av_docid_to_tid` and `av_tid_to_docid`.

## Ownership and thread safety

Header only.
