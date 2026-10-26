/**
 * @file Smarty grammar for tree-sitter.
 *
 * Based on the MIT-licensed HTML grammar by Max Brunsfeld, extended with
 * Smarty template tags. The HTML portion (elements, attributes, entities,
 * and raw-text for <script>/<style>) is preserved so templates parse
 * correctly and script/style bodies stay opaque (no false Smarty tags are
 * detected inside them). Smarty tags, comments and their inner content are
 * added on top.
 *
 * Delimiter handling is done in the external scanner (src/scanner.c): it
 * emits smarty_open / smarty_close / smarty_comment, tracking whether a
 * single-brace ({...}) or double-brace ({{...}}) tag is open so the
 * correct close delimiter is recognised. Inner tag content is matched by
 * the regex tokens below.
 *
 * @license HTML portion: MIT (Max Brunsfeld). Smarty extension: MIT.
 */

/* eslint-disable arrow-parens */
/* eslint-disable camelcase */
// @ts-check

// Smarty control / block tags and common functions & modifiers, used to
// highlight tag words. (Variables use the separate $... token.)
const smartyKeywords = [
  'if', 'elseif', 'else', 'endif',
  'foreach', 'foreachelse', 'endforeach',
  'for', 'endfor',
  'section', 'sectionelse', 'endsection',
  'strip', 'endstrip',
  'capture', 'endcapture',
  'block', 'endblock',
  'cache', 'endcache',
  'literal', 'endliteral',
  'setfilter',
  'include', 'include_php', 'insert', 'load_plugin',
  'php', 'php_tag', 'config_load', 'call',
  'counter', 'cycle', 'fetch', 'math',
  'html_options', 'html_select_date', 'html_select_time',
  'html_select_options', 'html_table',
  'append', 'assign', 'display', 'print',
  'default', 'escape', 'truncate', 'wordwrap', 'count',
  'date_format', 'strip_tags', 'upper', 'lower', 'capitalize',
  'explode', 'implode', 'substr', 'join', 'size', 'in_array',
  'count_characters', 'count_words', 'nl2br', 'spacepad', 'string_format',
  'regex_replace', 'replace', 'array_merge', 'sha1', 'md5',
  'date', 'time', 'now', 'abs', 'ceil', 'floor', 'round',
  'is_array', 'is_bool', 'is_digit', 'is_even', 'is_odd',
  'is_divisible', 'is_set', 'isset', 'empty', 'is_null', 'defined',
];

module.exports = grammar({
  name: 'smarty',

  extras: $ => [
    $.smarty_comment,
    /\s+/,
  ],

  externals: $ => [
    $._start_tag_name,
    $._script_start_tag_name,
    $._style_start_tag_name,
    $._end_tag_name,
    $.erroneous_end_tag_name,
    '/>',
    $._implicit_end_tag,
    $.raw_text,
    $.comment,
    $.smarty_open,
    $.smarty_close,
    $.smarty_comment,
  ],

  rules: {
    document: $ => repeat($._node),

    doctype: $ => seq(
      '<!',
      alias($._doctype, 'doctype'),
      /[^>]+/,
      '>',
    ),

    _doctype: _ => /[Dd][Oo][Cc][Tt][Yy][Pp][Ee]/,

    _node: $ => choice(
      $.doctype,
      $.entity,
      $.text,
      $.element,
      $.script_element,
      $.style_element,
      $.erroneous_end_tag,
      $.smarty_tag,
      $.smarty_brace,
    ),

    element: $ => choice(
      seq(
        $.start_tag,
        repeat($._node),
        choice($.end_tag, $._implicit_end_tag),
      ),
      $.self_closing_tag,
    ),

    script_element: $ => seq(
      alias($.script_start_tag, $.start_tag),
      optional($.raw_text),
      $.end_tag,
    ),

    style_element: $ => seq(
      alias($.style_start_tag, $.start_tag),
      optional($.raw_text),
      $.end_tag,
    ),

    start_tag: $ => seq(
      '<',
      alias($._start_tag_name, $.tag_name),
      repeat($.attribute),
      '>',
    ),

    script_start_tag: $ => seq(
      '<',
      alias($._script_start_tag_name, $.tag_name),
      repeat($.attribute),
      '>',
    ),

    style_start_tag: $ => seq(
      '<',
      alias($._style_start_tag_name, $.tag_name),
      repeat($.attribute),
      '>',
    ),

    self_closing_tag: $ => seq(
      '<',
      alias($._start_tag_name, $.tag_name),
      repeat($.attribute),
      '/>',
    ),

    end_tag: $ => seq(
      '</',
      alias($._end_tag_name, $.tag_name),
      '>',
    ),

    erroneous_end_tag: $ => seq(
      '</',
      $.erroneous_end_tag_name,
      '>',
    ),

    attribute: $ => seq(
      $.attribute_name,
      optional(seq(
        '=',
        choice(
          $.attribute_value,
          $.quoted_attribute_value,
        ),
      )),
    ),

    attribute_name: _ => /[^<>"'/=\s]+/,

    attribute_value: _ => /[^<>"'=\s]+/,

    // An entity can be named, numeric (decimal), or numeric (hexacecimal). The
    // longest entity name is 29 characters long, and the HTML spec says that
    // no more will ever be added.
    entity: _ => /&(#([xX][0-9a-fA-F]{1,6}|[0-9]{1,5})|[A-Za-z]{1,30});?/,

    quoted_attribute_value: $ => choice(
      seq('\'', optional(alias(/[^']+/, $.attribute_value)), '\''),
      seq('"', optional(alias(/[^"]+/, $.attribute_value)), '"'),
    ),

    text: _ => /[^<>&\s{}]([^<>&{}]*[^<>&\s{}])?/,

    // ----- Smarty -----

    // A Smarty tag: { ... } or {{ ... }} (delimiters are external tokens).
    smarty_tag: $ => seq(
      $.smarty_open,
      repeat($._tag_content),
      $.smarty_close,
    ),

    _tag_content: $ => choice(
      $.smarty_variable,
      $.smarty_string,
      $.smarty_number,
      $.smarty_operator,
      $.smarty_punct,
      $.smarty_word,
    ),

    smarty_variable: _ => /\$[A-Za-z_][\w.]*/,

    smarty_string: _ => /'(?:[^'\\]|\\.)*'|"(?:[^"\\]|\\.)*"/,

    smarty_number: _ => /\d+(?:\.\d+)?/,

    smarty_operator: _ => /===|!==|==|!=|<=|>=|<>|[-+*/%=<>!&|?]+/,

    smarty_punct: _ => /[,;:()\[\]]/,

    // A lone "}" inside a tag (e.g. inside a double-brace tag) that is not
    // a tag delimiter. Top-level "{" is matched by `text`; the close "}" is
    // claimed by the scanner, so this only needs to cover an interior "}".
    smarty_brace: _ => /[{}]/,

    smarty_word: $ => choice(
      $.smarty_keyword,
      $.smarty_ident,
    ),

    smarty_keyword: _ => token(prec(2, new RegExp(smartyKeywords.join('|')))),

    smarty_ident: _ => token(prec(1, /[A-Za-z_]\w*/)),
  },
});
