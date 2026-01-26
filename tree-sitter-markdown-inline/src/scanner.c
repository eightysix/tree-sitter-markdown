#include "tree_sitter/parser.h"

#ifdef _MSC_VER
#define UNUSED __pragma(warning(suppress : 4101))
#else
#define UNUSED __attribute__((unused))
#endif

// For explanation of the tokens see grammar.js
typedef enum {
    ERROR,
    TRIGGER_ERROR,
    CODE_SPAN_START,
    CODE_SPAN_CLOSE,
    EMPHASIS_OPEN_STAR,
    EMPHASIS_OPEN_UNDERSCORE,
    EMPHASIS_CLOSE_STAR,
    EMPHASIS_CLOSE_UNDERSCORE,
    LAST_TOKEN_WHITESPACE,
    LAST_TOKEN_PUNCTUATION,
    STRIKETHROUGH_OPEN,
    STRIKETHROUGH_CLOSE,
    LATEX_SPAN_START,
    LATEX_SPAN_CLOSE,
    UNCLOSED_SPAN,
    CITATION_AT
} TokenType;

// Determines if a character is punctuation as defined by the markdown spec.
static bool is_punctuation(int32_t chr) {
    return (chr >= '!' && chr <= '/') || (chr >= ':' && chr <= '@') ||
           (chr >= '[' && chr <= '`') || (chr >= '{' && chr <= '~');
}

// State bitflags used with `Scanner.state`

// TODO
static UNUSED const uint8_t STATE_EMPHASIS_DELIMITER_MOD_3 = 0x3;
// Current delimiter run is opening
static const uint8_t STATE_EMPHASIS_DELIMITER_IS_OPEN = 0x1 << 2;

// Convenience function to emit the error token. This is done to stop invalid
// parse branches. Specifically:
// 1. When encountering a newline after a line break that ended a paragraph, and
// no new block
//    has been opened.
// 2. When encountering a new block after a soft line break.
// 3. When a `$._trigger_error` token is valid, which is used to stop parse
// branches through
//    normal tree-sitter grammar rules.
//
// See also the `$._soft_line_break` and `$._paragraph_end_newline` tokens in
// grammar.js
static bool error(TSLexer *lexer) {
    lexer->result_symbol = ERROR;
    return true;
}

typedef struct {
    // Parser state flags
    uint8_t state;
    uint8_t code_span_delimiter_length;
    uint8_t latex_span_delimiter_length;
    // The number of characters remaining in the currrent emphasis delimiter
    // run.
    uint8_t num_emphasis_delimiters_left;

} Scanner;

// Write the whole state of a Scanner to a byte buffer
static unsigned serialize(Scanner *s, char *buffer) {
    unsigned size = 0;
    buffer[size++] = (char)s->state;
    buffer[size++] = (char)s->code_span_delimiter_length;
    buffer[size++] = (char)s->latex_span_delimiter_length;
    buffer[size++] = (char)s->num_emphasis_delimiters_left;
    return size;
}

// Read the whole state of a Scanner from a byte buffer
// `serizalize` and `deserialize` should be fully symmetric.
static void deserialize(Scanner *s, const char *buffer, unsigned length) {
    s->state = 0;
    s->code_span_delimiter_length = 0;
    s->latex_span_delimiter_length = 0;
    s->num_emphasis_delimiters_left = 0;
    if (length > 0) {
        size_t size = 0;
        s->state = (uint8_t)buffer[size++];
        s->code_span_delimiter_length = (uint8_t)buffer[size++];
        s->latex_span_delimiter_length = (uint8_t)buffer[size++];
        s->num_emphasis_delimiters_left = (uint8_t)buffer[size++];
    }
}

static bool is_whitespace(int32_t chr) {
    return chr == ' ' || chr == '\t' || chr == '\n' || chr == '\r';
}

// Updated parse_leaf_delimiter with padding logic
static bool parse_leaf_delimiter(TSLexer *lexer, uint8_t *delimiter_length,
                                 const bool *valid_symbols,
                                 const char delimiter,
                                 const TokenType open_token,
                                 const TokenType close_token,
                                 bool allow_padding) { // New parameter
    uint8_t level = 0;
    while (lexer->lookahead == delimiter) {
        lexer->advance(lexer, false);
        level++;
    }

    // CLOSING DELIMITER LOGIC
    if (level == *delimiter_length && valid_symbols[close_token]) {
        *delimiter_length = 0;
        lexer->result_symbol = close_token;
        return true;
    }

    // OPENING DELIMITER LOGIC
    if (valid_symbols[open_token]) {
        // Rule: If padding is not allowed, the next char cannot be whitespace
        if (!allow_padding && is_whitespace(lexer->lookahead)) {
            return false;
        }

        // Rule: If padding is not allowed, the next char cannot be EOF
        if (!allow_padding && lexer->eof(lexer)) {
            return false;
        }

        // Parse ahead to check if there is a matching closing delimiter
        lexer->mark_end(lexer);
        
        size_t close_level = 0;
        int32_t previous_char = 0; // To check preceding whitespace for closer

        while (!lexer->eof(lexer)) {
            if (lexer->lookahead == delimiter) {
                // Potential closer found.
                // Rule: If padding is not allowed, closer cannot be preceded by whitespace.
                // We check 'previous_char' which holds the char before this sequence of delimiters.
                bool preceded_by_whitespace = is_whitespace(previous_char);

                // Count the delimiters
                close_level = 0;
                while (lexer->lookahead == delimiter) {
                    close_level++;
                    lexer->advance(lexer, false);
                }

                if (close_level == level) {
                    // Length matches. Now check padding constraints.
                    if (allow_padding || !preceded_by_whitespace) {
                        // Found a valid matching delimiter
                        *delimiter_length = level;
                        lexer->result_symbol = open_token;
                        return true;
                    }
                    // If we are here, we found delimiters of correct length, 
                    // but they were invalid (e.g. preceded by space in inline math).
                    // We treat them as content and continue searching.
                }
                
                // If length didn't match or padding check failed, reset and continue.
                previous_char = delimiter; // The last char we saw was the delimiter itself
            } else {
                previous_char = lexer->lookahead;
                lexer->advance(lexer, false);
            }
        }
        
        // No matching delimiter found
        if (valid_symbols[UNCLOSED_SPAN]) {
            lexer->result_symbol = UNCLOSED_SPAN;
            return true;
        }
    }
    return false;
}

static bool parse_backtick(Scanner *s, TSLexer *lexer,
                           const bool *valid_symbols) {
    // Backticks (code spans) always allow padding: ` code ` is valid
    return parse_leaf_delimiter(lexer, &s->code_span_delimiter_length,
                                valid_symbols, '`', CODE_SPAN_START,
                                CODE_SPAN_CLOSE, true);
}

static bool parse_dollar(Scanner *s, TSLexer *lexer,
                         const bool *valid_symbols) {
    // 1. Consume dollars to determine if this is $ (inline) or $$ (block)
    // We need to peek first because parse_leaf_delimiter consumes them.
    // However, parse_leaf_delimiter logic is built to consume.
    // We can't know the length until we consume. 
    // But we need to pass the 'allow_padding' flag *into* parse_leaf_delimiter.
    
    // Workaround: We manually check length first, then rewind? 
    // Or better, we fork the logic inside parse_dollar before calling the helper, 
    // but the helper does the scanning.
    
    // Let's modify the approach: Check the length inside parse_leaf_delimiter?
    // No, that function is generic.
    
    // Strategy: Peek at the length first without consuming permanently (mark_end isn't enough).
    // Actually, simply assume Inline ($) logic (strict) unless we see 2 chars.
    // But we can't implement that easily with the single helper function as written.
    
    // Simplest fix: Re-implement parse_dollar logic explicitly to handle the $ vs $$ distinction.
    
    uint8_t level = 0;
    // Lookahead to count delimiters
    while (lexer->lookahead == '$') {
        lexer->advance(lexer, false);
        level++;
    }
    
    // Decide based on length
    // $ = Inline (No padding allowed)
    // $$ = Block (Padding allowed)
    bool allow_padding = level >= 2;
    TokenType open_token = LATEX_SPAN_START;
    TokenType close_token = LATEX_SPAN_CLOSE;

    // CLOSING LOGIC
    if (level == s->latex_span_delimiter_length && valid_symbols[close_token]) {
        s->latex_span_delimiter_length = 0;
        lexer->result_symbol = close_token;
        return true;
    }

    // OPENING LOGIC
    if (valid_symbols[open_token]) {
        if (!allow_padding && (is_whitespace(lexer->lookahead) || lexer->eof(lexer))) {
            return false;
        }

        lexer->mark_end(lexer);
        
        size_t close_level = 0;
        int32_t previous_char = 0; 
        
        // Note: previous_char is technically the last '$' we consumed.
        // But for the check inside the loop, we care about the char before the *closer*.
        // So initializing to 0 is fine, provided we track inside the loop.

        while (!lexer->eof(lexer)) {
            if (lexer->lookahead == '$') {
                bool preceded_by_whitespace = is_whitespace(previous_char);
                
                close_level = 0;
                while (lexer->lookahead == '$') {
                    close_level++;
                    lexer->advance(lexer, false);
                }

                if (close_level == level) {
                    if (allow_padding || !preceded_by_whitespace) {
                        s->latex_span_delimiter_length = level;
                        lexer->result_symbol = open_token;
                        return true;
                    }
                }
                previous_char = '$';
            } else {
                previous_char = lexer->lookahead;
                lexer->advance(lexer, false);
            }
        }
        
        if (valid_symbols[UNCLOSED_SPAN]) {
             lexer->result_symbol = UNCLOSED_SPAN;
             return true;
        }
    }
    
    return false;
}

static bool parse_star(Scanner *s, TSLexer *lexer, const bool *valid_symbols) {
    lexer->advance(lexer, false);
    // If `num_emphasis_delimiters_left` is not zero then we already decided
    // that this should be part of an emphasis delimiter run, so interpret it as
    // such.
    if (s->num_emphasis_delimiters_left > 0) {
        // The `STATE_EMPHASIS_DELIMITER_IS_OPEN` state flag tells us wether it
        // should be open or close.
        if ((s->state & STATE_EMPHASIS_DELIMITER_IS_OPEN) &&
            valid_symbols[EMPHASIS_OPEN_STAR]) {
            s->state &= (~STATE_EMPHASIS_DELIMITER_IS_OPEN);
            lexer->result_symbol = EMPHASIS_OPEN_STAR;
            s->num_emphasis_delimiters_left--;
            return true;
        }
        if (valid_symbols[EMPHASIS_CLOSE_STAR]) {
            lexer->result_symbol = EMPHASIS_CLOSE_STAR;
            s->num_emphasis_delimiters_left--;
            return true;
        }
    }
    lexer->mark_end(lexer);
    // Otherwise count the number of stars
    uint8_t star_count = 1;
    while (lexer->lookahead == '*') {
        star_count++;
        lexer->advance(lexer, false);
    }
    bool line_end = lexer->lookahead == '\n' || lexer->lookahead == '\r' ||
                    lexer->eof(lexer);
    if (valid_symbols[EMPHASIS_OPEN_STAR] ||
        valid_symbols[EMPHASIS_CLOSE_STAR]) {
        // The desicion made for the first star also counts for all the
        // following stars in the delimiter run. Rembemer how many there are.
        s->num_emphasis_delimiters_left = star_count - 1;
        // Look ahead to the next symbol (after the last star) to find out if it
        // is whitespace punctuation or other.
        bool next_symbol_whitespace =
            line_end || lexer->lookahead == ' ' || lexer->lookahead == '\t';
        bool next_symbol_punctuation = is_punctuation(lexer->lookahead);
        // Information about the last token is in valid_symbols. See grammar.js
        // for these tokens for how this is done.
        if (valid_symbols[EMPHASIS_CLOSE_STAR] &&
            !valid_symbols[LAST_TOKEN_WHITESPACE] &&
            (!valid_symbols[LAST_TOKEN_PUNCTUATION] ||
             next_symbol_punctuation || next_symbol_whitespace)) {
            // Closing delimiters take precedence
            s->state &= ~STATE_EMPHASIS_DELIMITER_IS_OPEN;
            lexer->result_symbol = EMPHASIS_CLOSE_STAR;
            return true;
        }
        if (!next_symbol_whitespace && (!next_symbol_punctuation ||
                                        valid_symbols[LAST_TOKEN_PUNCTUATION] ||
                                        valid_symbols[LAST_TOKEN_WHITESPACE])) {
            s->state |= STATE_EMPHASIS_DELIMITER_IS_OPEN;
            lexer->result_symbol = EMPHASIS_OPEN_STAR;
            return true;
        }
    }
    return false;
}

static bool parse_tilde(Scanner *s, TSLexer *lexer, const bool *valid_symbols) {
    lexer->advance(lexer, false);
    // If `num_emphasis_delimiters_left` is not zero then we already decided
    // that this should be part of an emphasis delimiter run, so interpret it as
    // such.
    if (s->num_emphasis_delimiters_left > 0) {
        // The `STATE_EMPHASIS_DELIMITER_IS_OPEN` state flag tells us wether it
        // should be open or close.
        if ((s->state & STATE_EMPHASIS_DELIMITER_IS_OPEN) &&
            valid_symbols[STRIKETHROUGH_OPEN]) {
            s->state &= (~STATE_EMPHASIS_DELIMITER_IS_OPEN);
            lexer->result_symbol = STRIKETHROUGH_OPEN;
            s->num_emphasis_delimiters_left--;
            return true;
        }
        if (valid_symbols[STRIKETHROUGH_CLOSE]) {
            lexer->result_symbol = STRIKETHROUGH_CLOSE;
            s->num_emphasis_delimiters_left--;
            return true;
        }
    }
    lexer->mark_end(lexer);
    // Otherwise count the number of tildes
    uint8_t star_count = 1;
    while (lexer->lookahead == '~') {
        star_count++;
        lexer->advance(lexer, false);
    }
    bool line_end = lexer->lookahead == '\n' || lexer->lookahead == '\r' ||
                    lexer->eof(lexer);
    if (valid_symbols[STRIKETHROUGH_OPEN] ||
        valid_symbols[STRIKETHROUGH_CLOSE]) {
        // The desicion made for the first star also counts for all the
        // following stars in the delimiter run. Rembemer how many there are.
        s->num_emphasis_delimiters_left = star_count - 1;
        // Look ahead to the next symbol (after the last star) to find out if it
        // is whitespace punctuation or other.
        bool next_symbol_whitespace =
            line_end || lexer->lookahead == ' ' || lexer->lookahead == '\t';
        bool next_symbol_punctuation = is_punctuation(lexer->lookahead);
        // Information about the last token is in valid_symbols. See grammar.js
        // for these tokens for how this is done.
        if (valid_symbols[STRIKETHROUGH_CLOSE] &&
            !valid_symbols[LAST_TOKEN_WHITESPACE] &&
            (!valid_symbols[LAST_TOKEN_PUNCTUATION] ||
             next_symbol_punctuation || next_symbol_whitespace)) {
            // Closing delimiters take precedence
            s->state &= ~STATE_EMPHASIS_DELIMITER_IS_OPEN;
            lexer->result_symbol = STRIKETHROUGH_CLOSE;
            return true;
        }
        if (!next_symbol_whitespace && (!next_symbol_punctuation ||
                                        valid_symbols[LAST_TOKEN_PUNCTUATION] ||
                                        valid_symbols[LAST_TOKEN_WHITESPACE])) {
            s->state |= STATE_EMPHASIS_DELIMITER_IS_OPEN;
            lexer->result_symbol = STRIKETHROUGH_OPEN;
            return true;
        }
    }
    return false;
}

static bool parse_underscore(Scanner *s, TSLexer *lexer,
                             const bool *valid_symbols) {
    lexer->advance(lexer, false);
    // If `num_emphasis_delimiters_left` is not zero then we already decided
    // that this should be part of an emphasis delimiter run, so interpret it as
    // such.
    if (s->num_emphasis_delimiters_left > 0) {
        // The `STATE_EMPHASIS_DELIMITER_IS_OPEN` state flag tells us wether it
        // should be open or close.
        if ((s->state & STATE_EMPHASIS_DELIMITER_IS_OPEN) &&
            valid_symbols[EMPHASIS_OPEN_UNDERSCORE]) {
            s->state &= (~STATE_EMPHASIS_DELIMITER_IS_OPEN);
            lexer->result_symbol = EMPHASIS_OPEN_UNDERSCORE;
            s->num_emphasis_delimiters_left--;
            return true;
        }
        if (valid_symbols[EMPHASIS_CLOSE_UNDERSCORE]) {
            lexer->result_symbol = EMPHASIS_CLOSE_UNDERSCORE;
            s->num_emphasis_delimiters_left--;
            return true;
        }
    }
    lexer->mark_end(lexer);
    // Otherwise count the number of stars
    uint8_t underscore_count = 1;
    while (lexer->lookahead == '_') {
        underscore_count++;
        lexer->advance(lexer, false);
    }
    bool line_end = lexer->lookahead == '\n' || lexer->lookahead == '\r' ||
                    lexer->eof(lexer);
    if (valid_symbols[EMPHASIS_OPEN_UNDERSCORE] ||
        valid_symbols[EMPHASIS_CLOSE_UNDERSCORE]) {
        // The desicion made for the first underscore also counts for all the
        // following underscores in the delimiter run. Rembemer how many there are.
        s->num_emphasis_delimiters_left = underscore_count - 1;
        // Look ahead to the next symbol (after the last underscore) to find out if it
        // is whitespace punctuation or other.
        bool next_symbol_whitespace =
            line_end || lexer->lookahead == ' ' || lexer->lookahead == '\t';
        bool next_symbol_punctuation = is_punctuation(lexer->lookahead);
        // Information about the last token is in valid_symbols. See grammar.js
        // for these tokens for how this is done.
        if (valid_symbols[EMPHASIS_CLOSE_UNDERSCORE] &&
            !valid_symbols[LAST_TOKEN_WHITESPACE] &&
            (!valid_symbols[LAST_TOKEN_PUNCTUATION] ||
             next_symbol_punctuation || next_symbol_whitespace)) {
            // Closing delimiters take precedence
            s->state &= ~STATE_EMPHASIS_DELIMITER_IS_OPEN;
            lexer->result_symbol = EMPHASIS_CLOSE_UNDERSCORE;
            return true;
        }
        if (!next_symbol_whitespace && (!next_symbol_punctuation ||
                                        valid_symbols[LAST_TOKEN_PUNCTUATION] ||
                                        valid_symbols[LAST_TOKEN_WHITESPACE])) {
            s->state |= STATE_EMPHASIS_DELIMITER_IS_OPEN;
            lexer->result_symbol = EMPHASIS_OPEN_UNDERSCORE;
            return true;
        }
    }
    return false;
}

static bool scan(Scanner *s, TSLexer *lexer, const bool *valid_symbols) {
    // A normal tree-sitter rule decided that the current branch is invalid and
    // now "requests" an error to stop the branch
    if (valid_symbols[TRIGGER_ERROR]) {
        return error(lexer);
    }

    // Decide which tokens to consider based on the first non-whitespace
    // character
    switch (lexer->lookahead) {
        case '@':
            if (valid_symbols[CITATION_AT]) {
                // 1. Check valid preceding context (already in your code)
                if (valid_symbols[LAST_TOKEN_WHITESPACE] || valid_symbols[LAST_TOKEN_PUNCTUATION]) {
            
                    lexer->advance(lexer, false); // Move past '@'

                    // 2. Lookahead: Check if this is "fig:" or "tbl:"
                    // We use a small temporary buffer or nested checks
                    if (lexer->lookahead == 'f') {
                        lexer->advance(lexer, false);
                        if (lexer->lookahead == 'i') {
                            lexer->advance(lexer, false);
                            if (lexer->lookahead == 'g') {
                                lexer->advance(lexer, false);
                                if (lexer->lookahead == ':') return false; // It's a crossref!
                            }
                        }
                    } else if (lexer->lookahead == 't') {
                        lexer->advance(lexer, false);
                        if (lexer->lookahead == 'b') {
                            lexer->advance(lexer, false);
                            if (lexer->lookahead == 'l') {
                                lexer->advance(lexer, false);
                                if (lexer->lookahead == ':') return false; // It's a crossref!
                            }
                        }
                    }

                    // 3. If it wasn't fig: or tbl:, it's a valid citation start
                    lexer->result_symbol = CITATION_AT;
                    return true;
                }
            }
        break;
        case '`':
            // A backtick could mark the beginning or ending of a code span or a
            // fenced code block.
            return parse_backtick(s, lexer, valid_symbols);
        case '$':
            return parse_dollar(s, lexer, valid_symbols);
        case '*':
            // A star could either mark the beginning or ending of emphasis, a
            // list item or thematic break. This code is similar to the code for
            // '_' and '+'.
            return parse_star(s, lexer, valid_symbols);
        case '_':
            return parse_underscore(s, lexer, valid_symbols);
        case '~':
            return parse_tilde(s, lexer, valid_symbols);
    }
    return false;
}

void *tree_sitter_markdown_inline_external_scanner_create() {
    Scanner *s = (Scanner *)malloc(sizeof(Scanner));
    deserialize(s, NULL, 0);
    return s;
}

bool tree_sitter_markdown_inline_external_scanner_scan(
    void *payload, TSLexer *lexer, const bool *valid_symbols) {
    Scanner *scanner = (Scanner *)payload;
    return scan(scanner, lexer, valid_symbols);
}

unsigned tree_sitter_markdown_inline_external_scanner_serialize(void *payload,
                                                                char *buffer) {
    Scanner *scanner = (Scanner *)payload;
    return serialize(scanner, buffer);
}

void tree_sitter_markdown_inline_external_scanner_deserialize(void *payload,
                                                              const char *buffer,
                                                              unsigned length) {
    Scanner *scanner = (Scanner *)payload;
    deserialize(scanner, buffer, length);
}

void tree_sitter_markdown_inline_external_scanner_destroy(void *payload) {
    Scanner *scanner = (Scanner *)payload;
    free(scanner);
}
