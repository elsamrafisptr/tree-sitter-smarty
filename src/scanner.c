// Smarty-in-HTML external scanner.
//
// Ports the MIT-licensed HTML external scanner (Max Brunsfeld) and adds
// Smarty template delimiter handling. The HTML portion is preserved so
// templates parse correctly and <script>/<style> bodies stay opaque (braces
// inside them are never treated as Smarty tags).
//
// The Smarty portion emits the tag delimiters as external tokens:
//   SMARTY_OPEN    -> "{" or "{{"   (start of a tag)
//   SMARTY_CLOSE   -> "}" or "}}"   (end of a tag)
//   SMARTY_COMMENT -> "{* ... *}"   (opaque comment)
//
// A brace-width state (0 = not in a tag, 1 = {..}, 2 = {{..}}) lets the
// scanner recognise the matching close delimiter. The external scanner is
// consulted first at each lex position and takes precedence over regex
// tokens; inner tag content is matched by the grammar's regex tokens.
//
// Two invariants keep this correct:
//  1. Smarty actions are gated on `valid_symbols`, so a "{" where a tag is
//     not allowed (e.g. inside an HTML attribute value) is left for regex.
//  2. If the scanner returns false after advancing, the lexer resets to the
//     call position and falls back to regex lexing, so lookahead-by-advance
//     is safe.
#include "tag.h"
#include "tree_sitter/parser.h"

#include <wctype.h>

// NOTE: This enum order MUST match the order of `externals` in grammar.js.
// The tree-sitter CLI derives each external token's index from its position
// in that array, so a mismatch silently breaks token dispatch.
enum TokenType {
    START_TAG_NAME,             // 0  $._start_tag_name
    SCRIPT_START_TAG_NAME,      // 1  $._script_start_tag_name
    STYLE_START_TAG_NAME,       // 2  $._style_start_tag_name
    END_TAG_NAME,               // 3  $._end_tag_name
    ERRONEOUS_END_TAG_NAME,     // 4  $.erroneous_end_tag_name
    SELF_CLOSING_TAG_DELIMITER, // 5  '/>'
    IMPLICIT_END_TAG,           // 6  $._implicit_end_tag
    RAW_TEXT,                   // 7  $.raw_text
    COMMENT,                    // 8  $.comment
    SMARTY_OPEN,                // 9  $.smarty_open
    SMARTY_CLOSE,               // 10 $.smarty_close
    SMARTY_COMMENT,             // 11 $.smarty_comment
};

typedef struct {
    Array(Tag) tags;
    uint8_t brace_width; // 0 = not in a tag, 1 = {..}, 2 = {{..}}
} Scanner;

#define MAX(a, b) ((a) > (b) ? (a) : (b))

static inline void advance(TSLexer *lexer) { lexer->advance(lexer, false); }

static inline void skip(TSLexer *lexer) { lexer->advance(lexer, true); }

static unsigned serialize(Scanner *scanner, char *buffer) {
    uint16_t tag_count = scanner->tags.size > UINT16_MAX ? UINT16_MAX : scanner->tags.size;
    uint16_t serialized_tag_count = 0;

    // Layout:
    //   [0..1] serialized_tag_count
    //   [2..3] tag_count
    //   [4]    brace_width
    //   [5...] tag entries
    unsigned size = sizeof(tag_count);
    memcpy(&buffer[size], &tag_count, sizeof(tag_count));
    size += sizeof(tag_count);

    buffer[size++] = (char)scanner->brace_width;

    for (; serialized_tag_count < tag_count; serialized_tag_count++) {
        Tag tag = scanner->tags.contents[serialized_tag_count];
        if (tag.type == CUSTOM) {
            unsigned name_length = tag.custom_tag_name.size;
            if (name_length > UINT8_MAX) {
                name_length = UINT8_MAX;
            }
            if (size + 2 + name_length >= TREE_SITTER_SERIALIZATION_BUFFER_SIZE) {
                break;
            }
            buffer[size++] = (char)tag.type;
            buffer[size++] = (char)name_length;
            strncpy(&buffer[size], tag.custom_tag_name.contents, name_length);
            size += name_length;
        } else {
            if (size + 1 >= TREE_SITTER_SERIALIZATION_BUFFER_SIZE) {
                break;
            }
            buffer[size++] = (char)tag.type;
        }
    }

    memcpy(&buffer[0], &serialized_tag_count, sizeof(serialized_tag_count));
    return size;
}

static void deserialize(Scanner *scanner, const char *buffer, unsigned length) {
    for (unsigned i = 0; i < scanner->tags.size; i++) {
        tag_free(&scanner->tags.contents[i]);
    }
    array_clear(&scanner->tags);
    scanner->brace_width = 0;

    if (length > 0) {
        unsigned size = 0;
        uint16_t tag_count = 0;
        uint16_t serialized_tag_count = 0;

        memcpy(&serialized_tag_count, &buffer[size], sizeof(serialized_tag_count));
        size += sizeof(serialized_tag_count);

        memcpy(&tag_count, &buffer[size], sizeof(tag_count));
        size += sizeof(tag_count);

        if (size < length) {
            scanner->brace_width = (uint8_t)buffer[size++];
        }

        array_reserve(&scanner->tags, tag_count);
        if (tag_count > 0) {
            unsigned iter = 0;
            for (iter = 0; iter < serialized_tag_count; iter++) {
                Tag tag = tag_new();
                tag.type = (TagType)buffer[size++];
                if (tag.type == CUSTOM) {
                    uint16_t name_length = (uint8_t)buffer[size++];
                    array_reserve(&tag.custom_tag_name, name_length);
                    tag.custom_tag_name.size = name_length;
                    memcpy(&tag.custom_tag_name.contents, &buffer[size], name_length);
                    size += name_length;
                }
                array_push(&scanner->tags, tag);
            }
            for (; iter < tag_count; iter++) {
                array_push(&scanner->tags, tag_new());
            }
        }
    }
}

static String scan_tag_name(TSLexer *lexer) {
    String tag_name = array_new();
    while (iswalnum(lexer->lookahead) || lexer->lookahead == '-' || lexer->lookahead == ':') {
        array_push(&tag_name, towupper(lexer->lookahead));
        advance(lexer);
    }
    return tag_name;
}

static bool scan_comment(TSLexer *lexer) {
    if (lexer->lookahead != '-') {
        return false;
    }
    advance(lexer);
    if (lexer->lookahead != '-') {
        return false;
    }
    advance(lexer);

    unsigned dashes = 0;
    while (lexer->lookahead) {
        switch (lexer->lookahead) {
            case '-':
                ++dashes;
                break;
            case '>':
                if (dashes >= 2) {
                    lexer->result_symbol = COMMENT;
                    advance(lexer);
                    lexer->mark_end(lexer);
                    return true;
                }
            default:
                dashes = 0;
        }
        advance(lexer);
    }
    return false;
}

static bool scan_raw_text(Scanner *scanner, TSLexer *lexer) {
    if (scanner->tags.size == 0) {
        return false;
    }

    lexer->mark_end(lexer);

    const char *end_delimiter = array_back(&scanner->tags)->type == SCRIPT ? "</SCRIPT" : "</STYLE";

    unsigned delimiter_index = 0;
    while (lexer->lookahead) {
        if (towupper(lexer->lookahead) == end_delimiter[delimiter_index]) {
            delimiter_index++;
            if (delimiter_index == strlen(end_delimiter)) {
                break;
            }
            advance(lexer);
        } else {
            delimiter_index = 0;
            advance(lexer);
            lexer->mark_end(lexer);
        }
    }

    lexer->result_symbol = RAW_TEXT;
    return true;
}

static void pop_tag(Scanner *scanner) {
    Tag popped_tag = array_pop(&scanner->tags);
    tag_free(&popped_tag);
}

static bool scan_implicit_end_tag(Scanner *scanner, TSLexer *lexer) {
    Tag *parent = scanner->tags.size == 0 ? NULL : array_back(&scanner->tags);

    bool is_closing_tag = false;
    if (lexer->lookahead == '/') {
        is_closing_tag = true;
        advance(lexer);
    } else {
        if (parent && tag_is_void(parent)) {
            pop_tag(scanner);
            lexer->result_symbol = IMPLICIT_END_TAG;
            return true;
        }
    }

    String tag_name = scan_tag_name(lexer);
    if (tag_name.size == 0 && !lexer->eof(lexer)) {
        array_delete(&tag_name);
        return false;
    }

    Tag next_tag = tag_for_name(tag_name);

    if (is_closing_tag) {
        if (scanner->tags.size > 0 && tag_eq(array_back(&scanner->tags), &next_tag)) {
            tag_free(&next_tag);
            return false;
        }
        for (unsigned i = scanner->tags.size; i > 0; i--) {
            if (scanner->tags.contents[i - 1].type == next_tag.type) {
                pop_tag(scanner);
                lexer->result_symbol = IMPLICIT_END_TAG;
                tag_free(&next_tag);
                return true;
            }
        }
    } else if (
        parent &&
        (
            !tag_can_contain(parent, &next_tag) ||
            ((parent->type == HTML || parent->type == HEAD || parent->type == BODY) && lexer->eof(lexer))
        )
    ) {
        pop_tag(scanner);
        lexer->result_symbol = IMPLICIT_END_TAG;
        tag_free(&next_tag);
        return true;
    }

    tag_free(&next_tag);
    return false;
}

static bool scan_start_tag_name(Scanner *scanner, TSLexer *lexer) {
    String tag_name = scan_tag_name(lexer);
    if (tag_name.size == 0) {
        array_delete(&tag_name);
        return false;
    }

    Tag tag = tag_for_name(tag_name);
    array_push(&scanner->tags, tag);
    switch (tag.type) {
        case SCRIPT:
            lexer->result_symbol = SCRIPT_START_TAG_NAME;
            break;
        case STYLE:
            lexer->result_symbol = STYLE_START_TAG_NAME;
            break;
        default:
            lexer->result_symbol = START_TAG_NAME;
            break;
    }
    return true;
}

static bool scan_end_tag_name(Scanner *scanner, TSLexer *lexer) {
    String tag_name = scan_tag_name(lexer);

    if (tag_name.size == 0) {
        array_delete(&tag_name);
        return false;
    }

    Tag tag = tag_for_name(tag_name);
    if (scanner->tags.size > 0 && tag_eq(array_back(&scanner->tags), &tag)) {
        pop_tag(scanner);
        lexer->result_symbol = END_TAG_NAME;
    } else {
        lexer->result_symbol = ERRONEOUS_END_TAG_NAME;
    }

    tag_free(&tag);
    return true;
}

static bool scan_self_closing_tag_delimiter(Scanner *scanner, TSLexer *lexer) {
    advance(lexer);
    if (lexer->lookahead == '>') {
        advance(lexer);
        if (scanner->tags.size > 0) {
            pop_tag(scanner);
            lexer->result_symbol = SELF_CLOSING_TAG_DELIMITER;
        }
        return true;
    }
    return false;
}

// ----- Smarty delimiter handling -----

// At the position of an open "{" (the first "{" has already been committed by
// the caller). Emits SMARTY_COMMENT for "{* ... *}", SMARTY_OPEN for a tag,
// or returns false so the lexer resets and the regex tokens consume the brace.
static bool scan_smarty_open_rest(Scanner *scanner, TSLexer *lexer) {
    if (lexer->lookahead == '*') {
        // "{* ... *}" comment: consume through the matching "*}".
        advance(lexer); // "*"
        while (lexer->lookahead) {
            if (lexer->lookahead == '*') {
                advance(lexer); // "*"
                if (lexer->lookahead == '}') {
                    advance(lexer); // "}"
                    lexer->result_symbol = SMARTY_COMMENT;
                    return true;
                }
            } else {
                advance(lexer);
            }
        }
        // Unterminated comment: consume through EOF.
        lexer->result_symbol = SMARTY_COMMENT;
        return true;
    }

    if (lexer->lookahead == '{') {
        // Potential double-brace tag "{{ ... }}".
        advance(lexer); // second "{"
        if (lexer->lookahead == '\0' || iswspace(lexer->lookahead) || lexer->lookahead == '}') {
            return false;
        }
        scanner->brace_width = 2;
    } else {
        // Single-brace tag "{ ... }": the character after "{" must be a
        // non-space, non-"}" character (mirrors the TextMate grammar's
        // (?=[^{}\s]) lookahead).
        if (lexer->lookahead == '\0' || iswspace(lexer->lookahead) || lexer->lookahead == '}') {
            return false;
        }
        scanner->brace_width = 1;
    }

    lexer->result_symbol = SMARTY_OPEN;
    return true;
}

// At the position of a close "}" (the first "}" has already been committed by
// the caller). Emits SMARTY_CLOSE, consuming the second "}" in double-brace
// mode. Returns false when a lone "}" is not the close (double mode) so the
// lexer resets and the regex tokens treat it as content.
static bool scan_smarty_close_rest(Scanner *scanner, TSLexer *lexer) {
    if (scanner->brace_width == 2) {
        if (lexer->lookahead != '}') {
            return false;
        }
        advance(lexer); // second "}"
    }
    scanner->brace_width = 0;
    lexer->result_symbol = SMARTY_CLOSE;
    return true;
}

// Dispatch Smarty handling. The caller has confirmed the lookahead is "{" or
// "}" and the relevant valid_symbols are set.
static bool scan_smarty(Scanner *scanner, TSLexer *lexer) {
    if (lexer->lookahead == '}') {
        if (scanner->brace_width == 0) {
            return false;
        }
        advance(lexer); // commit first "}"
        return scan_smarty_close_rest(scanner, lexer);
    }
    // lexer->lookahead == '{'
    advance(lexer); // commit first "{"
    return scan_smarty_open_rest(scanner, lexer);
}

static bool scan(Scanner *scanner, TSLexer *lexer, const bool *valid_symbols) {
    if (valid_symbols[RAW_TEXT] && !valid_symbols[START_TAG_NAME] && !valid_symbols[END_TAG_NAME]) {
        return scan_raw_text(scanner, lexer);
    }

    // Skip leading whitespace first so that a "{" preceded by whitespace is
    // seen at the post-skip position. (skip() advances the lexer and is not
    // reliably rewound on a false return, so checking before the skip would
    // leave the brace unconsumed and it would be lost to error recovery.)
    while (iswspace(lexer->lookahead)) {
        skip(lexer);
    }

    // Smarty delimiters are decided before HTML tag handling: at a "{" or "}"
    // the scanner must claim the token before the regex tokens run. Gating on
    // valid_symbols keeps braces where a Smarty tag is not allowed for regex.
    if (lexer->lookahead == '}' && scanner->brace_width > 0 && valid_symbols[SMARTY_CLOSE]) {
        return scan_smarty(scanner, lexer);
    }
    if (lexer->lookahead == '{' && (valid_symbols[SMARTY_OPEN] || valid_symbols[SMARTY_COMMENT])) {
        return scan_smarty(scanner, lexer);
    }

    switch (lexer->lookahead) {
        case '<':
            lexer->mark_end(lexer);
            advance(lexer);

            if (lexer->lookahead == '!') {
                advance(lexer);
                return scan_comment(lexer);
            }

            if (valid_symbols[IMPLICIT_END_TAG]) {
                return scan_implicit_end_tag(scanner, lexer);
            }
            break;

        case '\0':
            if (valid_symbols[IMPLICIT_END_TAG]) {
                return scan_implicit_end_tag(scanner, lexer);
            }
            break;

        case '/':
            if (valid_symbols[SELF_CLOSING_TAG_DELIMITER]) {
                return scan_self_closing_tag_delimiter(scanner, lexer);
            }
            break;

        default:
            if ((valid_symbols[START_TAG_NAME] || valid_symbols[END_TAG_NAME]) && !valid_symbols[RAW_TEXT]) {
                return valid_symbols[START_TAG_NAME] ? scan_start_tag_name(scanner, lexer)
                                                     : scan_end_tag_name(scanner, lexer);
            }
    }

    return false;
}

void *tree_sitter_smarty_external_scanner_create() {
    Scanner *scanner = (Scanner *)ts_calloc(1, sizeof(Scanner));
    return scanner;
}

bool tree_sitter_smarty_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
    Scanner *scanner = (Scanner *)payload;
    return scan(scanner, lexer, valid_symbols);
}

unsigned tree_sitter_smarty_external_scanner_serialize(void *payload, char *buffer) {
    Scanner *scanner = (Scanner *)payload;
    return serialize(scanner, buffer);
}

void tree_sitter_smarty_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
    Scanner *scanner = (Scanner *)payload;
    deserialize(scanner, buffer, length);
}

void tree_sitter_smarty_external_scanner_destroy(void *payload) {
    Scanner *scanner = (Scanner *)payload;
    for (unsigned i = 0; i < scanner->tags.size; i++) {
        tag_free(&scanner->tags.contents[i]);
    }
    array_delete(&scanner->tags);
    ts_free(scanner);
}
