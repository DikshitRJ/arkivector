/*
 * filter/lexer.h: tokenizer for the Arkiv `where` language (docs/api/filters.md section 4,
 * RESEARCH/arkiv-docs/04-query-engine-and-ast/query-language-grammar.md section 2).
 *
 * Whitespace is space, tab, CR, LF. Comments run from `--` to end of line. Tokens
 * reference the input buffer (no copies, no ownership); the caller keeps the input alive.
 * The lexer never reads past n bytes and never needs a NUL terminator. Input is untrusted
 * (fuzz target: fuzz_filter_parser).
 *
 *   IDENT  [A-Za-z][A-Za-z0-9._-]* (stops before a `--` comment start)
 *   SYS    $ followed by ASCII letters
 *   NUM    -?digits(.digits)?
 *   HEX    0x / 0X followed by zero or more hex digits
 *   STR    'text' with '' as the escape; the token spans the quotes
 *   OP     = != < <= > >=
 *   LPAREN, RPAREN, EOF
 */
#ifndef AV_FILTER_LEXER_H
#define AV_FILTER_LEXER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "log.h"

typedef enum {
    AV_TOK_EOF = 0,
    AV_TOK_IDENT,
    AV_TOK_SYS,
    AV_TOK_NUM,
    AV_TOK_HEX,
    AV_TOK_STR,
    AV_TOK_OP,
    AV_TOK_LPAREN,
    AV_TOK_RPAREN
} av_tok_kind;

typedef struct {
    av_tok_kind kind;
    size_t pos; /* byte offset of the first byte */
    size_t len; /* byte length of the token as written */
} av_token;

typedef struct {
    const char *s;
    size_t n;
    size_t i;
} av_lexer;

void av_lexer_init(av_lexer *lx, const char *s, size_t n);

/*
 * Next token. On a lexical error returns false and sets e (code invalid_filter, position).
 * After EOF, keeps returning EOF.
 */
bool av_lexer_next(av_lexer *lx, av_token *t, av_err *e);

/*
 * Decode a STR token (strip quotes, collapse ''). Writes at most cap bytes to out and
 * returns the decoded length; a return value > cap means the string is too long (out is
 * then incomplete). Never fails otherwise: av_lexer_next already validated the quoting.
 */
size_t av_lex_decode_str(const av_lexer *lx, const av_token *t, uint8_t *out, size_t cap);

#endif /* AV_FILTER_LEXER_H */
