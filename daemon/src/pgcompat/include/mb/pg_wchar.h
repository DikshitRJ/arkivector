/* mb/pg_wchar.h (pgcompat stub): pg_mblen_cstr() for error messages in bit_in. UTF-8 only. ARKIVECTOR-owned. */
#ifndef PG_WCHAR_H
#define PG_WCHAR_H
#include "postgres.h"
static inline int pg_mblen_cstr(const char *mbstr)
{
    unsigned char c = (unsigned char) *mbstr;
    int len = (c & 0x80) == 0 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 1;
    for (int i = 1; i < len; i++)
        if (mbstr[i] == '\0')
            return i;
    return len;
}
#endif
