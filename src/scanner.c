#include "tree_sitter/alloc.h"
#include "tree_sitter/array.h"
#include "tree_sitter/parser.h"
#include <stdint.h>
#include <string.h>
#include "../tree-sitter-c-sharp/src/scanner.c"

#ifdef DEBUG_SCANNER
#include <stdio.h>
#define DEBUG_PRINT(...) fprintf(stderr, __VA_ARGS__)
#else
#define DEBUG_PRINT(...)
#endif

#define CSHARP_TOKEN_COUNT 12

enum RazorTokenType {
    TEXT_WITH_LITERAL_AT = CSHARP_TOKEN_COUNT,
    HTML_TEXT_CONTENT,
    CSHARP_CODE_BLOCK_START,
    CSHARP_EXPLICIT_EXPR_START,
    RAZOR_BLOCK_OPEN,
    CSHARP_CONTEXT_CLOSE,
    CSHARP_COMMENT,
    PREPROC_REGION,
    PREPROC_ENDREGION,
    PREPROC_LINE,
    PREPROC_PRAGMA,
    PREPROC_NULLABLE,
    PREPROC_ERROR,
    PREPROC_WARNING,
    PREPROC_DEFINE,
    PREPROC_UNDEF,
    PREPROC_DIRECTIVE,
    SCRIPT_CONTENT,
    STYLE_CONTENT,
    TITLE_CONTENT,
    TEXTAREA_CONTENT,
    IMPLICIT_EXPR_END,
    RAZOR_BLOCK_AT,
    USING_NOT_ALIAS,
    RAZOR_COMMENT_START,
    RAZOR_COMMENT,
    RAZOR_COMMENT_EXTRA,  // Razor comment as extra (only in C# context)
    HTML_TAG_OPEN,
    HTML_END_TAG_OPEN,
    HTML_TAG_CLOSE,
    HTML_COMMENT,
    DOCTYPE,
    IMPLICIT_PAREN_OPEN,
    IMPLICIT_BRACKET_OPEN,
    IMPLICIT_CONDITIONAL_BRACKET_OPEN,
    TEXT_LITERAL_CONTENT,
};

typedef enum {
    CONTEXT_HTML = 0,
    CONTEXT_CSHARP_BRACE = 1,
    CONTEXT_CSHARP_PAREN = 2,
    CONTEXT_HTML_TAG = 3,
    CONTEXT_CSHARP_BRACKET = 4,
} ContextType;

typedef struct {
    void *csharp_scanner;
    Array(uint8_t) context_stack;
} RazorScanner;

static inline void razor_advance(TSLexer *lexer) { lexer->advance(lexer, false); }
static inline void razor_skip(TSLexer *lexer) { lexer->advance(lexer, true); }

static inline bool in_csharp_context(RazorScanner *scanner) {
    if (scanner->context_stack.size == 0) return false;
    ContextType top = *array_back(&scanner->context_stack);
    return top == CONTEXT_CSHARP_BRACE || top == CONTEXT_CSHARP_PAREN || top == CONTEXT_CSHARP_BRACKET;
}

static inline bool in_html_tag_context(RazorScanner *scanner) {
    return scanner->context_stack.size > 0 && *array_back(&scanner->context_stack) == CONTEXT_HTML_TAG;
}

// Check if character is a Unicode letter (approximation covering main categories)
static inline bool is_unicode_letter(int32_t c) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return true;  // ASCII
    if (c >= 0x00C0 && c <= 0x00FF && c != 0x00D7 && c != 0x00F7) return true;  // Latin-1 Supplement
    if (c >= 0x0100 && c <= 0x017F) return true;  // Latin Extended-A
    if (c >= 0x0180 && c <= 0x024F) return true;  // Latin Extended-B
    if (c >= 0x0370 && c <= 0x03FF) return true;  // Greek and Coptic
    if (c >= 0x0400 && c <= 0x04FF) return true;  // Cyrillic
    if (c >= 0x0590 && c <= 0x05FF) return true;  // Hebrew
    if (c >= 0x0600 && c <= 0x06FF) return true;  // Arabic
    if (c >= 0x0900 && c <= 0x097F) return true;  // Devanagari
    if (c >= 0x0E00 && c <= 0x0E7F) return true;  // Thai
    if (c >= 0x4E00 && c <= 0x9FFF) return true;  // CJK Unified Ideographs
    if (c >= 0x3040 && c <= 0x309F) return true;  // Hiragana
    if (c >= 0x30A0 && c <= 0x30FF) return true;  // Katakana
    if (c >= 0xAC00 && c <= 0xD7AF) return true;  // Hangul Syllables
    return false;
}

// Check if character is a Unicode decimal digit (category Nd)
// Matches .NET's char.IsDigit() behavior
static inline bool is_unicode_digit(int32_t c) {
    if (c >= 0x0030 && c <= 0x0039) return true;  // ASCII
    if (c < 0x0660) return false;
    // BMP digits
    if (c >= 0x0660 && c <= 0x0669) return true;  // Arabic-Indic
    if (c >= 0x06F0 && c <= 0x06F9) return true;  // Extended Arabic-Indic
    if (c >= 0x07C0 && c <= 0x07C9) return true;  // NKo
    if (c >= 0x0966 && c <= 0x096F) return true;  // Devanagari
    if (c >= 0x09E6 && c <= 0x09EF) return true;  // Bengali
    if (c >= 0x0A66 && c <= 0x0A6F) return true;  // Gurmukhi
    if (c >= 0x0AE6 && c <= 0x0AEF) return true;  // Gujarati
    if (c >= 0x0B66 && c <= 0x0B6F) return true;  // Oriya
    if (c >= 0x0BE6 && c <= 0x0BEF) return true;  // Tamil
    if (c >= 0x0C66 && c <= 0x0C6F) return true;  // Telugu
    if (c >= 0x0CE6 && c <= 0x0CEF) return true;  // Kannada
    if (c >= 0x0D66 && c <= 0x0D6F) return true;  // Malayalam
    if (c >= 0x0DE6 && c <= 0x0DEF) return true;  // Sinhala
    if (c >= 0x0E50 && c <= 0x0E59) return true;  // Thai
    if (c >= 0x0ED0 && c <= 0x0ED9) return true;  // Lao
    if (c >= 0x0F20 && c <= 0x0F29) return true;  // Tibetan
    if (c >= 0x1040 && c <= 0x1049) return true;  // Myanmar
    if (c >= 0x1090 && c <= 0x1099) return true;  // Myanmar Shan
    if (c >= 0x17E0 && c <= 0x17E9) return true;  // Khmer
    if (c >= 0x1810 && c <= 0x1819) return true;  // Mongolian
    if (c >= 0x1946 && c <= 0x194F) return true;  // Limbu
    if (c >= 0x19D0 && c <= 0x19D9) return true;  // New Tai Lue
    if (c >= 0x1A80 && c <= 0x1A89) return true;  // Tai Tham Hora
    if (c >= 0x1A90 && c <= 0x1A99) return true;  // Tai Tham Tham
    if (c >= 0x1B50 && c <= 0x1B59) return true;  // Balinese
    if (c >= 0x1BB0 && c <= 0x1BB9) return true;  // Sundanese
    if (c >= 0x1C40 && c <= 0x1C49) return true;  // Lepcha
    if (c >= 0x1C50 && c <= 0x1C59) return true;  // Ol Chiki
    if (c >= 0xA620 && c <= 0xA629) return true;  // Vai
    if (c >= 0xA8D0 && c <= 0xA8D9) return true;  // Saurashtra
    if (c >= 0xA900 && c <= 0xA909) return true;  // Kayah Li
    if (c >= 0xA9D0 && c <= 0xA9D9) return true;  // Javanese
    if (c >= 0xA9F0 && c <= 0xA9F9) return true;  // Myanmar Tai Laing
    if (c >= 0xAA50 && c <= 0xAA59) return true;  // Cham
    if (c >= 0xABF0 && c <= 0xABF9) return true;  // Meetei Mayek
    if (c >= 0xFF10 && c <= 0xFF19) return true;  // Fullwidth
    // SMP digits
    if (c >= 0x104A0 && c <= 0x104A9) return true;  // Osmanya
    if (c >= 0x10D30 && c <= 0x10D39) return true;  // Hanifi Rohingya
    if (c >= 0x11066 && c <= 0x1106F) return true;  // Brahmi
    if (c >= 0x110F0 && c <= 0x110F9) return true;  // Sora Sompeng
    if (c >= 0x11136 && c <= 0x1113F) return true;  // Chakma
    if (c >= 0x111D0 && c <= 0x111D9) return true;  // Sharada
    if (c >= 0x112F0 && c <= 0x112F9) return true;  // Khudawadi
    if (c >= 0x11450 && c <= 0x11459) return true;  // Newa
    if (c >= 0x114D0 && c <= 0x114D9) return true;  // Tirhuta
    if (c >= 0x11650 && c <= 0x11659) return true;  // Modi
    if (c >= 0x116C0 && c <= 0x116C9) return true;  // Takri
    if (c >= 0x11730 && c <= 0x11739) return true;  // Ahom
    if (c >= 0x118E0 && c <= 0x118E9) return true;  // Warang Citi
    if (c >= 0x11950 && c <= 0x11959) return true;  // Dives Akuru
    if (c >= 0x11C50 && c <= 0x11C59) return true;  // Bhaiksuki
    if (c >= 0x11D50 && c <= 0x11D59) return true;  // Masaram Gondi
    if (c >= 0x11DA0 && c <= 0x11DA9) return true;  // Gunjala Gondi
    if (c >= 0x11F50 && c <= 0x11F59) return true;  // Kawi
    if (c >= 0x16A60 && c <= 0x16A69) return true;  // Mro
    if (c >= 0x16AC0 && c <= 0x16AC9) return true;  // Tangsa
    if (c >= 0x16B50 && c <= 0x16B59) return true;  // Pahawh Hmong
    if (c >= 0x1D7CE && c <= 0x1D7D7) return true;  // Mathematical Bold
    if (c >= 0x1D7D8 && c <= 0x1D7E1) return true;  // Mathematical Double-Struck
    if (c >= 0x1D7E2 && c <= 0x1D7EB) return true;  // Mathematical Sans-Serif
    if (c >= 0x1D7EC && c <= 0x1D7F5) return true;  // Mathematical Sans-Serif Bold
    if (c >= 0x1D7F6 && c <= 0x1D7FF) return true;  // Mathematical Monospace
    if (c >= 0x1E140 && c <= 0x1E149) return true;  // Nyiakeng Puachue Hmong
    if (c >= 0x1E2F0 && c <= 0x1E2F9) return true;  // Wancho
    if (c >= 0x1E4F0 && c <= 0x1E4F9) return true;  // Nag Mundari
    if (c >= 0x1E950 && c <= 0x1E959) return true;  // Adlam
    if (c >= 0x1FBF0 && c <= 0x1FBF9) return true;  // Segmented
    return false;
}

static inline bool is_whitespace(int32_t c) {
    return c == ' ' || c == '\n' || c == '\r' ||
           c == '\t' || c == '\v' || c == '\f' ||
           c == 0x00A0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) ||
           c == 0x202F || c == 0x205F || c == 0x3000;
}

static inline bool is_end_tag_terminator(int32_t c) {
    return c == '\t' || c == '\n' || c == '\f' || c == ' ' || c == '/' || c == '>';
}

static inline bool is_email_char(int32_t c) {
    return is_unicode_letter(c) || is_unicode_digit(c);
}

static inline bool is_identifier_char(int32_t c) {
    return is_unicode_letter(c) || is_unicode_digit(c) || c == '_';
}

static inline bool is_identifier_start(int32_t c) {
    return is_unicode_letter(c) || c == '_';
}

void *tree_sitter_razor_external_scanner_create() {
    RazorScanner *scanner = ts_calloc(1, sizeof(RazorScanner));
    scanner->csharp_scanner = tree_sitter_c_sharp_external_scanner_create();
    array_init(&scanner->context_stack);
    return scanner;
}

void tree_sitter_razor_external_scanner_destroy(void *payload) {
    RazorScanner *scanner = (RazorScanner *)payload;
    tree_sitter_c_sharp_external_scanner_destroy(scanner->csharp_scanner);
    array_delete(&scanner->context_stack);
    ts_free(scanner);
}

unsigned tree_sitter_razor_external_scanner_serialize(void *payload, char *buffer) {
    RazorScanner *scanner = (RazorScanner *)payload;

    // First, serialize C# scanner state
    unsigned csharp_size = tree_sitter_c_sharp_external_scanner_serialize(scanner->csharp_scanner, buffer);

    // Check if we have room for Razor state
    unsigned razor_size = 1 + scanner->context_stack.size;  // 1 byte for count + stack contents
    if (csharp_size + razor_size > TREE_SITTER_SERIALIZATION_BUFFER_SIZE) {
        return 0;
    }

    // Append Razor state after C# state
    unsigned size = csharp_size;
    buffer[size++] = (char)scanner->context_stack.size;
    for (unsigned i = 0; i < scanner->context_stack.size; i++) {
        buffer[size++] = (char)scanner->context_stack.contents[i];
    }

    return size;
}

void tree_sitter_razor_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
    RazorScanner *scanner = (RazorScanner *)payload;

    array_clear(&scanner->context_stack);

    if (length == 0) {
        tree_sitter_c_sharp_external_scanner_deserialize(scanner->csharp_scanner, buffer, 0);
        return;
    }

    // The C# scanner serializes: 1 byte quote_count + 1 byte interpolation_count + 4 bytes per interpolation
    // We need to figure out how much of the buffer belongs to C#
    // Format: [quote_count:1][interp_count:1][interp_data:4*count][razor_context_count:1][context_data:count]

    unsigned char quote_count = (unsigned char)buffer[0];
    (void)quote_count;  // Not used directly, just for calculating size
    unsigned char interp_count = (unsigned char)buffer[1];
    unsigned csharp_size = 2 + interp_count * 4;

    // Deserialize C# state
    tree_sitter_c_sharp_external_scanner_deserialize(scanner->csharp_scanner, buffer, csharp_size);

    // Deserialize Razor state
    if (length > csharp_size) {
        unsigned context_count = (unsigned char)buffer[csharp_size++];
        array_extend(&scanner->context_stack, context_count, &buffer[csharp_size]);
    }
}

bool tree_sitter_razor_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
    RazorScanner *scanner = (RazorScanner *)payload;

    DEBUG_PRINT("scan called: ctx_size=%zu, lookahead='%c' (%d)\n",
                scanner->context_stack.size,
                lexer->lookahead > 31 && lexer->lookahead < 127 ? lexer->lookahead : '?',
                lexer->lookahead);
    if (scanner->context_stack.size > 0) {
        DEBUG_PRINT("  top context: %d\n", *array_back(&scanner->context_stack));
    }

    // RAZOR_BLOCK_AT in C# context - must check early to beat main lexer's @ token
    if (valid_symbols[RAZOR_BLOCK_AT] && in_csharp_context(scanner)) {
        while (is_whitespace(lexer->lookahead)) {
            razor_skip(lexer);
        }
        if (lexer->lookahead == '@') {
            razor_advance(lexer);
            int32_t after_at = lexer->lookahead;

            if (after_at != ':' && after_at != '@' && after_at != '*') {
                lexer->mark_end(lexer);
                lexer->result_symbol = RAZOR_BLOCK_AT;
                return true;
            }

            if (after_at == '*' && valid_symbols[RAZOR_COMMENT_START]) {
                razor_advance(lexer);
                lexer->mark_end(lexer);
                lexer->result_symbol = RAZOR_COMMENT_START;
                return true;
            }

            return false;
        }
    }

    // Implicit expression terminator (zero-width token)
    // Must check before whitespace skipping
    if (valid_symbols[IMPLICIT_EXPR_END] && !valid_symbols[RAZOR_BLOCK_OPEN]) {
        int32_t c = lexer->lookahead;

        if (is_whitespace(c) ||
            c == '<' || c == '@' || c == '"' || c == '\'' ||
            c == '>' || c == '}' || c == ')' || c == ']' ||
            c == ',' || c == ';' || c == ':' || c == '&' ||
            lexer->eof(lexer)) {
            lexer->result_symbol = IMPLICIT_EXPR_END;
            return true;
        }

        if (c == '.') {
            lexer->mark_end(lexer);
            razor_advance(lexer);
            int32_t after_dot = lexer->lookahead;
            if (is_unicode_letter(after_dot) || after_dot == '_') {
                return false;
            }
            lexer->result_symbol = IMPLICIT_EXPR_END;
            return true;
        }

        if (c == '?') {
            lexer->mark_end(lexer);
            razor_advance(lexer);
            int32_t after_q = lexer->lookahead;
            if (after_q == '.') {
                // ?. conditional access - don't end expression
                return false;
            }
            if (after_q == '[') {
                // ?[ conditional element access - either match as IMPLICIT_CONDITIONAL_BRACKET_OPEN or don't end
                if (valid_symbols[IMPLICIT_CONDITIONAL_BRACKET_OPEN]) {
                    razor_advance(lexer);
                    lexer->mark_end(lexer);
                    array_push(&scanner->context_stack, CONTEXT_CSHARP_BRACKET);
                    lexer->result_symbol = IMPLICIT_CONDITIONAL_BRACKET_OPEN;
                    return true;
                }
                return false;
            }
            lexer->result_symbol = IMPLICIT_EXPR_END;
            return true;
        }
    }

    // @{ or @( or @* tokens
    bool check_at_tokens = valid_symbols[CSHARP_CODE_BLOCK_START] ||
                           valid_symbols[CSHARP_EXPLICIT_EXPR_START] ||
                           valid_symbols[RAZOR_COMMENT];

    if (check_at_tokens) {
        while (is_whitespace(lexer->lookahead)) {
            razor_skip(lexer);
        }
        if (lexer->lookahead == '@') {
            razor_advance(lexer);

            if (valid_symbols[RAZOR_COMMENT] && !in_html_tag_context(scanner) && lexer->lookahead == '*') {
                razor_advance(lexer);
                while (!lexer->eof(lexer)) {
                    if (lexer->lookahead == '*') {
                        razor_advance(lexer);
                        if (lexer->lookahead == '@') {
                            razor_advance(lexer);
                            lexer->mark_end(lexer);
                            lexer->result_symbol = RAZOR_COMMENT;
                            return true;
                        }
                    } else {
                        razor_advance(lexer);
                    }
                }
                lexer->mark_end(lexer);
                lexer->result_symbol = RAZOR_COMMENT;
                return true;
            }

            if (valid_symbols[CSHARP_CODE_BLOCK_START] && lexer->lookahead == '{') {
                razor_advance(lexer);
                lexer->mark_end(lexer);
                array_push(&scanner->context_stack, CONTEXT_CSHARP_BRACE);
                lexer->result_symbol = CSHARP_CODE_BLOCK_START;
                return true;
            }
            if (valid_symbols[CSHARP_EXPLICIT_EXPR_START] && lexer->lookahead == '(') {
                razor_advance(lexer);
                lexer->mark_end(lexer);
                array_push(&scanner->context_stack, CONTEXT_CSHARP_PAREN);
                lexer->result_symbol = CSHARP_EXPLICIT_EXPR_START;
                return true;
            }
            return false;
        }
    }

    // Using directive lookahead (zero-width token to disambiguate @using forms)
    if (valid_symbols[USING_NOT_ALIAS] && scanner->context_stack.size == 0) {
        while (is_whitespace(lexer->lookahead)) {
            razor_skip(lexer);
        }
        if (lexer->lookahead != '=' && lexer->lookahead != '.') {
            lexer->result_symbol = USING_NOT_ALIAS;
            return true;
        }
        return false;
    }

    // Raw text content handlers (script, style, title, textarea)
    // Must come before HTML tag handling to consume non-matching tags as content

    // Script content
    if (valid_symbols[SCRIPT_CONTENT]) {
        if (lexer->lookahead == '<' && valid_symbols[HTML_END_TAG_OPEN]) {
            // Fall through to HTML tag handling
        } else {
            bool has_content = false;
            while (!lexer->eof(lexer)) {
                if (lexer->lookahead == '<') {
                    lexer->mark_end(lexer);
                    razor_advance(lexer);
                    if (lexer->lookahead == '/') {
                        razor_advance(lexer);
                        const char *tag = "script";
                        int i = 0;
                        bool matches = true;
                        while (tag[i] && matches) {
                            int32_t c = lexer->lookahead;
                            if (c != tag[i] && c != (tag[i] - 32)) {
                                matches = false;
                            } else {
                                razor_advance(lexer);
                                i++;
                            }
                        }
                        if (matches && i == 6 && is_end_tag_terminator(lexer->lookahead)) {
                            // Found closing tag - return content so far
                            break;
                        }
                    }
                    // Not closing tag - include as content
                    has_content = true;
                    lexer->mark_end(lexer);
                } else {
                    razor_advance(lexer);
                    has_content = true;
                    lexer->mark_end(lexer);
                }
            }
            if (has_content) {
                lexer->result_symbol = SCRIPT_CONTENT;
                return true;
            }
            return false;
        }
    }

    // Style content
    if (valid_symbols[STYLE_CONTENT]) {
        if (lexer->lookahead == '<' && valid_symbols[HTML_END_TAG_OPEN]) {
            // Fall through to HTML tag handling
        } else {
            bool has_content = false;
            while (!lexer->eof(lexer)) {
                if (lexer->lookahead == '<') {
                    lexer->mark_end(lexer);
                    razor_advance(lexer);
                    if (lexer->lookahead == '/') {
                        razor_advance(lexer);
                        const char *tag = "style";
                        int i = 0;
                        bool matches = true;
                        while (tag[i] && matches) {
                            int32_t c = lexer->lookahead;
                            if (c != tag[i] && c != (tag[i] - 32)) {
                                matches = false;
                            } else {
                                razor_advance(lexer);
                                i++;
                            }
                        }
                        if (matches && i == 5 && is_end_tag_terminator(lexer->lookahead)) {
                            break;
                        }
                    }
                    has_content = true;
                    lexer->mark_end(lexer);
                } else {
                    razor_advance(lexer);
                    has_content = true;
                    lexer->mark_end(lexer);
                }
            }
            if (has_content) {
                lexer->result_symbol = STYLE_CONTENT;
                return true;
            }
            return false;
        }
    }

    // Title content
    if (valid_symbols[TITLE_CONTENT]) {
        if (lexer->lookahead == '<') {
            // Fall through - let grammar handle tag
        } else {
            bool has_content = false;
            while (!lexer->eof(lexer)) {
                if (lexer->lookahead == '<') {
                    lexer->mark_end(lexer);
                    razor_advance(lexer);
                    if (lexer->lookahead == '/') {
                        razor_advance(lexer);
                        const char *tag = "title";
                        int i = 0;
                        bool matches = true;
                        while (tag[i] && matches) {
                            int32_t c = lexer->lookahead;
                            if (c != tag[i] && c != (tag[i] - 32)) {
                                matches = false;
                            } else {
                                razor_advance(lexer);
                                i++;
                            }
                        }
                        if (matches && i == 5 && is_end_tag_terminator(lexer->lookahead)) {
                            break;
                        }
                    }
                    has_content = true;
                    lexer->mark_end(lexer);
                } else {
                    razor_advance(lexer);
                    has_content = true;
                    lexer->mark_end(lexer);
                }
            }
            if (has_content) {
                lexer->result_symbol = TITLE_CONTENT;
                return true;
            }
            return false;
        }
    }

    // Textarea content (case-insensitive end tag matching)
    if (valid_symbols[TEXTAREA_CONTENT]) {
        if (lexer->lookahead == '<') {
            razor_advance(lexer);
            if (lexer->lookahead == '/') {
                razor_advance(lexer);
                if (lexer->lookahead == '!') {
                    razor_advance(lexer);
                }
                lexer->mark_end(lexer);

                const char *tag = "textarea";
                int i = 0;
                bool matches = true;
                while (tag[i] && matches) {
                    int32_t c = lexer->lookahead;
                    int32_t lower_c = (c >= 'A' && c <= 'Z') ? c + 32 : c;
                    if (lower_c != tag[i]) {
                        matches = false;
                    } else {
                        razor_advance(lexer);
                        i++;
                    }
                }
                if (matches && i == 8 && is_end_tag_terminator(lexer->lookahead)) {
                    array_push(&scanner->context_stack, CONTEXT_HTML_TAG);
                    lexer->result_symbol = HTML_END_TAG_OPEN;
                    return true;
                }
                // Not </textarea> - consume as content until real </textarea>
                lexer->mark_end(lexer);
                while (!lexer->eof(lexer)) {
                    if (lexer->lookahead == '<') {
                        lexer->mark_end(lexer);
                        razor_advance(lexer);
                        if (lexer->lookahead == '/') {
                            razor_advance(lexer);
                            if (lexer->lookahead == '!') {
                                razor_advance(lexer);
                            }
                            const char *tag2 = "textarea";
                            int j = 0;
                            bool matches2 = true;
                            while (tag2[j] && matches2) {
                                int32_t c = lexer->lookahead;
                                int32_t lower_c = (c >= 'A' && c <= 'Z') ? c + 32 : c;
                                if (lower_c != tag2[j]) {
                                    matches2 = false;
                                } else {
                                    razor_advance(lexer);
                                    j++;
                                }
                            }
                            if (matches2 && j == 8 && is_end_tag_terminator(lexer->lookahead)) {
                                break;
                            }
                        }
                        lexer->mark_end(lexer);
                    } else {
                        razor_advance(lexer);
                        lexer->mark_end(lexer);
                    }
                }
                lexer->result_symbol = TEXTAREA_CONTENT;
                return true;
            } else {
                // '<' not followed by '/' - return HTML_TAG_OPEN for speculative parsing
                if (lexer->lookahead == '!') {
                    razor_advance(lexer);
                }
                lexer->mark_end(lexer);
                array_push(&scanner->context_stack, CONTEXT_HTML_TAG);
                lexer->result_symbol = HTML_TAG_OPEN;
                return true;
            }
        } else {
            bool has_content = false;
            while (!lexer->eof(lexer)) {
                if (lexer->lookahead == '<') {
                    lexer->mark_end(lexer);
                    razor_advance(lexer);
                    if (lexer->lookahead == '/') {
                        razor_advance(lexer);
                        const char *tag = "textarea";
                        int i = 0;
                        bool matches = true;
                        while (tag[i] && matches) {
                            int32_t c = lexer->lookahead;
                            int32_t lower_c = (c >= 'A' && c <= 'Z') ? c + 32 : c;
                            if (lower_c != tag[i]) {
                                matches = false;
                            } else {
                                razor_advance(lexer);
                                i++;
                            }
                        }
                        if (matches && i == 8 && is_end_tag_terminator(lexer->lookahead)) {
                            break;  // Found closing tag
                        }
                    }
                    has_content = true;
                    lexer->mark_end(lexer);
                } else {
                    razor_advance(lexer);
                    has_content = true;
                    lexer->mark_end(lexer);
                }
            }
            if (has_content) {
                lexer->result_symbol = TEXTAREA_CONTENT;
                return true;
            }
            return false;
        }
    }

    // HTML tag tokens (<, </, <!, <!--, <!DOCTYPE)
    if ((valid_symbols[HTML_TAG_OPEN] || valid_symbols[HTML_END_TAG_OPEN] ||
         valid_symbols[HTML_COMMENT] || valid_symbols[DOCTYPE]) && lexer->lookahead == '<') {
        razor_advance(lexer);

        if (lexer->lookahead == '/' && valid_symbols[HTML_END_TAG_OPEN]) {
            razor_advance(lexer);
            if (lexer->lookahead == '!') {
                razor_advance(lexer);
            }
            lexer->mark_end(lexer);
            array_push(&scanner->context_stack, CONTEXT_HTML_TAG);
            lexer->result_symbol = HTML_END_TAG_OPEN;
            return true;
        }

        if (lexer->lookahead == '!') {
            razor_advance(lexer);
            if (lexer->lookahead == '-' && valid_symbols[HTML_COMMENT]) {
                razor_advance(lexer);
                if (lexer->lookahead == '-') {
                    razor_advance(lexer);
                    while (!lexer->eof(lexer)) {
                        if (lexer->lookahead == '-') {
                            razor_advance(lexer);
                            if (lexer->lookahead == '-') {
                                razor_advance(lexer);
                                if (lexer->lookahead == '>') {
                                    razor_advance(lexer);
                                    lexer->mark_end(lexer);
                                    lexer->result_symbol = HTML_COMMENT;
                                    return true;
                                }
                            }
                        } else {
                            razor_advance(lexer);
                        }
                    }
                    lexer->mark_end(lexer);
                    lexer->result_symbol = HTML_COMMENT;
                    return true;
                }
                return false;
            }
            if ((lexer->lookahead == 'd' || lexer->lookahead == 'D') && valid_symbols[DOCTYPE]) {
                razor_advance(lexer);
                const char *expected = "octype";
                bool matched = true;
                for (int i = 0; expected[i] && matched; i++) {
                    if (lexer->lookahead != expected[i] &&
                        lexer->lookahead != expected[i] - 32 &&
                        lexer->lookahead != expected[i] + 32) {
                        matched = false;
                    } else {
                        razor_advance(lexer);
                    }
                }
                if (matched) {
                    while (!lexer->eof(lexer) && lexer->lookahead != '>') {
                        razor_advance(lexer);
                    }
                    if (lexer->lookahead == '>') {
                        razor_advance(lexer);
                    }
                    lexer->mark_end(lexer);
                    lexer->result_symbol = DOCTYPE;
                    return true;
                }
            }
            if (valid_symbols[HTML_TAG_OPEN]) {
                lexer->mark_end(lexer);
                array_push(&scanner->context_stack, CONTEXT_HTML_TAG);
                lexer->result_symbol = HTML_TAG_OPEN;
                return true;
            }
            return false;
        }

        if (valid_symbols[HTML_TAG_OPEN]) {
            lexer->mark_end(lexer);
            array_push(&scanner->context_stack, CONTEXT_HTML_TAG);
            lexer->result_symbol = HTML_TAG_OPEN;
            return true;
        }

        return false;
    }

    // > closes HTML tag
    DEBUG_PRINT("HTML_TAG_CLOSE check: valid=%d, in_html_tag=%d, lookahead='%c' (%d)\n",
                valid_symbols[HTML_TAG_CLOSE], in_html_tag_context(scanner),
                lexer->lookahead > 31 && lexer->lookahead < 127 ? lexer->lookahead : '?',
                lexer->lookahead);
    if (valid_symbols[HTML_TAG_CLOSE] && in_html_tag_context(scanner)) {
        // Skip whitespace before > (C# extras handle whitespace, but we need to skip it here)
        while (is_whitespace(lexer->lookahead)) {
            razor_skip(lexer);
        }
        if (lexer->lookahead == '>') {
            razor_advance(lexer);
            lexer->mark_end(lexer);
            array_pop(&scanner->context_stack);
            lexer->result_symbol = HTML_TAG_CLOSE;
            DEBUG_PRINT("  -> matched HTML_TAG_CLOSE\n");
            return true;
        }
    }

    // Text with literal @ (email addresses like user@example.com)
    if (valid_symbols[TEXT_WITH_LITERAL_AT] && !valid_symbols[HTML_TEXT_CONTENT]) {
        bool found_literal_at = false;
        bool last_was_word = false;

        while (!lexer->eof(lexer) && lexer->lookahead != '<' &&
               lexer->lookahead != '"' && lexer->lookahead != '\'') {

            if (lexer->lookahead == '@') {
                if (last_was_word) {
                    razor_advance(lexer);
                    if (is_email_char(lexer->lookahead)) {
                        found_literal_at = true;
                        while (is_email_char(lexer->lookahead) ||
                               lexer->lookahead == '.' ||
                               lexer->lookahead == '-') {
                            razor_advance(lexer);
                        }
                        lexer->mark_end(lexer);
                        last_was_word = false;
                        continue;
                    }
                }
                break;
            }

            last_was_word = is_email_char(lexer->lookahead);
            razor_advance(lexer);

            if (found_literal_at) {
                lexer->mark_end(lexer);
            }
        }

        if (found_literal_at) {
            lexer->result_symbol = TEXT_WITH_LITERAL_AT;
            return true;
        }
    }

    // HTML text content - stops before keywords (else/catch/finally/where)
    if (valid_symbols[HTML_TEXT_CONTENT] && !in_csharp_context(scanner)) {
        if (lexer->lookahead == '"' || lexer->lookahead == '\'') {
            // Fall through - let grammar try string_literal
        } else {
        bool has_content = false;
        bool has_nonwhitespace = false;
        bool found_keyword = false;
        bool at_line_start = true;

        while (!lexer->eof(lexer)) {
            if (lexer->lookahead == '<' || lexer->lookahead == '@') break;
            if (lexer->lookahead == '[' || lexer->lookahead == '(' || lexer->lookahead == '.') break;
            if (lexer->lookahead == '{' || lexer->lookahead == '}') break;

            if (lexer->lookahead == '\n' || lexer->lookahead == '\r') {
                razor_advance(lexer);
                has_content = true;
                lexer->mark_end(lexer);
                at_line_start = true;
                continue;
            }

            if (at_line_start && is_whitespace(lexer->lookahead)) {
                razor_advance(lexer);
                has_content = true;
                lexer->mark_end(lexer);
                continue;
            }

            // Check for keywords that should stop text scanning
            bool check_keyword = false;
            char keyword_buf[8] = {0};
            int keyword_len = 0;
            int32_t start_char = lexer->lookahead;

            if (at_line_start && (start_char == 'e' || start_char == 'c' || start_char == 'f')) {
                check_keyword = true;
            }
            if (start_char == 'w' && !has_nonwhitespace) {
                check_keyword = true;
            }

            if (check_keyword) {
                lexer->mark_end(lexer);
                while (keyword_len < 7 && is_identifier_char(lexer->lookahead)) {
                    keyword_buf[keyword_len++] = (char)lexer->lookahead;
                    razor_advance(lexer);
                }
                keyword_buf[keyword_len] = '\0';

                bool is_keyword = false;
                if (!is_identifier_char(lexer->lookahead)) {
                    if (at_line_start) {
                        if (start_char == 'e' && strcmp(keyword_buf, "else") == 0) is_keyword = true;
                        else if (start_char == 'c' && strcmp(keyword_buf, "catch") == 0) is_keyword = true;
                        else if (start_char == 'f' && strcmp(keyword_buf, "finally") == 0) is_keyword = true;
                    }
                    if (start_char == 'w' && strcmp(keyword_buf, "where") == 0) is_keyword = true;
                }

                if (is_keyword) {
                    found_keyword = true;
                    break;
                }

                has_content = true;
                has_nonwhitespace = true;
                lexer->mark_end(lexer);
                at_line_start = false;
                continue;
            }

            razor_advance(lexer);
            has_content = true;
            has_nonwhitespace = true;
            lexer->mark_end(lexer);
            at_line_start = false;
        }

        if (found_keyword) {
            return false;
        }

        if (has_nonwhitespace) {
            lexer->result_symbol = HTML_TEXT_CONTENT;
            return true;
        }
        }
    }

    // { opens Razor block (enters C# brace context)
    if (valid_symbols[RAZOR_BLOCK_OPEN] && !valid_symbols[CSHARP_CONTEXT_CLOSE]) {
        while (is_whitespace(lexer->lookahead)) {
            razor_skip(lexer);
        }
        if (lexer->lookahead == '{') {
            razor_advance(lexer);
            lexer->mark_end(lexer);
            array_push(&scanner->context_stack, CONTEXT_CSHARP_BRACE);
            lexer->result_symbol = RAZOR_BLOCK_OPEN;
            return true;
        }
    }

    // ( in implicit expression - push C# paren context
    if (valid_symbols[IMPLICIT_PAREN_OPEN]) {
        while (is_whitespace(lexer->lookahead)) {
            razor_skip(lexer);
        }
        if (lexer->lookahead == '(') {
            razor_advance(lexer);
            lexer->mark_end(lexer);
            array_push(&scanner->context_stack, CONTEXT_CSHARP_PAREN);
            lexer->result_symbol = IMPLICIT_PAREN_OPEN;
            return true;
        }
    }

    // [ in implicit expression - push C# bracket context
    if (valid_symbols[IMPLICIT_BRACKET_OPEN]) {
        while (is_whitespace(lexer->lookahead)) {
            razor_skip(lexer);
        }
        if (lexer->lookahead == '[') {
            razor_advance(lexer);
            lexer->mark_end(lexer);
            array_push(&scanner->context_stack, CONTEXT_CSHARP_BRACKET);
            lexer->result_symbol = IMPLICIT_BRACKET_OPEN;
            return true;
        }
    }

    // } or ) or ] closes C# context
    if (valid_symbols[CSHARP_CONTEXT_CLOSE] && scanner->context_stack.size > 0) {
        while (is_whitespace(lexer->lookahead)) {
            razor_skip(lexer);
        }

        ContextType top = scanner->context_stack.contents[scanner->context_stack.size - 1];
        if ((top == CONTEXT_CSHARP_BRACE && lexer->lookahead == '}') ||
            (top == CONTEXT_CSHARP_PAREN && lexer->lookahead == ')') ||
            (top == CONTEXT_CSHARP_BRACKET && lexer->lookahead == ']')) {
            razor_advance(lexer);
            lexer->mark_end(lexer);
            array_pop(&scanner->context_stack);
            lexer->result_symbol = CSHARP_CONTEXT_CLOSE;
            return true;
        }
    }

    // @ in C# context (handles @* comment and nested @ expressions)
    if ((valid_symbols[RAZOR_COMMENT_START] || valid_symbols[RAZOR_BLOCK_AT]) && in_csharp_context(scanner)) {
        while (is_whitespace(lexer->lookahead)) {
            razor_skip(lexer);
        }

        if (lexer->lookahead == '@') {
            razor_advance(lexer);

            if (valid_symbols[RAZOR_COMMENT_START] && lexer->lookahead == '*') {
                razor_advance(lexer);
                lexer->mark_end(lexer);
                lexer->result_symbol = RAZOR_COMMENT_START;
                return true;
            }

            if (lexer->lookahead == ':' || lexer->lookahead == '@') {
                return false;
            }

            if (valid_symbols[RAZOR_BLOCK_AT]) {
                lexer->mark_end(lexer);
                lexer->result_symbol = RAZOR_BLOCK_AT;
                return true;
            }

            return false;
        }
    }

    // C# comments (only in C# context)
    if (valid_symbols[CSHARP_COMMENT] && in_csharp_context(scanner)) {
        if (lexer->lookahead == '/') {
            razor_advance(lexer);
            if (lexer->lookahead == '/') {
                razor_advance(lexer);
                while (!lexer->eof(lexer) && lexer->lookahead != '\n' && lexer->lookahead != '\r') {
                    razor_advance(lexer);
                }
                lexer->mark_end(lexer);
                lexer->result_symbol = CSHARP_COMMENT;
                return true;
            } else if (lexer->lookahead == '*') {
                razor_advance(lexer);
                while (!lexer->eof(lexer)) {
                    if (lexer->lookahead == '*') {
                        razor_advance(lexer);
                        if (lexer->lookahead == '/') {
                            razor_advance(lexer);
                            lexer->mark_end(lexer);
                            lexer->result_symbol = CSHARP_COMMENT;
                            return true;
                        }
                    } else {
                        razor_advance(lexer);
                    }
                }
                lexer->mark_end(lexer);
                lexer->result_symbol = CSHARP_COMMENT;
                return true;
            }
            return false;
        }
    }

    // Razor comment as extra (only in C# context)
    // This allows @* ... *@ to appear anywhere C# comments can appear
    if (valid_symbols[RAZOR_COMMENT_EXTRA] && in_csharp_context(scanner)) {
        while (is_whitespace(lexer->lookahead)) {
            razor_skip(lexer);
        }
        if (lexer->lookahead == '@') {
            razor_advance(lexer);
            if (lexer->lookahead == '*') {
                razor_advance(lexer);
                while (!lexer->eof(lexer)) {
                    if (lexer->lookahead == '*') {
                        razor_advance(lexer);
                        if (lexer->lookahead == '@') {
                            razor_advance(lexer);
                            lexer->mark_end(lexer);
                            lexer->result_symbol = RAZOR_COMMENT_EXTRA;
                            return true;
                        }
                    } else {
                        razor_advance(lexer);
                    }
                }
                lexer->mark_end(lexer);
                lexer->result_symbol = RAZOR_COMMENT_EXTRA;
                return true;
            }
            return false;
        }
    }

    // C# preprocessor directives (only in C# context)
    // Note: #if/#else/#elif/#endif are handled by C#'s grammar
    if (in_csharp_context(scanner) && lexer->lookahead == '#') {
        bool any_preproc_valid = valid_symbols[PREPROC_REGION] ||
                                  valid_symbols[PREPROC_ENDREGION] ||
                                  valid_symbols[PREPROC_LINE] ||
                                  valid_symbols[PREPROC_PRAGMA] ||
                                  valid_symbols[PREPROC_NULLABLE] ||
                                  valid_symbols[PREPROC_ERROR] ||
                                  valid_symbols[PREPROC_WARNING] ||
                                  valid_symbols[PREPROC_DEFINE] ||
                                  valid_symbols[PREPROC_UNDEF] ||
                                  valid_symbols[PREPROC_DIRECTIVE];

        if (any_preproc_valid) {
            lexer->mark_end(lexer);
            razor_advance(lexer);

            while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
                razor_advance(lexer);
            }

            char keyword[16] = {0};
            int keyword_len = 0;
            while (keyword_len < 15 && is_identifier_char(lexer->lookahead)) {
                keyword[keyword_len++] = (char)lexer->lookahead;
                razor_advance(lexer);
            }
            keyword[keyword_len] = '\0';
            lexer->mark_end(lexer);

            if (valid_symbols[PREPROC_REGION] && strcmp(keyword, "region") == 0) {
                lexer->result_symbol = PREPROC_REGION;
                return true;
            }
            if (valid_symbols[PREPROC_ENDREGION] && strcmp(keyword, "endregion") == 0) {
                lexer->result_symbol = PREPROC_ENDREGION;
                return true;
            }
            if (valid_symbols[PREPROC_LINE] && strcmp(keyword, "line") == 0) {
                lexer->result_symbol = PREPROC_LINE;
                return true;
            }
            if (valid_symbols[PREPROC_PRAGMA] && strcmp(keyword, "pragma") == 0) {
                lexer->result_symbol = PREPROC_PRAGMA;
                return true;
            }
            if (valid_symbols[PREPROC_NULLABLE] && strcmp(keyword, "nullable") == 0) {
                lexer->result_symbol = PREPROC_NULLABLE;
                return true;
            }
            if (valid_symbols[PREPROC_ERROR] && strcmp(keyword, "error") == 0) {
                lexer->result_symbol = PREPROC_ERROR;
                return true;
            }
            if (valid_symbols[PREPROC_WARNING] && strcmp(keyword, "warning") == 0) {
                lexer->result_symbol = PREPROC_WARNING;
                return true;
            }
            if (valid_symbols[PREPROC_DEFINE] && strcmp(keyword, "define") == 0) {
                lexer->result_symbol = PREPROC_DEFINE;
                return true;
            }
            if (valid_symbols[PREPROC_UNDEF] && strcmp(keyword, "undef") == 0) {
                lexer->result_symbol = PREPROC_UNDEF;
                return true;
            }

            if (valid_symbols[PREPROC_DIRECTIVE] && keyword_len > 0 &&
                strcmp(keyword, "if") != 0 &&
                strcmp(keyword, "else") != 0 &&
                strcmp(keyword, "elif") != 0 &&
                strcmp(keyword, "endif") != 0) {
                lexer->result_symbol = PREPROC_DIRECTIVE;
                return true;
            }

            return false;
        }
    }

    // Text literal content - captures everything from @: to end of line
    // Content will be re-parsed via tree-sitter injection to handle Razor/HTML
    if (valid_symbols[TEXT_LITERAL_CONTENT]) {
        bool has_content = false;
        while (!lexer->eof(lexer)) {
            // Stop at newline (text literal is single-line only)
            if (lexer->lookahead == '\r' || lexer->lookahead == '\n') {
                break;
            }
            razor_advance(lexer);
            has_content = true;
        }
        if (has_content) {
            lexer->mark_end(lexer);
            lexer->result_symbol = TEXT_LITERAL_CONTENT;
            return true;
        }
    }

    return tree_sitter_c_sharp_external_scanner_scan(scanner->csharp_scanner, lexer, valid_symbols);
}
