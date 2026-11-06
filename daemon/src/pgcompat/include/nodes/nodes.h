/*
 * nodes/nodes.h (pgcompat stub): the Node header and the four list NodeTags that
 * PostgreSQL's pg_list.h/list.c (copied verbatim) need. copyObjectImpl() and
 * equal() exist only for list_copy_deep()/list_member(), which pgvector does not
 * use on node lists; they raise ERROR. ARKIVECTOR-owned.
 */
#ifndef NODES_H
#define NODES_H

typedef enum NodeTag {
    T_Invalid = 0,
    T_List,
    T_IntList,
    T_OidList,
    T_XidList
} NodeTag;

typedef struct Node {
    NodeTag type;
} Node;

#define nodeTag(nodeptr) (((const Node *) (nodeptr))->type)
#define IsA(nodeptr, _type_) (nodeTag(nodeptr) == T_##_type_)
#define castNode(_type_, nodeptr) ((_type_ *) (nodeptr))
#define makeNode(_type_) ((_type_ *) newNode(sizeof(_type_), T_##_type_))

static inline Node *newNode(size_t size, NodeTag tag)
{
    Node *result = (Node *) palloc0(size);
    result->type = tag;
    return result;
}

extern void *copyObjectImpl(const void *from);
#define copyObject(obj) ((__typeof__(obj)) copyObjectImpl(obj))
extern bool equal(const void *a, const void *b);

#endif /* NODES_H */
