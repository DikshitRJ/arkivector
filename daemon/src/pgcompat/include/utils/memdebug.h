/* utils/memdebug.h (pgcompat stub): Valgrind client-request macros as no-ops. ARKIVECTOR-owned. */
#ifndef MEMDEBUG_H
#define MEMDEBUG_H
#define VALGRIND_CHECK_MEM_IS_DEFINED(addr, size) \
    do {                                          \
    } while (0)
#define VALGRIND_CREATE_MEMPOOL(context, redzones, zeroed) \
    do {                                                   \
    } while (0)
#define VALGRIND_DESTROY_MEMPOOL(context) \
    do {                                  \
    } while (0)
#define VALGRIND_MAKE_MEM_DEFINED(addr, size) \
    do {                                      \
    } while (0)
#define VALGRIND_MAKE_MEM_NOACCESS(addr, size) \
    do {                                       \
    } while (0)
#define VALGRIND_MAKE_MEM_UNDEFINED(addr, size) \
    do {                                        \
    } while (0)
#define VALGRIND_MEMPOOL_ALLOC(context, addr, size) \
    do {                                            \
    } while (0)
#define VALGRIND_MEMPOOL_FREE(context, addr) \
    do {                                     \
    } while (0)
#define VALGRIND_MEMPOOL_CHANGE(context, optr, nptr, size) \
    do {                                                   \
    } while (0)
#endif
