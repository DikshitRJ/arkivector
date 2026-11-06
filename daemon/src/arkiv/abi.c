/*
 * arkiv/abi.c: Arkiv execute((uint8,bytes)[]) ABI encoder and decoder,
 * error catalogue decoder, and key prediction.
 */
#include "arkiv/abi.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "arkiv/keccak.h"

/* 22-error catalogue */
static const struct {
    uint8_t sel[4];
    const char *name;
} g_revert_catalogue[22] = {{{0xc2, 0xe5, 0x34, 0x7d}, "EmptyBatch"},
                            {{0x6b, 0xd8, 0x47, 0xc6}, "InvalidOpType"},
                            {{0x89, 0xbc, 0x94, 0x48}, "NonCanonicalOperationData"},
                            {{0x32, 0x06, 0x84, 0x6a}, "AttributesNotSorted"},
                            {{0xd7, 0xce, 0x98, 0x1c}, "TooManyAttributes"},
                            {{0x78, 0xa5, 0x85, 0x38}, "EmptyMutations"},
                            {{0x42, 0x50, 0x1b, 0xac}, "TombstoneInCreate"},
                            {{0x01, 0x9f, 0x59, 0xdf}, "TombstoneValueNotEmpty"},
                            {{0x79, 0xb2, 0xb3, 0x98}, "SystemAttributeNotWritable"},
                            {{0x63, 0x9f, 0xb7, 0xab}, "InvalidValueType"},
                            {{0xc1, 0x09, 0xae, 0xf4}, "Ident32Empty"},
                            {{0x27, 0x6f, 0x77, 0x98}, "Ident32InvalidByte"},
                            {{0x66, 0xa8, 0x9f, 0x2a}, "EntityNotFound"},
                            {{0x24, 0x11, 0xcf, 0x56}, "EntityExpired"},
                            {{0xa3, 0x22, 0x1e, 0xf2}, "NotOwner"},
                            {{0xd8, 0x58, 0xa8, 0x24}, "ReadOnlyEntity"},
                            {{0xdc, 0xc1, 0x86, 0x11}, "ReservedCreationFlags"},
                            {{0x26, 0x04, 0x9d, 0x1c}, "ExpiryDeadOnArrival"},
                            {{0xdd, 0x7e, 0x98, 0x6b}, "ExpiryBeyondMaxLifetime"},
                            {{0x33, 0x26, 0x8a, 0x0d}, "ExpiryNotExtended"},
                            {{0x87, 0x42, 0xc6, 0x7c}, "TransferToZeroAddress"},
                            {{0xe3, 0x24, 0xb1, 0x69}, "TransferToSelf"}};

const char *av_abi_decode_revert(const uint8_t *data, size_t len)
{
    if (data == NULL || len < 4)
        return NULL;
    for (size_t i = 0; i < 22; i++) {
        if (memcmp(data, g_revert_catalogue[i].sel, 4) == 0)
            return g_revert_catalogue[i].name;
    }
    return NULL;
}

const char *av_abi_revert_error_name(const uint8_t *data, size_t len)
{
    return av_abi_decode_revert(data, len);
}

void av_abi_predict_entity_key(uint64_t chain_id, const uint8_t contract_addr[20], const uint8_t owner[20],
                               uint64_t nonce, const uint8_t salt[16], uint8_t out_key[32])
{
    uint8_t packed[96];
    memset(packed, 0, 96);
    /* uint256 chainId (32 bytes, big-endian) */
    for (int i = 0; i < 8; i++) {
        packed[31 - i] = (uint8_t) ((chain_id >> (8 * i)) & 0xff);
    }
    /* address arkiv (20 bytes) */
    memcpy(packed + 32, contract_addr, 20);
    /* address owner (20 bytes) */
    memcpy(packed + 52, owner, 20);
    /* uint64 nonce (8 bytes, big-endian) */
    for (int i = 0; i < 8; i++) {
        packed[79 - i] = (uint8_t) ((nonce >> (8 * i)) & 0xff);
    }
    /* uint128 salt (16 bytes, big-endian) */
    memcpy(packed + 80, salt, 16);

    av_keccak256(packed, 96, out_key);
}

/* Dynamic byte buffer for ABI serialization */
typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
    bool oom;
} abi_buf;

static void buf_init(abi_buf *b, size_t initial_cap)
{
    b->cap = initial_cap > 0 ? initial_cap : 256;
    b->data = malloc(b->cap);
    b->len = 0;
    b->oom = (b->data == NULL);
}

static void buf_free(abi_buf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static bool buf_ensure(abi_buf *b, size_t extra)
{
    if (b->oom)
        return false;
    if (b->len + extra > b->cap) {
        size_t ncap = b->cap * 2;
        while (b->len + extra > ncap)
            ncap *= 2;
        uint8_t *nd = realloc(b->data, ncap);
        if (nd == NULL) {
            b->oom = true;
            return false;
        }
        b->data = nd;
        b->cap = ncap;
    }
    return true;
}

static void buf_write_raw(abi_buf *b, const void *ptr, size_t n)
{
    if (!buf_ensure(b, n))
        return;
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
}

static void buf_write_u32_be(abi_buf *b, uint32_t val)
{
    uint8_t raw[4];
    raw[0] = (uint8_t) ((val >> 24) & 0xff);
    raw[1] = (uint8_t) ((val >> 16) & 0xff);
    raw[2] = (uint8_t) ((val >> 8) & 0xff);
    raw[3] = (uint8_t) (val & 0xff);
    buf_write_raw(b, raw, 4);
}

static void buf_write_word_u64(abi_buf *b, uint64_t val)
{
    uint8_t w[32];
    memset(w, 0, 32);
    for (int i = 0; i < 8; i++) {
        w[31 - i] = (uint8_t) ((val >> (8 * i)) & 0xff);
    }
    buf_write_raw(b, w, 32);
}

static void buf_write_word_bytes(abi_buf *b, const uint8_t *src, size_t len)
{
    uint8_t w[32];
    memset(w, 0, 32);
    if (src != NULL && len > 0) {
        size_t copy_len = len < 32 ? len : 32;
        memcpy(w, src, copy_len);
    }
    buf_write_raw(b, w, 32);
}

static void buf_write_word_addr(abi_buf *b, const uint8_t addr[20])
{
    uint8_t w[32];
    memset(w, 0, 32);
    if (addr != NULL) {
        memcpy(w + 12, addr, 20);
    }
    buf_write_raw(b, w, 32);
}

static void buf_write_bytes_padded(abi_buf *b, const uint8_t *bytes, size_t len)
{
    buf_write_word_u64(b, len);
    if (len > 0 && bytes != NULL) {
        buf_write_raw(b, bytes, len);
    }
    size_t pad = (32 - (len % 32)) % 32;
    if (pad > 0) {
        uint8_t zeros[32] = {0};
        buf_write_raw(b, zeros, pad);
    }
}

static void buf_set_word_u64(abi_buf *b, size_t offset, uint64_t val)
{
    if (offset + 32 > b->len)
        return;
    uint8_t *w = b->data + offset;
    memset(w, 0, 32);
    for (int i = 0; i < 8; i++) {
        w[31 - i] = (uint8_t) ((val >> (8 * i)) & 0xff);
    }
}

/* Compare attribute names for ascending sort */
static int attr_cmp(const void *a, const void *b)
{
    const av_attr *x = (const av_attr *) a;
    const av_attr *y = (const av_attr *) b;
    return strncmp(x->name, y->name, AV_ATTR_NAME_MAX);
}

static void encode_attributes(abi_buf *b, const av_attr *attrs, size_t n_attrs)
{
    buf_write_word_u64(b, n_attrs);
    if (n_attrs == 0)
        return;

    /* Sort attributes in strictly ascending order by name */
    av_attr *sorted = malloc(n_attrs * sizeof(av_attr));
    if (sorted == NULL) {
        b->oom = true;
        return;
    }
    memcpy(sorted, attrs, n_attrs * sizeof(av_attr));
    qsort(sorted, n_attrs, sizeof(av_attr), attr_cmp);

    /* Attribute head offsets relative to the start of the Attribute[] array data */
    size_t head_start = b->len;
    for (size_t i = 0; i < n_attrs; i++) {
        buf_write_word_u64(b, 0); /* placeholder */
    }

    /* Encode each Attribute tuple: (bytes32 name, uint8 typeId, bytes value) */
    for (size_t i = 0; i < n_attrs; i++) {
        size_t tuple_start = b->len;
        size_t rel_offset = tuple_start - head_start;
        buf_set_word_u64(b, head_start + i * 32, rel_offset);

        /* 1. bytes32 name: left-aligned ASCII */
        uint8_t name_word[32];
        memset(name_word, 0, 32);
        size_t nlen = strlen(sorted[i].name);
        if (nlen > 32)
            nlen = 32;
        memcpy(name_word, sorted[i].name, nlen);
        buf_write_raw(b, name_word, 32);

        /* 2. uint8 typeId: (type & 0x7F) | (indexed ? 0x80 : 0) */
        uint8_t type_id = (uint8_t) ((sorted[i].type & 0x7f) | (sorted[i].indexed ? 0x80 : 0));
        buf_write_word_u64(b, type_id);

        /* 3. offset to bytes value: 3 * 32 = 96 */
        buf_write_word_u64(b, 96);

        /* 4. bytes value (length + padded content) */
        buf_write_bytes_padded(b, sorted[i].val, sorted[i].len);
    }

    free(sorted);
}

static void encode_op_payload(abi_buf *b, const av_mutation_op *op)
{
    switch (op->type) {
    case AV_OP_CREATE: {
        /* abi.encode(CreateOp): the struct is dynamic (Attribute[]), so a 0x20 head offset comes first.
         * Verified against Rabdi, which rejects payloads without it ("type check failed for offset"). */
        buf_write_word_u64(b, 32);
        /* uint128 salt */
        uint8_t salt_w[32];
        memset(salt_w, 0, 32);
        memcpy(salt_w + 16, op->u.create.salt, 16);
        buf_write_raw(b, salt_w, 32);

        /* uint64 expiresAt */
        buf_write_word_u64(b, op->u.create.expires_at);

        /* uint64 minLifetime */
        buf_write_word_u64(b, op->u.create.min_lifetime);

        /* uint8 creationFlags */
        buf_write_word_u64(b, op->u.create.creation_flags);

        /* offset to Attribute[] = 5 * 32 = 160 */
        buf_write_word_u64(b, 160);

        encode_attributes(b, op->u.create.attributes, op->u.create.n_attributes);
        break;
    }
    case AV_OP_PATCH: {
        buf_write_word_u64(b, 32); /* abi.encode(PatchOp): dynamic struct head offset */
        /* bytes32 entityKey */
        buf_write_word_bytes(b, op->u.patch.entity_key, 32);

        /* offset to Attribute[] = 2 * 32 = 64 */
        buf_write_word_u64(b, 64);

        encode_attributes(b, op->u.patch.mutations, op->u.patch.n_mutations);
        break;
    }
    case AV_OP_EXTEND_EXPIRY: {
        buf_write_word_bytes(b, op->u.extend.entity_key, 32);
        buf_write_word_u64(b, op->u.extend.expires_at);
        buf_write_word_u64(b, op->u.extend.min_lifetime);
        break;
    }
    case AV_OP_TRANSFER_OWNERSHIP: {
        buf_write_word_bytes(b, op->u.transfer.entity_key, 32);
        buf_write_word_addr(b, op->u.transfer.new_owner);
        break;
    }
    case AV_OP_DELETE: {
        buf_write_word_bytes(b, op->u.del.entity_key, 32);
        break;
    }
    }
}

av_status av_abi_encode_execute(const av_mutation_op *ops, size_t n_ops, av_arena *arena,
                                uint8_t **out_calldata, size_t *out_len, av_err *e)
{
    abi_buf b;
    buf_init(&b, 1024);

    /* 4-byte selector 0x49650044 */
    buf_write_u32_be(&b, AV_ARKIV_EXECUTE_SELECTOR);

    /* Head: offset to Operation[] = 32 */
    buf_write_word_u64(&b, 32);

    /* Array count */
    buf_write_word_u64(&b, n_ops);

    size_t head_start = b.len;
    for (size_t i = 0; i < n_ops; i++) {
        buf_write_word_u64(&b, 0); /* placeholder for offset */
    }

    for (size_t i = 0; i < n_ops; i++) {
        size_t tuple_start = b.len;
        size_t rel_offset = tuple_start - head_start;
        buf_set_word_u64(&b, head_start + i * 32, rel_offset);

        /* uint8 operation */
        buf_write_word_u64(&b, (uint64_t) ops[i].type);

        /* offset to bytes operationData = 64 */
        buf_write_word_u64(&b, 64);

        /* Build operationData payload */
        abi_buf op_buf;
        buf_init(&op_buf, 256);
        encode_op_payload(&op_buf, &ops[i]);
        if (op_buf.oom) {
            buf_free(&op_buf);
            buf_free(&b);
            av_err_set(e, "internal", "out of memory encoding op payload");
            return AV_ERR;
        }

        buf_write_bytes_padded(&b, op_buf.data, op_buf.len);
        buf_free(&op_buf);
    }

    if (b.oom) {
        buf_free(&b);
        av_err_set(e, "internal", "out of memory encoding execute calldata");
        return AV_ERR;
    }

    uint8_t *res = av_arena_alloc(arena, b.len, 0);
    if (res == NULL) {
        buf_free(&b);
        av_err_set(e, "internal", "arena out of memory for calldata");
        return AV_ERR;
    }
    memcpy(res, b.data, b.len);
    *out_calldata = res;
    *out_len = b.len;
    buf_free(&b);
    return AV_OK;
}

/* Helper reader for decoding */
static uint64_t read_u64(const uint8_t *p)
{
    uint64_t val = 0;
    for (int i = 0; i < 8; i++) {
        val = (val << 8) | p[24 + i];
    }
    return val;
}

/* An ABI offset/length word: the upper 24 bytes must be zero and the value must fit the buffer. */
static bool read_size(const uint8_t *p, size_t limit, size_t *out)
{
    for (int i = 0; i < 24; i++) {
        if (p[i] != 0)
            return false;
    }
    uint64_t v = read_u64(p);
    if (v > (uint64_t) limit)
        return false;
    *out = (size_t) v;
    return true;
}

/* True when [off, off + need) lies inside a buffer of len bytes (overflow-safe). */
static bool fits(size_t off, size_t need, size_t len)
{
    return off <= len && need <= len - off;
}

static bool decode_attributes(const uint8_t *data, size_t len, size_t offset, av_arena *arena,
                              av_attr **out_attrs, size_t *out_n)
{
    *out_attrs = NULL;
    *out_n = 0;
    size_t n;
    if (!fits(offset, 32, len) || !read_size(data + offset, len, &n))
        return false;
    if (n == 0)
        return true;
    size_t head_start = offset + 32;
    if (n > (len - head_start) / 32) /* every element needs a 32-byte head word */
        return false;

    av_attr *arr = av_arena_alloc(arena, n * sizeof(av_attr), 0);
    if (arr == NULL)
        return false;

    for (size_t i = 0; i < n; i++) {
        size_t tuple_rel;
        if (!read_size(data + head_start + i * 32, len, &tuple_rel) || !fits(head_start, tuple_rel, len))
            return false;
        size_t tuple_off = head_start + tuple_rel;
        if (!fits(tuple_off, 96, len))
            return false;

        /* 1. bytes32 name */
        memcpy(arr[i].name, data + tuple_off, 32);
        arr[i].name[32] = '\0';

        /* 2. uint8 typeId */
        uint8_t tid = (uint8_t) (read_u64(data + tuple_off + 32) & 0xff);
        arr[i].type = (uint8_t) (tid & 0x7f);
        arr[i].indexed = (tid & 0x80) != 0;

        /* 3. bytes value (offset relative to the tuple) */
        size_t val_rel;
        if (!read_size(data + tuple_off + 64, len, &val_rel) || !fits(tuple_off, val_rel, len))
            return false;
        size_t val_off = tuple_off + val_rel;
        size_t vlen;
        if (!fits(val_off, 32, len) || !read_size(data + val_off, len, &vlen) ||
            !fits(val_off + 32, vlen, len))
            return false;
        arr[i].len = vlen;
        arr[i].val = NULL;
        if (vlen > 0) {
            uint8_t *vcopy = av_arena_alloc(arena, vlen, 0);
            if (vcopy == NULL)
                return false;
            memcpy(vcopy, data + val_off + 32, vlen);
            arr[i].val = vcopy;
        }
    }

    *out_attrs = arr;
    *out_n = n;
    return true;
}

av_status av_abi_decode_execute(const uint8_t *calldata, size_t len, av_arena *arena,
                                av_mutation_op **out_ops, size_t *out_n_ops, av_err *e)
{
    *out_ops = NULL;
    *out_n_ops = 0;

    if (calldata == NULL || len < 4) {
        av_err_set(e, "invalid_argument", "calldata too short");
        return AV_ERR;
    }

    /* Verify selector 0x49650044 */
    if (calldata[0] != 0x49 || calldata[1] != 0x65 || calldata[2] != 0x00 || calldata[3] != 0x44) {
        av_err_set(e, "invalid_argument", "unknown function selector");
        return AV_ERR;
    }

    const uint8_t *args = calldata + 4;
    size_t args_len = len - 4;

    if (args_len < 64) {
        av_err_set(e, "invalid_argument", "calldata argument truncated");
        return AV_ERR;
    }

    size_t array_offset;
    if (!read_size(args, args_len, &array_offset) || !fits(array_offset, 32, args_len)) {
        av_err_set(e, "invalid_argument", "invalid array offset");
        return AV_ERR;
    }

    size_t n_ops;
    if (!read_size(args + array_offset, args_len, &n_ops)) {
        av_err_set(e, "invalid_argument", "invalid ops count");
        return AV_ERR;
    }
    if (n_ops == 0)
        return AV_OK;

    size_t head_start = array_offset + 32;
    if (n_ops > (args_len - head_start) / 32) {
        av_err_set(e, "invalid_argument", "ops head table truncated");
        return AV_ERR;
    }

    av_mutation_op *ops = av_arena_alloc(arena, n_ops * sizeof(av_mutation_op), 0);
    if (ops == NULL) {
        av_err_set(e, "internal", "out of memory allocating ops");
        return AV_ERR;
    }
    memset(ops, 0, n_ops * sizeof(av_mutation_op));

    for (size_t i = 0; i < n_ops; i++) {
        size_t op_rel;
        if (!read_size(args + head_start + i * 32, args_len, &op_rel) ||
            !fits(head_start, op_rel, args_len) || !fits(head_start + op_rel, 64, args_len)) {
            av_err_set(e, "invalid_argument", "op tuple truncated");
            return AV_ERR;
        }
        size_t op_off = head_start + op_rel;

        uint64_t optype = read_u64(args + op_off);
        ops[i].type = (av_op_type) optype;

        size_t data_rel;
        if (!read_size(args + op_off + 32, args_len, &data_rel) || !fits(op_off, data_rel, args_len) ||
            !fits(op_off + data_rel, 32, args_len)) {
            av_err_set(e, "invalid_argument", "operationData truncated");
            return AV_ERR;
        }
        size_t data_off = op_off + data_rel;

        size_t dlen;
        const uint8_t *pdata = args + data_off + 32;
        if (!read_size(args + data_off, args_len, &dlen) || !fits(data_off + 32, dlen, args_len)) {
            av_err_set(e, "invalid_argument", "operationData payload truncated");
            return AV_ERR;
        }

        switch (ops[i].type) {
        case AV_OP_CREATE: {
            size_t hoff = 0;
            if (dlen < 32 || !read_size(pdata, dlen, &hoff) || !fits(hoff, 0, dlen)) {
                av_err_set(e, "invalid_argument", "CREATE head offset invalid");
                return AV_ERR;
            }
            pdata += hoff;
            dlen -= hoff;
            if (dlen < 160) {
                av_err_set(e, "invalid_argument", "CREATE payload too short");
                return AV_ERR;
            }
            memcpy(ops[i].u.create.salt, pdata + 16, 16);
            ops[i].u.create.expires_at = read_u64(pdata + 32);
            ops[i].u.create.min_lifetime = read_u64(pdata + 64);
            ops[i].u.create.creation_flags = (uint8_t) (read_u64(pdata + 96) & 0xff);
            size_t attr_off = 0;
            if (!read_size(pdata + 128, dlen, &attr_off)) {
                av_err_set(e, "invalid_argument", "invalid CREATE attributes offset");
                return AV_ERR;
            }
            if (!decode_attributes(pdata, dlen, attr_off, arena, &ops[i].u.create.attributes,
                                   &ops[i].u.create.n_attributes)) {
                av_err_set(e, "invalid_argument", "failed to decode CREATE attributes");
                return AV_ERR;
            }
            break;
        }
        case AV_OP_PATCH: {
            size_t hoff = 0;
            if (dlen < 32 || !read_size(pdata, dlen, &hoff) || !fits(hoff, 0, dlen)) {
                av_err_set(e, "invalid_argument", "PATCH head offset invalid");
                return AV_ERR;
            }
            pdata += hoff;
            dlen -= hoff;
            if (dlen < 64) {
                av_err_set(e, "invalid_argument", "PATCH payload too short");
                return AV_ERR;
            }
            memcpy(ops[i].u.patch.entity_key, pdata, 32);
            size_t mut_off = 0;
            if (!read_size(pdata + 32, dlen, &mut_off)) {
                av_err_set(e, "invalid_argument", "invalid PATCH mutations offset");
                return AV_ERR;
            }
            if (!decode_attributes(pdata, dlen, mut_off, arena, &ops[i].u.patch.mutations,
                                   &ops[i].u.patch.n_mutations)) {
                av_err_set(e, "invalid_argument", "failed to decode PATCH mutations");
                return AV_ERR;
            }
            break;
        }
        case AV_OP_EXTEND_EXPIRY: {
            if (dlen < 96) {
                av_err_set(e, "invalid_argument", "EXTEND payload too short");
                return AV_ERR;
            }
            memcpy(ops[i].u.extend.entity_key, pdata, 32);
            ops[i].u.extend.expires_at = read_u64(pdata + 32);
            ops[i].u.extend.min_lifetime = read_u64(pdata + 64);
            break;
        }
        case AV_OP_TRANSFER_OWNERSHIP: {
            if (dlen < 64) {
                av_err_set(e, "invalid_argument", "TRANSFER payload too short");
                return AV_ERR;
            }
            memcpy(ops[i].u.transfer.entity_key, pdata, 32);
            memcpy(ops[i].u.transfer.new_owner, pdata + 32 + 12, 20);
            break;
        }
        case AV_OP_DELETE: {
            if (dlen < 32) {
                av_err_set(e, "invalid_argument", "DELETE payload too short");
                return AV_ERR;
            }
            memcpy(ops[i].u.del.entity_key, pdata, 32);
            break;
        }
        default:
            av_err_set(e, "invalid_argument", "unknown operation type %llu", (unsigned long long) optype);
            return AV_ERR;
        }
    }

    *out_ops = ops;
    *out_n_ops = n_ops;
    return AV_OK;
}
