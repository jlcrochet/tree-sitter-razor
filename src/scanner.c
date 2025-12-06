/**
 * Razor external scanner
 *
 * This scanner wraps the C# scanner and adds Razor-specific token handling.
 * The C# scanner is treated as a black box - we include it and delegate to it
 * for C# tokens.
 */

#include "tree_sitter/alloc.h"
#include "tree_sitter/array.h"
#include "tree_sitter/parser.h"

// =============================================================================
// Include C# scanner
// =============================================================================

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
    CSHARP_CODE_BLOCK_START, // @{ - enters C# context
    CSHARP_EXPLICIT_EXPR_START, // @( - enters C# context
    RAZOR_BLOCK_OPEN,        // { after Razor statement - enters C# context
    CSHARP_CONTEXT_CLOSE,    // } or ) that exits C# context
    CSHARP_COMMENT,          // /* */ or // comment, only valid in C# context
    CSHARP_PREPROC,          // #directive, only valid in C# context
    // Script, style, title, textarea content
    SCRIPT_CONTENT,          // Raw content inside <script> tags
    STYLE_CONTENT,           // Raw content inside <style> tags
    TITLE_CONTENT,           // Raw content inside <title> tags
    TEXTAREA_CONTENT,        // Raw content inside <textarea> tags
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
    return scanner->context_stack.size > 0;
}

// Check if character is a Unicode letter.
// This is a locale-independent check that covers the main Unicode letter categories.
// Note: This is an approximation - a complete implementation would need full Unicode tables.
static inline bool is_unicode_letter(int32_t c) {
    // ASCII letters
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return true;

    // Latin-1 Supplement letters (U+00C0-U+00FF, excluding some non-letters)
    if (c >= 0x00C0 && c <= 0x00FF && c != 0x00D7 && c != 0x00F7) return true;

    // Latin Extended-A (U+0100-U+017F)
    if (c >= 0x0100 && c <= 0x017F) return true;

    // Latin Extended-B (U+0180-U+024F)
    if (c >= 0x0180 && c <= 0x024F) return true;

    // Greek and Coptic (U+0370-U+03FF)
    if (c >= 0x0370 && c <= 0x03FF) return true;

    // Cyrillic (U+0400-U+04FF)
    if (c >= 0x0400 && c <= 0x04FF) return true;

    // Hebrew (U+0590-U+05FF)
    if (c >= 0x0590 && c <= 0x05FF) return true;

    // Arabic (U+0600-U+06FF)
    if (c >= 0x0600 && c <= 0x06FF) return true;

    // Devanagari (U+0900-U+097F)
    if (c >= 0x0900 && c <= 0x097F) return true;

    // Thai (U+0E00-U+0E7F)
    if (c >= 0x0E00 && c <= 0x0E7F) return true;

    // CJK Unified Ideographs (U+4E00-U+9FFF)
    if (c >= 0x4E00 && c <= 0x9FFF) return true;

    // Hiragana (U+3040-U+309F)
    if (c >= 0x3040 && c <= 0x309F) return true;

    // Katakana (U+30A0-U+30FF)
    if (c >= 0x30A0 && c <= 0x30FF) return true;

    // Hangul Syllables (U+AC00-U+D7AF)
    if (c >= 0xAC00 && c <= 0xD7AF) return true;

    return false;
}

// Check if character is a Unicode decimal digit (0-9 in various scripts)
static inline bool is_unicode_digit(int32_t c) {
    // ASCII digits
    if (c >= '0' && c <= '9') return true;

    // Other common digit ranges (Arabic-Indic, Extended Arabic-Indic, Devanagari, etc.)
    // For simplicity, we mainly care about ASCII digits for email detection
    return false;
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
    // Razor-specific tokens (HTML context only)
    // -------------------------------------------------------------------------

    // Text containing literal @ (when preceded by word character)
    // This handles email addresses like user@example.com or mailto:user@example.com
    // Pattern: [text]word@word[text] where the @ is preceded by a word char
    // NOTE: Don't match in C# context - email patterns aren't needed in C# code
    if (valid_symbols[TEXT_WITH_LITERAL_AT] && !in_csharp_context(scanner)) {
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

            // Stop at characters that shouldn't be in text (expression continuations)
            if (lexer->lookahead == '.' || lexer->lookahead == '[' || lexer->lookahead == '(') {
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
            if (at_line_start && (lexer->lookahead == ' ' || lexer->lookahead == '\t')) {
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

    // @{ and @( - enter C# context
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
        // Not @{ or @( - don't match
        return false;
    }

    // { in Razor block context (after @if, @for, etc.) - enters C# brace context
    if (valid_symbols[RAZOR_BLOCK_OPEN]) {
        while (iswspace(lexer->lookahead)) {
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
        while (iswspace(lexer->lookahead)) {
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

    // C# preprocessor directive - only valid when in C# context
    if (valid_symbols[CSHARP_PREPROC] && in_csharp_context(scanner) && lexer->lookahead == '#') {
        razor_advance(lexer);

        // Consume rest of line (the directive content)
        while (!lexer->eof(lexer) && lexer->lookahead != '\n' && lexer->lookahead != '\r') {
            razor_advance(lexer);
        }
        // Consume the newline
        if (!lexer->eof(lexer) && lexer->lookahead == '\r') {
            razor_advance(lexer);
        }
        if (!lexer->eof(lexer) && lexer->lookahead == '\n') {
            razor_advance(lexer);
        }
        lexer->result_symbol = CSHARP_PREPROC;
        return true;
    }

    // -------------------------------------------------------------------------
    // Script and style content - raw text until closing tag
    // -------------------------------------------------------------------------

    // Script content - scan until </script>
    if (valid_symbols[SCRIPT_CONTENT]) {
        // First check if we're immediately at the end tag
        if (lexer->lookahead == '<') {
            // Peek to see if this is </script>
            lexer->mark_end(lexer);
            razor_advance(lexer);
            if (lexer->lookahead == '/') {
                razor_advance(lexer);
                // Check for 'script' (case insensitive)
                int32_t c = lexer->lookahead;
                if (c == 's' || c == 'S') {
                    // Likely </script> - don't match any content
                    return false;
                }
            }
            // Not </script>, so < is content - but we need to restart
            // Return false to let the parser try again
            return false;
        }

        bool has_content = false;

        while (!lexer->eof(lexer)) {
            // Check for end tag
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
                        if (c != tag[i] && c != (tag[i] - 32)) { // case insensitive
                            matches = false;
                        } else {
                            razor_advance(lexer);
                            i++;
                        }
                    }
                    if (matches && i == 6) {
                        // Found </script - stop before the <
                        // mark_end was already called at <
                        break;
                    }
                }
                // Not </script>, continue - the < and / and any other chars are content
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
        // First check if we're immediately at the end tag
        if (lexer->lookahead == '<') {
            lexer->mark_end(lexer);
            razor_advance(lexer);
            if (lexer->lookahead == '/') {
                razor_advance(lexer);
                int32_t c = lexer->lookahead;
                if (c == 's' || c == 'S') {
                    // Likely </style> - don't match any content
                    return false;
                }
            }
            return false;
        }

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
                    if (matches && i == 5) {
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
        // First check if we're immediately at the end tag
        if (lexer->lookahead == '<') {
            lexer->mark_end(lexer);
            razor_advance(lexer);
            if (lexer->lookahead == '/') {
                razor_advance(lexer);
                int32_t c = lexer->lookahead;
                if (c == 't' || c == 'T') {
                    return false;
                }
            }
            return false;
        }

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
                    if (matches && i == 5) {
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
        // First check if we're immediately at the end tag
        if (lexer->lookahead == '<') {
            lexer->mark_end(lexer);
            razor_advance(lexer);
            if (lexer->lookahead == '/') {
                razor_advance(lexer);
                int32_t c = lexer->lookahead;
                if (c == 't' || c == 'T') {
                    return false;
                }
            }
            return false;
        }

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
                    if (matches && i == 8) {
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
