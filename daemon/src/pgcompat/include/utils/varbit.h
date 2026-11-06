/*
 * utils/varbit.h (pgcompat, mode S): the VarBit struct and access macros from
 * PostgreSQL 17 src/include/utils/varbit.h (struct + macros only; no SQL
 * function declarations). ARKIVECTOR-owned subset.
 */
#ifndef VARBIT_H
#define VARBIT_H

#include <limits.h>

#include "fmgr.h"
#include "varatt.h"

typedef struct
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	int32		bit_len;		/* number of valid bits */
	bits8		bit_dat[FLEXIBLE_ARRAY_MEMBER]; /* bit string, most sig. byte
												 * first */
} VarBit;

static inline VarBit *
DatumGetVarBitP(Datum X)
{
	return (VarBit *) PG_DETOAST_DATUM(X);
}

static inline Datum
VarBitPGetDatum(const VarBit *X)
{
	return PointerGetDatum(X);
}

#define PG_GETARG_VARBIT_P(n)		DatumGetVarBitP(PG_GETARG_DATUM(n))
#define PG_RETURN_VARBIT_P(x)		return VarBitPGetDatum(x)

#define VARBITHDRSZ			sizeof(int32)
#define VARBITLEN(PTR)		(((VarBit *) (PTR))->bit_len)
#define VARBITS(PTR)		(((VarBit *) (PTR))->bit_dat)
#define VARBITBYTES(PTR)	(VARSIZE(PTR) - VARHDRSZ - VARBITHDRSZ)
#define VARBITPAD(PTR)		(VARBITBYTES(PTR)*BITS_PER_BYTE - VARBITLEN(PTR))
#define VARBITTOTALLEN(BITLEN)	(((BITLEN) + BITS_PER_BYTE-1)/BITS_PER_BYTE + \
								 VARHDRSZ + VARBITHDRSZ)
#define VARBITMAXLEN		(INT_MAX - BITS_PER_BYTE + 1)
#define VARBITEND(PTR)		(((bits8 *) (PTR)) + VARSIZE(PTR))
#define BITMASK				0xFF

#ifndef BITS_PER_BYTE
#define BITS_PER_BYTE 8
#endif

#endif
