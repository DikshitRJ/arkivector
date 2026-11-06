/*
 * filter/lexer.c: tokenizer for the `where` language (see lexer.h).
 */
#include "filter/lexer.h"

#include "filter/ast.h"

static bool is_alpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static bool is_hex(char c)
{
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

void av_lexer_init(av_lexer *lx, const char *s, size_t n)
{
    lx->s = s;
    lx->n = n;
    lx->i = 0;
}

static bool lex_error(av_err *e, size_t pos, const char *msg)
{
    av_filter_err(e, (int) pos, "%s", msg);
    return false;
}

bool av_lexer_next(av_lexer *lx, av_token *t, av_err *e)
{
    const char *s = lx->s;
    size_t n = lx->n;
    size_t i = lx->i;
    for (;;) {
        while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) {
            i++;
        }
        if (i + 1 < n && s[i] == '-' && s[i + 1] == '-') {
            while (i < n && s[i] != '\n') {
                i++;
            }
            continue;
        }
        break;
    }
    t->pos = i;
    t->len = 0;
    if (i >= n) {
        t->kind = AV_TOK_EOF;
        lx->i = n;
        return true;
    }
    char c = s[i];
    size_t j;
    if (c == '\'') {
        j = i + 1;
        for (;;) {
            if (j >= n) {
                lx->i = n;
                return lex_error(e, i, "unterminated string literal");
            }
            if (s[j] == '\'') {
                if (j + 1 < n && s[j + 1] == '\'') {
                    j += 2;
                    continue;
                }
                break;
            }
            j++;
        }
        t->kind = AV_TOK_STR;
        j++;
    } else if (c == '$') {
        j = i + 1;
        while (j < n && is_alpha(s[j])) {
            j++;
        }
        if (j == i + 1) {
            lx->i = n;
            return lex_error(e, i, "expected a system field name after '$'");
        }
        t->kind = AV_TOK_SYS;
    } else if (c == '0' && i + 1 < n && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
        j = i + 2;
        while (j < n && is_hex(s[j])) {
            j++;
        }
        t->kind = AV_TOK_HEX;
    } else if (is_digit(c) || (c == '-' && i + 1 < n && is_digit(s[i + 1]))) {
        j = i + 1;
        while (j < n && is_digit(s[j])) {
            j++;
        }
        if (j + 1 < n && s[j] == '.' && is_digit(s[j + 1])) {
            j++;
            while (j < n && is_digit(s[j])) {
                j++;
            }
        }
        t->kind = AV_TOK_NUM;
    } else if (is_alpha(c)) {
        j = i + 1;
        while (j < n && (is_alpha(s[j]) || is_digit(s[j]) || s[j] == '.' || s[j] == '_' || s[j] == '-')) {
            if (s[j] == '-' && j + 1 < n && s[j + 1] == '-') {
                break; /* comment start */
            }
            j++;
        }
        t->kind = AV_TOK_IDENT;
    } else if (c == '(') {
        j = i + 1;
        t->kind = AV_TOK_LPAREN;
    } else if (c == ')') {
        j = i + 1;
        t->kind = AV_TOK_RPAREN;
    } else if (c == '=') {
        j = i + 1;
        t->kind = AV_TOK_OP;
    } else if (c == '<' || c == '>') {
        j = (i + 1 < n && s[i + 1] == '=') ? i + 2 : i + 1;
        t->kind = AV_TOK_OP;
    } else if (c == '!' && i + 1 < n && s[i + 1] == '=') {
        j = i + 2;
        t->kind = AV_TOK_OP;
    } else {
        lx->i = n;
        return lex_error(e, i, "unexpected character");
    }
    t->len = j - i;
    lx->i = j;
    return true;
}

size_t av_lex_decode_str(const av_lexer *lx, const av_token *t, uint8_t *out, size_t cap)
{
    const char *p = lx->s + t->pos + 1;
    const char *end = lx->s + t->pos + t->len - 1; /* closing quote */
    size_t n = 0;
    while (p < end) {
        if (n < cap) {
            out[n] = (uint8_t) *p;
        }
        n++;
        if (*p == '\'' && p + 1 < end && p[1] == '\'') {
            p++;
        }
        p++;
    }
    return n;
}
