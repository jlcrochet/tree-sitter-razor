#include "tree_sitter/alloc.h"
#include "tree_sitter/array.h"
#include "tree_sitter/parser.h"
#include <assert.h>
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

// Number of external tokens defined by the C# scanner.
// This must match the C# grammar's external token count.
// Run `make check-csharp-tokens` to verify this value is correct.
// The C# scanner's external tokens are (in order):
//   0: _optional_semi
//   1: interpolation_regular_start
//   2: interpolation_verbatim_start
//   3: interpolation_raw_start
//   4: interpolation_start_quote
//   5: interpolation_end_quote
//   6: interpolation_open_brace
//   7: interpolation_close_brace
//   8: interpolation_string_content
//   9: raw_string_start
//  10: raw_string_end
//  11: raw_string_content
// If the C# grammar adds or removes external tokens, update this constant.
#define CSHARP_TOKEN_COUNT 12

// C# scanner serialization format (for documentation and validation):
// [quote_count:1 byte][interp_count:1 byte][interp_data:4 bytes per interpolation]
// Total C# scanner serialized size = 2 + (interp_count * 4)
//
// Razor scanner serialization format (appended after C# state):
// [context_count:varint][context_data:1 byte each]
#define CSHARP_SERIALIZATION_HEADER_SIZE 2
#define CSHARP_INTERPOLATION_ENTRY_SIZE 4

typedef enum {
    RazorTokenType_TextWithLiteralAt = CSHARP_TOKEN_COUNT,
    RazorTokenType_HtmlTextContent,
    RazorTokenType_CsharpCodeBlockStart,
    RazorTokenType_CsharpExplicitExprStart,
    RazorTokenType_RazorBlockOpen,
    RazorTokenType_CsharpContextClose,
    RazorTokenType_CsharpComment,
    RazorTokenType_ScriptContent,
    RazorTokenType_StyleContent,
    RazorTokenType_TitleContent,
    RazorTokenType_TextareaContent,
    RazorTokenType_ImplicitExprEnd,
    RazorTokenType_RazorBlockAt,
    RazorTokenType_UsingNotAlias,
    RazorTokenType_RazorCommentStart,
    RazorTokenType_RazorComment,
    RazorTokenType_RazorCommentExtra,
    RazorTokenType_HtmlTagOpen,
    RazorTokenType_HtmlEndTagOpen,
    RazorTokenType_HtmlTagClose,
    RazorTokenType_HtmlComment,
    RazorTokenType_Doctype,
    RazorTokenType_ImplicitParenOpen,
    RazorTokenType_ImplicitBracketOpen,
    RazorTokenType_ImplicitConditionalBracketOpen,
    RazorTokenType_TextLiteralContent,
    RazorTokenType_TopLevelCsharpComment,
    RazorTokenType_FullCharacterReference,
    RazorTokenType_ShortCharacterReference,
    RazorTokenType_InvalidCharacterReference,
} RazorTokenType;

typedef enum {
    ContextType_Html = 0,
    ContextType_CsharpBrace = 1,
    ContextType_CsharpParen = 2,
    ContextType_HtmlTag = 3,
    ContextType_CsharpBracket = 4,
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
    return top == ContextType_CsharpBrace || top == ContextType_CsharpParen || top == ContextType_CsharpBracket;
}

static inline bool in_html_tag_context(RazorScanner *scanner) {
    return scanner->context_stack.size > 0 && *array_back(&scanner->context_stack) == ContextType_HtmlTag;
}

static inline bool is_end_tag_terminator(int32_t c) {
    return c == '\t' || c == '\n' || c == '\f' || c == ' ' || c == '/' || c == '>';
}

// Check for case-insensitive closing tag after "</". Advances lexer past tag name.
// Note: tag_name must be lowercase ASCII.
static bool check_closing_tag(TSLexer *lexer, const char *tag_name, int tag_len) {
    for (int i = 0; i < tag_len; i++) {
        if (lexer->eof(lexer)) return false;
        int32_t c = lexer->lookahead;
        // Normalize to lowercase for comparison
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c != tag_name[i]) {
            return false;
        }
        razor_advance(lexer);
    }
    return !lexer->eof(lexer) && is_end_tag_terminator(lexer->lookahead);
}

// Scan raw text content until matching closing tag. Returns true if content found.
// Used for script and style elements which cannot contain nested HTML.
static bool scan_raw_text_content(TSLexer *lexer, const char *tag_name, int tag_len,
                                   RazorTokenType result_token) {
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
// - Returns RazorTokenType_HtmlEndTagOpen when the matching closing tag is found
// - Returns RazorTokenType_HtmlTagOpen for nested start tags if valid
// - Stops at & to allow grammar to parse character references
// - Stops at @ to allow grammar to parse Razor expressions
// - Consumes non-matching end tags as content
static bool scan_escapable_raw_text_content(TSLexer *lexer, RazorScanner *scanner,
                                             const char *tag_name, int tag_len,
                                             RazorTokenType result_token,
                                             const bool *valid_symbols) {
    // Handle < at start - could be closing tag or nested start tag
    if (lexer->lookahead == '<') {
        razor_advance(lexer);
        if (lexer->lookahead == '/') {
            razor_advance(lexer);
            lexer->mark_end(lexer);
            if (check_closing_tag(lexer, tag_name, tag_len)) {
                // Matching closing tag - return RazorTokenType_HtmlEndTagOpen
                if (valid_symbols[RazorTokenType_HtmlEndTagOpen]) {
                    array_push(&scanner->context_stack, ContextType_HtmlTag);
                    lexer->result_symbol = RazorTokenType_HtmlEndTagOpen;
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
        } else if (valid_symbols[RazorTokenType_HtmlTagOpen]) {
            // Start tag inside escapable raw text
            if (lexer->lookahead == '!') razor_advance(lexer);
            lexer->mark_end(lexer);
            array_push(&scanner->context_stack, ContextType_HtmlTag);
            lexer->result_symbol = RazorTokenType_HtmlTagOpen;
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
static bool scan_csharp_comment(TSLexer *lexer, RazorTokenType result_token) {
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

// Scan Razor comment content (@* ... *@). Assumes @* has already been consumed.
// Returns true and sets result_symbol on success.
static bool scan_razor_comment_content(TSLexer *lexer, RazorTokenType result_token) {
    while (!lexer->eof(lexer)) {
        if (lexer->lookahead == '*') {
            razor_advance(lexer);
            if (lexer->lookahead == '@') {
                razor_advance(lexer);
                lexer->mark_end(lexer);
                lexer->result_symbol = result_token;
                return true;
            }
        } else {
            razor_advance(lexer);
        }
    }
    // Unterminated Razor comment - still return it
    lexer->mark_end(lexer);
    lexer->result_symbol = result_token;
    return true;
}

// Scan character reference starting at &.
// Returns true if a valid character reference was found.
// Sets result_symbol to RazorTokenType_FullCharacterReference, RazorTokenType_ShortCharacterReference, or RazorTokenType_InvalidCharacterReference.
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
            while (!lexer->eof(lexer) &&
                   ((lexer->lookahead >= '0' && lexer->lookahead <= '9') ||
                    (lexer->lookahead >= 'a' && lexer->lookahead <= 'f') ||
                    (lexer->lookahead >= 'A' && lexer->lookahead <= 'F'))) {
                razor_advance(lexer);
                has_digits = true;
            }
        } else {
            while (!lexer->eof(lexer) && lexer->lookahead >= '0' && lexer->lookahead <= '9') {
                razor_advance(lexer);
                has_digits = true;
            }
        }

        if (!has_digits) return false;

        if (lexer->lookahead == ';') {
            razor_advance(lexer);
            lexer->mark_end(lexer);
            // Numeric references are always "full" (require semicolon)
            lexer->result_symbol = RazorTokenType_FullCharacterReference;
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

    while (name_len < 63 && !lexer->eof(lexer) &&
           ((lexer->lookahead >= 'a' && lexer->lookahead <= 'z') ||
            (lexer->lookahead >= 'A' && lexer->lookahead <= 'Z') ||
            (lexer->lookahead >= '0' && lexer->lookahead <= '9'))) {
        name[name_len++] = (char)lexer->lookahead;
        razor_advance(lexer);

        // Check if the current prefix is a valid short reference
        // Per HTML spec, we want the longest matching short reference
        if (valid_symbols[RazorTokenType_ShortCharacterReference]) {
            if (lookup_short_character_reference(name, name_len) != ShortCharacterReference_Unknown) {
                found_short = true;
                lexer->mark_end(lexer);
                lexer->result_symbol = RazorTokenType_ShortCharacterReference;
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
            lexer->result_symbol = RazorTokenType_FullCharacterReference;
            return true;
        } else if (found_short) {
            // Name with semicolon not in full table, but we found a short prefix earlier
            // Return the short match (mark_end was already called at the right position)
            return true;
        } else if (valid_symbols[RazorTokenType_InvalidCharacterReference]) {
            // Invalid: matches pattern but not in either table
            lexer->mark_end(lexer);
            lexer->result_symbol = RazorTokenType_InvalidCharacterReference;
            return true;
        }
        return false;
    } else if (found_short) {
        // No semicolon but valid short reference (mark_end already called at right position)
        return true;
    }

    return false;
}

// Unicode letter ranges (sorted by start for binary search)
// Matches .NET's char.IsLetter() behavior
// Note: Latin-1 Supplement (0x00C0-0x00FF) excludes 0x00D7 and 0x00F7, so split into sub-ranges
static const TSCharacterRange unicode_letter_ranges[] = {
    {0x0041, 0x005A},  // ASCII uppercase
    {0x0061, 0x007A},  // ASCII lowercase
    {0x00C0, 0x00D6},  // Latin-1 Supplement (before ×)
    {0x00D8, 0x00F6},  // Latin-1 Supplement (between × and ÷)
    {0x00F8, 0x00FF},  // Latin-1 Supplement (after ÷)
    {0x0100, 0x017F},  // Latin Extended-A
    {0x0180, 0x024F},  // Latin Extended-B
    {0x0370, 0x03FF},  // Greek and Coptic
    {0x0400, 0x04FF},  // Cyrillic
    {0x0590, 0x05FF},  // Hebrew
    {0x0600, 0x06FF},  // Arabic
    {0x0900, 0x097F},  // Devanagari
    {0x0E00, 0x0E7F},  // Thai
    {0x3040, 0x309F},  // Hiragana
    {0x30A0, 0x30FF},  // Katakana
    {0x4E00, 0x9FFF},  // CJK Unified Ideographs
    {0xAC00, 0xD7AF},  // Hangul Syllables
};

// Check if character is a Unicode letter (approximation covering main categories)
static inline bool is_unicode_letter(int32_t c) {
    // Fast path for ASCII
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return true;
    if (c < 0x00C0) return false;
    return set_contains(unicode_letter_ranges,
                        sizeof(unicode_letter_ranges) / sizeof(unicode_letter_ranges[0]),
                        c);
}

// Unicode decimal digit ranges (sorted by start for binary search)
// Matches .NET's char.IsDigit() behavior (category Nd)
static const TSCharacterRange unicode_digit_ranges[] = {
    {0x0030, 0x0039},  // ASCII
    {0x0660, 0x0669},  // Arabic-Indic
    {0x06F0, 0x06F9},  // Extended Arabic-Indic
    {0x07C0, 0x07C9},  // NKo
    {0x0966, 0x096F},  // Devanagari
    {0x09E6, 0x09EF},  // Bengali
    {0x0A66, 0x0A6F},  // Gurmukhi
    {0x0AE6, 0x0AEF},  // Gujarati
    {0x0B66, 0x0B6F},  // Oriya
    {0x0BE6, 0x0BEF},  // Tamil
    {0x0C66, 0x0C6F},  // Telugu
    {0x0CE6, 0x0CEF},  // Kannada
    {0x0D66, 0x0D6F},  // Malayalam
    {0x0DE6, 0x0DEF},  // Sinhala
    {0x0E50, 0x0E59},  // Thai
    {0x0ED0, 0x0ED9},  // Lao
    {0x0F20, 0x0F29},  // Tibetan
    {0x1040, 0x1049},  // Myanmar
    {0x1090, 0x1099},  // Myanmar Shan
    {0x17E0, 0x17E9},  // Khmer
    {0x1810, 0x1819},  // Mongolian
    {0x1946, 0x194F},  // Limbu
    {0x19D0, 0x19D9},  // New Tai Lue
    {0x1A80, 0x1A89},  // Tai Tham Hora
    {0x1A90, 0x1A99},  // Tai Tham Tham
    {0x1B50, 0x1B59},  // Balinese
    {0x1BB0, 0x1BB9},  // Sundanese
    {0x1C40, 0x1C49},  // Lepcha
    {0x1C50, 0x1C59},  // Ol Chiki
    {0xA620, 0xA629},  // Vai
    {0xA8D0, 0xA8D9},  // Saurashtra
    {0xA900, 0xA909},  // Kayah Li
    {0xA9D0, 0xA9D9},  // Javanese
    {0xA9F0, 0xA9F9},  // Myanmar Tai Laing
    {0xAA50, 0xAA59},  // Cham
    {0xABF0, 0xABF9},  // Meetei Mayek
    {0xFF10, 0xFF19},  // Fullwidth
    // SMP digits
    {0x104A0, 0x104A9},  // Osmanya
    {0x10D30, 0x10D39},  // Hanifi Rohingya
    {0x11066, 0x1106F},  // Brahmi
    {0x110F0, 0x110F9},  // Sora Sompeng
    {0x11136, 0x1113F},  // Chakma
    {0x111D0, 0x111D9},  // Sharada
    {0x112F0, 0x112F9},  // Khudawadi
    {0x11450, 0x11459},  // Newa
    {0x114D0, 0x114D9},  // Tirhuta
    {0x11650, 0x11659},  // Modi
    {0x116C0, 0x116C9},  // Takri
    {0x11730, 0x11739},  // Ahom
    {0x118E0, 0x118E9},  // Warang Citi
    {0x11950, 0x11959},  // Dives Akuru
    {0x11C50, 0x11C59},  // Bhaiksuki
    {0x11D50, 0x11D59},  // Masaram Gondi
    {0x11DA0, 0x11DA9},  // Gunjala Gondi
    {0x11F50, 0x11F59},  // Kawi
    {0x16A60, 0x16A69},  // Mro
    {0x16AC0, 0x16AC9},  // Tangsa
    {0x16B50, 0x16B59},  // Pahawh Hmong
    {0x1D7CE, 0x1D7FF},  // Mathematical digits (Bold through Monospace, merged)
    {0x1E140, 0x1E149},  // Nyiakeng Puachue Hmong
    {0x1E2F0, 0x1E2F9},  // Wancho
    {0x1E4F0, 0x1E4F9},  // Nag Mundari
    {0x1E950, 0x1E959},  // Adlam
    {0x1FBF0, 0x1FBF9},  // Segmented
};

// Check if character is a Unicode decimal digit (category Nd)
static inline bool is_unicode_digit(int32_t c) {
    // Fast path for ASCII
    if (c >= '0' && c <= '9') return true;
    if (c < 0x0660) return false;
    return set_contains(unicode_digit_ranges,
                        sizeof(unicode_digit_ranges) / sizeof(unicode_digit_ranges[0]),
                        c);
}

static inline bool is_whitespace(int32_t c) {
    return c == ' ' || c == '\n' || c == '\r' ||
           c == '\t' || c == '\v' || c == '\f' ||
           c == 0x00A0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) ||
           c == 0x202F || c == 0x205F || c == 0x3000;
}

// Mirrors Razor's HtmlTokenizer heuristic: char.IsLetter/IsDigit.
static inline bool is_email_char(int32_t c) {
    return is_unicode_letter(c) || is_unicode_digit(c);
}

static inline bool is_identifier_char(int32_t c) {
    return is_unicode_letter(c) || is_unicode_digit(c) || c == '_';
}

static inline bool is_identifier_start(int32_t c) {
    return is_unicode_letter(c) || c == '_';
}

// Skip whitespace characters, respecting EOF
static inline void skip_whitespace(TSLexer *lexer) {
    while (!lexer->eof(lexer) && is_whitespace(lexer->lookahead)) {
        razor_skip(lexer);
    }
}

static inline unsigned varint_len_u32(uint32_t value) {
    unsigned len = 1;
    while (value >= 0x80) {
        value >>= 7;
        len++;
    }
    return len;
}

static unsigned write_varint_u32(char *buffer, unsigned max, uint32_t value) {
    unsigned i = 0;
    do {
        if (i >= max) return 0;
        uint8_t byte = (uint8_t)(value & 0x7F);
        value >>= 7;
        if (value) byte |= 0x80;
        buffer[i++] = (char)byte;
    } while (value);
    return i;
}

static bool read_varint_u32(const char *buffer, unsigned length, uint32_t *value, unsigned *consumed) {
    uint32_t result = 0;
    unsigned shift = 0;
    unsigned i = 0;
    while (i < length && shift <= 28) {
        uint8_t byte = (uint8_t)buffer[i++];
        result |= (uint32_t)(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            *value = result;
            *consumed = i;
            return true;
        }
        shift += 7;
    }
    return false;
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
    uint32_t context_count = scanner->context_stack.size;
    unsigned count_size = varint_len_u32(context_count);
    unsigned razor_size = count_size + context_count;
    if (csharp_size + razor_size > TREE_SITTER_SERIALIZATION_BUFFER_SIZE) {
        return 0;
    }

    // Append Razor state after C# state
    unsigned size = csharp_size;
    unsigned written = write_varint_u32(buffer + size, TREE_SITTER_SERIALIZATION_BUFFER_SIZE - size, context_count);
    if (written == 0) return 0;
    size += written;
    for (unsigned i = 0; i < context_count; i++) {
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
    // Format: [quote_count:1][interp_count:1][interp_data:4*count][razor_context_count:varint][context_data:count]

    // Validate minimum buffer size for C# header
    if (length < CSHARP_SERIALIZATION_HEADER_SIZE) {
        // Corrupted data - not enough bytes for C# header
        assert(false && "Corrupted serialization: buffer too small for C# header");
        return;
    }

    unsigned char interp_count = (unsigned char)buffer[1];
    unsigned csharp_size = CSHARP_SERIALIZATION_HEADER_SIZE + interp_count * CSHARP_INTERPOLATION_ENTRY_SIZE;

    // Validate that we have enough data for the C# scanner state
    if (csharp_size > length) {
        // Corrupted data - interpolation count claims more data than available
        assert(false && "Corrupted serialization: C# state size exceeds buffer length");
        return;
    }

    // Deserialize C# state
    tree_sitter_c_sharp_external_scanner_deserialize(scanner->csharp_scanner, buffer, csharp_size);

    // Deserialize Razor state
    if (length > csharp_size) {
        unsigned remaining = length - csharp_size;
        uint32_t context_count = 0;
        unsigned consumed = 0;
        if (!read_varint_u32(buffer + csharp_size, remaining, &context_count, &consumed)) {
            assert(false && "Corrupted serialization: invalid context count varint");
            return;
        }

        // Validate that we have enough bytes for the context stack
        if (context_count > remaining - consumed) {
            // Corrupted data - context count claims more data than available
            assert(false && "Corrupted serialization: context count exceeds remaining buffer");
            return;
        }

        array_extend(&scanner->context_stack, context_count, (const uint8_t *)&buffer[csharp_size + consumed]);
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
                valid_symbols[RazorTokenType_ImplicitExprEnd], valid_symbols[RazorTokenType_TextWithLiteralAt],
                valid_symbols[RazorTokenType_HtmlTextContent]);

    // RazorTokenType_RazorBlockAt in C# context - must check early to beat main lexer's @ token
    if (valid_symbols[RazorTokenType_RazorBlockAt] && in_csharp_context(scanner)) {
        skip_whitespace(lexer);
        if (lexer->lookahead == '@') {
            razor_advance(lexer);
            int32_t after_at = lexer->lookahead;

            if (after_at != ':' && after_at != '@' && after_at != '*') {
                lexer->mark_end(lexer);
                lexer->result_symbol = RazorTokenType_RazorBlockAt;
                return true;
            }

            if (after_at == '*' && valid_symbols[RazorTokenType_RazorCommentStart]) {
                razor_advance(lexer);
                lexer->mark_end(lexer);
                lexer->result_symbol = RazorTokenType_RazorCommentStart;
                return true;
            }

            return false;
        }
    }

    // Implicit expression terminator (zero-width token)
    // Must check before whitespace skipping
    // But if RazorTokenType_HtmlTagOpen is valid and we see '<', let that handler run instead
    if (valid_symbols[RazorTokenType_ImplicitExprEnd] && !valid_symbols[RazorTokenType_RazorBlockOpen]) {
        int32_t c = lexer->lookahead;

        // If we see '<' and RazorTokenType_HtmlTagOpen is valid, don't return RazorTokenType_ImplicitExprEnd here
        // Let the HTML tag handler consume the '<' instead
        if (c == '<' && valid_symbols[RazorTokenType_HtmlTagOpen]) {
            // Fall through to HTML tag handling
        } else if (is_whitespace(c) ||
            c == '<' || c == '@' || c == '"' || c == '\'' ||
            c == '>' || c == '}' || c == ')' || c == ']' ||
            c == ',' || c == ';' || c == ':' || c == '&' ||
            c == '/' || c == '#' ||  // URL path separator, hash for fragments
            lexer->eof(lexer)) {
            lexer->result_symbol = RazorTokenType_ImplicitExprEnd;
            return true;
        }

        if (c == '.') {
            lexer->mark_end(lexer);
            razor_advance(lexer);
            int32_t after_dot = lexer->lookahead;
            if (is_unicode_letter(after_dot) || after_dot == '_') {
                return false;
            }
            lexer->result_symbol = RazorTokenType_ImplicitExprEnd;
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
                // ?[ conditional element access - either match as RazorTokenType_ImplicitConditionalBracketOpen or don't end
                if (valid_symbols[RazorTokenType_ImplicitConditionalBracketOpen]) {
                    razor_advance(lexer);
                    lexer->mark_end(lexer);
                    array_push(&scanner->context_stack, ContextType_CsharpBracket);
                    lexer->result_symbol = RazorTokenType_ImplicitConditionalBracketOpen;
                    return true;
                }
                return false;
            }
            lexer->result_symbol = RazorTokenType_ImplicitExprEnd;
            return true;
        }
    }

    // @{ or @( or @* tokens
    // Only check for @ tokens if we see @ (or whitespace that could precede @)
    // But don't consume whitespace if RazorTokenType_HtmlTextContent is valid, as that whitespace
    // might be part of text content
    bool check_at_tokens = valid_symbols[RazorTokenType_CsharpCodeBlockStart] ||
                           valid_symbols[RazorTokenType_CsharpExplicitExprStart] ||
                           valid_symbols[RazorTokenType_RazorComment];

    DEBUG_PRINT("  check_at_tokens=%d, lookahead='%c'\n", check_at_tokens, lexer->lookahead);
    if (check_at_tokens) {
        // Don't skip whitespace if we're inside text content (HTML, title, or textarea)
        // as that whitespace should be captured as part of the content
        if (!valid_symbols[RazorTokenType_HtmlTextContent] && !valid_symbols[RazorTokenType_TitleContent] && !valid_symbols[RazorTokenType_TextareaContent]) {
            while (is_whitespace(lexer->lookahead)) {
                DEBUG_PRINT("  Skipping whitespace in check_at_tokens\n");
                razor_skip(lexer);
            }
        }
        if (lexer->lookahead == '@') {
            razor_advance(lexer);

            if (valid_symbols[RazorTokenType_RazorComment] && !in_html_tag_context(scanner) && lexer->lookahead == '*') {
                razor_advance(lexer);
                return scan_razor_comment_content(lexer, RazorTokenType_RazorComment);
            }

            if (valid_symbols[RazorTokenType_CsharpCodeBlockStart] && lexer->lookahead == '{') {
                razor_advance(lexer);
                lexer->mark_end(lexer);
                array_push(&scanner->context_stack, ContextType_CsharpBrace);
                lexer->result_symbol = RazorTokenType_CsharpCodeBlockStart;
                return true;
            }
            if (valid_symbols[RazorTokenType_CsharpExplicitExprStart] && lexer->lookahead == '(') {
                razor_advance(lexer);
                lexer->mark_end(lexer);
                array_push(&scanner->context_stack, ContextType_CsharpParen);
                lexer->result_symbol = RazorTokenType_CsharpExplicitExprStart;
                return true;
            }
            return false;
        }
    }

    // Using directive lookahead (zero-width token to disambiguate @using forms)
    // Don't match if RazorTokenType_HtmlTagOpen is valid and we're at '<' (after skipping whitespace)
    if (valid_symbols[RazorTokenType_UsingNotAlias] && scanner->context_stack.size == 0) {
        skip_whitespace(lexer);
        // After skipping whitespace, check if we're at '<' with RazorTokenType_HtmlTagOpen valid
        if (lexer->lookahead == '<' && valid_symbols[RazorTokenType_HtmlTagOpen]) {
            // Fall through to HTML tag handling
        } else if (lexer->lookahead != '=' && lexer->lookahead != '.') {
            lexer->result_symbol = RazorTokenType_UsingNotAlias;
            return true;
        } else {
            return false;
        }
    }

    // Raw text content (script, style, title, textarea)
    // At '<' with RazorTokenType_HtmlEndTagOpen valid: fall through to HTML tag handler
    // Otherwise: scan content until matching closing tag
    if (valid_symbols[RazorTokenType_ScriptContent]) {
        if (!(lexer->lookahead == '<' && valid_symbols[RazorTokenType_HtmlEndTagOpen])) {
            if (scan_raw_text_content(lexer, "script", 6, RazorTokenType_ScriptContent)) return true;
        }
    }

    if (valid_symbols[RazorTokenType_StyleContent]) {
        if (!(lexer->lookahead == '<' && valid_symbols[RazorTokenType_HtmlEndTagOpen])) {
            if (scan_raw_text_content(lexer, "style", 5, RazorTokenType_StyleContent)) return true;
        }
    }

    // Escapable raw text content (title, textarea)
    // These elements can contain character references and need proper HTML tag context handling
    if (valid_symbols[RazorTokenType_TitleContent]) {
        if (scan_escapable_raw_text_content(lexer, scanner, "title", 5, RazorTokenType_TitleContent, valid_symbols)) {
            return true;
        }
    }

    if (valid_symbols[RazorTokenType_TextareaContent]) {
        if (scan_escapable_raw_text_content(lexer, scanner, "textarea", 8, RazorTokenType_TextareaContent, valid_symbols)) {
            return true;
        }
    }

    // Character references (&...; or &... for short references)
    // Short references (without semicolon) are only enabled when requested by the grammar
    // (attribute values disable them by not setting the short-ref symbol).
    if (lexer->lookahead == '&' &&
        (valid_symbols[RazorTokenType_FullCharacterReference] ||
         valid_symbols[RazorTokenType_ShortCharacterReference] ||
         valid_symbols[RazorTokenType_InvalidCharacterReference])) {
        if (scan_character_reference(lexer, valid_symbols)) {
            return true;
        }
    }

    // HTML tag tokens (<, </, <!, <!--, <!DOCTYPE)
    // Skip leading whitespace for end tags (e.g., newlines before </div>)
    if (valid_symbols[RazorTokenType_HtmlTagOpen] || valid_symbols[RazorTokenType_HtmlEndTagOpen] ||
        valid_symbols[RazorTokenType_HtmlComment] || valid_symbols[RazorTokenType_Doctype]) {
        // For end tags, skip whitespace first - BUT only if RazorTokenType_HtmlTextContent is not valid
        // Otherwise the whitespace might need to be returned as text
        while (valid_symbols[RazorTokenType_HtmlEndTagOpen] && !valid_symbols[RazorTokenType_HtmlTextContent] &&
               !lexer->eof(lexer) && is_whitespace(lexer->lookahead)) {
            razor_skip(lexer);
        }
        if (lexer->lookahead == '<') {
            razor_advance(lexer);

            if (lexer->lookahead == '/' && valid_symbols[RazorTokenType_HtmlEndTagOpen]) {
                razor_advance(lexer);
                if (lexer->lookahead == '!') {
                    razor_advance(lexer);
                }
                lexer->mark_end(lexer);
                array_push(&scanner->context_stack, ContextType_HtmlTag);
                lexer->result_symbol = RazorTokenType_HtmlEndTagOpen;
                return true;
            }

            if (lexer->lookahead == '!') {
                razor_advance(lexer);
                if (lexer->lookahead == '-' && valid_symbols[RazorTokenType_HtmlComment]) {
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
                                        lexer->result_symbol = RazorTokenType_HtmlComment;
                                        return true;
                                    }
                                }
                            } else {
                                razor_advance(lexer);
                            }
                        }
                        lexer->mark_end(lexer);
                        lexer->result_symbol = RazorTokenType_HtmlComment;
                        return true;
                    }
                    return false;
                }
                if ((lexer->lookahead == 'd' || lexer->lookahead == 'D') && valid_symbols[RazorTokenType_Doctype]) {
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
                        lexer->result_symbol = RazorTokenType_Doctype;
                        return true;
                    }
                }
                if (valid_symbols[RazorTokenType_HtmlTagOpen]) {
                    lexer->mark_end(lexer);
                    array_push(&scanner->context_stack, ContextType_HtmlTag);
                    lexer->result_symbol = RazorTokenType_HtmlTagOpen;
                    return true;
                }
                return false;
            }

            if (valid_symbols[RazorTokenType_HtmlTagOpen]) {
                lexer->mark_end(lexer);
                array_push(&scanner->context_stack, ContextType_HtmlTag);
                lexer->result_symbol = RazorTokenType_HtmlTagOpen;
                return true;
            }

            return false;
        }
    }

    // > closes HTML tag
    if (valid_symbols[RazorTokenType_HtmlTagClose] && in_html_tag_context(scanner)) {
        skip_whitespace(lexer);
        if (lexer->lookahead == '>') {
            razor_advance(lexer);
            lexer->mark_end(lexer);
            array_pop(&scanner->context_stack);
            lexer->result_symbol = RazorTokenType_HtmlTagClose;
            return true;
        }
    }

    // Text with literal @ (email addresses like user@example.com)
    DEBUG_PRINT("  Before TEXT_AT: lookahead='%c' (%d)\n", lexer->lookahead, lexer->lookahead);
    if (valid_symbols[RazorTokenType_TextWithLiteralAt] && !valid_symbols[RazorTokenType_HtmlTextContent]) {
        DEBUG_PRINT("  TEXT_AT block entered\n");
        bool found_literal_at = false;
        int32_t last_char = 0;

        while (!lexer->eof(lexer) && lexer->lookahead != '<' &&
               lexer->lookahead != '"' && lexer->lookahead != '\'') {

            if (lexer->lookahead == '@') {
                if (is_email_char(last_char)) {
                    razor_advance(lexer);
                    if (is_email_char(lexer->lookahead)) {
                        found_literal_at = true;
                        while (is_email_char(lexer->lookahead) ||
                               lexer->lookahead == '.' ||
                               lexer->lookahead == '-') {
                            last_char = lexer->lookahead;
                            razor_advance(lexer);
                        }
                        lexer->mark_end(lexer);
                        last_char = 0;
                        continue;
                    }
                }
                break;
            }

            last_char = lexer->lookahead;
            razor_advance(lexer);

            if (found_literal_at) {
                lexer->mark_end(lexer);
            }
        }

        if (found_literal_at) {
            lexer->result_symbol = RazorTokenType_TextWithLiteralAt;
            return true;
        }
    }

    // Top-level C# comments (between control flow clauses like } // comment \n else)
    // Must check before RazorTokenType_HtmlTextContent to handle \n// correctly
    // BUT only skip whitespace if we're NOT inside an element (RazorTokenType_HtmlEndTagOpen not valid)
    // When inside an element, whitespace might need to be preserved as text
    if (valid_symbols[RazorTokenType_TopLevelCsharpComment] && !in_csharp_context(scanner)) {
        if (!valid_symbols[RazorTokenType_HtmlEndTagOpen]) {
            skip_whitespace(lexer);
        }
        if (scan_csharp_comment(lexer, RazorTokenType_TopLevelCsharpComment)) return true;
    }

    // Top-level Razor comments (between control flow clauses)
    // Note: Only handles @* comments; regular @ expressions are handled by the grammar
    // Only skip whitespace if HTML_TEXT is NOT valid - otherwise let HTML_TEXT handler decide
    if (valid_symbols[RazorTokenType_RazorComment] && !in_csharp_context(scanner) && !in_html_tag_context(scanner)) {
        if (!valid_symbols[RazorTokenType_HtmlTextContent]) {
            skip_whitespace(lexer);
        }
        if (lexer->lookahead == '@') {
            // Peek ahead to check for comment without consuming @
            lexer->mark_end(lexer);
            razor_advance(lexer);
            if (lexer->lookahead == '*') {
                // It's a Razor comment - consume and scan
                razor_advance(lexer);
                return scan_razor_comment_content(lexer, RazorTokenType_RazorComment);
            }
            // Not a comment - return false without consuming the @
            // The grammar will handle @ as a Razor expression
            return false;
        }
    }

    // HTML text content - stops before C# keywords (else/catch/finally/where)
    // Note: Works in both HTML and C# context because elements inside Razor blocks need text scanning
    if (valid_symbols[RazorTokenType_HtmlTextContent]) {
        DEBUG_PRINT("  HTML_TEXT: starting, lookahead='%c' (%d)\n", lexer->lookahead, lexer->lookahead);
        if (lexer->lookahead == '"' || lexer->lookahead == '\'') {
            // Let grammar try string_literal
            DEBUG_PRINT("  HTML_TEXT: quote char, skipping\n");
        } else if (is_whitespace(lexer->lookahead)) {
            // At whitespace - we MUST handle this to ensure whitespace before keywords
            // doesn't become a separate text token that blocks keyword matching

            // First, consume the whitespace
            while (!lexer->eof(lexer) && is_whitespace(lexer->lookahead)) {
                razor_advance(lexer);
            }

            // Now check what follows the whitespace
            if (lexer->eof(lexer)) {
                return false;
            }
            if (lexer->lookahead == '"' || lexer->lookahead == '\'') {
                // Whitespace followed by quote - let grammar handle string literal
                // Don't return the whitespace; it's just formatting before a directive arg
                DEBUG_PRINT("  HTML_TEXT: whitespace then quote, skipping\n");
                return false;
            }
            if (lexer->lookahead == '/' && (valid_symbols[RazorTokenType_CsharpComment] || valid_symbols[RazorTokenType_TopLevelCsharpComment])) {
                // Whitespace followed by / - might be a C# comment, skip whitespace
                DEBUG_PRINT("  HTML_TEXT: whitespace then slash, skipping for comment\n");
                return false;
            }
            if (lexer->lookahead == '@') {
                // Peek ahead to see if it's @* (Razor comment)
                lexer->mark_end(lexer);
                razor_advance(lexer);  // consume @
                if (lexer->lookahead == '*' && valid_symbols[RazorTokenType_RazorComment]) {
                    // It's a Razor comment - scan and return it directly
                    DEBUG_PRINT("  HTML_TEXT: whitespace then @*, scanning comment\n");
                    razor_advance(lexer);  // consume *
                    return scan_razor_comment_content(lexer, RazorTokenType_RazorComment);
                }
                // Not a comment - the @ is a Razor expression
                // But we're inside an element (RazorTokenType_HtmlEndTagOpen valid), so return whitespace as text
                // and let the grammar handle the @ on the next call
                if (valid_symbols[RazorTokenType_HtmlEndTagOpen]) {
                    // We consumed @ but that's fine - mark_end was called before @
                    // Return just the whitespace as text
                    DEBUG_PRINT("  HTML_TEXT: whitespace then @ inside element, returning whitespace\n");
                    lexer->result_symbol = RazorTokenType_HtmlTextContent;
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
                while (keyword_len < 7 && !lexer->eof(lexer) && is_identifier_char(lexer->lookahead)) {
                    keyword_buf[keyword_len++] = (char)lexer->lookahead;
                    razor_advance(lexer);
                }
                keyword_buf[keyword_len] = '\0';

                if (!is_identifier_char(lexer->lookahead)) {
                    if ((keyword_len == 4 && memcmp(keyword_buf, "else", 4) == 0) ||
                        (keyword_len == 5 && memcmp(keyword_buf, "catch", 5) == 0) ||
                        (keyword_len == 7 && memcmp(keyword_buf, "finally", 7) == 0) ||
                        (keyword_len == 5 && memcmp(keyword_buf, "where", 5) == 0)) {
                        // Keyword found - return false so grammar can match the keyword
                        // Note: whitespace is NOT returned as text; it's skipped as formatting
                        DEBUG_PRINT("  HTML_TEXT: whitespace then keyword %s, skipping\n", keyword_buf);
                        return false;
                    }
                }

                // Not a keyword - return whitespace + word as text
                DEBUG_PRINT("  HTML_TEXT: whitespace then non-keyword word, returning as text\n");
                lexer->mark_end(lexer);  // Include the word we just read
                lexer->result_symbol = RazorTokenType_HtmlTextContent;
                return true;
            }

            // Check what follows the whitespace
            if (lexer->lookahead == '}' || lexer->lookahead == '{') {
                // Whitespace followed by brace - don't return text, let grammar handle block close
                DEBUG_PRINT("  HTML_TEXT: whitespace then brace, skipping\n");
                return false;
            }

            // At top level (RazorTokenType_HtmlEndTagOpen not valid), skip whitespace before elements
            // This ensures <br>\n<hr> doesn't have text nodes between elements
            if (!valid_symbols[RazorTokenType_HtmlEndTagOpen] && lexer->lookahead == '<') {
                DEBUG_PRINT("  HTML_TEXT: top-level whitespace before tag, skipping\n");
                return false;
            }

            // Inside elements (RazorTokenType_HtmlEndTagOpen valid), return whitespace as text
            // This ensures spaces between character references like &amp; &lt; are captured
            lexer->mark_end(lexer);
            DEBUG_PRINT("  HTML_TEXT: whitespace as text (inside element=%d)\n",
                        valid_symbols[RazorTokenType_HtmlEndTagOpen]);
            lexer->result_symbol = RazorTokenType_HtmlTextContent;
            return true;
        } else {
            bool has_content = false;
            bool has_nonwhitespace = false;
            bool found_keyword = false;
            bool at_line_start = true;

            int32_t last_char = 0;  // Track previous char for email heuristic

            while (!lexer->eof(lexer)) {
                DEBUG_PRINT("  HTML_TEXT loop: lookahead='%c' (%d), at_line_start=%d\n", lexer->lookahead, lexer->lookahead, at_line_start);
                if (lexer->lookahead == '<') break;
                if (lexer->lookahead == '[' || lexer->lookahead == '(' ||
                    (lexer->lookahead == '.' && !valid_symbols[RazorTokenType_HtmlEndTagOpen])) {
                    break;
                }
                if (lexer->lookahead == '{' || lexer->lookahead == '}') break;
                if (lexer->lookahead == '&') break;

                // Handle @ - check if it's part of an email address
                if (lexer->lookahead == '@') {
                    if (is_email_char(last_char)) {
                        // Might be email - look ahead to see if followed by identifier
                        razor_advance(lexer);
                        if (is_email_char(lexer->lookahead)) {
                            // It's an email pattern - consume the domain part
                            while (is_email_char(lexer->lookahead) ||
                                   lexer->lookahead == '.' ||
                                   lexer->lookahead == '-') {
                                last_char = lexer->lookahead;
                                razor_advance(lexer);
                            }
                            has_content = true;
                            has_nonwhitespace = true;
                            lexer->mark_end(lexer);
                            last_char = 0;
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
                    last_char = 0;
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
                    last_char = 0;
                    continue;
                }

                if (at_line_start && (valid_symbols[RazorTokenType_CsharpComment] || valid_symbols[RazorTokenType_TopLevelCsharpComment]) && lexer->lookahead == '/') {
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
                    while (keyword_len < 7 && !lexer->eof(lexer) && is_identifier_char(lexer->lookahead)) {
                        keyword_buf[keyword_len++] = (char)lexer->lookahead;
                        razor_advance(lexer);
                    }
                    keyword_buf[keyword_len] = '\0';

                    bool is_keyword = false;
                    if (lexer->eof(lexer) || !is_identifier_char(lexer->lookahead)) {
                        if (at_line_start) {
                            if (keyword_len == 4 && memcmp(keyword_buf, "else", 4) == 0) is_keyword = true;
                            else if (keyword_len == 5 && memcmp(keyword_buf, "catch", 5) == 0) is_keyword = true;
                            else if (keyword_len == 7 && memcmp(keyword_buf, "finally", 7) == 0) is_keyword = true;
                        }
                        if (keyword_len == 5 && memcmp(keyword_buf, "where", 5) == 0) is_keyword = true;
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
                    if (keyword_len > 0) {
                        last_char = (unsigned char)keyword_buf[keyword_len - 1];
                    } else {
                        last_char = 0;
                    }
                    continue;
                }

                last_char = lexer->lookahead;
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
                lexer->result_symbol = RazorTokenType_HtmlTextContent;
                return true;
            } else if (has_content) {
                DEBUG_PRINT("  HTML_TEXT returning false (only whitespace, has_content=%d)\n", has_content);
            }
        }
    }

    // { opens Razor block (enters C# brace context)
    if (valid_symbols[RazorTokenType_RazorBlockOpen] && !valid_symbols[RazorTokenType_CsharpContextClose]) {
        skip_whitespace(lexer);
        if (lexer->lookahead == '{') {
            razor_advance(lexer);
            lexer->mark_end(lexer);
            array_push(&scanner->context_stack, ContextType_CsharpBrace);
            lexer->result_symbol = RazorTokenType_RazorBlockOpen;
            return true;
        }
    }

    // ( in implicit expression - push C# paren context
    if (valid_symbols[RazorTokenType_ImplicitParenOpen]) {
        skip_whitespace(lexer);
        if (lexer->lookahead == '(') {
            razor_advance(lexer);
            lexer->mark_end(lexer);
            array_push(&scanner->context_stack, ContextType_CsharpParen);
            lexer->result_symbol = RazorTokenType_ImplicitParenOpen;
            return true;
        }
    }

    // [ in implicit expression - push C# bracket context
    if (valid_symbols[RazorTokenType_ImplicitBracketOpen]) {
        skip_whitespace(lexer);
        if (lexer->lookahead == '[') {
            razor_advance(lexer);
            lexer->mark_end(lexer);
            array_push(&scanner->context_stack, ContextType_CsharpBracket);
            lexer->result_symbol = RazorTokenType_ImplicitBracketOpen;
            return true;
        }
    }

    // } or ) or ] closes C# context
    if (valid_symbols[RazorTokenType_CsharpContextClose] && scanner->context_stack.size > 0) {
        skip_whitespace(lexer);

        ContextType top = scanner->context_stack.contents[scanner->context_stack.size - 1];
        if ((top == ContextType_CsharpBrace && lexer->lookahead == '}') ||
            (top == ContextType_CsharpParen && lexer->lookahead == ')') ||
            (top == ContextType_CsharpBracket && lexer->lookahead == ']')) {
            razor_advance(lexer);
            lexer->mark_end(lexer);
            array_pop(&scanner->context_stack);
            lexer->result_symbol = RazorTokenType_CsharpContextClose;
            return true;
        }
    }

    // @ in C# context (handles @* comment and nested @ expressions)
    if ((valid_symbols[RazorTokenType_RazorCommentStart] || valid_symbols[RazorTokenType_RazorBlockAt]) && in_csharp_context(scanner)) {
        skip_whitespace(lexer);

        if (lexer->lookahead == '@') {
            razor_advance(lexer);

            if (valid_symbols[RazorTokenType_RazorCommentStart] && lexer->lookahead == '*') {
                razor_advance(lexer);
                lexer->mark_end(lexer);
                lexer->result_symbol = RazorTokenType_RazorCommentStart;
                return true;
            }

            if (lexer->lookahead == ':' || lexer->lookahead == '@') {
                return false;
            }

            if (valid_symbols[RazorTokenType_RazorBlockAt]) {
                lexer->mark_end(lexer);
                lexer->result_symbol = RazorTokenType_RazorBlockAt;
                return true;
            }

            return false;
        }
    }

    // C# comments
    if (valid_symbols[RazorTokenType_CsharpComment] && in_csharp_context(scanner)) {
        if (scan_csharp_comment(lexer, RazorTokenType_CsharpComment)) return true;
    }

    // Note: RazorTokenType_TopLevelCsharpComment is handled earlier, before RazorTokenType_HtmlTextContent

    // Razor comment as extra in C# context
    if (valid_symbols[RazorTokenType_RazorCommentExtra] && in_csharp_context(scanner)) {
        skip_whitespace(lexer);
        if (lexer->lookahead == '@') {
            razor_advance(lexer);
            if (lexer->lookahead == '*') {
                razor_advance(lexer);
                return scan_razor_comment_content(lexer, RazorTokenType_RazorCommentExtra);
            }
            return false;
        }
    }

    // Text literal content (@: to end of line)
    if (valid_symbols[RazorTokenType_TextLiteralContent]) {
        bool has_content = false;
        while (!lexer->eof(lexer) && lexer->lookahead != '\r' && lexer->lookahead != '\n') {
            razor_advance(lexer);
            has_content = true;
        }
        if (has_content) {
            lexer->mark_end(lexer);
            lexer->result_symbol = RazorTokenType_TextLiteralContent;
            return true;
        }
    }

    return tree_sitter_c_sharp_external_scanner_scan(scanner->csharp_scanner, lexer, valid_symbols);
}
