/* storage/block.h (pgcompat stub, PostgreSQL 17 layout): BlockNumber and BlockIdData. ARKIVECTOR-owned. */
#ifndef BLOCK_H
#define BLOCK_H
typedef uint32 BlockNumber;
#define InvalidBlockNumber ((BlockNumber) 0xFFFFFFFF)
#define MaxBlockNumber ((BlockNumber) 0xFFFFFFFE)
typedef struct BlockIdData {
    uint16 bi_hi;
    uint16 bi_lo;
} BlockIdData;
typedef BlockIdData *BlockId;
static inline bool BlockNumberIsValid(BlockNumber blockNumber)
{
    return blockNumber != InvalidBlockNumber;
}
static inline void BlockIdSet(BlockIdData *blockId, BlockNumber blockNumber)
{
    blockId->bi_hi = (uint16) (blockNumber >> 16);
    blockId->bi_lo = (uint16) (blockNumber & 0xffff);
}
static inline bool BlockIdEquals(const BlockIdData *blockId1, const BlockIdData *blockId2)
{
    return (blockId1->bi_hi == blockId2->bi_hi && blockId1->bi_lo == blockId2->bi_lo);
}
static inline BlockNumber BlockIdGetBlockNumber(const BlockIdData *blockId)
{
    return (((BlockNumber) blockId->bi_hi) << 16) | ((BlockNumber) blockId->bi_lo);
}
#endif
