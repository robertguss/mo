(module_header
  "module" @context
  name: (module_path) @name) @item

(struct_declaration
  "struct" @context
  name: (type_identifier) @name) @item

(enum_declaration
  "enum" @context
  name: (type_identifier) @name) @item

(actor_declaration
  "actor" @context
  name: (type_identifier) @name) @item

(service_declaration
  "service" @name) @item

(api_declaration
  "api" @name) @item

(function_declaration
  "fn" @context
  name: (identifier) @name) @item

(test_declaration
  "test" @context
  name: (string) @name) @item
