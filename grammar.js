/**
 * @file Razor grammar for Tree-sitter
 * @author Jeffrey Crochet <jlcrochet91@pm.me>
 * @license MIT
 */

/// <reference types="tree-sitter-cli/dsl" />
// @ts-check

const csharp = require("./tree-sitter-c-sharp/grammar");

module.exports = grammar(csharp, {
  name: "razor",

  conflicts: ($, original) => original.concat([
    [$._razor_access_chain, $._razor_element_access],
    [$._razor_access_chain, $._razor_invocation],
  ]),

  supertypes: ($, original) => original.concat([
    $.razor_directive,
  ]),

  extras: ($, original) => {
    // Filter out the old preproc rules and comment - we override them with context-aware versions
    const filtered = [
      'preproc_region', 'preproc_endregion', 'preproc_line', 'preproc_pragma',
      'preproc_nullable', 'preproc_error', 'preproc_warning', 'preproc_define', 'preproc_undef',
      'comment'
    ];
    return original.filter(rule => {
      // Keep rules that aren't the filtered rules
      if (filtered.some(name => rule.name === name)) return false;
      // Keep whitespace in extras - it's required for C# parsing
      return true;
    }).concat([
      // Add context-aware versions
      $.comment,
      $.preproc_region,
      $.preproc_endregion,
      $.preproc_line,
      $.preproc_pragma,
      $.preproc_nullable,
      $.preproc_error,
      $.preproc_warning,
      $.preproc_define,
      $.preproc_undef,
    ]);
  },

  externals: ($, original) => original.concat([
    // Text containing literal @ (email addresses, etc.)
    $._text_with_literal_at,   // word+@ pattern like "user@example.com"
    // HTML text content that stops before else/catch/finally keywords
    $._html_text_content,
    // Context-tracking tokens for C# vs HTML mode
    $._csharp_code_block_start,     // @{ - enters C# context
    $._csharp_explicit_expr_start,  // @( - enters C# context
    $._razor_block_open,            // { after Razor statement - enters C# context
    $._csharp_context_close,        // } or ) that exits C# context
    $._csharp_comment,              // C# comment, only valid in C# context
    $._preproc_directive_start,     // # at start of preprocessor directive, only valid in C# context
    // Script/style/title/textarea raw text content
    $._script_content,              // Raw content inside <script> tags
    $._style_content,               // Raw content inside <style> tags
    $._title_content,               // Raw content inside <title> tags
    $._textarea_content,            // Raw content inside <textarea> tags
    // Implicit expression terminator - detects whitespace after identifier
    $._implicit_expr_end,           // Zero-width token that matches when whitespace follows
    // @ token in Razor block context (to prevent C# @identifier tokenization for expressions)
    $._razor_block_at,              // @ in Razor block - starts nested Razor expression
    // Using directive: zero-width token that matches when NOT followed by = or .
    $._using_not_alias,
    // @keyword tokens for nested control flow (matched as single token to avoid seq overhead)
    $._nested_at_if,                // @if in C# context
    $._nested_at_for,               // @for in C# context
    $._nested_at_foreach,           // @foreach in C# context
    $._nested_at_while,             // @while in C# context
    $._nested_at_do,                // @do in C# context
    $._nested_at_switch,            // @switch in C# context
    $._nested_at_try,               // @try in C# context
    $._nested_at_lock,              // @lock in C# context
    $._nested_at_using,             // @using in C# context
  ]),

  rules: {
    // Override compilation_unit to be the Razor entry point
    compilation_unit: $ => repeat($._node),

    // A node can be HTML content, a Razor construct, or an HTML element
    // At top level, text uses keyword-aware scanner to stop before else/catch/finally
    _node: $ => choice(
      $.doctype,
      $.script_element,
      $.style_element,
      $.title_element,
      $.textarea_element,
      $.element,
      $.self_closing_element,
      $.void_element,
      $.html_comment,
      $.razor_comment,
      $.razor_directive,
      $.razor_code_block,
      $.razor_statement,
      $.razor_explicit_expression,
      $.razor_implicit_expression,
      $.escaped_at,
      $._top_level_text,
    ),

    // HTML DOCTYPE declaration
    // The !DOCTYPE... part must be immediate after < and have higher precedence than opt-out !
    doctype: $ => seq(
      '<',
      token.immediate(prec(10, /![Dd][Oo][Cc][Tt][Yy][Pp][Ee][^>]*/)),
      '>',
    ),

    // @@ escapes to a literal @ character
    escaped_at: _ => '@@',

    // =========================================================================
    // Razor Statements (@if, @foreach, etc.)
    // =========================================================================

    // Razor statement uses @keyword as a single token for consistent highlighting
    // Higher precedence than razor_implicit_expression to ensure @if is parsed as control flow
    razor_statement: $ => prec(10, choice(
      alias($._razor_if_statement, $.if_statement),
      alias($._razor_for_statement, $.for_statement),
      alias($._razor_foreach_statement, $.foreach_statement),
      alias($._razor_while_statement, $.while_statement),
      alias($._razor_do_statement, $.do_statement),
      alias($._razor_switch_statement, $.switch_statement),
      alias($._razor_try_statement, $.try_statement),
      alias($._razor_lock_statement, $.lock_statement),
      alias($._razor_using_statement, $.using_statement),
    )),

    // Razor block - can contain statements AND HTML elements
    // Uses external scanner to track entering/exiting C# context
    razor_block: $ => seq(
      alias($._razor_block_open, '{'),
      repeat($._razor_block_content),
      alias($._csharp_context_close, '}'),
    ),

    // Content inside Razor blocks - can contain C# statements, HTML elements, and Razor expressions
    _razor_block_content: $ => choice(
      $._razor_statement,
      alias($._nested_razor_statement, $.razor_statement),  // Razor control flow like @if, @foreach, etc.
      $.element,
      $.self_closing_element,
      $.void_element,
      $.razor_text_literal,
      $._nested_razor_explicit_expression,
      $._nested_razor_implicit_expression,
    ),

    // Nested Razor statement inside a block
    // Must use external scanner $._razor_block_at to capture @ in C# context,
    // because token(prec()) doesn't help when C# lexer matches @identifier first
    _nested_razor_statement: $ => choice(
      alias($._nested_if_statement, $.if_statement),
      alias($._nested_for_statement, $.for_statement),
      alias($._nested_foreach_statement, $.foreach_statement),
      alias($._nested_while_statement, $.while_statement),
      alias($._nested_do_statement, $.do_statement),
      alias($._nested_switch_statement, $.switch_statement),
      alias($._nested_try_statement, $.try_statement),
      alias($._nested_lock_statement, $.lock_statement),
      alias($._nested_using_statement, $.using_statement),
    ),

    // Nested statement definitions - use external @keyword tokens (single token, avoids seq overhead)
    _nested_if_statement: $ => seq(alias($._nested_at_if, '@if'), $._razor_if_statement_body),
    _nested_for_statement: $ => seq(alias($._nested_at_for, '@for'), $._razor_for_statement_body),
    _nested_foreach_statement: $ => seq(optional('await'), alias($._nested_at_foreach, '@foreach'), $._razor_foreach_statement_body),
    _nested_while_statement: $ => seq(alias($._nested_at_while, '@while'), $._razor_while_statement_body),
    _nested_do_statement: $ => seq(alias($._nested_at_do, '@do'), $._razor_do_statement_body),
    _nested_switch_statement: $ => seq(alias($._nested_at_switch, '@switch'), $._razor_switch_statement_body),
    _nested_try_statement: $ => seq(alias($._nested_at_try, '@try'), $._razor_try_statement_body),
    _nested_lock_statement: $ => seq(alias($._nested_at_lock, '@lock'), $._razor_lock_statement_body),
    _nested_using_statement: $ => seq(optional('await'), alias($._nested_at_using, '@using'), $._razor_using_statement_body),

    // Nested Razor expressions inside a block - uses external @ token
    _nested_razor_explicit_expression: $ => seq(
      alias($._razor_block_at, '@'),
      '(',
      $.expression,
      ')',
    ),

    // Lower precedence than _nested_razor_statement to ensure @if/@for/etc. are parsed as statements
    // prec.dynamic not needed here since _nested_razor_statement uses distinct @keyword tokens
    _nested_razor_implicit_expression: $ => seq(
      alias($._razor_block_at, '@'),
      $._razor_implicit_expr_chain,
      $._implicit_expr_end,
    ),

    // Statements valid inside Razor code blocks (function body context)
    // This is more restrictive than C#'s full statement rule, excluding:
    // - break/continue (only valid in loops, but those have their own blocks)
    // - return/yield (Razor blocks aren't functions that return values)
    // - goto/labeled_statement (discouraged/rare in Razor)
    // - unsafe/fixed/checked (low-level constructs not typical in Razor)
    // NOTE: Control flow statements (if, for, foreach, while, do, switch, try, lock, using)
    // are handled by _nested_razor_statement with @ prefix, not included here
    // NOTE: Preproc directives are handled as extras with context-aware tokens
    _razor_statement: $ => choice(
      $.block,
      $.empty_statement,
      $.expression_statement,
      $.local_declaration_statement,
      $.local_function_statement,
      $.throw_statement,
    ),

    // @: for single-line text literals inside code blocks
    // Can contain Razor expressions like @person.Name
    razor_text_literal: $ => prec.right(seq(
      '@:',
      repeat($._razor_text_literal_content),
    )),

    _razor_text_literal_content: $ => prec.right(10, choice(
      // Plain text (anything except @ and newline)
      // High token precedence to beat C# identifier in extras
      token(prec(100, /[^@\r\n]+/)),
      // Razor expressions within text literal
      alias($._text_literal_explicit_expression, $.razor_explicit_expression),
      alias($._text_literal_implicit_expression, $.razor_implicit_expression),
      // Escaped @@ in text literal
      $.escaped_at,
    )),

    // Higher precedence versions for text literal context
    // Uses token(prec()) to give @ higher lexical precedence than C# verbatim identifier
    _text_literal_explicit_expression: $ => seq(
      token(prec(10, '@')),
      '(',
      $.expression,
      ')',
    ),

    _text_literal_implicit_expression: $ => seq(
      token(prec(10, '@')),
      $._razor_implicit_expr_chain,
    ),

    // -------------------------------------------------------------------------
    // Razor if statement
    // -------------------------------------------------------------------------
    // Uses high-precedence token to beat C# verbatim identifier @if
    _razor_if_statement: $ => seq(token(prec(100, '@if')), $._razor_if_statement_body),
    // Shared body - uses prec.right so else/else if are associated with this if
    _razor_if_statement_body: $ => prec.right(seq(
      '(',
      field('condition', $.expression),
      ')',
      field('consequence', $.razor_block),
      optional(field('alternative', choice(
        $.razor_else_if_clause,
        alias($.razor_else_clause, $.else_clause),
      ))),
    )),

    // Else-if clause (middle of if chain)
    razor_else_if_clause: $ => prec.right(seq(
      'else',
      'if',
      '(',
      field('condition', $.expression),
      ')',
      field('consequence', $.razor_block),
      optional(field('alternative', choice(
        $.razor_else_if_clause,
        alias($.razor_else_clause, $.else_clause),
      ))),
    )),

    // Else clause (final else in an if chain)
    razor_else_clause: $ => seq(
      'else',
      $.razor_block,
    ),

    // -------------------------------------------------------------------------
    // Razor for statement
    // -------------------------------------------------------------------------
    _razor_for_statement: $ => seq(token(prec(100, '@for')), $._razor_for_statement_body),
    _razor_for_statement_body: $ => seq(
      $._for_statement_conditions,
      field('body', $.razor_block),
    ),

    // -------------------------------------------------------------------------
    // Razor foreach statement
    // -------------------------------------------------------------------------
    _razor_foreach_statement: $ => seq(optional('await'), token(prec(100, '@foreach')), $._razor_foreach_statement_body),
    _razor_foreach_statement_body: $ => seq(
      '(',
      choice(
        seq(
          field('type', $.type),
          field('left', choice($.identifier, $.tuple_pattern)),
        ),
        field('left', $.expression),
      ),
      'in',
      field('right', $.expression),
      ')',
      field('body', $.razor_block),
    ),

    // -------------------------------------------------------------------------
    // Razor while statement
    // -------------------------------------------------------------------------
    _razor_while_statement: $ => seq(token(prec(100, '@while')), $._razor_while_statement_body),
    _razor_while_statement_body: $ => seq(
      '(',
      field('condition', $.expression),
      ')',
      field('body', $.razor_block),
    ),

    // -------------------------------------------------------------------------
    // Razor do statement
    // -------------------------------------------------------------------------
    _razor_do_statement: $ => seq(token(prec(100, '@do')), $._razor_do_statement_body),
    _razor_do_statement_body: $ => seq(
      field('body', $.razor_block),
      'while',
      '(',
      field('condition', $.expression),
      ')',
      ';',
    ),

    // -------------------------------------------------------------------------
    // Razor switch statement
    // -------------------------------------------------------------------------
    _razor_switch_statement: $ => seq(token(prec(100, '@switch')), $._razor_switch_statement_body),
    _razor_switch_statement_body: $ => seq(
      '(',
      field('value', $.expression),
      ')',
      field('body', alias($.razor_switch_body, $.switch_body)),
    ),

    razor_switch_body: $ => seq('{', repeat($.razor_switch_section), '}'),

    razor_switch_section: $ => prec.left(seq(
      choice(
        seq(
          'case',
          choice(
            $.expression,
            seq($.pattern, optional($.when_clause)),
          ),
        ),
        'default',
      ),
      ':',
      repeat($._razor_switch_section_content),
    )),

    // Content inside switch sections - same as _razor_block_content but allows break
    _razor_switch_section_content: $ => choice(
      $._razor_statement,
      alias($._nested_razor_statement, $.razor_statement),  // Razor control flow like @if, @foreach, etc.
      $.break_statement,
      $.element,
      $.self_closing_element,
      $.void_element,
      $.razor_text_literal,
      $._nested_razor_explicit_expression,
      $._nested_razor_implicit_expression,
    ),

    // -------------------------------------------------------------------------
    // Razor try statement
    // -------------------------------------------------------------------------
    _razor_try_statement: $ => seq(token(prec(100, '@try')), $._razor_try_statement_body),
    // Uses prec.right so catch/finally are associated with this try
    _razor_try_statement_body: $ => prec.right(seq(
      field('body', $.razor_block),
      repeat(alias($.razor_catch_clause, $.catch_clause)),
      optional(alias($.razor_finally_clause, $.finally_clause)),
    )),

    // Catch clause for try statements
    razor_catch_clause: $ => seq(
      'catch',
      repeat(choice($.catch_declaration, $.catch_filter_clause)),
      field('body', $.razor_block),
    ),

    // Finally clause for try statements
    razor_finally_clause: $ => seq('finally', $.razor_block),

    // -------------------------------------------------------------------------
    // Razor lock statement
    // -------------------------------------------------------------------------
    _razor_lock_statement: $ => seq(token(prec(100, '@lock')), $._razor_lock_statement_body),
    _razor_lock_statement_body: $ => seq('(', $.expression, ')', $.razor_block),

    // -------------------------------------------------------------------------
    // Razor using statement (not directive)
    // -------------------------------------------------------------------------
    _razor_using_statement: $ => seq(optional('await'), token(prec(100, '@using')), $._razor_using_statement_body),
    _razor_using_statement_body: $ => seq(
      '(',
      choice(
        alias($.using_variable_declaration, $.variable_declaration),
        $.expression,
      ),
      ')',
      field('body', $.razor_block),
    ),

    // =========================================================================
    // HTML Elements
    // =========================================================================

    // Use token.immediate() to ensure no whitespace between < and element name
    // This prevents "< div>" from being parsed as valid HTML

    // Element content uses _element_content which doesn't stop at else/catch/finally
    element: $ => seq(
      $.start_tag,
      repeat($._element_content),
      $.end_tag,
    ),

    // Void elements are HTML elements that cannot have content and must not have an end tag
    // https://html.spec.whatwg.org/multipage/syntax.html#void-elements
    // area, base, br, col, embed, hr, img, input, link, meta, source, track, wbr
    void_element: $ => seq(
      '<',
      optional($._tag_helper_opt_out),
      field('name', alias($._void_element_name, $.element_name)),
      repeat($._html_attribute),
      optional('/'),
      token.immediate('>'),
    ),

    // Case-insensitive void element names
    _void_element_name: _ => token.immediate(choice(
      /area/i,
      /base/i,
      /br/i,
      /col/i,
      /embed/i,
      /hr/i,
      /img/i,
      /input/i,
      /link/i,
      /meta/i,
      /source/i,
      /track/i,
      /wbr/i,
    )),

    // Content inside elements - doesn't need keyword awareness
    _element_content: $ => choice(
      $.script_element,
      $.style_element,
      $.title_element,
      $.textarea_element,
      $.element,
      $.self_closing_element,
      $.void_element,
      $.html_comment,
      $.razor_comment,
      $.razor_directive,
      $.razor_code_block,
      $.razor_statement,
      $.razor_explicit_expression,
      $.razor_implicit_expression,
      $.escaped_at,
      $.text,  // Regular text, not keyword-aware
    ),

    self_closing_element: $ => seq(
      '<',
      optional($._tag_helper_opt_out),
      field('name', alias($._immediate_element_name, $.element_name)),
      repeat($._html_attribute),
      '/',
      token.immediate('>'),
    ),

    start_tag: $ => seq(
      '<',
      optional($._tag_helper_opt_out),
      field('name', alias($._immediate_element_name, $.element_name)),
      repeat($._html_attribute),
      '>',
    ),

    end_tag: $ => seq(
      '</',
      optional($._tag_helper_opt_out),
      field('name', alias($._immediate_element_name, $.element_name)),
      '>',
    ),

    // Tag Helper opt-out character - must immediately follow < or </
    _tag_helper_opt_out: _ => token.immediate('!'),

    // Element name that must immediately follow < or </ (or ! if opt-out)
    // Includes . for Blazor fully-qualified component names like NTExtractor.Components.Admin.Extractor
    _immediate_element_name: _ => token.immediate(/[a-zA-Z][a-zA-Z0-9.:-]*/),

    element_name: _ => /[a-zA-Z][a-zA-Z0-9.:-]*/,

    // =========================================================================
    // Script and Style Elements
    // =========================================================================

    // Script elements contain raw JavaScript content that shouldn't be parsed as HTML
    // Higher precedence than regular elements to ensure <script> is matched first
    script_element: $ => prec(1, seq(
      $.script_start_tag,
      optional($.script_content),
      $.script_end_tag,
    )),

    script_start_tag: $ => seq(
      '<',
      alias(token.immediate(prec(1, /script/i)), $.element_name),
      repeat($._html_attribute),
      '>',
    ),

    script_end_tag: $ => seq(
      '</',
      alias(token.immediate(prec(1, /script/i)), $.element_name),
      '>',
    ),

    script_content: $ => $._script_content,

    // Style elements contain raw CSS content that shouldn't be parsed as HTML
    // Higher precedence than regular elements to ensure <style> is matched first
    style_element: $ => prec(1, seq(
      $.style_start_tag,
      optional($.style_content),
      $.style_end_tag,
    )),

    style_start_tag: $ => seq(
      '<',
      alias(token.immediate(prec(1, /style/i)), $.element_name),
      repeat($._html_attribute),
      '>',
    ),

    style_end_tag: $ => seq(
      '</',
      alias(token.immediate(prec(1, /style/i)), $.element_name),
      '>',
    ),

    style_content: $ => $._style_content,

    // Title elements contain raw text (but can have character references)
    // Higher precedence than regular elements to ensure <title> is matched first
    title_element: $ => prec(1, seq(
      $.title_start_tag,
      optional($.title_content),
      $.title_end_tag,
    )),

    title_start_tag: $ => seq(
      '<',
      alias(token.immediate(prec(1, /title/i)), $.element_name),
      repeat($._html_attribute),
      '>',
    ),

    title_end_tag: $ => seq(
      '</',
      alias(token.immediate(prec(1, /title/i)), $.element_name),
      '>',
    ),

    // Title content is raw text - doesn't contain child elements
    title_content: $ => $._title_content,

    // Textarea elements contain raw text (but can have character references)
    // Higher precedence than regular elements to ensure <textarea> is matched first
    textarea_element: $ => prec(1, seq(
      $.textarea_start_tag,
      optional($.textarea_content),
      $.textarea_end_tag,
    )),

    textarea_start_tag: $ => seq(
      '<',
      alias(token.immediate(prec(1, /textarea/i)), $.element_name),
      repeat($._html_attribute),
      '>',
    ),

    textarea_end_tag: $ => seq(
      '</',
      alias(token.immediate(prec(1, /textarea/i)), $.element_name),
      '>',
    ),

    // Textarea content is raw text - doesn't contain child elements
    textarea_content: $ => $._textarea_content,

    // =========================================================================
    // HTML Attributes
    // =========================================================================

    // Note: HTML attribute rules are prefixed with "html_" to avoid collision
    // with C#'s `attribute` rule (used for [Attribute] syntax)

    _html_attribute: $ => choice(
      $.html_attribute,
      $.razor_attribute,
    ),

    html_attribute: $ => choice(
      // Attribute with value: name="value" or name = "value"
      seq(
        $.html_attribute_name,
        '=',
        $.html_attribute_value,
      ),
      // Boolean attribute: name (no value)
      $.html_attribute_name,
    ),

    // Note: @ is NOT allowed at start - @attributes are handled by razor_attribute
    html_attribute_name: _ => /[a-zA-Z_:][a-zA-Z0-9_.:-]*/,

    html_attribute_value: $ => choice(
      $.html_quoted_attribute_value,
      $.html_unquoted_attribute_value,
    ),

    html_quoted_attribute_value: $ => choice(
      seq('"', optional($._html_double_quoted_attribute_content), '"'),
      seq("'", optional($._html_single_quoted_attribute_content), "'"),
    ),

    _html_double_quoted_attribute_content: $ => repeat1(choice(
      /[^"@]+/,
      // Email/literal @ in attribute values (e.g., mailto:user@example.com)
      $._text_with_literal_at,
      $.razor_explicit_expression,
      $.razor_implicit_expression,
    )),

    _html_single_quoted_attribute_content: $ => repeat1(choice(
      /[^'@]+/,
      // Email/literal @ in attribute values
      $._text_with_literal_at,
      $.razor_explicit_expression,
      $.razor_implicit_expression,
    )),

    // Unquoted attribute value - can be plain text or Razor expressions
    // In Blazor, you often see: Property=@(s => s.Id) or Items=@Model.Items
    html_unquoted_attribute_value: $ => choice(
      // Razor explicit expression as unquoted value: Property=@(s => s.Id)
      $.razor_explicit_expression,
      // Razor implicit expression as unquoted value: Items=@Model.Items
      $.razor_implicit_expression,
      // Plain unquoted value (no @ or Razor): class=my-class
      /[^\s"'=<>`@]+/,
    ),

    // Razor-specific attribute (e.g., @onclick, @bind)
    razor_attribute: $ => choice(
      // With value: @onclick="handler"
      seq(
        '@',
        $.html_attribute_name,
        '=',
        $.html_attribute_value,
      ),
      // Without value: @rendermode
      seq('@', $.html_attribute_name),
    ),

    // =========================================================================
    // Comments
    // =========================================================================

    // Razor comment @* ... *@
    // Simple approach: match everything until we see *@
    razor_comment: $ => token(seq(
      '@*',
      /([^*]|\*[^@])*/,
      '*@',
    )),

    // HTML comment <!-- ... -->
    html_comment: $ => token(seq(
      '<!--',
      /([^-]|-[^-]|--[^>])*/,
      '-->',
    )),

    // Override C# comment to use external scanner for context-awareness
    // This ensures comments only match in C# context, not HTML
    comment: $ => $._csharp_comment,

    // =========================================================================
    // C# Preprocessor Directives (context-aware overrides)
    // =========================================================================
    // These override the C# grammar's preproc rules to use an external scanner token
    // that only matches # in C# context, ensuring directives don't match in HTML.
    // The external token matches just #, then we use token.immediate() for the keyword.

    preproc_region: $ => seq(
      $._preproc_directive_start,
      alias(token.immediate(/[ \t]*region/), '#region'),
      optional(field('content', $.preproc_arg)),
      /\n/,
    ),

    preproc_endregion: $ => seq(
      $._preproc_directive_start,
      alias(token.immediate(/[ \t]*endregion/), '#endregion'),
      optional(field('content', $.preproc_arg)),
      /\n/,
    ),

    preproc_line: $ => seq(
      $._preproc_directive_start,
      alias(token.immediate(/[ \t]*line/), '#line'),
      choice(
        'default',
        'hidden',
        seq($.integer_literal, optional($.string_literal)),
        seq(
          '(', $.integer_literal, ',', $.integer_literal, ')',
          '-',
          '(', $.integer_literal, ',', $.integer_literal, ')',
          optional($.integer_literal),
          $.string_literal,
        ),
      ),
      /\n/,
    ),

    preproc_pragma: $ => seq(
      $._preproc_directive_start,
      alias(token.immediate(/[ \t]*pragma/), '#pragma'),
      choice(
        seq('warning',
          choice('disable', 'restore'),
          commaSep(
            choice(
              $.identifier,
              $.integer_literal,
            ))),
        seq('checksum', $.string_literal, $.string_literal, $.string_literal),
      ),
      /\n/,
    ),

    preproc_nullable: $ => seq(
      $._preproc_directive_start,
      alias(token.immediate(/[ \t]*nullable/), '#nullable'),
      choice('enable', 'disable', 'restore'),
      optional(choice('annotations', 'warnings')),
      /\n/,
    ),

    preproc_error: $ => seq(
      $._preproc_directive_start,
      alias(token.immediate(/[ \t]*error/), '#error'),
      $.preproc_arg,
      /\n/,
    ),

    preproc_warning: $ => seq(
      $._preproc_directive_start,
      alias(token.immediate(/[ \t]*warning/), '#warning'),
      $.preproc_arg,
      /\n/,
    ),

    preproc_define: $ => seq(
      $._preproc_directive_start,
      alias(token.immediate(/[ \t]*define/), '#define'),
      $.preproc_arg,
      /\n/,
    ),

    preproc_undef: $ => seq(
      $._preproc_directive_start,
      alias(token.immediate(/[ \t]*undef/), '#undef'),
      $.preproc_arg,
      /\n/,
    ),

    // =========================================================================
    // Razor Directives
    // =========================================================================

    // Razor directives like @page, @model, @using, @inject, etc.
    razor_directive: $ => choice(
      $.razor_page_directive,
      $.razor_model_directive,
      $.razor_using_directive,
      $.razor_inject_directive,
      $.razor_inherits_directive,
      $.razor_namespace_directive,
      $.razor_functions_directive,
      $.razor_code_directive,
      $.razor_section_directive,
      $.razor_layout_directive,
      $.razor_attribute_directive,
      $.razor_implements_directive,
      $.razor_typeparam_directive,
      $.razor_preservewhitespace_directive,
      $.razor_rendermode_directive,
      $.razor_addtaghelper_directive,
      $.razor_removetaghelper_directive,
      $.razor_taghelperprefix_directive,
    ),

    // @page takes a route pattern as a string literal
    // Use prec.right to prefer continuing with string_literal over reducing early
    razor_page_directive: $ => prec.right(seq('@page', optional($.string_literal))),

    razor_model_directive: $ => seq('@model', $.type),

    // @using directive (imports namespace or creates alias)
    // The statement form is @using (...) which starts with ( after '@using'
    // The directive form is @using Namespace or @using Alias = Type
    // Uses same high-precedence token as statement to ensure consistent lexing
    razor_using_directive: $ => prec(1, seq(
      token(prec(100, '@using')),
      choice(
        // Static import: @using static Type
        seq('static', $._name),
        // Qualified namespace: @using Namespace.SubNamespace
        $.qualified_name,
        // Alias form: @using Alias = Type (higher dynamic precedence wins when = follows)
        $._using_alias,
        // Simple namespace: @using Identifier
        $._using_simple,
      ),
    )),

    // Helper rules for using directive
    _using_alias: $ => seq(
      field('alias', $.identifier),
      '=',
      field('type', $.type),
    ),

    // Simple form - uses external scanner to ensure NOT followed by = or .
    _using_simple: $ => seq($.identifier, $._using_not_alias),

    razor_inject_directive: $ => seq('@inject', field('type', $.type), field('name', $.identifier)),

    razor_inherits_directive: $ => seq('@inherits', $.type),

    razor_namespace_directive: $ => seq('@namespace', $._name),

    // Override declaration_list to use context-tracking tokens
    // This is used by @functions and @code directives
    declaration_list: $ => seq(
      alias($._razor_block_open, '{'),
      repeat($.declaration),
      alias($._csharp_context_close, '}'),
    ),

    razor_functions_directive: $ => seq('@functions', $.declaration_list),

    // @code is the Blazor equivalent of @functions
    razor_code_directive: $ => seq('@code', $.declaration_list),

    razor_section_directive: $ => seq(
      '@section',
      $.identifier,
      '{',
      repeat($._node),
      '}',
    ),

    razor_layout_directive: $ => seq('@layout', $.type),

    razor_attribute_directive: $ => seq('@attribute', $.attribute_list),

    razor_implements_directive: $ => seq('@implements', $.type),

    razor_typeparam_directive: $ => seq('@typeparam', $.identifier, optional($.type_parameter_constraints_clause)),

    razor_preservewhitespace_directive: $ => seq('@preservewhitespace', $.boolean_literal),

    razor_rendermode_directive: $ => seq('@rendermode', $.identifier),

    // @addTagHelper typePattern, assemblyName
    // e.g., @addTagHelper *, Microsoft.AspNetCore.Mvc.TagHelpers
    razor_addtaghelper_directive: $ => seq(
      '@addTagHelper',
      field('type_pattern', $.tag_helper_type_pattern),
      ',',
      field('assembly', $._name),
    ),

    // @removeTagHelper typePattern, assemblyName
    razor_removetaghelper_directive: $ => seq(
      '@removeTagHelper',
      field('type_pattern', $.tag_helper_type_pattern),
      ',',
      field('assembly', $._name),
    ),

    // @tagHelperPrefix prefix
    // e.g., @tagHelperPrefix th:
    razor_taghelperprefix_directive: $ => seq(
      '@tagHelperPrefix',
      field('prefix', $.tag_helper_prefix),
    ),

    // Type pattern for tag helpers: either "*" for all or a fully qualified type name
    tag_helper_type_pattern: $ => choice(
      '*',
      $._name,
    ),

    // Prefix for tag helpers (e.g., "th:")
    tag_helper_prefix: _ => /[a-zA-Z][a-zA-Z0-9]*:/,

    // =========================================================================
    // Razor Code Blocks
    // =========================================================================

    // @{ ... } code blocks
    // Uses external scanner to track entering/exiting C# context
    razor_code_block: $ => seq(
      alias($._csharp_code_block_start, '@{'),
      repeat($._razor_block_content),
      alias($._csharp_context_close, '}'),
    ),

    // =========================================================================
    // Razor Expressions
    // =========================================================================

    // Explicit expression: @(expression)
    // Uses external scanner to track entering/exiting C# context
    razor_explicit_expression: $ => seq(
      alias($._csharp_explicit_expr_start, '@('),
      $.expression,
      alias($._csharp_context_close, ')'),
    ),

    // Implicit expression: @identifier, @identifier.property, @identifier.Method()
    // The _implicit_expr_end token is zero-width and matches when whitespace or
    // a non-continuation character follows, forcing the expression to end
    razor_implicit_expression: $ => seq(
      '@',
      $._razor_implicit_expr_chain,
      $._implicit_expr_end,
    ),

    // Chain of member access, method calls, and indexers starting from identifier
    _razor_implicit_expr_chain: $ => choice(
      $.await_expression,
      $._razor_access_chain,
    ),

    // Matches: identifier, identifier.member, identifier.method(), identifier[index], identifier?.member
    // Also matches literal keywords: true, false, null
    // Note: Generics are NOT allowed in implicit expressions - <> is interpreted as HTML
    // Use @(GenericMethod<int>()) for generics
    //
    // Produces proper C#-style expression nodes:
    // - member_access_expression for .member
    // - invocation_expression for method calls
    // - element_access_expression for indexers
    // - conditional_access_expression for ?.member
    _razor_access_chain: $ => choice(
      $._razor_primary_expression,
      // These literals are unambiguous
      $.boolean_literal,
      $.null_literal,
    ),

    // Primary expression that can have member access, invocation, or indexing applied
    _razor_primary_expression: $ => choice(
      $.identifier,
      alias($._razor_member_access, $.member_access_expression),
      alias($._razor_invocation, $.invocation_expression),
      alias($._razor_element_access, $.element_access_expression),
      alias($._razor_conditional_access, $.conditional_access_expression),
    ),

    // Member access: expr.identifier
    // Uses token.immediate() to prevent whitespace before dot and identifier
    // The _implicit_expr_end scanner detects when . isn't followed by identifier
    _razor_member_access: $ => prec.left(seq(
      field('expression', $._razor_primary_expression),
      token.immediate('.'),
      field('name', alias(token.immediate(/[_\p{L}][_\p{L}\p{N}]*/u), $.identifier)),
    )),

    // Invocation: expr(args) or expr.method(args)
    _razor_invocation: $ => prec.left(seq(
      field('function', $._razor_primary_expression),
      field('arguments', $.argument_list),
    )),

    // Element access: expr[index]
    _razor_element_access: $ => prec.left(seq(
      field('expression', $._razor_primary_expression),
      '[',
      field('subscript', $.expression),
      ']',
    )),

    // Conditional access: expr?.identifier
    // Uses token.immediate() to prevent whitespace before ?. and identifier
    _razor_conditional_access: $ => prec.left(seq(
      field('expression', $._razor_primary_expression),
      token.immediate('?.'),
      field('name', alias(token.immediate(/[_\p{L}][_\p{L}\p{N}]*/u), $.identifier)),
    )),

    // =========================================================================
    // Templated Razor Delegate (@<element>...</element>)
    // =========================================================================

    // Allow @<element> as a C# expression that produces a templated Razor delegate
    // This is used for passing HTML content to methods or assigning to delegate variables
    // Example: RenderFragment template = @<p>Hello @item</p>;
    razor_fragment: $ => seq(
      '@',
      choice($.element, $.self_closing_element),
    ),

    // Extend expression to include razor_fragment
    non_lvalue_expression: ($, original) => choice(
      original,
      $.razor_fragment,
    ),

    // Override block to allow HTML elements inside nested C# blocks
    // This is needed for HTML inside local functions, lambdas, etc.
    // Uses context-tracking tokens to properly track C# vs HTML mode
    block: $ => seq(
      alias($._razor_block_open, '{'),
      repeat(choice(
        $.statement,
        $.element,
        $.self_closing_element,
        $.void_element,
        $.razor_text_literal,
      )),
      alias($._csharp_context_close, '}'),
    ),

    // =========================================================================
    // Text Content
    // =========================================================================

    // Top-level text that can appear between Razor statements
    // Uses external scanner to stop before else/catch/finally keywords
    _top_level_text: $ => choice(
      // Text containing literal @ (email addresses like user@example.com)
      prec(1, alias($._text_with_literal_at, $.text)),
      // Main text content - uses external scanner to be keyword-aware
      alias($._html_text_content, $.text),
      // Fallback for text that doesn't start with keyword characters
      // Excludes: e/c/f (keywords), < (HTML), @ (Razor), [ ( (expression), \n, " ' (strings), whitespace, . (C# qualified names)
      alias(prec(-100, /[^ecf.<@\[(\n"'\s][^.<@\[(\n"']*/), $.text),
      // Single non-keyword-starting character (also excludes quotes, whitespace, dot)
      alias(prec(-101, /[^ecf.<@\[(\n"'\s]/), $.text),
      // Punctuation [ and ( that could be expression continuations
      alias(prec(-102, /[\[(]/), $.text),
      // Dot as text - lowest precedence, only matches when nothing else can
      // This handles cases like "@foo .bar" where the dot is text after an expression
      alias(prec(-200, /\./), $.text),
    ),

    // Text inside elements - doesn't need to stop at keywords
    // Excludes [ ( which are handled separately for implicit expression chaining
    // Note: . is allowed since implicit expressions use token.immediate('.')
    text: $ => choice(
      // Text containing literal @ (email addresses like user@example.com)
      prec(1, alias($._text_with_literal_at, $.text)),
      // Regular text content - includes . since implicit expressions handle it
      prec(-10, /[^<@\[(]+/),
      // Punctuation that could be expression continuations, but parsed as text
      // when not following an identifier (lower precedence than implicit expressions)
      prec(-20, /[\[(]/),
    ),

  },
});

/**
 * Creates a rule to match zero or more comma-separated items
 * @param {Rule} rule
 * @returns {ChoiceRule}
 */
function commaSep(rule) {
  return optional(commaSep1(rule));
}

/**
 * Creates a rule to match one or more comma-separated items
 * @param {Rule} rule
 * @returns {SeqRule}
 */
function commaSep1(rule) {
  return seq(rule, repeat(seq(',', rule)));
}
