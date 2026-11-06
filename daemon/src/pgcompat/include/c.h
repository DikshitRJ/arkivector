/*
 * c.h (pgcompat stub): the subset of PostgreSQL's src/include/c.h that the copied
 * pgvector and PostgreSQL files use. ARKIVECTOR-owned; names and semantics follow
 * PostgreSQL 17 so copied code compiles unmodified. See STUBS.md.
 */
#ifndef C_H
#define C_H

#include "pg_config.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---- compiler attributes ---- */
#define pg_attribute_unused() __attribute__((unused))
#define pg_attribute_always_inline __attribute__((always_inline)) inline
#define pg_noinline __attribute__((noinline))
#define pg_attribute_noreturn() __attribute__((noreturn))
#define pg_noreturn _Noreturn
#define pg_attribute_cold __attribute__((cold))
#define pg_attribute_hot __attribute__((hot))
#define pg_nodiscard __attribute__((warn_unused_result))
#define pg_attribute_printf(f, a) __attribute__((format(printf, f, a)))
#define pg_attribute_format_arg(a) __attribute__((format_arg(a)))
#define pg_attribute_packed() __attribute__((packed))
#define pg_attribute_aligned(a) __attribute__((aligned(a)))
#define pg_attribute_no_sanitize_address() __attribute__((no_sanitize("address")))
#define pg_attribute_no_sanitize_alignment() __attribute__((no_sanitize("alignment")))
#define pg_unreachable() __builtin_unreachable()
#define likely(x) __builtin_expect((x) != 0, 1)
#define unlikely(x) __builtin_expect((x) != 0, 0)

#define PGDLLIMPORT
#define PGDLLEXPORT __attribute__((visibility("default")))

#define CppAsString(identifier) #identifier
#define CppAsString2(x) CppAsString(x)
#define CppConcat(x, y) x##y

#define gettext(x) (x)
#define dgettext(d, x) (x)
#define ngettext(s, p, n) ((n) == 1 ? (s) : (p))
#define dngettext(d, s, p, n) ((n) == 1 ? (s) : (p))
#define _(x) (x)
#define gettext_noop(x) (x)

#define FLEXIBLE_ARRAY_MEMBER /* empty */

/* ---- integer types ---- */
typedef signed char int8;
typedef signed short int16;
typedef signed int int32;
typedef long int int64;
typedef unsigned char uint8;
typedef unsigned short uint16;
typedef unsigned int uint32;
typedef unsigned long int uint64;
typedef __int128 int128;
typedef unsigned __int128 uint128;

typedef uint8 bits8;
typedef uint16 bits16;
typedef uint32 bits32;

typedef float float4;
typedef double float8;

typedef size_t Size;
typedef unsigned int Index;
typedef signed int Offset;
typedef char *Pointer;

typedef unsigned int Oid;
#define InvalidOid ((Oid)0)
#define OidIsValid(objectId) ((bool)((objectId) != InvalidOid))

typedef uint32 TransactionId;
typedef uint32 SubTransactionId;
typedef uint32 CommandId;
typedef Oid regproc;
typedef regproc RegProcedure;

#define INT64CONST(x) INT64_C(x)
#define UINT64CONST(x) UINT64_C(x)
#define INT64_FORMAT "%" PRId64
#define UINT64_FORMAT "%" PRIu64
#define INT64_MODIFIER "l"

#define PG_INT8_MIN (-0x7F - 1)
#define PG_INT8_MAX (0x7F)
#define PG_UINT8_MAX (0xFF)
#define PG_INT16_MIN (-0x7FFF - 1)
#define PG_INT16_MAX (0x7FFF)
#define PG_UINT16_MAX (0xFFFF)
#define PG_INT32_MIN (-0x7FFFFFFF - 1)
#define PG_INT32_MAX (0x7FFFFFFF)
#define PG_UINT32_MAX (0xFFFFFFFFU)
#define PG_INT64_MIN (-INT64CONST(0x7FFFFFFFFFFFFFFF) - 1)
#define PG_INT64_MAX INT64CONST(0x7FFFFFFFFFFFFFFF)
#define PG_UINT64_MAX UINT64CONST(0xFFFFFFFFFFFFFFFF)

#define NAMEDATALEN 64
typedef struct nameData {
    char data[NAMEDATALEN];
} NameData;
typedef NameData *Name;

/* ---- varlena ---- */
struct varlena {
    char vl_len_[4];
    char vl_dat[FLEXIBLE_ARRAY_MEMBER];
};
#define VARHDRSZ ((int32)sizeof(int32))
typedef struct varlena bytea;
typedef struct varlena text;
typedef struct varlena BpChar;
typedef struct varlena VarChar;

/* ---- helpers ---- */
#define lengthof(array) (sizeof(array) / sizeof((array)[0]))
#ifndef Max
#define Max(x, y) ((x) > (y) ? (x) : (y))
#endif
#ifndef Min
#define Min(x, y) ((x) < (y) ? (x) : (y))
#endif
#define Abs(x) ((x) >= 0 ? (x) : -(x))

#define PointerIsValid(pointer) ((const void *)(pointer) != NULL)
#define PointerIsAligned(pointer, type) (((uintptr_t)(pointer) % (sizeof(type))) == 0)
#define OffsetToPointer(base, offset) ((void *)((char *)(base) + (offset)))
#define BoolIsValid(boolean) ((boolean) == false || (boolean) == true)

#define TYPEALIGN(ALIGNVAL, LEN) (((uintptr_t)(LEN) + ((ALIGNVAL) - 1)) & ~((uintptr_t)((ALIGNVAL) - 1)))
#define SHORTALIGN(LEN) TYPEALIGN(ALIGNOF_SHORT, (LEN))
#define INTALIGN(LEN) TYPEALIGN(ALIGNOF_INT, (LEN))
#define LONGALIGN(LEN) TYPEALIGN(ALIGNOF_LONG, (LEN))
#define DOUBLEALIGN(LEN) TYPEALIGN(ALIGNOF_DOUBLE, (LEN))
#define MAXALIGN(LEN) TYPEALIGN(MAXIMUM_ALIGNOF, (LEN))
#define BUFFERALIGN(LEN) TYPEALIGN(ALIGNOF_BUFFER, (LEN))
#define CACHELINEALIGN(LEN) TYPEALIGN(PG_CACHE_LINE_SIZE, (LEN))
#define TYPEALIGN_DOWN(ALIGNVAL, LEN) (((uintptr_t)(LEN)) & ~((uintptr_t)((ALIGNVAL) - 1)))
#define MAXALIGN_DOWN(LEN) TYPEALIGN_DOWN(MAXIMUM_ALIGNOF, (LEN))
#define TYPEALIGN64(ALIGNVAL, LEN) (((uint64)(LEN) + ((ALIGNVAL) - 1)) & ~((uint64)((ALIGNVAL) - 1)))
#define MAXALIGN64(LEN) TYPEALIGN64(MAXIMUM_ALIGNOF, (LEN))

#define unconstify(underlying_type, expr) ((underlying_type)(expr))
#define unvolatize(underlying_type, expr) ((underlying_type)(expr))

#define MemSet(start, val, len) memset((start), (val), (len))
#define MemSetAligned(start, val, len) memset((start), (val), (len))

/* ---- assertions ---- */
#ifdef NDEBUG
#define Assert(condition) ((void)true)
#define AssertMacro(condition) ((void)true)
#else
#define USE_ASSERT_CHECKING 1
#define Assert(condition) assert(condition)
#define AssertMacro(condition) ((void)assert(condition))
#endif
#define AssertArg(condition) Assert(condition)
#define AssertState(condition) Assert(condition)
#define AssertPointerAlignment(ptr, bndr) Assert(TYPEALIGN(bndr, (uintptr_t)(ptr)) == (uintptr_t)(ptr))

#define StaticAssertDecl(condition, errmessage) _Static_assert(condition, errmessage)
#define StaticAssertStmt(condition, errmessage) \
    do {                                        \
        _Static_assert(condition, errmessage);  \
    } while (0)
#define StaticAssertExpr(condition, errmessage) ((void)({ StaticAssertStmt(condition, errmessage); true; }))

#define AssertVariableIsOfType(varname, typename)                                        \
    StaticAssertStmt(__builtin_types_compatible_p(__typeof__(varname), typename),         \
                     CppAsString(varname) " does not have type " CppAsString(typename))
#define AssertVariableIsOfTypeMacro(varname, typename)                                   \
    (StaticAssertExpr(__builtin_types_compatible_p(__typeof__(varname), typename),        \
                      CppAsString(varname) " does not have type " CppAsString(typename)))

/* ---- misc ---- */
#define HIGHBIT (0x80)
#define IS_HIGHBIT_SET(ch) ((unsigned char)(ch) & HIGHBIT)
#define SQL_STR_DOUBLE(ch, escape_backslash) ((ch) == '\'' || ((ch) == '\\' && (escape_backslash)))
#define ESCAPE_STRING_SYNTAX 'E'
#define STATUS_OK (0)
#define STATUS_ERROR (-1)

#define pg_memory_barrier() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define pg_read_barrier() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define pg_write_barrier() __atomic_thread_fence(__ATOMIC_RELEASE)
#define pg_compiler_barrier() __asm__ __volatile__("" ::: "memory")

/* Arkivector uses ASCII-only identifiers; these mirror port.h helpers used by copied parsers. */
#include "port.h"

#endif /* C_H */
