/*
 * storage/itemptr.h (pgcompat stub): PostgreSQL 17's 6-byte heap TID, kept with the
 * exact layout so pgvector's HnswElementData (heaptids[10]) is unchanged.
 * Arkivector stores its document id in it: block number = docid, offset = 1
 * (av_docid_to_tid / av_tid_to_docid). ARKIVECTOR-owned.
 */
#ifndef ITEMPTR_H
#define ITEMPTR_H

typedef struct ItemPointerData {
    BlockIdData ip_blkid;
    OffsetNumber ip_posid;
} __attribute__((packed, aligned(2))) ItemPointerData;

typedef ItemPointerData *ItemPointer;

StaticAssertDecl(sizeof(ItemPointerData) == 6, "ItemPointerData must be 6 bytes");

static inline bool ItemPointerIsValid(const ItemPointerData *pointer)
{
    return PointerIsValid(pointer) && pointer->ip_posid != 0;
}
static inline BlockNumber ItemPointerGetBlockNumberNoCheck(const ItemPointerData *pointer)
{
    return BlockIdGetBlockNumber(&pointer->ip_blkid);
}
static inline BlockNumber ItemPointerGetBlockNumber(const ItemPointerData *pointer)
{
    return ItemPointerGetBlockNumberNoCheck(pointer);
}
static inline OffsetNumber ItemPointerGetOffsetNumberNoCheck(const ItemPointerData *pointer)
{
    return pointer->ip_posid;
}
static inline OffsetNumber ItemPointerGetOffsetNumber(const ItemPointerData *pointer)
{
    return pointer->ip_posid;
}
static inline void ItemPointerSet(ItemPointerData *pointer, BlockNumber blockNumber, OffsetNumber offNum)
{
    BlockIdSet(&pointer->ip_blkid, blockNumber);
    pointer->ip_posid = offNum;
}
static inline void ItemPointerSetBlockNumber(ItemPointerData *pointer, BlockNumber blockNumber)
{
    BlockIdSet(&pointer->ip_blkid, blockNumber);
}
static inline void ItemPointerSetOffsetNumber(ItemPointerData *pointer, OffsetNumber offsetNumber)
{
    pointer->ip_posid = offsetNumber;
}
static inline void ItemPointerCopy(const ItemPointerData *fromPointer, ItemPointerData *toPointer)
{
    *toPointer = *fromPointer;
}
static inline void ItemPointerSetInvalid(ItemPointerData *pointer)
{
    BlockIdSet(&pointer->ip_blkid, InvalidBlockNumber);
    pointer->ip_posid = InvalidOffsetNumber;
}
static inline bool ItemPointerEquals(const ItemPointerData *a, const ItemPointerData *b)
{
    return BlockIdEquals(&a->ip_blkid, &b->ip_blkid) && a->ip_posid == b->ip_posid;
}
static inline int32 ItemPointerCompare(const ItemPointerData *arg1, const ItemPointerData *arg2)
{
    BlockNumber b1 = ItemPointerGetBlockNumberNoCheck(arg1);
    BlockNumber b2 = ItemPointerGetBlockNumberNoCheck(arg2);
    if (b1 < b2)
        return -1;
    else if (b1 > b2)
        return 1;
    else if (arg1->ip_posid < arg2->ip_posid)
        return -1;
    else if (arg1->ip_posid > arg2->ip_posid)
        return 1;
    return 0;
}

/* Arkivector docid <-> TID mapping. */
static inline void av_docid_to_tid(uint32 docid, ItemPointerData *tid)
{
    ItemPointerSet(tid, (BlockNumber) docid, (OffsetNumber) 1);
}
static inline uint32 av_tid_to_docid(const ItemPointerData *tid)
{
    return (uint32) ItemPointerGetBlockNumberNoCheck(tid);
}

#endif /* ITEMPTR_H */
