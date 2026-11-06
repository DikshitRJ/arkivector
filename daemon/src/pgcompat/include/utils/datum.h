/*
 * utils/datum.h (pgcompat stub): datumIsEqual() as in PostgreSQL 17
 * src/backend/utils/adt/datum.c for the cases pgvector uses (by-reference varlena,
 * typLen -1; and by-value). ARKIVECTOR-owned.
 */
#ifndef DATUM_H
#define DATUM_H
#include "postgres.h"
#include "varatt.h"
static inline bool datumIsEqual(Datum value1, Datum value2, bool typByVal, int typLen)
{
    if (typByVal)
        return value1 == value2;
    if (typLen == -1) {
        Size size1 = VARSIZE_ANY(DatumGetPointer(value1));
        Size size2 = VARSIZE_ANY(DatumGetPointer(value2));
        if (size1 != size2)
            return false;
        return memcmp(DatumGetPointer(value1), DatumGetPointer(value2), size1) == 0;
    }
    return memcmp(DatumGetPointer(value1), DatumGetPointer(value2), (Size) typLen) == 0;
}
#endif
