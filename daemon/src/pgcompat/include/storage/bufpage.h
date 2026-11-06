/*
 * storage/bufpage.h (pgcompat stub): only the sizes hnsw.h uses in its page-size
 * macros (HnswGetMaxLevel, HNSW_MAX_SIZE). Values equal PostgreSQL 17 so the
 * derived constants (e.g. max graph level) match pgvector exactly. ARKIVECTOR-owned.
 */
#ifndef BUFPAGE_H
#define BUFPAGE_H
typedef char *Page;
typedef struct ItemIdData {
    unsigned lp_off : 15, lp_flags : 2, lp_len : 15;
} ItemIdData;
typedef ItemIdData *ItemId;
typedef Pointer Item;
/* sizeof(PageHeaderData) up to pd_linp: 24 bytes in PostgreSQL 17 */
#define SizeOfPageHeaderData ((Size) 24)
StaticAssertDecl(sizeof(ItemIdData) == 4, "ItemIdData must be 4 bytes");
#endif
