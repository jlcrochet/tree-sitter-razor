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

  supertypes: ($, original) => original.concat([
    $.razor_directive,
  ]),

  extras: ($, original) => original.concat([
    $._csharp_razor_comment,
  ]),

  // GLR conflict: After @if(...){}, the parser can't tell if a following razor_comment
  // is element content or part of a potential else clause. GLR explores both paths.
  conflicts: ($, original) => original.concat([
    [$._razor_if_statement_body],
  ]),

  externals: ($, original) => original.concat([
    $._text_with_literal_at,
    $._html_text_content,
    $._csharp_code_block_start,
    $._csharp_explicit_expr_start,
    $._razor_block_open,
    $._csharp_context_close,
    $._csharp_comment,
    $._script_content,
    $._style_content,
    $._title_content,
    $._textarea_content,
    $._implicit_expr_end,
    $._razor_block_at,
    $._using_not_alias,
    $._razor_comment_start,
    $._razor_comment,
    $.razor_comment_extra,
    $._html_tag_open,
    $._html_end_tag_open,
    $._html_tag_close,
    $._html_comment,
    $._doctype,
    $._implicit_paren_open,
    $._implicit_bracket_open,
    $._implicit_conditional_bracket_open,
    $._text_literal_content,
    $._top_level_csharp_comment,
    $._full_character_reference,
    $._short_character_reference,
    $._invalid_character_reference,
  ]),

  // @ts-ignore
  rules: {
    compilation_unit: $ => repeat($._node),

    _node: $ => choice(
      $.doctype,
      alias($._script_element, $.element),
      alias($._style_element, $.element),
      alias($._title_element, $.element),
      alias($._textarea_element, $.element),
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
      // html_character_reference is handled via _top_level_text_fragment
      $._top_level_text,
    ),

    doctype: $ => $._doctype,
    escaped_at: _ => '@@',

    // HTML character references (entities)
    // Full: &amp; &lt; &gt; &#123; &#x7B; etc. (requires semicolon)
    // Short: &amp &lt &gt etc. (legacy, no semicolon required, NOT allowed in attributes)
    html_character_reference: $ => choice(
      $._full_character_reference,
      $._short_character_reference,
    ),

    // Invalid character reference: pattern matches but name not in spec (e.g., &invalid;)
    // Exposed as named node for distinct highlighting (e.g., warning color)
    invalid_character_reference: $ => $._invalid_character_reference,

    // =========================================================================
    // Razor Statements (@if, @foreach, etc.)
    // =========================================================================

    razor_statement: $ => choice(
      alias($._razor_if_statement, $.if_statement),
      alias($._razor_for_statement, $.for_statement),
      alias($._razor_foreach_statement, $.foreach_statement),
      alias($._razor_await_foreach_statement, $.foreach_statement),
      alias($._razor_while_statement, $.while_statement),
      alias($._razor_do_statement, $.do_statement),
      alias($._razor_switch_statement, $.switch_statement),
      alias($._razor_try_statement, $.try_statement),
      alias($._razor_lock_statement, $.lock_statement),
      alias($._razor_using_statement, $.using_statement),
    ),

    razor_block: $ => seq(
      alias($._razor_block_open, '{'),
      repeat($._razor_block_content),
      alias($._csharp_context_close, '}'),
    ),

    // Razor-specific @ constructs must come BEFORE _razor_statement to ensure
    // the external scanner matches @-prefixed content before C#'s verbatim identifier
    _razor_block_content: $ => choice(
      alias($._nested_razor_statement, $.razor_statement),
      alias($._nested_razor_explicit_expression, $.razor_explicit_expression),
      alias($._nested_razor_implicit_expression, $.razor_implicit_expression),
      $.element,
      $.self_closing_element,
      $.void_element,
      $.razor_text_literal,
      alias($._razor_block_comment, $.razor_comment),
      $._razor_statement,
    ),

    _nested_razor_statement: $ => choice(
      alias($._nested_if_statement, $.if_statement),
      alias($._nested_for_statement, $.for_statement),
      alias($._nested_foreach_statement, $.foreach_statement),
      alias($._nested_await_foreach_statement, $.foreach_statement),
      alias($._nested_while_statement, $.while_statement),
      alias($._nested_do_statement, $.do_statement),
      alias($._nested_switch_statement, $.switch_statement),
      alias($._nested_try_statement, $.try_statement),
      alias($._nested_lock_statement, $.lock_statement),
      alias($._nested_using_statement, $.using_statement),
    ),

    _nested_at: $ => alias($._razor_block_at, '@'),

    _nested_if_statement: $ => seq($._nested_at, token.immediate('if'), $._razor_if_statement_body),
    _nested_for_statement: $ => seq($._nested_at, token.immediate('for'), $._razor_for_statement_body),
    _nested_foreach_statement: $ => seq($._nested_at, token.immediate('foreach'), $._razor_foreach_statement_body),
    _nested_await_foreach_statement: $ => seq($._nested_at, token.immediate('await'), 'foreach', $._razor_foreach_statement_body),
    _nested_while_statement: $ => seq($._nested_at, token.immediate('while'), $._razor_while_statement_body),
    _nested_do_statement: $ => seq($._nested_at, token.immediate('do'), $._razor_do_statement_body),
    _nested_switch_statement: $ => seq($._nested_at, token.immediate('switch'), $._razor_switch_statement_body),
    _nested_try_statement: $ => seq($._nested_at, token.immediate('try'), $._razor_try_statement_body),
    _nested_lock_statement: $ => seq($._nested_at, token.immediate('lock'), $._razor_lock_statement_body),
    _nested_using_statement: $ => seq($._nested_at, token.immediate('using'), $._razor_using_statement_body),

    _nested_razor_explicit_expression: $ => seq(
      $._nested_at,
      token.immediate('('),
      $.expression,
      ')',
    ),

    _nested_razor_implicit_expression: $ => seq(
      $._nested_at,
      $._razor_implicit_expr_chain,
      $._implicit_expr_end,
    ),

    _razor_statement: $ => choice(
      $.statement,
      $.extern_alias_directive,
      $.using_directive,
      $.file_scoped_namespace_declaration,
      $.global_attribute,
      // Fallback for unknown preprocessor directives (uses low-precedence override)
      $.preproc_directive,
    ),

    // Override preproc_directive to use very low precedence
    // This allows specific preprocessor rules (preproc_region, etc.) from C#'s extras to match first
    preproc_directive: _ => token(prec(-100, /#[ \t]*[a-zA-Z0-9]\w*/)),

    // @: for single-line text literals inside code blocks
    // Tree-sitter injection parses Razor/HTML content within (see queries/injections.scm)
    razor_text_literal: $ => seq(
      '@:',
      optional(alias($._text_literal_content, $.text_literal_content)),
    ),

    // -------------------------------------------------------------------------
    // Razor if statement
    // -------------------------------------------------------------------------

    _razor_if_statement: $ => seq('@', token.immediate('if'), $._razor_if_statement_body),

    _razor_if_statement_body: $ => seq(
      alias($._implicit_paren_open, '('),
      field('condition', $.expression),
      alias($._csharp_context_close, ')'),
      field('consequence', $.razor_block),
      optional(field('alternative', choice(
        $.razor_else_if_clause,
        alias($.razor_else_clause, $.else_clause),
      ))),
    ),

    razor_else_if_clause: $ => prec.right(seq(
      repeat($._razor_clause_comment),
      'else',
      'if',
      alias($._implicit_paren_open, '('),
      field('condition', $.expression),
      alias($._csharp_context_close, ')'),
      field('consequence', $.razor_block),
      optional(field('alternative', choice(
        $.razor_else_if_clause,
        alias($.razor_else_clause, $.else_clause),
      ))),
    )),

    razor_else_clause: $ => seq(
      repeat($._razor_clause_comment),
      'else',
      $.razor_block,
    ),

    // Comments allowed between control flow clauses (e.g., } // comment \n else)
    // The enclosing clause rules use prec.right to prefer matching else over comments
    _razor_clause_comment: $ => choice(
      $.razor_comment,
      alias($._top_level_csharp_comment, $.comment),
    ),

    // -------------------------------------------------------------------------
    // Razor for statement
    // -------------------------------------------------------------------------

    _razor_for_statement: $ => seq('@', token.immediate('for'), $._razor_for_statement_body),
    _razor_for_statement_body: $ => seq($._for_statement_conditions, field('body', $.razor_block)),

    // -------------------------------------------------------------------------
    // Razor foreach statement
    // -------------------------------------------------------------------------

    _razor_foreach_statement: $ => seq('@', token.immediate('foreach'), $._razor_foreach_statement_body),
    _razor_await_foreach_statement: $ => seq('@', token.immediate('await'), 'foreach', $._razor_foreach_statement_body),
    _razor_foreach_statement_body: $ => seq(
      alias($._implicit_paren_open, '('),
      choice(
        seq(
          field('type', $.type),
          field('left', choice($.identifier, $.tuple_pattern)),
        ),
        field('left', $.expression),
      ),
      'in',
      field('right', $.expression),
      alias($._csharp_context_close, ')'),
      field('body', $.razor_block),
    ),

    // -------------------------------------------------------------------------
    // Razor while statement
    // -------------------------------------------------------------------------

    _razor_while_statement: $ => seq('@', token.immediate('while'), $._razor_while_statement_body),
    _razor_while_statement_body: $ => seq(
      alias($._implicit_paren_open, '('),
      field('condition', $.expression),
      alias($._csharp_context_close, ')'),
      field('body', $.razor_block),
    ),

    // -------------------------------------------------------------------------
    // Razor do statement
    // -------------------------------------------------------------------------

    _razor_do_statement: $ => seq('@', token.immediate('do'), $._razor_do_statement_body),
    _razor_do_statement_body: $ => seq(
      field('body', $.razor_block),
      'while',
      alias($._implicit_paren_open, '('),
      field('condition', $.expression),
      alias($._csharp_context_close, ')'),
      ';',
    ),

    // -------------------------------------------------------------------------
    // Razor switch statement
    // -------------------------------------------------------------------------

    _razor_switch_statement: $ => seq('@', token.immediate('switch'), $._razor_switch_statement_body),
    _razor_switch_statement_body: $ => seq(
      alias($._implicit_paren_open, '('),
      field('value', $.expression),
      alias($._csharp_context_close, ')'),
      field('body', alias($.razor_switch_body, $.switch_body)),
    ),

    razor_switch_body: $ => seq(
      alias($._razor_block_open, '{'),
      repeat($.razor_switch_section),
      alias($._csharp_context_close, '}'),
    ),

    razor_switch_section: $ => prec.left(seq(
      choice(
        seq('case', choice($.expression, seq($.pattern, optional($.when_clause)))),
        'default',
      ),
      ':',
      repeat($._razor_switch_section_content),
    )),

    _razor_switch_section_content: $ => choice(
      alias($._nested_razor_statement, $.razor_statement),
      alias($._nested_razor_explicit_expression, $.razor_explicit_expression),
      alias($._nested_razor_implicit_expression, $.razor_implicit_expression),
      $.break_statement,
      $.element,
      $.self_closing_element,
      $.void_element,
      $.razor_text_literal,
      alias($._razor_block_comment, $.razor_comment),
      $._razor_statement,
    ),

    // -------------------------------------------------------------------------
    // Razor try statement
    // -------------------------------------------------------------------------

    _razor_try_statement: $ => seq('@', token.immediate('try'), $._razor_try_statement_body),
    _razor_try_statement_body: $ => prec.right(seq(
      field('body', $.razor_block),
      repeat(alias($.razor_catch_clause, $.catch_clause)),
      optional(alias($.razor_finally_clause, $.finally_clause)),
    )),

    razor_catch_clause: $ => seq(
      repeat($._razor_clause_comment),
      'catch',
      repeat(choice($.catch_declaration, $.catch_filter_clause)),
      field('body', $.razor_block),
    ),

    razor_finally_clause: $ => seq(
      repeat($._razor_clause_comment),
      'finally',
      $.razor_block,
    ),

    // -------------------------------------------------------------------------
    // Razor lock statement
    // -------------------------------------------------------------------------

    _razor_lock_statement: $ => seq('@', token.immediate('lock'), $._razor_lock_statement_body),
    _razor_lock_statement_body: $ => seq(
      alias($._implicit_paren_open, '('),
      $.expression,
      alias($._csharp_context_close, ')'),
      $.razor_block,
    ),

    // -------------------------------------------------------------------------
    // Razor using statement (not directive)
    // -------------------------------------------------------------------------

    _razor_using_statement: $ => seq('@', token.immediate('using'), $._razor_using_statement_body),
    _razor_using_statement_body: $ => seq(
      alias($._implicit_paren_open, '('),
      choice(alias($.using_variable_declaration, $.variable_declaration), $.expression),
      alias($._csharp_context_close, ')'),
      field('body', $.razor_block),
    ),

    // =========================================================================
    // HTML Elements
    // =========================================================================

    element: $ => seq($.start_tag, repeat($._element_content), $.end_tag),

    // Void elements cannot have content or end tags
    void_element: $ => seq(
      alias($._html_tag_open, '<'),
      alias($._void_tag_name, $.tag_name),
      repeat($._html_attribute),
      optional('/'),
      alias($._html_tag_close, '>'),
    ),

    // Case-insensitive void element names
    _void_tag_name: _ => token.immediate(choice(
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

    // Content inside elements
    // razor_comment uses high precedence to win over _razor_clause_comment in else clauses
    // (handled via GLR conflict in _razor_if_statement_body)
    _element_content: $ => choice(
      alias($._script_element, $.element),
      alias($._style_element, $.element),
      alias($._title_element, $.element),
      alias($._textarea_element, $.element),
      $.element,
      $.self_closing_element,
      $.void_element,
      $.html_comment,
      prec(300, $.razor_comment),
      $.razor_directive,
      $.razor_code_block,
      $.razor_statement,
      $.razor_explicit_expression,
      $.razor_implicit_expression,
      $.escaped_at,
      $.html_character_reference,
      $.invalid_character_reference,
      $.text,
    ),

    // Note: Tag helper opt-out (!) is handled by the external scanner as part of
    // _html_tag_open and _html_end_tag_open (they match <, <!, </, and </!)
    self_closing_element: $ => seq(
      alias($._html_tag_open, '<'),
      alias($._immediate_tag_name, $.tag_name),
      repeat($._html_attribute),
      '/',
      alias($._html_tag_close, '>'),
    ),

    start_tag: $ => seq(
      alias($._html_tag_open, '<'),
      alias($._immediate_tag_name, $.tag_name),
      repeat($._html_attribute),
      alias($._html_tag_close, '>'),
    ),

    end_tag: $ => seq(
      alias($._html_end_tag_open, '</'),
      alias($._immediate_tag_name, $.tag_name),
      alias($._html_tag_close, '>'),
    ),

    // Element name that must immediately follow < or </ (or ! if opt-out)
    _immediate_tag_name: _ => token.immediate(/[a-zA-Z][a-zA-Z0-9.:-]*/),

    // =========================================================================
    // Script and Style Elements
    // =========================================================================

    // Script elements contain raw JavaScript content that shouldn't be parsed as HTML
    // Higher precedence than regular elements to ensure <script> is matched first
    // Aliased to `element` so queries can match all elements uniformly
    _script_element: $ => prec(1, seq(
      alias($._script_start_tag, $.start_tag),
      optional(alias($._script_content, $.raw_text)),
      alias($._script_end_tag, $.end_tag),
    )),

    _script_start_tag: $ => seq(
      alias($._html_tag_open, '<'),
      alias(token.immediate(prec(1, /script/i)), $.tag_name),
      repeat($._html_attribute),
      alias($._html_tag_close, '>'),
    ),

    _script_end_tag: $ => seq(
      alias($._html_end_tag_open, '</'),
      alias(token.immediate(prec(1, /script/i)), $.tag_name),
      alias($._html_tag_close, '>'),
    ),

    // Style elements contain raw CSS content that shouldn't be parsed as HTML
    // Higher precedence than regular elements to ensure <style> is matched first
    // Aliased to `element` so queries can match all elements uniformly
    _style_element: $ => prec(1, seq(
      alias($._style_start_tag, $.start_tag),
      optional(alias($._style_content, $.raw_text)),
      alias($._style_end_tag, $.end_tag),
    )),

    _style_start_tag: $ => seq(
      alias($._html_tag_open, '<'),
      alias(token.immediate(prec(1, /style/i)), $.tag_name),
      repeat($._html_attribute),
      alias($._html_tag_close, '>'),
    ),

    _style_end_tag: $ => seq(
      alias($._html_end_tag_open, '</'),
      alias(token.immediate(prec(1, /style/i)), $.tag_name),
      alias($._html_tag_close, '>'),
    ),

    // Title elements contain escapable raw text (can have character references and Razor)
    // Higher precedence than regular elements to ensure <title> is matched first
    // Aliased to `element` so queries can match all elements uniformly
    _title_element: $ => prec(1, seq(
      alias($._title_start_tag, $.start_tag),
      repeat($._title_text_fragment),
      alias($._title_end_tag, $.end_tag),
    )),

    _title_start_tag: $ => seq(
      alias($._html_tag_open, '<'),
      alias(token.immediate(prec(1, /title/i)), $.tag_name),
      repeat($._html_attribute),
      alias($._html_tag_close, '>'),
    ),

    _title_end_tag: $ => seq(
      alias($._html_end_tag_open, '</'),
      alias(token.immediate(prec(1, /title/i)), $.tag_name),
      alias($._html_tag_close, '>'),
    ),

    // Content fragments for title elements
    _title_text_fragment: $ => choice(
      alias($._title_content, $.escapable_raw_text),
      $._escapable_raw_text_content,
    ),

    // Textarea elements contain escapable raw text (can have character references and Razor)
    // Higher precedence than regular elements to ensure <textarea> is matched first
    // Aliased to `element` so queries can match all elements uniformly
    _textarea_element: $ => prec(1, seq(
      alias($._textarea_start_tag, $.start_tag),
      repeat($._textarea_text_fragment),
      alias($._textarea_end_tag, $.end_tag),
    )),

    _textarea_start_tag: $ => seq(
      alias($._html_tag_open, '<'),
      alias(token.immediate(prec(1, /textarea/i)), $.tag_name),
      repeat($._html_attribute),
      alias($._html_tag_close, '>'),
    ),

    _textarea_end_tag: $ => seq(
      alias($._html_end_tag_open, '</'),
      alias(token.immediate(prec(1, /textarea/i)), $.tag_name),
      alias($._html_tag_close, '>'),
    ),

    // Content fragments for textarea elements
    _textarea_text_fragment: $ => choice(
      alias($._textarea_content, $.escapable_raw_text),
      $._escapable_raw_text_content,
    ),

    // Shared content for escapable raw text elements (title, textarea)
    // Excludes the element-specific raw text token which must be aliased separately
    _escapable_raw_text_content: $ => choice(
      $.html_character_reference,
      $.invalid_character_reference,
      $.razor_explicit_expression,
      $.razor_implicit_expression,
      $.escaped_at,
      '&',  // Bare & that doesn't form a valid character reference
    ),

    // =========================================================================
    // HTML Attributes
    // =========================================================================

    // Note: HTML attribute rules are prefixed with "html_" to avoid collision
    // with C#'s `attribute` rule (used for [Attribute] syntax)

    _html_attribute: $ => choice(
      $.html_attribute,
      $.escaped_attribute,
      $.dynamic_attribute,  // Must come before razor_attribute (both start with @)
      $.razor_attribute,
      $.at_star_attribute,  // @* as attribute name (not Razor comment in tag context)
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

    // HTML attribute name per HTML spec - excludes control characters, space, quotes,
    // slash, equals, and noncharacters. Also excludes @ (handled by razor_attribute)
    // and & (to allow character references in unquoted attribute values).
    // This allows special attribute syntaxes like [item], (click), *directive, #local
    // See: https://html.spec.whatwg.org/multipage/syntax.html#attributes-2
    html_attribute_name: _ => /[^\u0000-\u001F\u007F-\u009F \n\r\t\f"'>/=@&\uFDD0-\uFDEF\uFFFE\uFFFF]+/u,

    html_attribute_value: $ => choice(
      $.html_quoted_attribute_value,
      // Razor expressions as unquoted values: Property=@(s => s.Id) or Items=@Model.Items
      $.razor_explicit_expression,
      $.razor_implicit_expression,
      // Plain unquoted value (no @ or Razor): class=my-class
      $.html_unquoted_attribute_value,
    ),

    html_quoted_attribute_value: $ => choice(
      seq('"', optional($._html_double_quoted_attribute_content), '"'),
      seq("'", optional($._html_single_quoted_attribute_content), "'"),
    ),

    _html_double_quoted_attribute_content: $ => repeat1(choice(
      // Razor expressions - must be first so @ triggers expression parsing
      $.razor_explicit_expression,
      $.razor_implicit_expression,
      // Email/literal @ in attribute values (e.g., mailto:user@example.com)
      $._text_with_literal_at,
      // Escaped @@ in attribute values
      $.escaped_at,
      // @* ... *@ is literal text in attribute values (not a Razor comment)
      $._attr_at_star_literal,
      // Text content - alphanumeric and common text characters including apostrophe
      /[a-zA-Z0-9_'-]+/,
      /\s+/,        // Whitespace as separate token
      // Punctuation and other characters as individual tokens (excludes quote chars and @&)
      /[^"@&\sa-zA-Z0-9_'-]/,
      // Character reference (only full refs allowed in attributes, NOT short refs)
      alias($._full_character_reference, $.html_character_reference),
      $.invalid_character_reference,
      // Bare @ followed by non-identifier char (literal @)
      '@',
      // Bare & that doesn't form a valid character reference
      '&',
    )),

    _html_single_quoted_attribute_content: $ => repeat1(choice(
      // Razor expressions - must be first so @ triggers expression parsing
      $.razor_explicit_expression,
      $.razor_implicit_expression,
      // Email/literal @ in attribute values
      $._text_with_literal_at,
      // Escaped @@ in attribute values
      $.escaped_at,
      // @* ... *@ is literal text in attribute values (not a Razor comment)
      $._attr_at_star_literal,
      // Text content - alphanumeric and common text characters including double quote
      /[a-zA-Z0-9_"-]+/,
      /\s+/,        // Whitespace as separate token
      // Punctuation and other characters as individual tokens (excludes quote chars and @&)
      /[^'@&\sa-zA-Z0-9_"-]/,
      // Character reference (only full refs allowed in attributes, NOT short refs)
      alias($._full_character_reference, $.html_character_reference),
      $.invalid_character_reference,
      // Bare @ followed by non-identifier char (literal @)
      '@',
      // Bare & that doesn't form a valid character reference
      '&',
    )),

    // @* ... *@ as literal text in attribute values
    // Razor comments are not valid inside attribute values, so this is just text
    _attr_at_star_literal: _ => /@\*([^*]|\*[^@])*\*@/,

    // Unquoted attribute value: class=my-class or data-value=foo&amp;bar
    // Per HTML spec, unquoted values can contain character references
    // Only full refs allowed in attributes (NOT short refs without semicolon)
    // Uses token.immediate for continuations to prevent html_attribute_name from matching
    html_unquoted_attribute_value: $ => seq(
      $._unquoted_attribute_value_start,
      repeat(choice(
        $._unquoted_attribute_value_continuation,
        // Character reference (only full refs allowed in attributes, NOT short refs)
        alias($._full_character_reference, $.html_character_reference),
        $.invalid_character_reference,
        // Bare & that doesn't form a valid character reference
        '&',
      )),
    ),

    // First part of unquoted attribute value (excludes &)
    _unquoted_attribute_value_start: _ => /[^\s"'=<>`@&]+/,

    // Continuation of unquoted attribute value (must be immediate, no whitespace)
    _unquoted_attribute_value_continuation: _ => token.immediate(/[^\s"'=<>`@&]+/),

    // Razor-specific attribute (e.g., @onclick, @bind)
    // Uses token.immediate() to prevent whitespace between @ and attribute name
    razor_attribute: $ => choice(
      // With value: @onclick="handler"
      seq(
        '@',
        alias($._immediate_html_attribute_name, $.html_attribute_name),
        '=',
        $.html_attribute_value,
      ),
      // Without value: @rendermode
      seq('@', alias($._immediate_html_attribute_name, $.html_attribute_name)),
    ),

    // Escaped attribute name (@@attr) - produces literal @attr as attribute name
    // e.g., <p @@attr="value" /> or <p @@attr />
    escaped_attribute: $ => choice(
      // With value: @@attr="value"
      seq(
        '@@',
        alias($._immediate_html_attribute_name, $.html_attribute_name),
        '=',
        $.html_attribute_value,
      ),
      // Without value: @@attr
      seq('@@', alias($._immediate_html_attribute_name, $.html_attribute_name)),
    ),

    // Dynamic attribute name using explicit expression
    // e.g., <div @(attributeName)="value" /> - attribute name comes from expression
    // Uses simple @( ... ) without context tracking since we're in HTML attribute position
    dynamic_attribute: $ => choice(
      // With value: @(attrName)="value"
      seq(
        alias($._dynamic_attr_expression, $.razor_explicit_expression),
        '=',
        $.html_attribute_value,
      ),
      // Without value: @(attrName)
      alias($._dynamic_attr_expression, $.razor_explicit_expression),
    ),

    // Explicit expression for dynamic attribute names - doesn't use context tracking
    _dynamic_attr_expression: $ => seq('@(', $.expression, ')'),

    // Attribute name that must immediately follow @ (no whitespace allowed)
    // Must start with letter, underscore, or colon (not with ( which would be dynamic_attribute)
    // This distinguishes @onclick from @(expr)
    _immediate_html_attribute_name: _ => token.immediate(/[a-zA-Z_:][^\u0000-\u001F\u007F-\u009F \n\r\t\f"'>/=@\uFDD0-\uFDEF\uFFFE\uFFFF]*/u),

    // @* as an HTML attribute name (not a Razor comment when in tag context)
    // Per HTML spec, @* is a valid attribute name. In Razor files, this takes
    // lower precedence than Razor comments in content positions, but inside
    // HTML tags it should be treated as an attribute name.
    // e.g., <div @* foo=bar *@> outputs the same in HTML
    at_star_attribute: $ => choice(
      // With value: @*="value"
      seq(
        alias($._at_star_attr_name, $.html_attribute_name),
        '=',
        $.html_attribute_value,
      ),
      // Without value: @* or *@
      alias($._at_star_attr_name, $.html_attribute_name),
    ),

    // Pattern for @* and *@ as attribute names (must be single token)
    _at_star_attr_name: _ => token(choice('@*', '*@')),

    // =========================================================================
    // Comments
    // =========================================================================

    // Razor comment @* ... *@ - uses external scanner for context-awareness
    // Only matches when NOT inside an HTML tag (where @* is a valid attribute name)
    razor_comment: $ => $._razor_comment,

    // Razor comment inside C# blocks - uses external scanner for @* to beat C# @identifier
    _razor_block_comment: $ => seq(
      alias($._razor_comment_start, '@*'),
      token.immediate(seq(
        /([^*]|\*[^@])*/,
        '*@',
      )),
    ),

    // Wrapper rule for Razor comment extra - aliases to razor_comment for query consistency
    _csharp_razor_comment: $ => alias($.razor_comment_extra, $.razor_comment),

    // HTML comment <!-- ... --> - uses external scanner to avoid conflicts with tag tracking
    html_comment: $ => $._html_comment,

    // Override C# comment to use external scanner for context-awareness
    // This ensures comments only match in C# context, not HTML
    comment: $ => $._csharp_comment,

    // =========================================================================
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
    razor_page_directive: $ => prec.right(seq('@', token.immediate('page'), optional($.string_literal))),

    razor_model_directive: $ => seq('@', token.immediate('model'), $.type),

    // @using directive (imports namespace or creates alias)
    // The statement form is @using (...) which starts with ( after '@using'
    // The directive form is @using Namespace or @using Alias = Type
    razor_using_directive: $ => prec(1, seq(
      '@',
      token.immediate('using'),
      choice(
        // Static import: @using static Type
        seq('static', $._name),
        // Qualified namespace: @using Namespace.SubNamespace
        // Uses _namespace_name to avoid matching generic types (which would consume <tag> as type args)
        alias($._namespace_name, $.qualified_name),
        // Alias form: @using Alias = Type (higher dynamic precedence wins when = follows)
        $._using_alias,
        // Simple namespace: @using Identifier
        $._using_simple,
      ),
    )),

    // Namespace name - dotted identifiers only, no generics
    // This prevents @using Foo.Bar<div> from parsing <div> as a type argument
    _namespace_name: $ => prec.left(seq(
      $.identifier,
      repeat1(seq(token.immediate('.'), $.identifier)),
    )),

    // Helper rules for using directive
    _using_alias: $ => seq(
      field('alias', $.identifier),
      '=',
      field('type', $.type),
    ),

    // Simple form - uses external scanner to ensure NOT followed by = or .
    _using_simple: $ => seq($.identifier, $._using_not_alias),

    razor_inject_directive: $ => seq('@', token.immediate('inject'), field('type', $.type), field('name', $.identifier)),

    razor_inherits_directive: $ => seq('@', token.immediate('inherits'), $.type),

    razor_namespace_directive: $ => seq('@', token.immediate('namespace'), $._name),

    // Override declaration_list to use context-tracking tokens
    // This is used by @functions and @code directives
    // Allows Razor comments alongside C# declarations
    declaration_list: $ => seq(
      alias($._razor_block_open, '{'),
      repeat(choice(
        $.declaration,
        alias($._razor_block_comment, $.razor_comment),
      )),
      alias($._csharp_context_close, '}'),
    ),

    razor_functions_directive: $ => seq('@', token.immediate('functions'), $.declaration_list),

    // @code is the Blazor equivalent of @functions
    razor_code_directive: $ => seq('@', token.immediate('code'), $.declaration_list),

    razor_section_directive: $ => seq(
      '@',
      token.immediate('section'),
      $.identifier,
      '{',
      repeat($._node),
      '}',
    ),

    razor_layout_directive: $ => seq('@', token.immediate('layout'), $.type),

    razor_attribute_directive: $ => seq('@', token.immediate('attribute'), $.attribute_list),

    razor_implements_directive: $ => seq('@', token.immediate('implements'), $.type),

    // @typeparam TItem where TItem : class
    // Two variants: with constraint (higher precedence) and without
    // This allows the parser to prefer matching the constraint over leaving it as text
    razor_typeparam_directive: $ => choice(
      // With constraint - higher precedence to prefer this over the version without
      prec(10, seq(
        '@',
        token.immediate('typeparam'),
        field('name', $.identifier),
        field('constraint', alias($._razor_typeparam_constraint, $.type_parameter_constraints_clause)),
      )),
      // Without constraint
      seq('@', token.immediate('typeparam'), field('name', $.identifier)),
    ),

    // Type parameter constraint clause for @typeparam directive
    // Uses token('where') with precedence to compete with text matching
    _razor_typeparam_constraint: $ => seq(
      token(prec(10, 'where')),
      $.identifier,
      ':',
      commaSep1($.type_parameter_constraint),
    ),

    razor_preservewhitespace_directive: $ => seq('@', token.immediate('preservewhitespace'), $.boolean_literal),

    razor_rendermode_directive: $ => seq('@', token.immediate('rendermode'), $.identifier),

    // @addTagHelper typePattern, assemblyName
    // e.g., @addTagHelper *, Microsoft.AspNetCore.Mvc.TagHelpers
    razor_addtaghelper_directive: $ => seq(
      '@',
      token.immediate('addTagHelper'),
      field('type_pattern', $.tag_helper_type_pattern),
      ',',
      field('assembly', $._name),
    ),

    // @removeTagHelper typePattern, assemblyName
    razor_removetaghelper_directive: $ => seq(
      '@',
      token.immediate('removeTagHelper'),
      field('type_pattern', $.tag_helper_type_pattern),
      ',',
      field('assembly', $._name),
    ),

    // @tagHelperPrefix prefix
    // e.g., @tagHelperPrefix th:
    razor_taghelperprefix_directive: $ => seq(
      '@',
      token.immediate('tagHelperPrefix'),
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
      alias($._razor_await_expression, $.await_expression),
      $._razor_access_chain,
    ),

    // Razor-specific await expression for implicit expressions
    // Uses token.immediate to prevent whitespace between @ and await
    _razor_await_expression: $ => prec.right(seq(
      token.immediate('await'),
      $._razor_access_chain,
    )),

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
    // Primary expression that can have member access, invocation, or indexing applied
    // Also includes literal keywords (true, false, null) which are unambiguous
    _razor_access_chain: $ => choice(
      $.identifier,
      $.boolean_literal,
      $.null_literal,
      alias($._razor_member_access, $.member_access_expression),
      alias($._razor_invocation, $.invocation_expression),
      alias($._razor_element_access, $.element_access_expression),
      alias($._razor_conditional_access, $.conditional_access_expression),
      alias($._razor_conditional_element_access, $.conditional_access_expression),
      alias($._razor_suppression_expression, $.postfix_unary_expression),
    ),

    // Immediate identifier for member access chains (no whitespace allowed before it)
    _immediate_identifier: $ => alias(token.immediate(/[_\p{L}][_\p{L}\p{N}]*/u), $.identifier),

    // Member access: expr.identifier
    // Uses token.immediate() to prevent whitespace before dot and identifier
    // The _implicit_expr_end scanner detects when . isn't followed by identifier
    _razor_member_access: $ => prec.left(seq(
      field('expression', $._razor_access_chain),
      token.immediate('.'),
      field('name', $._immediate_identifier),
    )),

    // Invocation: expr(args) or expr.method(args)
    // Uses context-aware ( to push C# context for Razor comments inside arguments
    _razor_invocation: $ => prec.left(seq(
      field('function', $._razor_access_chain),
      field('arguments', alias($._razor_argument_list, $.argument_list)),
    )),

    // Context-aware argument list that pushes C# context
    _razor_argument_list: $ => seq(
      alias($._implicit_paren_open, '('),
      commaSep($.argument),
      alias($._csharp_context_close, ')'),
    ),

    // Element access: expr[index]
    // Uses context-aware [ to push C# context for Razor comments inside subscript
    _razor_element_access: $ => prec.left(seq(
      field('expression', $._razor_access_chain),
      alias($._implicit_bracket_open, '['),
      field('subscript', $.expression),
      alias($._csharp_context_close, ']'),
    )),

    // Conditional access: expr?.identifier
    // Uses token.immediate() to prevent whitespace before ?. and identifier
    _razor_conditional_access: $ => prec.left(seq(
      field('expression', $._razor_access_chain),
      token.immediate('?.'),
      field('name', $._immediate_identifier),
    )),

    // Conditional element access: expr?[index]
    // Uses token.immediate() to prevent whitespace before ?[
    // Uses context-aware bracket close to support Razor comments inside subscript
    // Produces conditional_access_expression with element_binding_expression
    _razor_conditional_element_access: $ => prec.left(seq(
      field('expression', $._razor_access_chain),
      $._implicit_conditional_bracket_open,
      field('subscript', $.expression),
      alias($._csharp_context_close, ']'),
    )),

    // Null-forgiveness (suppression) operator: expr!
    // Uses token.immediate() to prevent whitespace before !
    // Can be chained: val!.Name, val![0], val!?.bar
    _razor_suppression_expression: $ => prec.left(seq(
      field('argument', $._razor_access_chain),
      token.immediate('!'),
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

    // Extend non_lvalue_expression to add razor_fragment
    non_lvalue_expression: ($, original) => choice(
      // @ts-ignore
      original,
      $.razor_fragment,
    ),

    // Extend statement to include razor_text_literal (@:) and HTML elements as statement bodies
    statement: ($, original) => choice(
      // @ts-ignore
      original,
      prec(-1, $.razor_text_literal),
      prec(-1, $.element),
      prec(-1, $.self_closing_element),
      prec(-1, $.void_element),
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
        alias($._razor_block_comment, $.razor_comment),
      )),
      alias($._csharp_context_close, '}'),
    ),

    // Note: We inherit C#'s _identifier_token which allows @ prefix for verbatim identifiers.
    // The external scanner's RAZOR_BLOCK_AT handles @ at statement start, ensuring @foo
    // is parsed as a Razor expression rather than a C# verbatim identifier.

    // =========================================================================
    // Text Content
    // =========================================================================

    // Top-level text that can appear between Razor statements
    // Uses repeat1 to combine consecutive text fragments into a single text node
    // Uses external scanner to stop before else/catch/finally keywords
    _top_level_text: $ => alias(prec.right(repeat1($._top_level_text_fragment)), $.text),

    // Individual top-level text fragments
    // Note: The external scanner handles & intelligently - stops only for valid character references
    // Uses distinct symbol names (_top_text_*) to avoid lexer conflicts with element text patterns
    _top_level_text_fragment: $ => choice(
      // Text containing literal @ (email addresses like user@example.com)
      prec(1, $._text_with_literal_at),
      // HTML character references (&amp;, &#123;, etc.)
      $.html_character_reference,
      // Invalid character references (&invalid;)
      $.invalid_character_reference,
      // Main text content - uses external scanner to be keyword-aware and handle & properly
      // Lower precedence (-1) so grammar keywords like 'where' can win
      prec(-1, $._html_text_content),
      // Fallback for text that doesn't start with keyword characters
      // Excludes: e/c/f (keywords), < (HTML), @ (Razor), [ ( (expression), \n, " ' (strings), whitespace, . (C# qualified names)
      $._top_text_content,
      // Single non-keyword-starting character (also excludes quotes, whitespace, dot)
      $._top_text_single,
      // Punctuation [ and ( that could be expression continuations
      $._top_text_punc,
      // Dot as text - lowest precedence, only matches when nothing else can
      // This handles cases like "@foo .bar" where the dot is text after an expression
      $._top_text_dot,
      // Fallback for else/catch/finally keywords as text when not following a Razor control flow
      // Very low precedence so they're only matched when nothing else can use them
      $._keyword_as_text,
    ),

    // Helper tokens for top-level text with distinct symbol names to prevent lexer conflicts
    // Note: Quotes (" and ') are excluded from these fallback patterns because they must be
    // handled by the external scanner. This allows the grammar to parse directive arguments
    // like @page "/route" correctly, while the external scanner handles quotes in regular text.
    // Braces {} are included as valid text since they're only special after @ in Razor.
    // Forward slash / is excluded so C# comments (//) can be handled by external scanner.
    _top_text_content: _ => token(prec(-100, /[^ecf.<@\[(\n"'\s/][^.<@\[(\n"'/]*/)),
    _top_text_single: _ => token(prec(-101, /[^ecf.<@\[(\n"'\s/]/)),
    _top_text_punc: _ => token(prec(-102, /[\[(]/)),
    _top_text_dot: _ => token(prec(-200, /\./)),
    // Fallback for control flow keywords (else/catch/finally) as plain text
    // These are matched by the external scanner but need a grammar rule to consume them
    // when they're not part of a Razor control flow statement
    _keyword_as_text: _ => token(prec(-300, choice('else', 'catch', 'finally'))),

    // Text inside elements - combines consecutive text fragments into a single node
    // This allows bare & to be part of text while still recognizing character references
    // Uses external scanner for keyword awareness (stops before else/catch/finally)
    text: $ => prec.right(repeat1($._text_fragment)),

    // Individual text fragments - separated by & or character references
    // Note: html_character_reference is handled by _element_content, not here
    // Uses external scanner for keyword awareness (HTML_TEXT_CONTENT)
    _text_fragment: $ => choice(
      // Text containing literal @ (email addresses like user@example.com)
      prec(1, $._text_with_literal_at),
      // Main text content - uses external scanner to be keyword-aware
      // This stops before else/catch/finally so Razor control flow can match
      prec(-1, $._html_text_content),
      // Fallback text: any non-special characters (very low precedence)
      // First char excludes: e/c/f (keywords), < @ [ ( & { } (special), whitespace/newlines
      // Rest excludes: < @ [ ( & { } (special) but allows e/c/f/whitespace
      // This only matches when the external scanner doesn't handle the content
      prec(-100, /[^ecf<@\[(&{}\s][^<@\[(&{}]*/),
      // Punctuation that could be expression continuations, but parsed as text
      prec(-20, /[\[({]/),
      // Literal & that doesn't start a valid character reference
      prec(-10, '&'),
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
