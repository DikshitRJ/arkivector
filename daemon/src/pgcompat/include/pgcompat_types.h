/*
 * pgcompat_types.h (pgcompat, ARKIVECTOR-owned): backend types that copied headers
 * (hnsw.h, vector.h, ...) mention only in prototypes of functions Arkivector never
 * calls (disk/WAL/AM code). They are opaque pointers or plain scalars so the
 * prototypes compile. Types whose layout matters live in their own headers:
 * storage/itemptr.h, storage/lwlock.h, storage/s_lock.h, storage/bufpage.h.
 */
#ifndef PGCOMPAT_TYPES_H
#define PGCOMPAT_TYPES_H

typedef struct RelationData *Relation;
typedef Relation *RelationPtr;
typedef struct IndexScanDescData *IndexScanDesc;
typedef struct TableScanDescData *TableScanDesc;
typedef struct ParallelTableScanDescData *ParallelTableScanDesc;
typedef struct IndexInfo IndexInfo;
typedef struct ParallelContext ParallelContext;
typedef struct dsm_segment dsm_segment;
typedef struct shm_toc shm_toc;
typedef struct SnapshotData *Snapshot;
typedef struct IndexBulkDeleteResult IndexBulkDeleteResult;
typedef struct IndexVacuumInfo IndexVacuumInfo;
typedef struct IndexBuildResult IndexBuildResult;
typedef struct BufferAccessStrategyData *BufferAccessStrategy;
typedef struct ScanKeyData *ScanKey;
typedef struct GenericXLogState GenericXLogState;
typedef struct ReadStream ReadStream;
typedef struct ArrayType ArrayType;
typedef struct StringInfoData *StringInfo;
typedef struct HeapTupleData *HeapTuple;
typedef struct TupleDescData *TupleDesc;
typedef struct IndexTupleData *IndexTuple;
typedef struct PlannerInfo PlannerInfo;
typedef struct IndexPath IndexPath;
typedef struct ExprContext ExprContext;



typedef int Buffer;
#define InvalidBuffer 0
typedef enum ForkNumber {
    InvalidForkNumber = -1,
    MAIN_FORKNUM = 0,
    FSM_FORKNUM,
    VISIBILITYMAP_FORKNUM,
    INIT_FORKNUM
} ForkNumber;
typedef enum ScanDirection {
    BackwardScanDirection = -1,
    NoMovementScanDirection = 0,
    ForwardScanDirection = 1
} ScanDirection;
typedef enum IndexUniqueCheck {
    UNIQUE_CHECK_NO,
    UNIQUE_CHECK_YES,
    UNIQUE_CHECK_PARTIAL,
    UNIQUE_CHECK_EXISTING
} IndexUniqueCheck;

typedef struct ConditionVariable {
    int unused;
} ConditionVariable;

#include "storage/block.h"
#include "storage/off.h"
#include "storage/itemptr.h"
#include "storage/s_lock.h"
#include "storage/lwlock.h"
#include "storage/bufpage.h"

typedef bool (*IndexBulkDeleteCallback)(ItemPointer itemptr, void *state);

#endif /* PGCOMPAT_TYPES_H */
