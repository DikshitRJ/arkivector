/*
 * fmgr.h (pgcompat stub): the function-manager calling convention used by copied
 * pgvector SQL-callable functions. A "PG function" is a plain C function taking a
 * FunctionCallInfo; FmgrInfo is just the function pointer. Mirrors PostgreSQL 17
 * names. ARKIVECTOR-owned. See STUBS.md.
 */
#ifndef FMGR_H
#define FMGR_H

typedef struct FunctionCallInfoBaseData *FunctionCallInfo;
typedef Datum (*PGFunction)(FunctionCallInfo fcinfo);

typedef struct FmgrInfo {
    PGFunction fn_addr;
    Oid fn_oid;
    short fn_nargs;
    bool fn_strict;
    bool fn_retset;
    void *fn_extra;
    MemoryContext fn_mcxt;
} FmgrInfo;

#define FUNC_MAX_ARGS 8

typedef struct FunctionCallInfoBaseData {
    FmgrInfo *flinfo;
    void *context;
    void *resultinfo;
    Oid fncollation;
    bool isnull;
    short nargs;
    NullableDatum args[FUNC_MAX_ARGS];
} FunctionCallInfoBaseData;

#define SizeForFunctionCallInfo(nargs) sizeof(FunctionCallInfoBaseData)
#define LOCAL_FCINFO(name, nargs)        \
    FunctionCallInfoBaseData name##data; \
    FunctionCallInfo name = &name##data

#define InitFunctionCallInfoData(Fcinfo, Flinfo, Nargs, Collation, Context, Resultinfo) \
    do {                                                                                \
        (Fcinfo).flinfo = (Flinfo);                                                     \
        (Fcinfo).context = (Context);                                                   \
        (Fcinfo).resultinfo = (Resultinfo);                                             \
        (Fcinfo).fncollation = (Collation);                                             \
        (Fcinfo).isnull = false;                                                        \
        (Fcinfo).nargs = (Nargs);                                                       \
    } while (0)

#define PG_FUNCTION_ARGS              FunctionCallInfo fcinfo
#define PG_FUNCTION_INFO_V1(funcname) extern Datum funcname(PG_FUNCTION_ARGS)
#define PG_MODULE_MAGIC               extern int no_such_variable
#define PG_MODULE_MAGIC_EXT(...)      extern int no_such_variable

#define PG_NARGS()         (fcinfo->nargs)
#define PG_ARGISNULL(n)    (fcinfo->args[n].isnull)
#define PG_GET_COLLATION() (fcinfo->fncollation)

#define PG_DETOAST_DATUM(datum)      ((struct varlena *) DatumGetPointer(datum))
#define PG_DETOAST_DATUM_COPY(datum) PG_DETOAST_DATUM(datum)
#define PG_FREE_IF_COPY(ptr, n)      ((void) 0)

#define PG_GETARG_DATUM(n)   (fcinfo->args[n].value)
#define PG_GETARG_INT32(n)   DatumGetInt32(PG_GETARG_DATUM(n))
#define PG_GETARG_UINT32(n)  DatumGetUInt32(PG_GETARG_DATUM(n))
#define PG_GETARG_INT16(n)   DatumGetInt16(PG_GETARG_DATUM(n))
#define PG_GETARG_INT64(n)   DatumGetInt64(PG_GETARG_DATUM(n))
#define PG_GETARG_BOOL(n)    DatumGetBool(PG_GETARG_DATUM(n))
#define PG_GETARG_OID(n)     DatumGetObjectId(PG_GETARG_DATUM(n))
#define PG_GETARG_POINTER(n) DatumGetPointer(PG_GETARG_DATUM(n))
#define PG_GETARG_CSTRING(n) DatumGetCString(PG_GETARG_DATUM(n))
#define PG_GETARG_FLOAT4(n)  DatumGetFloat4(PG_GETARG_DATUM(n))
#define PG_GETARG_FLOAT8(n)  DatumGetFloat8(PG_GETARG_DATUM(n))
#define PG_GETARG_BYTEA_P(n) ((bytea *) PG_GETARG_POINTER(n))

#define PG_RETURN_NULL()       \
    do {                       \
        fcinfo->isnull = true; \
        return (Datum) 0;      \
    } while (0)
#define PG_RETURN_VOID()     return (Datum) 0
#define PG_RETURN_DATUM(x)   return (x)
#define PG_RETURN_INT32(x)   return Int32GetDatum(x)
#define PG_RETURN_UINT32(x)  return UInt32GetDatum(x)
#define PG_RETURN_INT16(x)   return Int16GetDatum(x)
#define PG_RETURN_INT64(x)   return Int64GetDatum(x)
#define PG_RETURN_BOOL(x)    return BoolGetDatum(x)
#define PG_RETURN_OID(x)     return ObjectIdGetDatum(x)
#define PG_RETURN_POINTER(x) return PointerGetDatum(x)
#define PG_RETURN_CSTRING(x) return CStringGetDatum(x)
#define PG_RETURN_FLOAT4(x)  return Float4GetDatum(x)
#define PG_RETURN_FLOAT8(x)  return Float8GetDatum(x)
#define PG_RETURN_BYTEA_P(x) PG_RETURN_POINTER(x)

/* Direct calls (no FmgrInfo). Raise ERROR if the callee returns NULL, as in PostgreSQL. */
extern Datum DirectFunctionCall1Coll(PGFunction func, Oid collation, Datum arg1);
extern Datum DirectFunctionCall2Coll(PGFunction func, Oid collation, Datum arg1, Datum arg2);
extern Datum DirectFunctionCall3Coll(PGFunction func, Oid collation, Datum arg1, Datum arg2, Datum arg3);
#define DirectFunctionCall1(func, arg1)       DirectFunctionCall1Coll(func, InvalidOid, arg1)
#define DirectFunctionCall2(func, arg1, arg2) DirectFunctionCall2Coll(func, InvalidOid, arg1, arg2)
#define DirectFunctionCall3(func, arg1, arg2, arg3) \
    DirectFunctionCall3Coll(func, InvalidOid, arg1, arg2, arg3)

/* Calls through FmgrInfo. */
extern Datum FunctionCall0Coll(FmgrInfo *flinfo, Oid collation);
extern Datum FunctionCall1Coll(FmgrInfo *flinfo, Oid collation, Datum arg1);
extern Datum FunctionCall2Coll(FmgrInfo *flinfo, Oid collation, Datum arg1, Datum arg2);
extern Datum FunctionCall3Coll(FmgrInfo *flinfo, Oid collation, Datum arg1, Datum arg2, Datum arg3);
#define FunctionCall1(flinfo, arg1)       FunctionCall1Coll(flinfo, InvalidOid, arg1)
#define FunctionCall2(flinfo, arg1, arg2) FunctionCall2Coll(flinfo, InvalidOid, arg1, arg2)

/* Arkivector helper: fill an FmgrInfo for a C function. */
static inline void av_fmgr_info(FmgrInfo *finfo, PGFunction fn)
{
    memset(finfo, 0, sizeof(*finfo));
    finfo->fn_addr = fn;
    finfo->fn_mcxt = CurrentMemoryContext;
}

#endif /* FMGR_H */
