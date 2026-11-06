/*
 * varatt.h (pgcompat stub): varlena header macros. Arkivector structs always use a
 * plain 4-byte length header (PostgreSQL's 4-byte uncompressed form on little-endian:
 * the stored value is len << 2). Never TOASTed or compressed, so detoasting is the
 * identity. ARKIVECTOR-owned.
 */
#ifndef VARATT_H
#define VARATT_H

typedef union {
    struct {
        uint32 va_header;
        char va_data[FLEXIBLE_ARRAY_MEMBER];
    } va_4byte;
} varattrib_4b;

#define VARATT_IS_EXTENDED(PTR)   false
#define VARATT_IS_COMPRESSED(PTR) false
#define VARATT_IS_EXTERNAL(PTR)   false
#define VARATT_IS_SHORT(PTR)      false
#define VARATT_IS_4B_U(PTR)       true

#define VARSIZE_4B(PTR)          ((((const varattrib_4b *) (PTR))->va_4byte.va_header >> 2) & 0x3FFFFFFF)
#define SET_VARSIZE_4B(PTR, len) (((varattrib_4b *) (PTR))->va_4byte.va_header = (((uint32) (len)) << 2))

#define VARSIZE(PTR)           VARSIZE_4B(PTR)
#define SET_VARSIZE(PTR, len)  SET_VARSIZE_4B(PTR, len)
#define VARSIZE_ANY(PTR)       VARSIZE_4B(PTR)
#define VARSIZE_ANY_EXHDR(PTR) (VARSIZE_4B(PTR) - VARHDRSZ)
#define VARSIZE_EXHDR          VARSIZE_ANY_EXHDR
#define VARDATA(PTR)           (((varattrib_4b *) (PTR))->va_4byte.va_data)
#define VARDATA_ANY(PTR)       VARDATA(PTR)

#endif /* VARATT_H */
