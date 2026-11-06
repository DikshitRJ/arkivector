/*
 * Copied from PostgreSQL REL_17_11 src/backend/utils/adt/varbit.c (commit 083ac03),
 * mode F: HEXDIG, VARBIT_CORRECTLY_PADDED, bit_in, varbit_out, bit_out (unmodified). pgvector's bit type is
 * PostgreSQL's bit, so its text input/output is PostgreSQL's code.
 * See ../STUBS.md.
 */
#include "postgres.h"

#include "fmgr.h"
#include "mb/pg_wchar.h"
#include "nodes/nodes.h"
#include "utils/varbit.h"
#include "varatt.h"

#define HEXDIG(z) ((z) < 10 ? ((z) + '0') : ((z) - 10 + 'A'))

/*
 * Many functions work byte-by-byte, so they have a pointer handy to the
 * last-plus-one byte, which saves a cycle or two.
 */
#define VARBIT_PAD_LAST(vb, ptr)                   \
    do {                                           \
        int32 pad_ = VARBITPAD(vb);                \
        Assert(pad_ >= 0 && pad_ < BITS_PER_BYTE); \
        if (pad_ > 0)                              \
            *((ptr) - 1) &= BITMASK << pad_;       \
    } while (0)

/* Assert proper padding of a bitstring */
#ifdef USE_ASSERT_CHECKING
#define VARBIT_CORRECTLY_PADDED(vb)                                                            \
    do {                                                                                       \
        int32 pad_ = VARBITPAD(vb);                                                            \
        Assert(pad_ >= 0 && pad_ < BITS_PER_BYTE);                                             \
        Assert(pad_ == 0 || (*(VARBITS(vb) + VARBITBYTES(vb) - 1) & ~(BITMASK << pad_)) == 0); \
    } while (0)
#else
#define VARBIT_CORRECTLY_PADDED(vb) ((void) 0)
#endif

PGDLLEXPORT Datum bit_in(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum varbit_out(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum bit_out(PG_FUNCTION_ARGS);

/*
 * bit_in -
 *	  converts a char string to the internal representation of a bitstring.
 *		  The length is determined by the number of bits required plus
 *		  VARHDRSZ bytes or from atttypmod.
 */
Datum bit_in(PG_FUNCTION_ARGS)
{
    char *input_string = PG_GETARG_CSTRING(0);
#ifdef NOT_USED
    Oid typelem = PG_GETARG_OID(1);
#endif
    int32 atttypmod = PG_GETARG_INT32(2);
    Node *escontext = fcinfo->context;
    VarBit *result;   /* The resulting bit string			  */
    char *sp;         /* pointer into the character string  */
    bits8 *r;         /* pointer into the result */
    int len,          /* Length of the whole data structure */
        bitlen,       /* Number of bits in the bit string   */
        slen;         /* Length of the input string		  */
    bool bit_not_hex; /* false = hex string  true = bit string */
    int bc;
    bits8 x = 0;

    /* Check that the first character is a b or an x */
    if (input_string[0] == 'b' || input_string[0] == 'B') {
        bit_not_hex = true;
        sp = input_string + 1;
    } else if (input_string[0] == 'x' || input_string[0] == 'X') {
        bit_not_hex = false;
        sp = input_string + 1;
    } else {
        /*
		 * Otherwise it's binary.  This allows things like cast('1001' as bit)
		 * to work transparently.
		 */
        bit_not_hex = true;
        sp = input_string;
    }

    /*
	 * Determine bitlength from input string.  MaxAllocSize ensures a regular
	 * input is small enough, but we must check hex input.
	 */
    slen = strlen(sp);
    if (bit_not_hex)
        bitlen = slen;
    else {
        if (slen > VARBITMAXLEN / 4)
            ereturn(escontext, (Datum) 0,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("bit string length exceeds the maximum allowed (%d)", VARBITMAXLEN)));
        bitlen = slen * 4;
    }

    /*
	 * Sometimes atttypmod is not supplied. If it is supplied we need to make
	 * sure that the bitstring fits.
	 */
    if (atttypmod <= 0)
        atttypmod = bitlen;
    else if (bitlen != atttypmod)
        ereturn(escontext, (Datum) 0,
                (errcode(ERRCODE_STRING_DATA_LENGTH_MISMATCH),
                 errmsg("bit string length %d does not match type bit(%d)", bitlen, atttypmod)));

    len = VARBITTOTALLEN(atttypmod);
    /* set to 0 so that *r is always initialised and string is zero-padded */
    result = (VarBit *) palloc0(len);
    SET_VARSIZE(result, len);
    VARBITLEN(result) = atttypmod;

    r = VARBITS(result);
    if (bit_not_hex) {
        /* Parse the bit representation of the string */
        /* We know it fits, as bitlen was compared to atttypmod */
        x = HIGHBIT;
        for (; *sp; sp++) {
            if (*sp == '1')
                *r |= x;
            else if (*sp != '0')
                ereturn(escontext, (Datum) 0,
                        (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                         errmsg("\"%.*s\" is not a valid binary digit", pg_mblen_cstr(sp), sp)));

            x >>= 1;
            if (x == 0) {
                x = HIGHBIT;
                r++;
            }
        }
    } else {
        /* Parse the hex representation of the string */
        for (bc = 0; *sp; sp++) {
            if (*sp >= '0' && *sp <= '9')
                x = (bits8) (*sp - '0');
            else if (*sp >= 'A' && *sp <= 'F')
                x = (bits8) (*sp - 'A') + 10;
            else if (*sp >= 'a' && *sp <= 'f')
                x = (bits8) (*sp - 'a') + 10;
            else
                ereturn(escontext, (Datum) 0,
                        (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                         errmsg("\"%.*s\" is not a valid hexadecimal digit", pg_mblen_cstr(sp), sp)));

            if (bc) {
                *r++ |= x;
                bc = 0;
            } else {
                *r = x << 4;
                bc = 1;
            }
        }
    }

    PG_RETURN_VARBIT_P(result);
}

/*
 * varbit_out -
 *	  Prints the string as bits to preserve length accurately
 *
 * XXX varbit_recv() and hex input to varbit_in() can load a value that this
 * cannot emit.  Consider using hex output for such values.
 */
Datum varbit_out(PG_FUNCTION_ARGS)
{
    VarBit *s = PG_GETARG_VARBIT_P(0);
    char *result, *r;
    bits8 *sp;
    bits8 x;
    int i, k, len;

    /* Assertion to help catch any bit functions that don't pad correctly */
    VARBIT_CORRECTLY_PADDED(s);

    len = VARBITLEN(s);
    result = (char *) palloc(len + 1);
    sp = VARBITS(s);
    r = result;
    for (i = 0; i <= len - BITS_PER_BYTE; i += BITS_PER_BYTE, sp++) {
        /* print full bytes */
        x = *sp;
        for (k = 0; k < BITS_PER_BYTE; k++) {
            *r++ = IS_HIGHBIT_SET(x) ? '1' : '0';
            x <<= 1;
        }
    }
    if (i < len) {
        /* print the last partial byte */
        x = *sp;
        for (k = i; k < len; k++) {
            *r++ = IS_HIGHBIT_SET(x) ? '1' : '0';
            x <<= 1;
        }
    }
    *r = '\0';

    PG_RETURN_CSTRING(result);
}

Datum bit_out(PG_FUNCTION_ARGS)
{
#if 1
    /* same as varbit output */
    return varbit_out(fcinfo);
#else

    /*
	 * This is how one would print a hex string, in case someone wants to
	 * write a formatting function.
	 */
    VarBit *s = PG_GETARG_VARBIT_P(0);
    char *result, *r;
    bits8 *sp;
    int i, len, bitlen;

    /* Assertion to help catch any bit functions that don't pad correctly */
    VARBIT_CORRECTLY_PADDED(s);

    bitlen = VARBITLEN(s);
    len = (bitlen + 3) / 4;
    result = (char *) palloc(len + 2);
    sp = VARBITS(s);
    r = result;
    *r++ = 'X';
    /* we cheat by knowing that we store full bytes zero padded */
    for (i = 0; i < len; i += 2, sp++) {
        *r++ = HEXDIG((*sp) >> 4);
        *r++ = HEXDIG((*sp) & 0xF);
    }

    /*
	 * Go back one step if we printed a hex number that was not part of the
	 * bitstring anymore
	 */
    if (i > len)
        r--;
    *r = '\0';

    PG_RETURN_CSTRING(result);
#endif
}
