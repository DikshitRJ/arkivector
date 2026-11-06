/*
 * utils/builtins.h (pgcompat stub): pg_ltoa() (PostgreSQL 17 numutils.c semantics:
 * write the decimal form of an int32 plus NUL, return the length without NUL; the
 * buffer must hold 12 bytes). Used by sparsevec_out. ARKIVECTOR-owned.
 */
#ifndef AV_STUB_UTILS_BUILTINS_H
#define AV_STUB_UTILS_BUILTINS_H
#include "postgres.h"
extern int pg_ltoa(int32 value, char *a);
#endif
