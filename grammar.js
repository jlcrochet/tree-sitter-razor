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
    // Replace the individual preproc rules with our unified preproc rule
    // Filter out the old preproc rules and add our new one
    const oldPreprocs = [
      'preproc_region', 'preproc_endregion', 'preproc_line', 'preproc_pragma',
      'preproc_nullable', 'preproc_error', 'preproc_warning', 'preproc_define', 'preproc_undef'
    ];
    const filtered = original.filter(rule => {
      // Keep rules that aren't the old preproc rules
      return !oldPreprocs.some(name => rule.name === name);
    });
    return filtered.concat([$.preproc]);
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
    $._csharp_preproc,              // C# preprocessor directive, only valid in C# context
    // Script/style/title/textarea raw text content
    $._script_content,              // Raw content inside <script> tags
    $._style_content,               // Raw content inside <style> tags
    $._title_content,               // Raw content inside <title> tags
    $._textarea_content,            // Raw content inside <textarea> tags
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

    // Razor statement is @ followed by a C# statement
    razor_statement: $ => seq(
      '@',
      $._razor_supported_statement,
    ),

    _razor_supported_statement: $ => choice(
      alias($.razor_if_statement, $.if_statement),
      alias($.razor_for_statement, $.for_statement),
      alias($.razor_foreach_statement, $.foreach_statement),
      alias($.razor_while_statement, $.while_statement),
      alias($.razor_do_statement, $.do_statement),
      alias($.razor_switch_statement, $.switch_statement),
      alias($.razor_try_statement, $.try_statement),
      alias($.razor_lock_statement, $.lock_statement),
      alias($.razor_using_statement, $.using_statement),
    ),

    // Razor block - can contain statements AND HTML elements
    // Uses external scanner to track entering/exiting C# context
    razor_block: $ => seq(
      alias($._razor_block_open, '{'),
      repeat($._razor_block_content),
      alias($._csharp_context_close, '}'),
    ),

    // Content inside Razor blocks - can contain C# statements, HTML elements, and Razor expressions
    _razor_block_content: $ => choice(
      $.statement,
      $.element,
      $.self_closing_element,
      $.razor_text_literal,
      $.razor_explicit_expression,
      $.razor_implicit_expression,
    ),

    // @: for single-line text literals inside code blocks
    // Can contain Razor expressions like @person.Name
    razor_text_literal: $ => prec.right(prec.dynamic(100, seq(
      '@:',
      repeat($._razor_text_literal_content),
    ))),

    _razor_text_literal_content: $ => prec.right(10, choice(
      // Plain text (anything except @ and newline)
      /[^@\r\n]+/,
      // Razor expressions within text literal
      alias($._text_literal_explicit_expression, $.razor_explicit_expression),
      alias($._text_literal_implicit_expression, $.razor_implicit_expression),
      // Escaped @@ in text literal
      $.escaped_at,
    )),

    // Higher precedence versions for text literal context
    // Uses token(prec()) to give @ higher lexical precedence than C# verbatim identifier
    _text_literal_explicit_expression: $ => prec.dynamic(200, seq(
      token(prec(10, '@')),
      '(',
      $.expression,
      ')',
    )),

    _text_literal_implicit_expression: $ => prec.dynamic(200, seq(
      token(prec(10, '@')),
      $._razor_implicit_expr_chain,
    )),

    // Razor if statement with HTML support
    // Uses prec.right so that else/else if following the block are associated with this if
    razor_if_statement: $ => prec.right(seq(
      'if',
      '(',
      field('condition', $.expression),
      ')',
      field('consequence', $.razor_block),
      optional(field('alternative', choice(
        seq('else', alias($.razor_if_statement, $.if_statement)),
        alias($.razor_else_clause, $.else_clause),
      ))),
    )),

    // Else clause (final else in an if chain)
    razor_else_clause: $ => seq(
      'else',
      $.razor_block,
    ),

    // Razor for statement
    razor_for_statement: $ => seq(
      'for',
      $._for_statement_conditions,
      field('body', $.razor_block),
    ),

    // Razor foreach statement
    razor_foreach_statement: $ => seq(
      $._foreach_statement_initializer,
      field('body', $.razor_block),
    ),

    // Razor while statement
    razor_while_statement: $ => seq(
      'while',
      '(',
      field('condition', $.expression),
      ')',
      field('body', $.razor_block),
    ),

    // Razor do statement
    razor_do_statement: $ => seq(
      'do',
      field('body', $.razor_block),
      'while',
      '(',
      field('condition', $.expression),
      ')',
      ';',
    ),

    // Razor switch statement
    razor_switch_statement: $ => seq(
      'switch',
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
      repeat($._razor_block_content),
    )),

    // Razor try statement
    // Uses prec.right so that catch/finally following the block are associated with this try
    razor_try_statement: $ => prec.right(seq(
      'try',
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

    // Razor lock statement
    razor_lock_statement: $ => seq('lock', '(', $.expression, ')', $.razor_block),

    // Razor using statement (not directive)
    razor_using_statement: $ => seq(
      optional('await'),
      'using',
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

    // Content inside elements - doesn't need keyword awareness
    _element_content: $ => choice(
      $.script_element,
      $.style_element,
      $.title_element,
      $.textarea_element,
      $.element,
      $.self_closing_element,
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
    _immediate_element_name: _ => token.immediate(/[a-zA-Z][a-zA-Z0-9:-]*/),

    element_name: _ => /[a-zA-Z][a-zA-Z0-9:-]*/,

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
      alias(token.immediate(prec(1, /[Ss][Cc][Rr][Ii][Pp][Tt]/)), $.element_name),
      repeat($._html_attribute),
      '>',
    ),

    script_end_tag: $ => seq(
      '</',
      alias(token.immediate(prec(1, /[Ss][Cc][Rr][Ii][Pp][Tt]/)), $.element_name),
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
      alias(token.immediate(prec(1, /[Ss][Tt][Yy][Ll][Ee]/)), $.element_name),
      repeat($._html_attribute),
      '>',
    ),

    style_end_tag: $ => seq(
      '</',
      alias(token.immediate(prec(1, /[Ss][Tt][Yy][Ll][Ee]/)), $.element_name),
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
      alias(token.immediate(prec(1, /[Tt][Ii][Tt][Ll][Ee]/)), $.element_name),
      repeat($._html_attribute),
      '>',
    ),

    title_end_tag: $ => seq(
      '</',
      alias(token.immediate(prec(1, /[Tt][Ii][Tt][Ll][Ee]/)), $.element_name),
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
      alias(token.immediate(prec(1, /[Tt][Ee][Xx][Tt][Aa][Rr][Ee][Aa]/)), $.element_name),
      repeat($._html_attribute),
      '>',
    ),

    textarea_end_tag: $ => seq(
      '</',
      alias(token.immediate(prec(1, /[Tt][Ee][Xx][Tt][Aa][Rr][Ee][Aa]/)), $.element_name),
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

    html_unquoted_attribute_value: _ => /[^\s"'=<>`]+/,

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

    // Override C# preprocessor directives to use external scanner for context-awareness
    // This ensures preproc directives only match in C# context, not HTML
    // We define a single preproc rule that uses the external token, replacing all specific types
    preproc: $ => $._csharp_preproc,
    // Hide the original preproc rules by making them never match
    preproc_region: _ => token(prec(-100, /(?!x)x/)),
    preproc_endregion: _ => token(prec(-100, /(?!x)x/)),
    preproc_line: _ => token(prec(-100, /(?!x)x/)),
    preproc_pragma: _ => token(prec(-100, /(?!x)x/)),
    preproc_nullable: _ => token(prec(-100, /(?!x)x/)),
    preproc_error: _ => token(prec(-100, /(?!x)x/)),
    preproc_warning: _ => token(prec(-100, /(?!x)x/)),
    preproc_define: _ => token(prec(-100, /(?!x)x/)),
    preproc_undef: _ => token(prec(-100, /(?!x)x/)),

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
    razor_page_directive: $ => seq('@page', optional($.string_literal)),

    razor_model_directive: $ => seq('@model', $.type),

    // @using directive (imports namespace)
    // The statement form is @using (...) which starts with ( after 'using'
    // The directive form is @using Namespace which has a name after 'using'
    razor_using_directive: $ => prec(1, seq('@', 'using', $._name)),

    razor_inject_directive: $ => seq('@inject', $.type, $.identifier),

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

    razor_rendermode_directive: $ => seq('@rendermode', $.expression),

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
    razor_code_block: $ => prec.dynamic(100, seq(
      alias($._csharp_code_block_start, '@{'),
      repeat($._razor_block_content),
      alias($._csharp_context_close, '}'),
    )),

    // =========================================================================
    // Razor Expressions
    // =========================================================================

    // Explicit expression: @(expression)
    // Uses external scanner to track entering/exiting C# context
    razor_explicit_expression: $ => prec.dynamic(100, seq(
      alias($._csharp_explicit_expr_start, '@('),
      $.expression,
      alias($._csharp_context_close, ')'),
    )),

    // Implicit expression: @identifier, @identifier.property, @identifier.Method()
    // Use prec.dynamic to allow runtime conflict resolution
    razor_implicit_expression: $ => seq(
      '@',
      $._razor_implicit_expr_chain,
    ),

    // Chain of member access, method calls, and indexers starting from identifier
    _razor_implicit_expr_chain: $ => prec.dynamic(10, choice(
      $.await_expression,
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
    _razor_access_chain: $ => choice(
      $._razor_primary_expression,
      // These literals are unambiguous
      $.boolean_literal,
      $.null_literal,
    ),

    // Primary expression that can have member access, invocation, or indexing applied
    // Uses prec.left for left-to-right associativity (foo.bar.baz groups as ((foo.bar).baz))
    _razor_primary_expression: $ => choice(
      prec.dynamic(1, $.identifier),
      prec.dynamic(20, alias($._razor_member_access, $.member_access_expression)),
      prec.dynamic(20, alias($._razor_invocation, $.invocation_expression)),
      prec.dynamic(20, alias($._razor_element_access, $.element_access_expression)),
      prec.dynamic(20, alias($._razor_conditional_access, $.conditional_access_expression)),
    ),

    // Member access: expr.identifier
    _razor_member_access: $ => prec.left(seq(
      field('expression', $._razor_primary_expression),
      '.',
      field('name', $.identifier),
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
    _razor_conditional_access: $ => prec.left(seq(
      field('expression', $._razor_primary_expression),
      '?.',
      field('name', $.identifier),
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
    block: $ => seq(
      '{',
      repeat(choice(
        $.statement,
        $.element,
        $.self_closing_element,
        $.razor_text_literal,
      )),
      '}',
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
      // Punctuation that could be expression continuations
      prec(-20, /[.\[(]/),
    ),

    // Text inside elements - doesn't need to stop at keywords
    // Also excludes . [ ( which are handled separately for implicit expression chaining
    text: $ => choice(
      // Text containing literal @ (email addresses like user@example.com)
      prec(1, alias($._text_with_literal_at, $.text)),
      // Regular text content
      prec(-10, /[^<@.\[(]+/),
      // Punctuation that could be expression continuations, but parsed as text
      // when not following an identifier (lower precedence than implicit expressions)
      prec(-20, /[.\[(]/),
    ),

  },
});
