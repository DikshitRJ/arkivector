/*
 * utils/float.h (pgcompat stub): the float error helpers from PostgreSQL 17
 * src/include/utils/float.h used by pgvector (bodies in misc.c copy float.c).
 * ARKIVECTOR-owned.
 */
#ifndef FLOAT_H
#define FLOAT_H
#include <math.h>
#include "postgres.h"
extern void float_overflow_error(void) pg_attribute_noreturn();
extern void float_underflow_error(void) pg_attribute_noreturn();
extern void float_zero_divide_error(void) pg_attribute_noreturn();
#endif
