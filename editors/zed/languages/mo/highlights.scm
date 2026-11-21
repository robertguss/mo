; Comments
(doc_comment) @comment.doc
(comment) @comment

; Literals
(string) @string
(character) @string
(escape_sequence) @string.escape
(integer) @number
(decimal) @number
(boolean) @boolean

; String interpolation
(interpolation
  "#{" @punctuation.special
  "}" @punctuation.special)

; Wildcard
(wildcard) @variable.special

; Modules and imports
(module_header
  "module" @keyword
  name: (module_path) @namespace)

(use_clause
  "use" @keyword
  module: (module_path) @namespace)

"expose" @keyword

; Types and built-ins
(type_identifier) @type

((type_identifier) @type.builtin
  (#match? @type.builtin "^(Bool|Int|BigInt|Byte|Decimal|Float|Text|Char|Bytes|Unit|List|Map|Set|Range|Time|Duration|Option|Result|Ordering|Args|Env|Console|Files|ReadFiles|FileError|Entry|EntryKind|Simulation|SimulationError|Clock|Random|Network|Connection|NetworkError|Sqlite|SqliteError|Command|Actor|Order|Json|Http|Api|ApiTest|Unicode16)$"))

((type_identifier) @constructor
  (#match? @constructor "^(Some|None|Ok|Error|Less|Equal|Greater|NotFound|NotAllowed|NotText|Failed|File|Folder|Link|Other|ServiceDown|ToolFailed)$"))

(enum_case
  name: (type_identifier) @constructor)

(constraint
  type_parameter: (type_identifier) @type
  ability: (type_identifier) @type.builtin)

; Declarations
(function_declaration
  "fn" @keyword
  name: (identifier) @function)

(struct_declaration
  "struct" @keyword
  name: (type_identifier) @type)

(enum_declaration
  "enum" @keyword
  name: (type_identifier) @type)

(actor_declaration
  "actor" @keyword
  name: (type_identifier) @type)

(field_declaration
  name: (identifier) @property)

(parameter
  name: (identifier) @variable.parameter)

(named_argument
  name: (identifier) @property)

; Calls and member access
(call_expression
  function: (identifier) @function.call)

(call_expression
  function: (member_expression
    property: (identifier) @function.method))

(member_expression
  property: (identifier) @property)

; Service, actor, and API clauses
[
  "service"
  "api"
  "actor"
  "state"
  "tools"
  "calls"
  "every"
  "token"
  "from"
  "env"
  "on"
  "port"
  "cors"
  "sqlite"
  "auth"
  "via"
  "status"
] @keyword

(http_method) @keyword

(every_clause
  handler: (identifier) @function)

(auth_clause
  handler: (identifier) @function)

(route_clause
  path: (string) @string.special
  handler: (identifier) @function)

; Specifications and tests
[
  "promise"
  "examples"
  "test"
  "allows"
  "forbids"
  "assert"
  "stop"
] @keyword

; Control flow and bindings
[
  "fn"
  "struct"
  "enum"
  "var"
  "where"
  "if"
  "else"
  "case"
  "for"
  "in"
  "while"
  "return"
  "break"
  "continue"
  "try"
  "end"
] @keyword

; Word and symbolic operators
[
  "and"
  "or"
  "not"
  "is"
  "implies"
  "="
  "+="
  "-="
  "=="
  "!="
  "<"
  "<="
  ">"
  ">="
  "+"
  "-"
  "*"
  "/"
  "%"
  ".."
  "|"
] @operator

; Punctuation
[
  "("
  ")"
  "["
  "]"
  "{"
  "}"
] @punctuation.bracket

[
  ","
  ":"
  "."
] @punctuation.delimiter
