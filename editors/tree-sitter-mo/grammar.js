// Tree-sitter grammar for Mo (Language Reference v0.1), derived from grammar/mo.y and grammar/lexer.c.

const PREC = {
  IMPLIES: 1,
  OR: 2,
  AND: 3,
  NOT: 4,
  COMPARE: 5,
  RANGE: 6,
  SUM: 7,
  PRODUCT: 8,
  UNARY: 9,
  POSTFIX: 10,
};

function commaSep1(rule) {
  return seq(rule, repeat(seq(',', rule)));
}

function commaSep(rule) {
  return optional(commaSep1(rule));
}

const SOFT_KEYWORDS = [
  'state',
  'tools',
  'calls',
  'every',
  'token',
  'from',
  'env',
  'on',
  'port',
  'cors',
  'sqlite',
  'auth',
  'via',
  'get',
  'post',
  'put',
  'patch',
  'delete',
  'status',
];

module.exports = grammar({
  name: 'mo',

  externals: $ => [
    $._newline,
    $.string_content,
    $.multiline_string_content,
    $.multiline_string_end,
  ],

  extras: $ => [
    /[ \t\r\n]/,
    $.doc_comment,
    $.comment,
  ],

  word: $ => $.identifier,

  supertypes: $ => [
    $._expr,
    $._type,
  ],

  conflicts: $ => [
    [$._simple_statement, $._arm_patterns],
  ],

  rules: {
    source_file: $ => seq(
      optional($.module_header),
      repeat($._top_item),
    ),

    _terminator: $ => choice($._newline, '\0'),

    doc_comment: _ => token(seq('##', /[^\r\n]*/)),
    comment: _ => token(prec(-1, seq('#', /[^\r\n]*/))),

    identifier: _ => /[a-z][a-z0-9_]*\??|_[a-z0-9_]+\??/,
    wildcard: _ => '_',
    type_identifier: _ => /[A-Z][A-Za-z0-9]*/,

    _identifier: $ => choice(
      $.identifier,
      ...SOFT_KEYWORDS.map(w => alias(w, $.identifier)),
    ),

    module_header: $ => seq(
      'module',
      field('name', $.module_path),
      $._newline,
      optional($.expose_clause),
      repeat($.use_clause),
    ),

    module_path: $ => seq(
      $.type_identifier,
      repeat(seq('.', $.type_identifier)),
    ),

    expose_clause: $ => seq(
      'expose',
      commaSep1($._expose_item),
      $._newline,
    ),

    _expose_item: $ => choice(
      $._identifier,
      $.type_identifier,
      $.expose_all,
    ),

    expose_all: $ => seq(
      field('type', $.type_identifier),
      '(',
      '..',
      ')',
    ),

    use_clause: $ => seq(
      'use',
      field('module', $.module_path),
      optional(seq('{', commaSep1(choice($._identifier, $.type_identifier)), '}')),
      $._newline,
    ),

    _top_item: $ => choice(
      $.function_declaration,
      $.struct_declaration,
      $.enum_declaration,
      $.service_declaration,
      $.actor_declaration,
      $.api_declaration,
      $.test_declaration,
      $._statement,
    ),

    // Types
    _type: $ => choice(
      $.type_reference,
      $.generic_type,
      $.tuple_type,
      $.function_type,
    ),

    type_reference: $ => choice(
      $.type_identifier,
      $.qualified_type,
    ),

    qualified_type: $ => seq(
      field('module', $.type_identifier),
      '.',
      field('name', $.type_identifier),
    ),

    generic_type: $ => seq(
      field('name', $.type_reference),
      '(',
      commaSep1($._type),
      ')',
    ),

    tuple_type: $ => seq(
      '(',
      $._type,
      ',',
      commaSep1($._type),
      ')',
    ),

    function_type: $ => seq(
      'fn',
      '(',
      commaSep($._type),
      ')',
      optional(seq(':', field('return_type', $._type))),
    ),

    type_parameters: $ => seq(
      '(',
      commaSep1($.type_identifier),
      ')',
    ),

    // Declarations
    struct_declaration: $ => seq(
      'struct',
      field('name', $.type_identifier),
      optional(field('type_parameters', $.type_parameters)),
      $._newline,
      repeat($.field_declaration),
      'end',
      optional($._newline),
    ),

    field_declaration: $ => seq(
      field('name', $._identifier),
      ':',
      field('type', $._type),
      $._newline,
    ),

    enum_declaration: $ => seq(
      'enum',
      field('name', $.type_identifier),
      optional(field('type_parameters', $.type_parameters)),
      $._newline,
      repeat($.enum_case),
      'end',
      optional($._newline),
    ),

    enum_case: $ => seq(
      field('name', $.type_identifier),
      optional(choice(
        seq('(', $._type, ')'),
        seq('(', commaSep1($.parameter), optional(','), ')'),
      )),
      $._newline,
    ),

    function_declaration: $ => seq(
      'fn',
      field('name', $._identifier),
      field('parameters', $.parameter_list),
      optional(seq(':', field('return_type', $._type))),
      optional($.where_clause),
      $._newline,
      repeat(choice($.promise_block, $.examples_block)),
      repeat($._statement),
      'end',
      optional($._newline),
    ),

    parameter_list: $ => seq(
      '(',
      optional(seq(commaSep1($.parameter), optional(','))),
      ')',
    ),

    parameter: $ => seq(
      field('name', choice($._identifier, $.wildcard)),
      ':',
      field('type', $._type),
    ),

    where_clause: $ => seq(
      'where',
      commaSep1($.constraint),
    ),

    constraint: $ => seq(
      field('type_parameter', $.type_identifier),
      ':',
      field('ability', $.type_identifier),
    ),

    promise_block: $ => seq(
      'promise',
      optional(field('label', $.string)),
      optional(seq('on', field('tool', $._identifier))),
      $._newline,
      repeat(choice(
        $.anchor_clause,
        seq($._expr, $._newline),
      )),
      'end',
      $._newline,
    ),

    anchor_clause: $ => seq(
      choice('allows', 'forbids'),
      commaSep1($.named_argument),
      $._newline,
    ),

    examples_block: $ => seq(
      'examples',
      $._newline,
      repeat(seq($._expr, $._newline)),
      'end',
      $._newline,
    ),

    test_declaration: $ => seq(
      'test',
      field('name', $.string),
      $._newline,
      repeat($._statement),
      'end',
      optional($._newline),
    ),

    service_declaration: $ => seq(
      'service',
      $._newline,
      repeat($._service_clause),
      'end',
      optional($._newline),
    ),

    _service_clause: $ => choice(
      $.state_clause,
      $.tools_clause,
      $.every_clause,
      $.token_clause,
      $.promise_block,
    ),

    actor_declaration: $ => seq(
      'actor',
      field('name', $.type_identifier),
      $._newline,
      repeat($._actor_clause),
      'end',
      optional($._newline),
    ),

    _actor_clause: $ => choice(
      $.state_clause,
      $.calls_clause,
      $.every_clause,
      $.promise_block,
    ),

    state_clause: $ => seq(
      'state',
      field('type', $._type),
      '=',
      field('value', $._expr),
      $._newline,
    ),

    tools_clause: $ => seq(
      'tools',
      commaSep1($._identifier),
      $._newline,
    ),

    calls_clause: $ => seq(
      'calls',
      commaSep1($._identifier),
      $._newline,
    ),

    every_clause: $ => seq(
      'every',
      field('interval', $._expr),
      field('handler', $._identifier),
      $._newline,
    ),

    token_clause: $ => seq(
      'token',
      'from',
      'env',
      field('variable', $.string),
      $._newline,
    ),

    api_declaration: $ => seq(
      'api',
      $._newline,
      repeat($._api_clause),
      'end',
      optional($._newline),
    ),

    _api_clause: $ => choice(
      $.port_clause,
      $.cors_clause,
      $.sqlite_clause,
      $.api_actor_clause,
      $.auth_clause,
      $.route_clause,
      $.status_clause,
    ),

    port_clause: $ => seq('port', field('port', $.integer), $._newline),
    cors_clause: $ => seq('cors', field('origin', $.string), $._newline),
    sqlite_clause: $ => seq('sqlite', field('name', $._identifier), 'from', field('path', $.string), $._newline),
    api_actor_clause: $ => seq('actor', field('name', $._identifier), ':', field('type', $._type), $._newline),
    auth_clause: $ => seq('auth', field('principal', $._type), 'via', field('handler', $._identifier), $._newline),

    route_clause: $ => seq(
      field('method', $.http_method),
      field('path', $.string),
      field('handler', $._identifier),
      $._newline,
    ),

    http_method: _ => choice('get', 'post', 'put', 'patch', 'delete'),

    status_clause: $ => seq(
      'status',
      field('error_type', $.type_reference),
      optional(seq('(', commaSep1($.wildcard), ')')),
      '=',
      field('code', $.integer),
      $._newline,
    ),

    // Statements
    _statement: $ => choice(
      seq($._simple_statement, optional($._newline)),
      $.for_statement,
      $.while_statement,
    ),

    _simple_statement: $ => choice(
      $.var_statement,
      $.assignment_statement,
      $.return_statement,
      $.break_statement,
      $.continue_statement,
      $.assert_statement,
      $.if_expression,
      $._expr,
    ),

    for_statement: $ => seq(
      'for',
      field('binder', $._binder),
      'in',
      field('iterable', $._expr),
      $._newline,
      repeat($._statement),
      'end',
      optional($._newline),
    ),

    _binder: $ => choice(
      $._identifier,
      $.wildcard,
      seq('(', $. _binder, ',', commaSep1($._binder), ')'),
    ),

    while_statement: $ => seq(
      'while',
      field('condition', $._expr),
      $._newline,
      repeat($._statement),
      'end',
      optional($._newline),
    ),

    var_statement: $ => seq(
      'var',
      field('name', $._identifier),
      '=',
      field('value', choice($._expr, $.if_expression)),
    ),

    assignment_statement: $ => prec.right(seq(
      field('left', $._expr),
      field('operator', choice('=', '+=', '-=')),
      field('right', choice($._expr, $.if_expression)),
    )),

    return_statement: $ => prec.right(seq(
      'return',
      optional(field('value', $._expr)),
      optional(seq('if', field('guard', $._expr))),
    )),

    break_statement: $ => prec.right(seq(
      'break',
      optional(seq('if', field('guard', $._expr))),
    )),

    continue_statement: $ => prec.right(seq(
      'continue',
      optional(seq('if', field('guard', $._expr))),
    )),

    assert_statement: $ => seq(
      'assert',
      field('condition', $._expr),
    ),

    // Block if & Case
    if_expression: $ => seq(
      'if',
      field('condition', $._expr),
      $._newline,
      repeat($._statement),
      optional($.else_clause),
      'end',
    ),

    else_clause: $ => choice(
      seq('else', $._newline, repeat($._statement)),
      seq('else', 'if', field('condition', $._expr), $._newline, repeat($._statement), optional($.else_clause)),
    ),

    // Expressions
    _expr: $ => choice(
      $.line_if_expression,
      $.binary_expression,
      $.is_expression,
      $.range_expression,
      $.unary_expression,
      $.try_expression,
      $.call_expression,
      $.type_application,
      $.member_expression,
      $.tuple_index_expression,
      $.case_expression,
      $.lambda_expression,
      $.stop_expression,
      $.parenthesized_expression,
      $.tuple_expression,
      $.list_expression,
      $.map_expression,
      $.integer,
      $.decimal,
      $.character,
      $.string,
      $.boolean,
      $._identifier,
      $.type_identifier,
      $.wildcard,
    ),

    line_if_expression: $ => prec.right(seq(
      'if',
      field('condition', $._expr),
      ':',
      field('consequence', $._expr),
      'else',
      choice(
        seq(':', field('alternative', $._expr)),
        field('alternative', $.line_if_expression),
      ),
    )),

    binary_expression: $ => choice(
      prec.left(PREC.IMPLIES, seq(field('left', $._expr), field('operator', 'implies'), field('right', $._expr))),
      prec.left(PREC.OR, seq(field('left', $._expr), field('operator', 'or'), field('right', $._expr))),
      prec.left(PREC.AND, seq(field('left', $._expr), field('operator', 'and'), field('right', $._expr))),
      prec.left(PREC.COMPARE, seq(field('left', $._expr), field('operator', choice('==', '!=', '<', '<=', '>', '>=')), field('right', $._expr))),
      prec.left(PREC.SUM, seq(field('left', $._expr), field('operator', choice('+', '-')), field('right', $._expr))),
      prec.left(PREC.PRODUCT, seq(field('left', $._expr), field('operator', choice('*', '/', '%')), field('right', $._expr))),
    ),

    is_expression: $ => prec.left(PREC.COMPARE, seq(
      field('value', $._expr),
      'is',
      field('pattern', $._expr),
    )),

    range_expression: $ => prec.left(PREC.RANGE, seq(
      field('start', $._expr),
      '..',
      field('end', $._expr),
    )),

    unary_expression: $ => choice(
      prec(PREC.NOT, seq(field('operator', 'not'), field('operand', $._expr))),
      prec(PREC.UNARY, seq(field('operator', '-'), field('operand', $._expr))),
    ),

    try_expression: $ => prec(PREC.UNARY, seq(
      'try',
      field('operand', $._expr),
    )),

    member_expression: $ => prec.left(PREC.POSTFIX, seq(
      field('object', $._expr),
      '.',
      field('property', choice($._identifier, $.type_identifier)),
    )),

    tuple_index_expression: $ => prec.left(PREC.POSTFIX, seq(
      field('object', $._expr),
      '.',
      field('index', alias(/[0-9]+/, $.integer)),
    )),

    type_application: $ => prec.left(PREC.POSTFIX, seq(
      field('function', $._expr),
      '[',
      commaSep1($._type),
      ']',
    )),

    call_expression: $ => prec.left(PREC.POSTFIX, seq(
      field('function', $._expr),
      field('arguments', $.argument_list),
    )),

    argument_list: $ => seq(
      '(',
      optional(seq(commaSep1(choice($._expr, $.named_argument)), optional(','))),
      ')',
    ),

    named_argument: $ => seq(
      field('name', $._identifier),
      ':',
      field('value', $._expr),
    ),

    case_expression: $ => seq(
      'case',
      field('value', $._expr),
      $._newline,
      repeat(choice($.case_arm, $._statement)),
      'end',
    ),

    case_arm: $ => prec(2, seq(
      field('pattern', $._arm_patterns),
      optional(seq('if', field('guard', $._expr))),
      ':',
      optional(field('body', $._simple_statement)),
      $._newline,
    )),

    _arm_patterns: $ => seq(
      $._expr,
      repeat(seq('|', $._expr)),
    ),

    lambda_expression: $ => seq(
      'fn',
      '(',
      commaSep(choice($._identifier, $.wildcard)),
      ')',
      choice(
        field('body', $._expr),
        seq($._newline, repeat($._statement)),
      ),
      'end',
    ),

    stop_expression: $ => seq(
      'stop',
      field('reason', $.string),
    ),

    parenthesized_expression: $ => seq('(', $._expr, ')'),

    tuple_expression: $ => seq(
      '(',
      $._expr,
      ',',
      commaSep1($._expr),
      optional(','),
      ')',
    ),

    list_expression: $ => seq(
      '[',
      optional(seq(commaSep1(choice($._expr, $.spread_element)), optional(','))),
      ']',
    ),

    spread_element: $ => seq(
      '..',
      choice($._identifier, $.wildcard),
    ),

    map_expression: $ => seq(
      '{',
      optional(seq(commaSep1($.map_entry), optional(','))),
      '}',
    ),

    map_entry: $ => seq(
      field('key', $._expr),
      ':',
      field('value', $._expr),
    ),

    // Literals
    boolean: _ => choice('true', 'false'),

    integer: _ => token(choice(
      /0x[0-9a-fA-F]+(_[0-9a-fA-F]+)*/,
      /0b[01]+(_[01]+)*/,
      /[0-9]+(_[0-9]+)*/,
    )),

    decimal: _ => token(seq(
      /[0-9]+(_[0-9]+)*/,
      '.',
      /[0-9]+(_[0-9]+)*/,
    )),

    character: $ => seq(
      "'",
      choice(
        $.escape_sequence,
        token.immediate(/[^'\\\r\n]/),
      ),
      token.immediate("'"),
    ),

    string: $ => choice(
      $.multiline_string,
      $.quoted_string,
    ),

    quoted_string: $ => seq(
      '"',
      repeat(choice(
        $.escape_sequence,
        $.interpolation,
        $.string_content,
      )),
      token.immediate('"'),
    ),

    multiline_string: $ => seq(
      alias(token(prec(2, /"""[ \t]*\r?\n/)), $.multiline_string_start),
      repeat(choice(
        $.escape_sequence,
        $.interpolation,
        $.multiline_string_content,
      )),
      $.multiline_string_end,
    ),

    escape_sequence: _ => token.immediate(seq(
      '\\',
      choice(
        /[ntr\\"'#]/,
        /u\{[0-9a-fA-F]{1,6}\}/,
      ),
    )),

    interpolation: $ => seq(
      token.immediate('#{'),
      field('expression', $._expr),
      '}',
    ),
  },
});
