/*
 * parser/scansup.h (pgcompat, mode S): scanner_isspace() from PostgreSQL 17
 * src/backend/parser/scansup.c (body copied), used by pgvector's *_in parsers.
 */
#ifndef SCANSUP_H
#define SCANSUP_H
#include "postgres.h"

static inline bool scanner_isspace(char ch)
{
    /* This must match scan.l's list of {space} characters */
    if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' || ch == '\f')
        return true;
    return false;
}
#endif
