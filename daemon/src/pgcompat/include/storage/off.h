/* storage/off.h (pgcompat stub, PostgreSQL 17): OffsetNumber. ARKIVECTOR-owned. */
#ifndef OFF_H
#define OFF_H
typedef uint16 OffsetNumber;
#define InvalidOffsetNumber ((OffsetNumber) 0)
#define FirstOffsetNumber ((OffsetNumber) 1)
#define MaxOffsetNumber ((OffsetNumber) (BLCKSZ / sizeof(ItemIdData)))
#define OffsetNumberIsValid(offsetNumber) \
    ((bool) ((offsetNumber != InvalidOffsetNumber) && (offsetNumber <= MaxOffsetNumber)))
#define OffsetNumberNext(offsetNumber) ((OffsetNumber) (1 + (offsetNumber)))
#endif
