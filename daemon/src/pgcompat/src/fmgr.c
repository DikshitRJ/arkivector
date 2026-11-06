/*
 * pgcompat/src/fmgr.c: DirectFunctionCall* / FunctionCall*Coll. See fmgr.h.
 * ARKIVECTOR-owned.
 */
#include "postgres.h"

#include "fmgr.h"

static Datum call(PGFunction fn, FmgrInfo *flinfo, Oid collation, int nargs, Datum a1, Datum a2, Datum a3)
{
    LOCAL_FCINFO(fcinfo, 3);
    InitFunctionCallInfoData(*fcinfo, flinfo, (short) nargs, collation, NULL, NULL);
    fcinfo->args[0].value = a1;
    fcinfo->args[0].isnull = false;
    fcinfo->args[1].value = a2;
    fcinfo->args[1].isnull = false;
    fcinfo->args[2].value = a3;
    fcinfo->args[2].isnull = false;
    Datum result = fn(fcinfo);
    if (fcinfo->isnull) {
        elog(ERROR, "function %p returned NULL", (void *) fn);
    }
    return result;
}

Datum DirectFunctionCall1Coll(PGFunction func, Oid collation, Datum arg1)
{
    return call(func, NULL, collation, 1, arg1, 0, 0);
}

Datum DirectFunctionCall2Coll(PGFunction func, Oid collation, Datum arg1, Datum arg2)
{
    return call(func, NULL, collation, 2, arg1, arg2, 0);
}

Datum DirectFunctionCall3Coll(PGFunction func, Oid collation, Datum arg1, Datum arg2, Datum arg3)
{
    return call(func, NULL, collation, 3, arg1, arg2, arg3);
}

Datum FunctionCall0Coll(FmgrInfo *flinfo, Oid collation)
{
    return call(flinfo->fn_addr, flinfo, collation, 0, 0, 0, 0);
}

Datum FunctionCall1Coll(FmgrInfo *flinfo, Oid collation, Datum arg1)
{
    return call(flinfo->fn_addr, flinfo, collation, 1, arg1, 0, 0);
}

Datum FunctionCall2Coll(FmgrInfo *flinfo, Oid collation, Datum arg1, Datum arg2)
{
    return call(flinfo->fn_addr, flinfo, collation, 2, arg1, arg2, 0);
}

Datum FunctionCall3Coll(FmgrInfo *flinfo, Oid collation, Datum arg1, Datum arg2, Datum arg3)
{
    return call(flinfo->fn_addr, flinfo, collation, 3, arg1, arg2, arg3);
}
