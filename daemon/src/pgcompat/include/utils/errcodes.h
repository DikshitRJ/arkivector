/*
 * utils/errcodes.h (pgcompat stub): the SQLSTATE codes referenced by copied code.
 * PostgreSQL generates this file from src/backend/utils/errcodes.txt; values below
 * are taken from REL_17_11. ARKIVECTOR-owned.
 */
#ifndef AV_ERRCODES_H
#define AV_ERRCODES_H

#define ERRCODE_SUCCESSFUL_COMPLETION MAKE_SQLSTATE('0', '0', '0', '0', '0')
#define ERRCODE_WARNING MAKE_SQLSTATE('0', '1', '0', '0', '0')
#define ERRCODE_FEATURE_NOT_SUPPORTED MAKE_SQLSTATE('0', 'A', '0', '0', '0')
#define ERRCODE_DATA_EXCEPTION MAKE_SQLSTATE('2', '2', '0', '0', '0')
#define ERRCODE_STRING_DATA_RIGHT_TRUNCATION MAKE_SQLSTATE('2', '2', '0', '0', '1')
#define ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE MAKE_SQLSTATE('2', '2', '0', '0', '3')
#define ERRCODE_NULL_VALUE_NOT_ALLOWED MAKE_SQLSTATE('2', '2', '0', '0', '4')
#define ERRCODE_INVALID_PARAMETER_VALUE MAKE_SQLSTATE('2', '2', '0', '2', '3')
#define ERRCODE_INVALID_TEXT_REPRESENTATION MAKE_SQLSTATE('2', '2', 'P', '0', '2')
#define ERRCODE_INVALID_BINARY_REPRESENTATION MAKE_SQLSTATE('2', '2', 'P', '0', '3')
#define ERRCODE_STRING_DATA_LENGTH_MISMATCH MAKE_SQLSTATE('2', '2', '0', '2', '6')
#define ERRCODE_ARRAY_SUBSCRIPT_ERROR MAKE_SQLSTATE('2', '2', '0', '2', 'E')
#define ERRCODE_DIVISION_BY_ZERO MAKE_SQLSTATE('2', '2', '0', '1', '2')
#define ERRCODE_INVALID_OBJECT_DEFINITION MAKE_SQLSTATE('4', '2', 'P', '1', '7')
#define ERRCODE_PROGRAM_LIMIT_EXCEEDED MAKE_SQLSTATE('5', '4', '0', '0', '0')
#define ERRCODE_OUT_OF_MEMORY MAKE_SQLSTATE('5', '3', '2', '0', '0')
#define ERRCODE_QUERY_CANCELED MAKE_SQLSTATE('5', '7', '0', '1', '4')
#define ERRCODE_INTERNAL_ERROR MAKE_SQLSTATE('X', 'X', '0', '0', '0')
#define ERRCODE_DATA_CORRUPTED MAKE_SQLSTATE('X', 'X', '0', '0', '1')

#endif
