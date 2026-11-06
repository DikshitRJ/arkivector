/*
 * postgres.h (pgcompat stub): primary include for copied backend code.
 * Mirrors PostgreSQL 17 src/include/postgres.h: Datum and its conversion helpers,
 * plus c.h, elog.h and palloc.h. ARKIVECTOR-owned. See STUBS.md.
 */
#ifndef POSTGRES_H
#define POSTGRES_H

#include "c.h"
#include "utils/elog.h"
#include "utils/palloc.h"

/* ---- Datum ---- */
typedef uintptr_t Datum;
typedef struct NullableDatum {
    Datum value;
    bool isnull;
} NullableDatum;

#define SIZEOF_DATUM SIZEOF_VOID_P

static inline bool DatumGetBool(Datum X)
{
    return (X != 0);
}
static inline Datum BoolGetDatum(bool X)
{
    return (Datum)(X ? 1 : 0);
}
static inline char DatumGetChar(Datum X)
{
    return (char)X;
}
static inline Datum CharGetDatum(char X)
{
    return (Datum)X;
}
static inline int16 DatumGetInt16(Datum X)
{
    return (int16)X;
}
static inline Datum Int16GetDatum(int16 X)
{
    return (Datum)X;
}
static inline uint16 DatumGetUInt16(Datum X)
{
    return (uint16)X;
}
static inline Datum UInt16GetDatum(uint16 X)
{
    return (Datum)X;
}
static inline int32 DatumGetInt32(Datum X)
{
    return (int32)X;
}
static inline Datum Int32GetDatum(int32 X)
{
    return (Datum)X;
}
static inline uint32 DatumGetUInt32(Datum X)
{
    return (uint32)X;
}
static inline Datum UInt32GetDatum(uint32 X)
{
    return (Datum)X;
}
static inline Oid DatumGetObjectId(Datum X)
{
    return (Oid)X;
}
static inline Datum ObjectIdGetDatum(Oid X)
{
    return (Datum)X;
}
static inline Pointer DatumGetPointer(Datum X)
{
    return (Pointer)X;
}
#define PointerGetDatum(X) ((Datum)(X))
static inline char *DatumGetCString(Datum X)
{
    return (char *)DatumGetPointer(X);
}
#define CStringGetDatum(X) PointerGetDatum(X)
static inline int64 DatumGetInt64(Datum X)
{
    return (int64)X;
}
static inline Datum Int64GetDatum(int64 X)
{
    return (Datum)X;
}
static inline uint64 DatumGetUInt64(Datum X)
{
    return (uint64)X;
}
static inline Datum UInt64GetDatum(uint64 X)
{
    return (Datum)X;
}
static inline float4 DatumGetFloat4(Datum X)
{
    union {
        int32 value;
        float4 retval;
    } myunion;
    myunion.value = DatumGetInt32(X);
    return myunion.retval;
}
static inline Datum Float4GetDatum(float4 X)
{
    union {
        float4 value;
        int32 retval;
    } myunion;
    myunion.value = X;
    return Int32GetDatum(myunion.retval);
}
static inline float8 DatumGetFloat8(Datum X)
{
    union {
        int64 value;
        float8 retval;
    } myunion;
    myunion.value = DatumGetInt64(X);
    return myunion.retval;
}
static inline Datum Float8GetDatum(float8 X)
{
    union {
        float8 value;
        int64 retval;
    } myunion;
    myunion.value = X;
    return Int64GetDatum(myunion.retval);
}
#define Int64GetDatumFast(X) Int64GetDatum(X)
#define Float8GetDatumFast(X) Float8GetDatum(X)

/* Common opaque backend types that copied headers mention in prototypes only. */
#include "pgcompat_types.h"

#endif /* POSTGRES_H */
