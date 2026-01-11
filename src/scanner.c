#include "tree_sitter/alloc.h"
#include "tree_sitter/array.h"
#include "tree_sitter/parser.h"
#include <stdint.h>
#include <string.h>
#include "../tree-sitter-c-sharp/src/scanner.c"
#include "tables/full_character_references.h"
#include "tables/short_character_references.h"

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
    TOP_LEVEL_CSHARP_COMMENT,
    FULL_CHARACTER_REFERENCE,
    SHORT_CHARACTER_REFERENCE,
    INVALID_CHARACTER_REFERENCE,
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

static inline bool is_end_tag_terminator(int32_t c) {
    return c == '\t' || c == '\n' || c == '\f' || c == ' ' || c == '/' || c == '>';
}

// Check for case-insensitive closing tag after "</". Advances lexer past tag name.
static bool check_closing_tag(TSLexer *lexer, const char *tag_name, int tag_len) {
    int i = 0;
    while (tag_name[i]) {
        int32_t c = lexer->lookahead;
        // Case-insensitive comparison (works for ASCII a-z/A-Z)
        if (c != tag_name[i] && c != (tag_name[i] - 32) && c != (tag_name[i] + 32)) {
            return false;
        }
        razor_advance(lexer);
        i++;
    }
    return i == tag_len && is_end_tag_terminator(lexer->lookahead);
}

// Scan raw text content until matching closing tag. Returns true if content found.
// Used for script and style elements which cannot contain nested HTML.
static bool scan_raw_text_content(TSLexer *lexer, const char *tag_name, int tag_len,
                                   enum RazorTokenType result_token) {
    bool has_content = false;
    while (!lexer->eof(lexer)) {
        if (lexer->lookahead == '<') {
            lexer->mark_end(lexer);
            razor_advance(lexer);
            if (lexer->lookahead == '/') {
                razor_advance(lexer);
                if (check_closing_tag(lexer, tag_name, tag_len)) {
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
        lexer->result_symbol = result_token;
        return true;
    }
    return false;
}

// Scan escapable raw text content (title, textarea) until matching closing tag.
// Unlike raw text content, this handles HTML tag context transitions properly:
// - Returns HTML_END_TAG_OPEN when the matching closing tag is found
// - Returns HTML_TAG_OPEN for nested start tags if valid
// - Stops at & to allow grammar to parse character references
// - Stops at @ to allow grammar to parse Razor expressions
// - Consumes non-matching end tags as content
static bool scan_escapable_raw_text_content(TSLexer *lexer, RazorScanner *scanner,
                                             const char *tag_name, int tag_len,
                                             enum RazorTokenType result_token,
                                             const bool *valid_symbols) {
    // Handle < at start - could be closing tag or nested start tag
    if (lexer->lookahead == '<') {
        razor_advance(lexer);
        if (lexer->lookahead == '/') {
            razor_advance(lexer);
            lexer->mark_end(lexer);
            if (check_closing_tag(lexer, tag_name, tag_len)) {
                // Matching closing tag - return HTML_END_TAG_OPEN
                if (valid_symbols[HTML_END_TAG_OPEN]) {
                    array_push(&scanner->context_stack, CONTEXT_HTML_TAG);
                    lexer->result_symbol = HTML_END_TAG_OPEN;
                    return true;
                }
            }
            // Non-matching end tag - consume as content until matching closing tag, &, @, or start tag
            lexer->mark_end(lexer);
            while (!lexer->eof(lexer)) {
                if (lexer->lookahead == '&' || lexer->lookahead == '@') {
                    // Stop before & or @ to let grammar parse
                    break;
                }
                if (lexer->lookahead == '<') {
                    lexer->mark_end(lexer);
                    razor_advance(lexer);
                    if (lexer->lookahead == '/') {
                        razor_advance(lexer);
                        if (check_closing_tag(lexer, tag_name, tag_len)) break;
                        // Non-matching end tag - continue
                        lexer->mark_end(lexer);
                    } else {
                        // Start tag - stop before it
                        break;
                    }
                } else {
                    razor_advance(lexer);
                    lexer->mark_end(lexer);
                }
            }
            lexer->result_symbol = result_token;
            return true;
        } else if (valid_symbols[HTML_TAG_OPEN]) {
            // Start tag inside escapable raw text
            if (lexer->lookahead == '!') razor_advance(lexer);
            lexer->mark_end(lexer);
            array_push(&scanner->context_stack, CONTEXT_HTML_TAG);
            lexer->result_symbol = HTML_TAG_OPEN;
            return true;
        }
    }

    // Don't start at & or @ - let grammar parse character reference or Razor expression
    if (lexer->lookahead == '&' || lexer->lookahead == '@') {
        return false;
    }

    // Scan content until matching closing tag, &, @, or EOF
    // Non-matching end tags (e.g., </titlex> inside <title>) are consumed as content
    bool has_content = false;
    while (!lexer->eof(lexer)) {
        if (lexer->lookahead == '&' || lexer->lookahead == '@') {
            break;
        }
        if (lexer->lookahead == '<') {
            // Check if this is the matching closing tag
            lexer->mark_end(lexer);
            razor_advance(lexer);
            if (lexer->lookahead == '/') {
                razor_advance(lexer);
                if (check_closing_tag(lexer, tag_name, tag_len)) {
                    // Matching closing tag found - stop before it
                    break;
                }
                // Non-matching end tag - continue scanning
                has_content = true;
                lexer->mark_end(lexer);
            } else {
                // Start tag - stop before it (let grammar handle)
                break;
            }
        } else {
            razor_advance(lexer);
            has_content = true;
            lexer->mark_end(lexer);
        }
    }
    if (has_content) {
        lexer->result_symbol = result_token;
        return true;
    }
    return false;
}

// Match C# comment (// or /* */). Returns true and sets result_symbol on success.
static bool scan_csharp_comment(TSLexer *lexer, enum RazorTokenType result_token) {
    if (lexer->lookahead != '/') return false;

    razor_advance(lexer);
    if (lexer->lookahead == '/') {
        // Single-line comment
        razor_advance(lexer);
        while (!lexer->eof(lexer) && lexer->lookahead != '\n' && lexer->lookahead != '\r') {
            razor_advance(lexer);
        }
        lexer->mark_end(lexer);
        lexer->result_symbol = result_token;
        return true;
    } else if (lexer->lookahead == '*') {
        // Block comment
        razor_advance(lexer);
        while (!lexer->eof(lexer)) {
            if (lexer->lookahead == '*') {
                razor_advance(lexer);
                if (lexer->lookahead == '/') {
                    razor_advance(lexer);
                    lexer->mark_end(lexer);
                    lexer->result_symbol = result_token;
                    return true;
                }
            } else {
                razor_advance(lexer);
            }
        }
        // Unterminated block comment - still return it
        lexer->mark_end(lexer);
        lexer->result_symbol = result_token;
        return true;
    }
    return false;
}

// Scan character reference starting at &.
// Returns true if a valid character reference was found.
// Sets result_symbol to FULL_CHARACTER_REFERENCE, SHORT_CHARACTER_REFERENCE, or INVALID_CHARACTER_REFERENCE.
static bool scan_character_reference(TSLexer *lexer, const bool *valid_symbols) {
    if (lexer->lookahead != '&') return false;

    razor_advance(lexer);

    // Numeric character reference: &#[0-9]+; or &#[xX][0-9a-fA-F]+;
    if (lexer->lookahead == '#') {
        razor_advance(lexer);
        bool hex = false;
        if (lexer->lookahead == 'x' || lexer->lookahead == 'X') {
            hex = true;
            razor_advance(lexer);
        }

        bool has_digits = false;
        if (hex) {
            while ((lexer->lookahead >= '0' && lexer->lookahead <= '9') ||
                   (lexer->lookahead >= 'a' && lexer->lookahead <= 'f') ||
                   (lexer->lookahead >= 'A' && lexer->lookahead <= 'F')) {
                razor_advance(lexer);
                has_digits = true;
            }
        } else {
            while (lexer->lookahead >= '0' && lexer->lookahead <= '9') {
                razor_advance(lexer);
                has_digits = true;
            }
        }

        if (!has_digits) return false;

        if (lexer->lookahead == ';') {
            razor_advance(lexer);
            lexer->mark_end(lexer);
            // Numeric references are always "full" (require semicolon)
            lexer->result_symbol = FULL_CHARACTER_REFERENCE;
            return true;
        }
        return false;
    }

    // Named character reference: &[a-zA-Z][a-zA-Z0-9]*;?
    if (!((lexer->lookahead >= 'a' && lexer->lookahead <= 'z') ||
          (lexer->lookahead >= 'A' && lexer->lookahead <= 'Z'))) {
        return false;
    }

    // Collect the name, checking for short reference matches at each step
    // This handles cases like &notit; which should match &not (short) + it;
    char name[64];
    int name_len = 0;
    bool found_short = false;

    lexer->mark_end(lexer);  // Mark position after &

    while (name_len < 63 &&
           ((lexer->lookahead >= 'a' && lexer->lookahead <= 'z') ||
            (lexer->lookahead >= 'A' && lexer->lookahead <= 'Z') ||
            (lexer->lookahead >= '0' && lexer->lookahead <= '9'))) {
        name[name_len++] = (char)lexer->lookahead;
        razor_advance(lexer);

        // Check if the current prefix is a valid short reference
        // Per HTML spec, we want the longest matching short reference
        if (valid_symbols[SHORT_CHARACTER_REFERENCE]) {
            if (lookup_short_character_reference(name, name_len) != ShortCharacterReference_Unknown) {
                found_short = true;
                lexer->mark_end(lexer);
                lexer->result_symbol = SHORT_CHARACTER_REFERENCE;
            }
        }
    }
    name[name_len] = '\0';

    if (name_len == 0) return false;

    // Check for semicolon
    if (lexer->lookahead == ';') {
        razor_advance(lexer);
        // With semicolon - check full table
        if (lookup_full_character_reference(name, name_len) != FullCharacterReference_Unknown) {
            lexer->mark_end(lexer);
            lexer->result_symbol = FULL_CHARACTER_REFERENCE;
            return true;
        } else if (found_short) {
            // Name with semicolon not in full table, but we found a short prefix earlier
            // Return the short match (mark_end was already called at the right position)
            return true;
        } else if (valid_symbols[INVALID_CHARACTER_REFERENCE]) {
            // Invalid: matches pattern but not in either table
            lexer->mark_end(lexer);
            lexer->result_symbol = INVALID_CHARACTER_REFERENCE;
            return true;
        }
        return false;
    } else if (found_short) {
        // No semicolon but valid short reference (mark_end already called at right position)
        return true;
    }

    return false;
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
    DEBUG_PRINT("  valid: IMPL_END=%d TEXT_AT=%d HTML_TEXT=%d\n",
                valid_symbols[IMPLICIT_EXPR_END], valid_symbols[TEXT_WITH_LITERAL_AT],
                valid_symbols[HTML_TEXT_CONTENT]);

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
    // But if HTML_TAG_OPEN is valid and we see '<', let that handler run instead
    if (valid_symbols[IMPLICIT_EXPR_END] && !valid_symbols[RAZOR_BLOCK_OPEN]) {
        int32_t c = lexer->lookahead;

        // If we see '<' and HTML_TAG_OPEN is valid, don't return IMPLICIT_EXPR_END here
        // Let the HTML tag handler consume the '<' instead
        if (c == '<' && valid_symbols[HTML_TAG_OPEN]) {
            // Fall through to HTML tag handling
        } else if (is_whitespace(c) ||
            c == '<' || c == '@' || c == '"' || c == '\'' ||
            c == '>' || c == '}' || c == ')' || c == ']' ||
            c == ',' || c == ';' || c == ':' || c == '&' ||
            c == '/' || c == '#' ||  // URL path separator, hash for fragments
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
    // Only check for @ tokens if we see @ (or whitespace that could precede @)
    // But don't consume whitespace if HTML_TEXT_CONTENT is valid, as that whitespace
    // might be part of text content
    bool check_at_tokens = valid_symbols[CSHARP_CODE_BLOCK_START] ||
                           valid_symbols[CSHARP_EXPLICIT_EXPR_START] ||
                           valid_symbols[RAZOR_COMMENT];

    DEBUG_PRINT("  check_at_tokens=%d, lookahead='%c'\n", check_at_tokens, lexer->lookahead);
    if (check_at_tokens) {
        // Don't skip whitespace if we're inside text content (HTML, title, or textarea)
        // as that whitespace should be captured as part of the content
        if (!valid_symbols[HTML_TEXT_CONTENT] && !valid_symbols[TITLE_CONTENT] && !valid_symbols[TEXTAREA_CONTENT]) {
            while (is_whitespace(lexer->lookahead)) {
                DEBUG_PRINT("  Skipping whitespace in check_at_tokens\n");
                razor_skip(lexer);
            }
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
    // Don't match if HTML_TAG_OPEN is valid and we're at '<' (after skipping whitespace)
    if (valid_symbols[USING_NOT_ALIAS] && scanner->context_stack.size == 0) {
        while (is_whitespace(lexer->lookahead)) {
            razor_skip(lexer);
        }
        // After skipping whitespace, check if we're at '<' with HTML_TAG_OPEN valid
        if (lexer->lookahead == '<' && valid_symbols[HTML_TAG_OPEN]) {
            // Fall through to HTML tag handling
        } else if (lexer->lookahead != '=' && lexer->lookahead != '.') {
            lexer->result_symbol = USING_NOT_ALIAS;
            return true;
        } else {
            return false;
        }
    }

    // Raw text content (script, style, title, textarea)
    // At '<' with HTML_END_TAG_OPEN valid: fall through to HTML tag handler
    // Otherwise: scan content until matching closing tag
    if (valid_symbols[SCRIPT_CONTENT]) {
        if (!(lexer->lookahead == '<' && valid_symbols[HTML_END_TAG_OPEN])) {
            if (scan_raw_text_content(lexer, "script", 6, SCRIPT_CONTENT)) return true;
        }
    }

    if (valid_symbols[STYLE_CONTENT]) {
        if (!(lexer->lookahead == '<' && valid_symbols[HTML_END_TAG_OPEN])) {
            if (scan_raw_text_content(lexer, "style", 5, STYLE_CONTENT)) return true;
        }
    }

    // Escapable raw text content (title, textarea)
    // These elements can contain character references and need proper HTML tag context handling
    if (valid_symbols[TITLE_CONTENT]) {
        if (scan_escapable_raw_text_content(lexer, scanner, "title", 5, TITLE_CONTENT, valid_symbols)) {
            return true;
        }
    }

    if (valid_symbols[TEXTAREA_CONTENT]) {
        if (scan_escapable_raw_text_content(lexer, scanner, "textarea", 8, TEXTAREA_CONTENT, valid_symbols)) {
            return true;
        }
    }

    // Character references (&...; or &... for short references)
    // Short references (without semicolon) are NOT allowed in attribute values
    // Check if we're in an HTML tag context to determine if short refs are allowed
    if (lexer->lookahead == '&' &&
        (valid_symbols[FULL_CHARACTER_REFERENCE] ||
         valid_symbols[SHORT_CHARACTER_REFERENCE] ||
         valid_symbols[INVALID_CHARACTER_REFERENCE])) {
        if (scan_character_reference(lexer, valid_symbols)) {
            return true;
        }
    }

    // HTML tag tokens (<, </, <!, <!--, <!DOCTYPE)
    // Skip leading whitespace for end tags (e.g., newlines before </div>)
    if (valid_symbols[HTML_TAG_OPEN] || valid_symbols[HTML_END_TAG_OPEN] ||
        valid_symbols[HTML_COMMENT] || valid_symbols[DOCTYPE]) {
        // For end tags, skip whitespace first - BUT only if HTML_TEXT_CONTENT is not valid
        // Otherwise the whitespace might need to be returned as text
        while (valid_symbols[HTML_END_TAG_OPEN] && !valid_symbols[HTML_TEXT_CONTENT] &&
               is_whitespace(lexer->lookahead)) {
            razor_skip(lexer);
        }
        if (lexer->lookahead == '<') {
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
    }

    // > closes HTML tag
    if (valid_symbols[HTML_TAG_CLOSE] && in_html_tag_context(scanner)) {
        while (is_whitespace(lexer->lookahead)) razor_skip(lexer);
        if (lexer->lookahead == '>') {
            razor_advance(lexer);
            lexer->mark_end(lexer);
            array_pop(&scanner->context_stack);
            lexer->result_symbol = HTML_TAG_CLOSE;
            return true;
        }
    }

    // Text with literal @ (email addresses like user@example.com)
    DEBUG_PRINT("  Before TEXT_AT: lookahead='%c' (%d)\n", lexer->lookahead, lexer->lookahead);
    if (valid_symbols[TEXT_WITH_LITERAL_AT] && !valid_symbols[HTML_TEXT_CONTENT]) {
        DEBUG_PRINT("  TEXT_AT block entered\n");
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

    // Top-level C# comments (between control flow clauses like } // comment \n else)
    // Must check before HTML_TEXT_CONTENT to handle \n// correctly
    // BUT only skip whitespace if we're NOT inside an element (HTML_END_TAG_OPEN not valid)
    // When inside an element, whitespace might need to be preserved as text
    if (valid_symbols[TOP_LEVEL_CSHARP_COMMENT] && !in_csharp_context(scanner)) {
        if (!valid_symbols[HTML_END_TAG_OPEN]) {
            while (is_whitespace(lexer->lookahead)) {
                razor_skip(lexer);
            }
        }
        if (scan_csharp_comment(lexer, TOP_LEVEL_CSHARP_COMMENT)) return true;
    }

    // Top-level Razor comments (between control flow clauses)
    // Note: Only handles @* comments; regular @ expressions are handled by the grammar
    // Only skip whitespace if HTML_TEXT is NOT valid - otherwise let HTML_TEXT handler decide
    if (valid_symbols[RAZOR_COMMENT] && !in_csharp_context(scanner) && !in_html_tag_context(scanner)) {
        if (!valid_symbols[HTML_TEXT_CONTENT]) {
            while (is_whitespace(lexer->lookahead)) {
                razor_skip(lexer);
            }
        }
        if (lexer->lookahead == '@') {
            // Peek ahead to check for comment without consuming @
            lexer->mark_end(lexer);
            razor_advance(lexer);
            if (lexer->lookahead == '*') {
                // It's a Razor comment - consume and scan
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
            // Not a comment - return false without consuming the @
            // The grammar will handle @ as a Razor expression
            return false;
        }
    }

    // HTML text content - stops before C# keywords (else/catch/finally/where)
    // Note: Works in both HTML and C# context because elements inside Razor blocks need text scanning
    if (valid_symbols[HTML_TEXT_CONTENT]) {
        DEBUG_PRINT("  HTML_TEXT: starting, lookahead='%c' (%d)\n", lexer->lookahead, lexer->lookahead);
        if (lexer->lookahead == '"' || lexer->lookahead == '\'') {
            // Let grammar try string_literal
            DEBUG_PRINT("  HTML_TEXT: quote char, skipping\n");
        } else if (is_whitespace(lexer->lookahead)) {
            // At whitespace - we MUST handle this to ensure whitespace before keywords
            // doesn't become a separate text token that blocks keyword matching

            // First, consume the whitespace
            while (is_whitespace(lexer->lookahead)) {
                razor_advance(lexer);
            }

            // Now check what follows the whitespace
            if (lexer->lookahead == '"' || lexer->lookahead == '\'') {
                // Whitespace followed by quote - let grammar handle string literal
                // Don't return the whitespace; it's just formatting before a directive arg
                DEBUG_PRINT("  HTML_TEXT: whitespace then quote, skipping\n");
                return false;
            }
            if (lexer->lookahead == '/' && (valid_symbols[CSHARP_COMMENT] || valid_symbols[TOP_LEVEL_CSHARP_COMMENT])) {
                // Whitespace followed by / - might be a C# comment, skip whitespace
                DEBUG_PRINT("  HTML_TEXT: whitespace then slash, skipping for comment\n");
                return false;
            }
            if (lexer->lookahead == '@') {
                // Peek ahead to see if it's @* (Razor comment)
                lexer->mark_end(lexer);
                razor_advance(lexer);  // consume @
                if (lexer->lookahead == '*' && valid_symbols[RAZOR_COMMENT]) {
                    // It's a Razor comment - scan and return it directly
                    DEBUG_PRINT("  HTML_TEXT: whitespace then @*, scanning comment\n");
                    razor_advance(lexer);  // consume *
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
                // Not a comment - the @ is a Razor expression
                // But we're inside an element (HTML_END_TAG_OPEN valid), so return whitespace as text
                // and let the grammar handle the @ on the next call
                if (valid_symbols[HTML_END_TAG_OPEN]) {
                    // We consumed @ but that's fine - mark_end was called before @
                    // Return just the whitespace as text
                    DEBUG_PRINT("  HTML_TEXT: whitespace then @ inside element, returning whitespace\n");
                    lexer->result_symbol = HTML_TEXT_CONTENT;
                    return true;
                }
                // At top level - skip whitespace for the Razor expression
                DEBUG_PRINT("  HTML_TEXT: whitespace then @ at top level, skipping for Razor\n");
                return false;
            }

            // Check for C# keywords (else/catch/finally/where) that could continue a statement
            if (lexer->lookahead == 'e' || lexer->lookahead == 'c' || lexer->lookahead == 'f' ||
                lexer->lookahead == 'w') {
                // Save position after whitespace (before potential keyword)
                lexer->mark_end(lexer);

                char keyword_buf[8] = {0};
                int keyword_len = 0;
                int32_t start_char = lexer->lookahead;
                while (keyword_len < 7 && is_identifier_char(lexer->lookahead)) {
                    keyword_buf[keyword_len++] = (char)lexer->lookahead;
                    razor_advance(lexer);
                }
                keyword_buf[keyword_len] = '\0';

                if (!is_identifier_char(lexer->lookahead)) {
                    if ((start_char == 'e' && strcmp(keyword_buf, "else") == 0) ||
                        (start_char == 'c' && strcmp(keyword_buf, "catch") == 0) ||
                        (start_char == 'f' && strcmp(keyword_buf, "finally") == 0) ||
                        (start_char == 'w' && strcmp(keyword_buf, "where") == 0)) {
                        // Keyword found - return false so grammar can match the keyword
                        // Note: whitespace is NOT returned as text; it's skipped as formatting
                        DEBUG_PRINT("  HTML_TEXT: whitespace then keyword %s, skipping\n", keyword_buf);
                        return false;
                    }
                }

                // Not a keyword - return whitespace + word as text
                DEBUG_PRINT("  HTML_TEXT: whitespace then non-keyword word, returning as text\n");
                lexer->mark_end(lexer);  // Include the word we just read
                lexer->result_symbol = HTML_TEXT_CONTENT;
                return true;
            }

            // Check what follows the whitespace
            if (lexer->lookahead == '}' || lexer->lookahead == '{') {
                // Whitespace followed by brace - don't return text, let grammar handle block close
                DEBUG_PRINT("  HTML_TEXT: whitespace then brace, skipping\n");
                return false;
            }

            // At top level (HTML_END_TAG_OPEN not valid), skip whitespace before elements
            // This ensures <br>\n<hr> doesn't have text nodes between elements
            if (!valid_symbols[HTML_END_TAG_OPEN] && lexer->lookahead == '<') {
                DEBUG_PRINT("  HTML_TEXT: top-level whitespace before tag, skipping\n");
                return false;
            }

            // Inside elements (HTML_END_TAG_OPEN valid), return whitespace as text
            // This ensures spaces between character references like &amp; &lt; are captured
            lexer->mark_end(lexer);
            DEBUG_PRINT("  HTML_TEXT: whitespace as text (inside element=%d)\n",
                        valid_symbols[HTML_END_TAG_OPEN]);
            lexer->result_symbol = HTML_TEXT_CONTENT;
            return true;
        } else {
            bool has_content = false;
            bool has_nonwhitespace = false;
            bool found_keyword = false;
            bool at_line_start = true;

            bool last_was_email_char = false;  // Track if previous char was email-like

            while (!lexer->eof(lexer)) {
                DEBUG_PRINT("  HTML_TEXT loop: lookahead='%c' (%d), at_line_start=%d\n", lexer->lookahead, lexer->lookahead, at_line_start);
                if (lexer->lookahead == '<') break;
                if (lexer->lookahead == '[' || lexer->lookahead == '(' || lexer->lookahead == '.') break;
                if (lexer->lookahead == '{' || lexer->lookahead == '}') break;
                if (lexer->lookahead == '&') break;

                // Handle @ - check if it's part of an email address
                if (lexer->lookahead == '@') {
                    if (last_was_email_char) {
                        // Might be email - look ahead to see if followed by identifier
                        razor_advance(lexer);
                        if (is_email_char(lexer->lookahead)) {
                            // It's an email pattern - consume the domain part
                            while (is_email_char(lexer->lookahead) ||
                                   lexer->lookahead == '.' ||
                                   lexer->lookahead == '-') {
                                razor_advance(lexer);
                            }
                            has_content = true;
                            has_nonwhitespace = true;
                            lexer->mark_end(lexer);
                            last_was_email_char = false;
                            at_line_start = false;
                            continue;
                        }
                    }
                    break;  // Not an email - stop before @
                }

                if (lexer->lookahead == '\n' || lexer->lookahead == '\r') {
                    razor_advance(lexer);
                    has_content = true;
                    lexer->mark_end(lexer);
                    at_line_start = true;
                    last_was_email_char = false;
                    continue;
                }

                // Whitespace handling: consume and track, but only set has_nonwhitespace
                // if we've already seen non-whitespace content on this line
                if (is_whitespace(lexer->lookahead)) {
                    razor_advance(lexer);
                    has_content = true;
                    // Only set has_nonwhitespace if this is mid-line whitespace (not leading)
                    // This ensures "  else" at line start doesn't return whitespace as text
                    if (!at_line_start) {
                        has_nonwhitespace = true;
                    }
                    lexer->mark_end(lexer);
                    last_was_email_char = false;
                    continue;
                }

                if (at_line_start && (valid_symbols[CSHARP_COMMENT] || valid_symbols[TOP_LEVEL_CSHARP_COMMENT]) && lexer->lookahead == '/') {
                    break;
                }

                // Stop before C# keywords
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
                        DEBUG_PRINT("  Found keyword: %s\n", keyword_buf);
                        found_keyword = true;
                        break;
                    }

                    has_content = true;
                    has_nonwhitespace = true;
                    lexer->mark_end(lexer);
                    at_line_start = false;
                    last_was_email_char = is_email_char(keyword_buf[keyword_len - 1]);
                    continue;
                }

                last_was_email_char = is_email_char(lexer->lookahead);
                razor_advance(lexer);
                has_content = true;
                has_nonwhitespace = true;
                lexer->mark_end(lexer);
                at_line_start = false;
            }

            if (found_keyword) {
                DEBUG_PRINT("  HTML_TEXT returning false (found keyword, has_content=%d, has_nonws=%d)\n", has_content, has_nonwhitespace);
                return false;
            }

            if (has_nonwhitespace) {
                DEBUG_PRINT("  HTML_TEXT returning true (has_nonwhitespace)\n");
                lexer->result_symbol = HTML_TEXT_CONTENT;
                return true;
            } else if (has_content) {
                DEBUG_PRINT("  HTML_TEXT returning false (only whitespace, has_content=%d)\n", has_content);
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

    // C# comments
    if (valid_symbols[CSHARP_COMMENT] && in_csharp_context(scanner)) {
        if (scan_csharp_comment(lexer, CSHARP_COMMENT)) return true;
    }

    // Note: TOP_LEVEL_CSHARP_COMMENT is handled earlier, before HTML_TEXT_CONTENT

    // Razor comment as extra in C# context
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

    // Text literal content (@: to end of line)
    if (valid_symbols[TEXT_LITERAL_CONTENT]) {
        bool has_content = false;
        while (!lexer->eof(lexer) && lexer->lookahead != '\r' && lexer->lookahead != '\n') {
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
