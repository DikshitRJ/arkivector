/*
 * filter/parser.c: recursive-descent parser for the `where` language (see parser.h).
 */
#include "filter/parser.h"

#include <string.h>

#include "filter/lexer.h"

typedef struct {
    av_lexer lx;
    av_token tok; /* current token */
    av_fbuild fb;
    av_err *e;
    unsigned nest;
} parser;

static av_filter *p_or(parser *p);

/* ------------------------------------------------------------------------- */
/* token helpers                                                             */
/* ------------------------------------------------------------------------- */

static bool adv(parser *p)
{
    return av_lexer_next(&p->lx, &p->tok, p->e);
}

static const char *tok_text(const parser *p)
{
    return p->lx.s + p->tok.pos;
}

static bool ieq_n(const char *s, size_t n, const char *lit)
{
    if (strlen(lit) != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char) (c - 'A' + 'a');
        }
        if (c != lit[i]) {
            return false;
        }
    }
    return true;
}

static bool kw(const parser *p, const char *word)
{
    return p->tok.kind == AV_TOK_IDENT && ieq_n(tok_text(p), p->tok.len, word);
}

/* Printable description of the current token for error messages. */
static void describe(const parser *p, char out[48])
{
    if (p->tok.kind == AV_TOK_EOF) {
        (void) strcpy(out, "end of input");
        return;
    }
    size_t n = p->tok.len < 24 ? p->tok.len : 24;
    size_t k = 0;
    out[k++] = '\'';
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char) tok_text(p)[i];
        out[k++] = (c >= 0x20 && c < 0x7f) ? (char) c : '?';
    }
    if (n < p->tok.len) {
        out[k++] = '.';
        out[k++] = '.';
    }
    out[k++] = '\'';
    out[k] = '\0';
}

static bool fail_here(parser *p, const char *expected)
{
    char d[48];
    describe(p, d);
    av_filter_err(p->e, (int) p->tok.pos, "expected %s, found %s", expected, d);
    return false;
}

static bool expect(parser *p, av_tok_kind k, const char *what)
{
    if (p->tok.kind != k) {
        return fail_here(p, what);
    }
    return adv(p);
}

/* ------------------------------------------------------------------------- */
/* fields and literals                                                       */
/* ------------------------------------------------------------------------- */

static bool p_field(parser *p, av_field *f)
{
    if (p->tok.kind != AV_TOK_IDENT && p->tok.kind != AV_TOK_SYS) {
        return fail_here(p, "an attribute name or system field");
    }
    if (!av_fb_field(&p->fb, tok_text(p), p->tok.len, (int) p->tok.pos, f, p->e)) {
        return false;
    }
    return adv(p);
}

static bool type_from_token(const parser *p, av_attr_type *t)
{
    if (p->tok.kind != AV_TOK_IDENT || p->tok.len > 5) {
        return false;
    }
    char low[8];
    for (size_t i = 0; i < p->tok.len; i++) {
        char c = tok_text(p)[i];
        low[i] = (char) ((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
    }
    return av_type_from_name(low, p->tok.len, t);
}

static bool literal_error(parser *p, size_t pos, av_attr_type t, const char *why)
{
    av_filter_err(p->e, (int) pos, "invalid %s literal: %s", av_type_name(t), why);
    return false;
}

/* The constructor argument for type t; the current token is the first token after '('. */
static bool p_ctor_body(parser *p, av_attr_type t, av_value *out)
{
    const char *why = NULL;
    size_t pos = p->tok.pos;
    uint8_t buf[AV_ATTR_STR_MAX];
    switch (t) {
    case AV_AT_BOOL:
        if (!(kw(p, "true") || kw(p, "false"))) {
            return fail_here(p, "true or false");
        }
        break;
    case AV_AT_I32:
    case AV_AT_U64:
        if (p->tok.kind != AV_TOK_NUM) {
            return fail_here(p, "an integer");
        }
        break;
    case AV_AT_U256:
        if (p->tok.kind != AV_TOK_NUM && p->tok.kind != AV_TOK_HEX) {
            return fail_here(p, "an integer");
        }
        break;
    case AV_AT_DEC:
        if (p->tok.kind != AV_TOK_NUM && p->tok.kind != AV_TOK_STR) {
            return fail_here(p, "a decimal number");
        }
        break;
    case AV_AT_STR:
        if (p->tok.kind != AV_TOK_STR) {
            return fail_here(p, "a single-quoted string");
        }
        break;
    default: /* addr, key */
        if (p->tok.kind != AV_TOK_HEX) {
            return fail_here(p, "a 0x-prefixed hex value");
        }
        break;
    }
    if (p->tok.kind == AV_TOK_STR) {
        size_t n = av_lex_decode_str(&p->lx, &p->tok, buf, sizeof(buf));
        if (n > sizeof(buf)) {
            return literal_error(p, pos, t, "string exceeds 128 bytes");
        }
        if (!av_value_parse(t, (const char *) buf, n, out, &why)) {
            return literal_error(p, pos, t, why);
        }
    } else if (!av_value_parse(t, tok_text(p), p->tok.len, out, &why)) {
        return literal_error(p, pos, t, why);
    }
    return adv(p);
}

static bool p_literal(parser *p, av_value *out)
{
    const char *why = NULL;
    if (p->tok.kind == AV_TOK_NUM) {
        size_t pos = p->tok.pos;
        for (size_t i = 0; i < p->tok.len; i++) {
            if (tok_text(p)[i] == '.') {
                av_filter_err(p->e, (int) pos, "decimal literals need a constructor, e.g. dec(1.5)");
                return false;
            }
        }
        if (!av_value_parse(AV_AT_I32, tok_text(p), p->tok.len, out, &why)) {
            return literal_error(p, pos, AV_AT_I32, why);
        }
        return adv(p);
    }
    if (kw(p, "true") || kw(p, "false")) {
        bool v = kw(p, "true");
        av_value_bool(out, v);
        return adv(p);
    }
    av_attr_type t;
    if (type_from_token(p, &t)) {
        size_t pos = p->tok.pos;
        if (t == AV_AT_BYTES) {
            av_filter_err(p->e, (int) pos, "bytes values cannot be filtered");
            return false;
        }
        if (!adv(p) || !expect(p, AV_TOK_LPAREN, "'('") || !p_ctor_body(p, t, out)) {
            return false;
        }
        return expect(p, AV_TOK_RPAREN, "')'");
    }
    return fail_here(p, "a literal (true, false, an integer or a typed constructor such as str('x'))");
}

/* ------------------------------------------------------------------------- */
/* predicates                                                                */
/* ------------------------------------------------------------------------- */

static bool op_from_token(const parser *p, av_cmp_op *op)
{
    if (p->tok.kind != AV_TOK_OP) {
        return false;
    }
    const char *s = tok_text(p);
    if (p->tok.len == 1) {
        *op = s[0] == '=' ? AV_OP_EQ : (s[0] == '<' ? AV_OP_LT : AV_OP_GT);
    } else {
        *op = s[0] == '!' ? AV_OP_NE : (s[0] == '<' ? AV_OP_LE : AV_OP_GE);
    }
    return true;
}

static av_filter *p_predicate(parser *p)
{
    int start = (int) p->tok.pos;
    av_field f;
    if (kw(p, "exists")) {
        if (!adv(p) || !expect(p, AV_TOK_LPAREN, "'('") || !p_field(p, &f) || !expect(p, AV_TOK_RPAREN, "')'")) {
            return NULL;
        }
        return av_fb_exists(&p->fb, start, &f, p->e);
    }
    if (kw(p, "typeof")) {
        if (!adv(p) || !expect(p, AV_TOK_LPAREN, "'('") || !p_field(p, &f) || !expect(p, AV_TOK_RPAREN, "')'")) {
            return NULL;
        }
        av_cmp_op op;
        if (!op_from_token(p, &op) || op != AV_OP_EQ) {
            fail_here(p, "'='");
            return NULL;
        }
        if (!adv(p)) {
            return NULL;
        }
        av_attr_type t;
        if (!type_from_token(p, &t)) {
            fail_here(p, "a type name");
            return NULL;
        }
        if (!adv(p)) {
            return NULL;
        }
        return av_fb_typeof(&p->fb, start, &f, t, p->e);
    }
    if (!p_field(p, &f)) {
        return NULL;
    }
    av_value v;
    if (kw(p, "between")) {
        av_value hi;
        if (!adv(p) || !p_literal(p, &v)) {
            return NULL;
        }
        if (!kw(p, "and")) {
            fail_here(p, "AND");
            return NULL;
        }
        if (!adv(p) || !p_literal(p, &hi)) {
            return NULL;
        }
        return av_fb_between(&p->fb, start, &f, &v, &hi, p->e);
    }
    if (kw(p, "startswith")) {
        if (!adv(p) || !p_literal(p, &v)) {
            return NULL;
        }
        return av_fb_startswith(&p->fb, start, &f, &v, p->e);
    }
    av_cmp_op op;
    if (op_from_token(p, &op)) {
        if (!adv(p) || !p_literal(p, &v)) {
            return NULL;
        }
        return av_fb_cmp(&p->fb, start, &f, op, &v, p->e);
    }
    fail_here(p, "a comparison operator, BETWEEN or STARTSWITH");
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* expressions                                                               */
/* ------------------------------------------------------------------------- */

static bool enter(parser *p)
{
    if (p->nest >= AV_FILTER_MAX_NEST) {
        av_filter_err(p->e, (int) p->tok.pos, "filter is nested deeper than %d levels", AV_FILTER_MAX_NEST);
        return false;
    }
    p->nest++;
    return true;
}

static av_filter *p_unary(parser *p)
{
    int start = (int) p->tok.pos;
    if (kw(p, "not")) {
        if (!enter(p) || !adv(p)) {
            return NULL;
        }
        av_filter *kid = p_unary(p);
        p->nest--;
        return kid == NULL ? NULL : av_fb_not(&p->fb, start, kid, p->e);
    }
    if (p->tok.kind == AV_TOK_LPAREN) {
        if (!enter(p) || !adv(p)) {
            return NULL;
        }
        av_filter *inner = p_or(p);
        p->nest--;
        if (inner == NULL || !expect(p, AV_TOK_RPAREN, "')'")) {
            return NULL;
        }
        return inner;
    }
    return p_predicate(p);
}

/* Parse `sub (WORD sub)*`; two or more operands become an n-ary group. */
static av_filter *p_chain(parser *p, bool is_or)
{
    int start = (int) p->tok.pos;
    av_filter *first = is_or ? p_chain(p, false) : p_unary(p);
    const char *word = is_or ? "or" : "and";
    if (first == NULL || !kw(p, word)) {
        return first;
    }
    size_t cap = 4;
    size_t n = 0;
    av_filter **kids = av_arena_alloc(p->fb.arena, cap * sizeof(*kids), 0);
    if (kids == NULL) {
        av_err_set(p->e, "internal", "out of memory building filter");
        return NULL;
    }
    kids[n++] = first;
    while (kw(p, word)) {
        if (!adv(p)) {
            return NULL;
        }
        av_filter *next = is_or ? p_chain(p, false) : p_unary(p);
        if (next == NULL) {
            return NULL;
        }
        if (n == cap) {
            av_filter **bigger = av_arena_alloc(p->fb.arena, cap * 2 * sizeof(*kids), 0);
            if (bigger == NULL) {
                av_err_set(p->e, "internal", "out of memory building filter");
                return NULL;
            }
            memcpy(bigger, kids, n * sizeof(*kids));
            kids = bigger;
            cap *= 2;
        }
        kids[n++] = next;
    }
    return av_fb_group(&p->fb, is_or ? AV_F_OR : AV_F_AND, start, kids, n, p->e);
}

static av_filter *p_or(parser *p)
{
    return p_chain(p, true);
}

av_status av_filter_from_where(const char *s, size_t n, const av_coldef *sch, av_arena *a, av_filter **out,
                               av_err *e)
{
    if (out != NULL) {
        *out = NULL;
    }
    av_err_clear(e);
    if (out == NULL || a == NULL || (s == NULL && n > 0)) {
        av_err_set(e, "internal", "av_filter_from_where: bad arguments");
        return AV_ERR;
    }
    if (n > AV_FILTER_MAX_WHERE_BYTES) {
        av_filter_err(e, AV_FILTER_MAX_WHERE_BYTES, "filter text is longer than %d bytes",
                      AV_FILTER_MAX_WHERE_BYTES);
        return AV_ERR;
    }
    parser p;
    memset(&p, 0, sizeof(p));
    av_lexer_init(&p.lx, s == NULL ? "" : s, n);
    av_fb_init(&p.fb, a, sch);
    p.e = e;
    if (!adv(&p)) {
        return AV_ERR;
    }
    if (p.tok.kind == AV_TOK_EOF) {
        av_filter_err(e, (int) p.tok.pos, "empty filter");
        return AV_ERR;
    }
    av_filter *f = p_or(&p);
    if (f == NULL) {
        return AV_ERR;
    }
    if (p.tok.kind != AV_TOK_EOF) {
        if (kw(&p, "order")) {
            av_filter_err(e, (int) p.tok.pos, "ORDER BY is not allowed in a filter");
        } else {
            fail_here(&p, "AND, OR or end of input");
        }
        return AV_ERR;
    }
    *out = f;
    return AV_OK;
}
