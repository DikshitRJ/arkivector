/* storage/bufmgr.h (pgcompat stub): only BufferIsValid, referenced by dead disk branches of HnswSearchLayer. ARKIVECTOR-owned. */
#ifndef BUFMGR_H
#define BUFMGR_H
#include "postgres.h"
static inline bool BufferIsValid(Buffer bufnum)
{
    return bufnum != InvalidBuffer;
}
#endif
