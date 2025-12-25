#include "tree_sitter/alloc.h"
#include "tree_sitter/array.h"
#include "tree_sitter/parser.h"
#include <stdint.h>
#include "../tree-sitter-c-sharp/src/scanner.c"

// =============================================================================
// Razor-specific token types (appended after C# tokens)
// =============================================================================

// Number of tokens in the C# scanner
#define CSHARP_TOKEN_COUNT 12

// Razor tokens start after C# tokens
enum RazorTokenType {
    // Razor-specific tokens
    TEXT_WITH_LITERAL_AT = CSHARP_TOKEN_COUNT,  // Text containing @ preceded by word char (e.g., email)
    HTML_TEXT_CONTENT,       // HTML text content, aware of else/catch/finally keywords
    // Context-aware tokens for tracking C# vs HTML mode
    CSHARP_CODE_BLOCK_START, // @{ - enters C# brace context
    CSHARP_EXPLICIT_EXPR_START, // @( - enters C# paren context
    RAZOR_BLOCK_OPEN,        // { after Razor statement - enters C# context
    CSHARP_CONTEXT_CLOSE,    // } or ) that exits C# context
    CSHARP_COMMENT,          // /* */ or // comment, only valid in C# context
    PREPROC_DIRECTIVE_START, // # at start of preprocessor directive, only valid in C# context
    // Script, style, title, textarea content
    SCRIPT_CONTENT,          // Raw content inside <script> tags
    STYLE_CONTENT,           // Raw content inside <style> tags
    TITLE_CONTENT,           // Raw content inside <title> tags
    TEXTAREA_CONTENT,        // Raw content inside <textarea> tags
    // Implicit expression terminator
    IMPLICIT_EXPR_END,       // Zero-width token that matches when expression should end
    // @ in Razor block context
    RAZOR_BLOCK_AT,          // @ inside a Razor block - starts nested Razor expression
    // Using directive lookahead
    USING_NOT_ALIAS,         // Zero-width token that matches when NOT followed by = or .
    // Razor comment start in C# context
    RAZOR_COMMENT_START,     // @* - start of Razor comment, beats C# @identifier
};

// =============================================================================
// Razor scanner state
// =============================================================================

// Context types for tracking C# vs HTML mode
typedef enum {
    CONTEXT_HTML = 0,
    CONTEXT_CSHARP_BRACE = 1,   // Inside @{ } or { } block
    CONTEXT_CSHARP_PAREN = 2,   // Inside @( ) expression
} ContextType;

typedef struct {
    void *csharp_scanner;  // Embedded C# scanner (opaque)
    Array(uint8_t) context_stack;  // Context tracking for C# vs HTML mode
} RazorScanner;

// =============================================================================
// Helper functions
// =============================================================================

static inline void razor_advance(TSLexer *lexer) { lexer->advance(lexer, false); }

static inline void razor_skip(TSLexer *lexer) { lexer->advance(lexer, true); }

// Check if scanner is currently in C# context
static inline bool in_csharp_context(RazorScanner *scanner) {
    return scanner->context_stack.size > 0 && *array_back(&scanner->context_stack) != CONTEXT_HTML;
}

// Check if character is a Unicode letter.
// This is a locale-independent check that covers the main Unicode letter categories.
// Note: This is an approximation - a complete implementation would need full Unicode tables.
static inline bool is_unicode_letter(int32_t c) {
    // ASCII letters
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return true;
    // Latin-1 Supplement letters (U+00C0-U+00FF, excluding some non-letters)
    if (c >= 0x00C0 && c <= 0x00FF && c != 0x00D7 && c != 0x00F7) return true;
    // Latin Extended-A
    if (c >= 0x0100 && c <= 0x017F) return true;
    // Latin Extended-B
    if (c >= 0x0180 && c <= 0x024F) return true;
    // Greek and Coptic
    if (c >= 0x0370 && c <= 0x03FF) return true;
    // Cyrillic
    if (c >= 0x0400 && c <= 0x04FF) return true;
    // Hebrew
    if (c >= 0x0590 && c <= 0x05FF) return true;
    // Arabic
    if (c >= 0x0600 && c <= 0x06FF) return true;
    // Devanagari
    if (c >= 0x0900 && c <= 0x097F) return true;
    // Thai
    if (c >= 0x0E00 && c <= 0x0E7F) return true;
    // CJK Unified Ideographs
    if (c >= 0x4E00 && c <= 0x9FFF) return true;
    // Hiragana
    if (c >= 0x3040 && c <= 0x309F) return true;
    // Katakana
    if (c >= 0x30A0 && c <= 0x30FF) return true;
    // Hangul Syllables
    if (c >= 0xAC00 && c <= 0xD7AF) return true;

    return false;
}

// Check if character is a Unicode decimal digit (category Nd - DecimalDigitNumber)
// This matches .NET's char.IsDigit() behavior
static inline bool is_unicode_digit(int32_t c) {
    // Fast path: ASCII digits - covers 99%+ of real-world cases
    if (c >= 0x0030 && c <= 0x0039) return true;
    // Quick rejection for common non-digit characters
    if (c < 0x0660) return false;

    // === Basic Multilingual Plane (BMP) digits ===

    // Arabic-Indic
    if (c >= 0x0660 && c <= 0x0669) return true;
    // Extended Arabic-Indic
    if (c >= 0x06F0 && c <= 0x06F9) return true;
    // NKo
    if (c >= 0x07C0 && c <= 0x07C9) return true;
    // Devanagari
    if (c >= 0x0966 && c <= 0x096F) return true;
    // Bengali
    if (c >= 0x09E6 && c <= 0x09EF) return true;
    // Gurmukhi
    if (c >= 0x0A66 && c <= 0x0A6F) return true;
    // Gujarati
    if (c >= 0x0AE6 && c <= 0x0AEF) return true;
    // Oriya
    if (c >= 0x0B66 && c <= 0x0B6F) return true;
    // Tamil
    if (c >= 0x0BE6 && c <= 0x0BEF) return true;
    // Telugu
    if (c >= 0x0C66 && c <= 0x0C6F) return true;
    // Kannada
    if (c >= 0x0CE6 && c <= 0x0CEF) return true;
    // Malayalam
    if (c >= 0x0D66 && c <= 0x0D6F) return true;
    // Sinhala
    if (c >= 0x0DE6 && c <= 0x0DEF) return true;
    // Thai
    if (c >= 0x0E50 && c <= 0x0E59) return true;
    // Lao
    if (c >= 0x0ED0 && c <= 0x0ED9) return true;
    // Tibetan
    if (c >= 0x0F20 && c <= 0x0F29) return true;
    // Myanmar
    if (c >= 0x1040 && c <= 0x1049) return true;
    // Myanmar Shan
    if (c >= 0x1090 && c <= 0x1099) return true;
    // Khmer
    if (c >= 0x17E0 && c <= 0x17E9) return true;
    // Mongolian
    if (c >= 0x1810 && c <= 0x1819) return true;
    // Limbu
    if (c >= 0x1946 && c <= 0x194F) return true;
    // New Tai Lue
    if (c >= 0x19D0 && c <= 0x19D9) return true;
    // Tai Tham Hora
    if (c >= 0x1A80 && c <= 0x1A89) return true;
    // Tai Tham Tham
    if (c >= 0x1A90 && c <= 0x1A99) return true;
    // Balinese
    if (c >= 0x1B50 && c <= 0x1B59) return true;
    // Sundanese
    if (c >= 0x1BB0 && c <= 0x1BB9) return true;
    // Lepcha
    if (c >= 0x1C40 && c <= 0x1C49) return true;
    // Ol Chiki
    if (c >= 0x1C50 && c <= 0x1C59) return true;
    // Vai
    if (c >= 0xA620 && c <= 0xA629) return true;
    // Saurashtra
    if (c >= 0xA8D0 && c <= 0xA8D9) return true;
    // Kayah Li
    if (c >= 0xA900 && c <= 0xA909) return true;
    // Javanese
    if (c >= 0xA9D0 && c <= 0xA9D9) return true;
    // Myanmar Tai Laing
    if (c >= 0xA9F0 && c <= 0xA9F9) return true;
    // Cham
    if (c >= 0xAA50 && c <= 0xAA59) return true;
    // Meetei Mayek
    if (c >= 0xABF0 && c <= 0xABF9) return true;
    // Fullwidth digits
    if (c >= 0xFF10 && c <= 0xFF19) return true;

    // === Supplementary Multilingual Plane (SMP) digits ===

    // Osmanya
    if (c >= 0x104A0 && c <= 0x104A9) return true;
    // Hanifi Rohingya
    if (c >= 0x10D30 && c <= 0x10D39) return true;
    // Brahmi
    if (c >= 0x11066 && c <= 0x1106F) return true;
    // Sora Sompeng
    if (c >= 0x110F0 && c <= 0x110F9) return true;
    // Chakma
    if (c >= 0x11136 && c <= 0x1113F) return true;
    // Sharada
    if (c >= 0x111D0 && c <= 0x111D9) return true;
    // Khudawadi
    if (c >= 0x112F0 && c <= 0x112F9) return true;
    // Newa
    if (c >= 0x11450 && c <= 0x11459) return true;
    // Tirhuta
    if (c >= 0x114D0 && c <= 0x114D9) return true;
    // Modi
    if (c >= 0x11650 && c <= 0x11659) return true;
    // Takri
    if (c >= 0x116C0 && c <= 0x116C9) return true;
    // Ahom
    if (c >= 0x11730 && c <= 0x11739) return true;
    // Warang Citi
    if (c >= 0x118E0 && c <= 0x118E9) return true;
    // Dives Akuru
    if (c >= 0x11950 && c <= 0x11959) return true;
    // Bhaiksuki
    if (c >= 0x11C50 && c <= 0x11C59) return true;
    // Masaram Gondi
    if (c >= 0x11D50 && c <= 0x11D59) return true;
    // Gunjala Gondi
    if (c >= 0x11DA0 && c <= 0x11DA9) return true;
    // Kawi
    if (c >= 0x11F50 && c <= 0x11F59) return true;
    // Mro
    if (c >= 0x16A60 && c <= 0x16A69) return true;
    // Tangsa
    if (c >= 0x16AC0 && c <= 0x16AC9) return true;
    // Pahawh Hmong
    if (c >= 0x16B50 && c <= 0x16B59) return true;
    // Mathematical Bold
    if (c >= 0x1D7CE && c <= 0x1D7D7) return true;
    // Mathematical Double-Struck
    if (c >= 0x1D7D8 && c <= 0x1D7E1) return true;
    // Mathematical Sans-Serif
    if (c >= 0x1D7E2 && c <= 0x1D7EB) return true;
    // Mathematical Sans-Serif Bold
    if (c >= 0x1D7EC && c <= 0x1D7F5) return true;
    // Mathematical Monospace
    if (c >= 0x1D7F6 && c <= 0x1D7FF) return true;
    // Nyiakeng Puachue Hmong
    if (c >= 0x1E140 && c <= 0x1E149) return true;
    // Wancho
    if (c >= 0x1E2F0 && c <= 0x1E2F9) return true;
    // Nag Mundari
    if (c >= 0x1E4F0 && c <= 0x1E4F9) return true;
    // Adlam
    if (c >= 0x1E950 && c <= 0x1E959) return true;
    // Segmented digits
    if (c >= 0x1FBF0 && c <= 0x1FBF9) return true;

    return false;
}

// Check if character is a whitespace character according to C# rules:
// - Unicode category Zs (space separators)
// - Horizontal tab, vertical tab, form feed
// - Newlines (CR, LF)
static inline bool is_whitespace(int32_t c) {
    return c == ' ' ||
           c == '\n' || c == '\r' ||
           c == '\t' || c == '\v' || c == '\f' ||
           c == 0x00A0 ||
           c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) ||
           c == 0x202F ||
           c == 0x205F ||
           c == 0x3000;
}

// Check if character is a valid HTML end tag terminator per the HTML spec.
// Valid terminators after tag name: tab, newline, form feed, space, /, >
static inline bool is_end_tag_terminator(int32_t c) {
    return c == '\t' || c == '\n' || c == '\f' || c == ' ' || c == '/' || c == '>';
}

// Check if character is a "word" character for email address detection.
// Per Razor lexer: char.IsLetter(c) || char.IsDigit(c)
// - IsLetter: UppercaseLetter, LowercaseLetter, TitlecaseLetter, ModifierLetter, OtherLetter
// - IsDigit: DecimalDigitNumber
static inline bool is_email_char(int32_t c) {
    return is_unicode_letter(c) || is_unicode_digit(c);
}

// Check if character can be part of a C# identifier (for keyword boundary detection)
static inline bool is_identifier_char(int32_t c) {
    return is_unicode_letter(c) || is_unicode_digit(c) || c == '_';
}

// Check if character can start a C# identifier (letter or underscore, not digit)
static inline bool is_identifier_start(int32_t c) {
    return is_unicode_letter(c) || c == '_';
}

// =============================================================================
// Scanner lifecycle functions
// =============================================================================

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

// =============================================================================
// Main scan function
// =============================================================================

bool tree_sitter_razor_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
    RazorScanner *scanner = (RazorScanner *)payload;

    // -------------------------------------------------------------------------
    // Using directive lookahead - matches when NOT followed by = or .
    // This is a zero-width token used to disambiguate:
    // - @using Namespace (simple identifier)
    // - @using Namespace.SubNamespace (qualified name, has .)
    // - @using Alias = Type (alias form, has =)
    // -------------------------------------------------------------------------

    if (valid_symbols[USING_NOT_ALIAS]) {
        // Skip whitespace
        while (is_whitespace(lexer->lookahead)) {
            razor_skip(lexer);
        }

        // Match if NOT followed by = or . (so alias and qualified_name can match)
        if (lexer->lookahead != '=' && lexer->lookahead != '.') {
            lexer->result_symbol = USING_NOT_ALIAS;
            return true;
        }
        // Followed by = or . - don't match, let alias form or qualified_name be tried
        return false;
    }

    // -------------------------------------------------------------------------
    // Implicit expression terminator - matches when expression should end
    // This is a zero-width token that prevents whitespace from being skipped
    // -------------------------------------------------------------------------

    // Don't match IMPLICIT_EXPR_END if RAZOR_BLOCK_OPEN is also valid
    // In that case, we're expecting a block, not ending an expression
    if (valid_symbols[IMPLICIT_EXPR_END] && !valid_symbols[RAZOR_BLOCK_OPEN]) {
        // The expression should end if we see:
        // - Whitespace
        // - Characters that aren't valid expression continuations
        // Valid continuations are: . ?. ( [ (handled by token.immediate in grammar)
        int32_t c = lexer->lookahead;

        // If we're at whitespace or any character that isn't a continuation,
        // the expression ends here
        if (is_whitespace(c) ||
            c == '<' || c == '@' || c == '"' || c == '\'' ||
            c == '>' || c == '}' || c == ')' || c == ']' ||
            c == ',' || c == ';' || c == ':' ||
            lexer->eof(lexer)) {
            // Zero-width token - don't advance
            lexer->result_symbol = IMPLICIT_EXPR_END;
            return true;
        }

        // For . and ?. we need to check if they're followed by identifier
        // If not (e.g., just . at end of line or ". bar"), expression should end
        if (c == '.') {
            // Look ahead to see what follows the dot
            lexer->mark_end(lexer);
            razor_advance(lexer);  // consume .
            int32_t after_dot = lexer->lookahead;
            // If followed by identifier start, let grammar handle member access
            if (is_unicode_letter(after_dot) || after_dot == '_') {
                return false;
            }
            // Otherwise, dot not followed by identifier - end expression here
            // (Don't consume the dot - mark_end was set before we advanced)
            lexer->result_symbol = IMPLICIT_EXPR_END;
            return true;
        }

        if (c == '?') {
            // Check if this is ?. followed by identifier
            lexer->mark_end(lexer);
            razor_advance(lexer);  // consume ?
            if (lexer->lookahead == '.') {
                razor_advance(lexer);  // consume .
                int32_t after_dot = lexer->lookahead;
                // If followed by identifier start, let grammar handle conditional access
                if (is_unicode_letter(after_dot) || after_dot == '_') {
                    return false;
                }
            }
            // Not ?. or not followed by identifier - end expression
            lexer->result_symbol = IMPLICIT_EXPR_END;
            return true;
        }

        if (c == '(' || c == '[') {
            // These continue the expression (invocation/indexer)
            return false;
        }

        // Any other character - expression ends
        // (This includes letters/digits which would be invalid after expression)
        lexer->result_symbol = IMPLICIT_EXPR_END;
        return true;
    }

    // -------------------------------------------------------------------------
    // Razor-specific tokens (HTML context only)
    // -------------------------------------------------------------------------

    // Text containing literal @ (when preceded by word character)
    // This handles email addresses like user@example.com or mailto:user@example.com
    // Pattern: [text]word@word[text] where the @ is preceded by a word char
    // NOTE: The grammar controls when this token is valid through valid_symbols.
    // If valid_symbols[TEXT_WITH_LITERAL_AT] is true, we're in an HTML text context
    // (element content, attribute values) even if we're inside a Razor block.
    if (valid_symbols[TEXT_WITH_LITERAL_AT]) {
        bool found_literal_at = false;
        bool last_was_word = false;

        // Scan forward looking for word+@ pattern
        while (!lexer->eof(lexer) && lexer->lookahead != '<' &&
               lexer->lookahead != '"' && lexer->lookahead != '\'') {

            if (lexer->lookahead == '@') {
                if (last_was_word) {
                    // Found word char followed by @
                    razor_advance(lexer);  // consume @

                    // Check if followed by letter/digit (domain part)
                    if (is_email_char(lexer->lookahead)) {
                        found_literal_at = true;
                        // Continue consuming the rest
                        while (is_email_char(lexer->lookahead) ||
                               lexer->lookahead == '.' ||
                               lexer->lookahead == '-') {
                            razor_advance(lexer);
                        }
                        lexer->mark_end(lexer);
                        // Continue scanning in case there are more @ signs
                        last_was_word = false;
                        continue;
                    }
                }
                // @ not preceded by word, or not followed by word
                // Stop here - this @ might be a Razor construct
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
        // No word@word pattern found, don't match
    }

    // HTML text content - matches text but stops before keywords like else/catch/finally
    // This allows the grammar to recognize these keywords after @if/@try blocks
    // NOTE: Don't match in C# context - HTML text isn't valid inside C# code
    if (valid_symbols[HTML_TEXT_CONTENT] && !in_csharp_context(scanner)) {
        bool has_content = false;
        bool found_keyword = false;
        bool at_line_start = true;  // Track if we're at the logical start of a line

        while (!lexer->eof(lexer)) {
            // Stop at HTML/Razor markers
            if (lexer->lookahead == '<' || lexer->lookahead == '@') {
                break;
            }

            // Stop at characters that could be expression continuations
            // Note: . could be C# qualified name continuation (directives use regular C# . token)
            if (lexer->lookahead == '[' || lexer->lookahead == '(' || lexer->lookahead == '.') {
                break;
            }

            // Stop at string delimiters (for directive arguments like @page "/route")
            if (lexer->lookahead == '"' || lexer->lookahead == '\'') {
                break;
            }

            // Track newlines to know when we're at line start
            if (lexer->lookahead == '\n' || lexer->lookahead == '\r') {
                razor_advance(lexer);
                has_content = true;
                lexer->mark_end(lexer);
                at_line_start = true;
                continue;
            }

            // Whitespace at line start doesn't change at_line_start
            if (at_line_start && is_whitespace(lexer->lookahead)) {
                razor_advance(lexer);
                has_content = true;
                lexer->mark_end(lexer);
                continue;
            }

            // Check for keywords only at line start
            // If we see 'e', 'c', or 'f' at line start, check for else/catch/finally
            if (at_line_start && (lexer->lookahead == 'e' || lexer->lookahead == 'c' || lexer->lookahead == 'f')) {
                lexer->mark_end(lexer);

                // Peek ahead to check for keywords
                char keyword_buf[8] = {0};
                int keyword_len = 0;
                int32_t start_char = lexer->lookahead;

                while (keyword_len < 7 && is_identifier_char(lexer->lookahead)) {
                    keyword_buf[keyword_len++] = (char)lexer->lookahead;
                    razor_advance(lexer);
                }
                keyword_buf[keyword_len] = '\0';

                // Check if we found a keyword followed by whitespace, brace, or paren
                bool is_keyword = false;
                if (!is_identifier_char(lexer->lookahead)) {
                    if (start_char == 'e' && strcmp(keyword_buf, "else") == 0) {
                        is_keyword = true;
                    } else if (start_char == 'c' && strcmp(keyword_buf, "catch") == 0) {
                        is_keyword = true;
                    } else if (start_char == 'f' && strcmp(keyword_buf, "finally") == 0) {
                        is_keyword = true;
                    }
                }

                if (is_keyword) {
                    // Stop here - don't consume the keyword
                    found_keyword = true;
                    break;
                }

                // Not a keyword, the characters we advanced over are content
                has_content = true;
                lexer->mark_end(lexer);
                at_line_start = false;
                continue;
            }

            // Any other character - no longer at line start
            razor_advance(lexer);
            has_content = true;
            lexer->mark_end(lexer);
            at_line_start = false;
        }

        if (has_content) {
            lexer->result_symbol = HTML_TEXT_CONTENT;
            return true;
        }

        // If we found a keyword immediately (no content), don't match
        // This lets the grammar try to match the keyword
        if (found_keyword) {
            return false;
        }
    }

    // -------------------------------------------------------------------------
    // Context-tracking tokens for C# vs HTML mode
    // -------------------------------------------------------------------------

    // @{ or @( - enter C# context
    // Check both together to avoid consuming @ then failing
    if ((valid_symbols[CSHARP_CODE_BLOCK_START] || valid_symbols[CSHARP_EXPLICIT_EXPR_START]) &&
        lexer->lookahead == '@') {
        razor_advance(lexer);
        if (valid_symbols[CSHARP_CODE_BLOCK_START] && lexer->lookahead == '{') {
            razor_advance(lexer);
            array_push(&scanner->context_stack, CONTEXT_CSHARP_BRACE);
            lexer->result_symbol = CSHARP_CODE_BLOCK_START;
            return true;
        }
        if (valid_symbols[CSHARP_EXPLICIT_EXPR_START] && lexer->lookahead == '(') {
            razor_advance(lexer);
            array_push(&scanner->context_stack, CONTEXT_CSHARP_PAREN);
            lexer->result_symbol = CSHARP_EXPLICIT_EXPR_START;
            return true;
        }
        // Neither @{ nor @( - don't match
        return false;
    }

    // { in Razor block context (after @if, @for, etc.) - enters C# brace context
    if (valid_symbols[RAZOR_BLOCK_OPEN]) {
        // Skip C# whitespace
        while (is_whitespace(lexer->lookahead)) {
            razor_skip(lexer);
        }
        if (lexer->lookahead == '{') {
            razor_advance(lexer);
            array_push(&scanner->context_stack, CONTEXT_CSHARP_BRACE);
            lexer->result_symbol = RAZOR_BLOCK_OPEN;
            return true;
        }
    }

    // } or ) that closes C# context
    if (valid_symbols[CSHARP_CONTEXT_CLOSE] && scanner->context_stack.size > 0) {
        // Skip C# whitespace
        while (is_whitespace(lexer->lookahead)) {
            razor_skip(lexer);
        }

        ContextType top = scanner->context_stack.contents[scanner->context_stack.size - 1];
        if ((top == CONTEXT_CSHARP_BRACE && lexer->lookahead == '}') ||
            (top == CONTEXT_CSHARP_PAREN && lexer->lookahead == ')')) {
            razor_advance(lexer);
            array_pop(&scanner->context_stack);
            lexer->result_symbol = CSHARP_CONTEXT_CLOSE;
            return true;
        }
    }

    // @ token handling in C# context - handles both @* (comment) and @ (Razor expression)
    // Must be matched before C# verbatim identifier lexing can grab @identifier
    if ((valid_symbols[RAZOR_COMMENT_START] || valid_symbols[RAZOR_BLOCK_AT]) && in_csharp_context(scanner)) {
        // Skip C# whitespace first
        while (is_whitespace(lexer->lookahead)) {
            razor_skip(lexer);
        }

        if (lexer->lookahead == '@') {
            razor_advance(lexer);    // Consume @

            // @* - start of Razor comment
            if (valid_symbols[RAZOR_COMMENT_START] && lexer->lookahead == '*') {
                razor_advance(lexer);    // Consume *
                lexer->result_symbol = RAZOR_COMMENT_START;
                return true;
            }

            // Check for @: (text literal) or @@ (escaped @) - don't match these as RAZOR_BLOCK_AT
            if (lexer->lookahead == ':' || lexer->lookahead == '@') {
                return false;
            }

            // @ for nested Razor expressions/statements
            if (valid_symbols[RAZOR_BLOCK_AT]) {
                lexer->result_symbol = RAZOR_BLOCK_AT;
                return true;
            }

            // RAZOR_COMMENT_START was valid but not @* - fallback
            return false;
        }
    }

    // -------------------------------------------------------------------------
    // Context-aware C# extras (comments, preproc) - only in C# context
    // -------------------------------------------------------------------------

    // C# comment - only valid when in C# context
    if (valid_symbols[CSHARP_COMMENT] && in_csharp_context(scanner)) {
        if (lexer->lookahead == '/') {
            razor_advance(lexer);
            if (lexer->lookahead == '/') {
                // Single-line comment
                razor_advance(lexer);
                while (!lexer->eof(lexer) && lexer->lookahead != '\n' && lexer->lookahead != '\r') {
                    razor_advance(lexer);
                }
                lexer->result_symbol = CSHARP_COMMENT;
                return true;
            } else if (lexer->lookahead == '*') {
                // Multi-line comment
                razor_advance(lexer);
                while (!lexer->eof(lexer)) {
                    if (lexer->lookahead == '*') {
                        razor_advance(lexer);
                        if (lexer->lookahead == '/') {
                            razor_advance(lexer);
                            lexer->result_symbol = CSHARP_COMMENT;
                            return true;
                        }
                    } else {
                        razor_advance(lexer);
                    }
                }
                // Unterminated comment - still return it
                lexer->result_symbol = CSHARP_COMMENT;
                return true;
            }
            // Just / alone - don't consume
            return false;
        }
    }

    // C# preprocessor directive start - only valid when in C# context
    // Just matches the # character; the rest of the directive is parsed by grammar rules
    if (valid_symbols[PREPROC_DIRECTIVE_START] && in_csharp_context(scanner) && lexer->lookahead == '#') {
        razor_advance(lexer);
        lexer->result_symbol = PREPROC_DIRECTIVE_START;
        return true;
    }

    // -------------------------------------------------------------------------
    // Script, style, title, textarea content - raw text until closing tag
    // -------------------------------------------------------------------------

    // Script content - scan until </script>
    if (valid_symbols[SCRIPT_CONTENT]) {
        bool has_content = false;

        while (!lexer->eof(lexer)) {
            if (lexer->lookahead == '<') {
                lexer->mark_end(lexer);
                razor_advance(lexer);
                if (lexer->lookahead == '/') {
                    razor_advance(lexer);
                    // Check for 'script' (case insensitive)
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
                    // Must match full tag name AND be followed by valid terminator
                    if (matches && i == 6 && is_end_tag_terminator(lexer->lookahead)) {
                        break;
                    }
                }
                // Not </script> with valid terminator, continue
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

    // Style content - scan until </style>
    if (valid_symbols[STYLE_CONTENT]) {
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

    // Title content - scan until </title>
    if (valid_symbols[TITLE_CONTENT]) {
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

    // Textarea content - scan until </textarea>
    if (valid_symbols[TEXTAREA_CONTENT]) {
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
                        if (c != tag[i] && c != (tag[i] - 32)) {
                            matches = false;
                        } else {
                            razor_advance(lexer);
                            i++;
                        }
                    }
                    if (matches && i == 8 && is_end_tag_terminator(lexer->lookahead)) {
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
            lexer->result_symbol = TEXTAREA_CONTENT;
            return true;
        }
        return false;
    }

    // -------------------------------------------------------------------------
    // Delegate to C# scanner for C# tokens
    // -------------------------------------------------------------------------

    return tree_sitter_c_sharp_external_scanner_scan(scanner->csharp_scanner, lexer, valid_symbols);
}
