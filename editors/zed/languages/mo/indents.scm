[
  (function_declaration)
  (struct_declaration)
  (enum_declaration)
  (service_declaration)
  (actor_declaration)
  (api_declaration)
  (scope_clause)
  (test_declaration)
  (promise_block)
  (examples_block)
  (for_statement)
  (while_statement)
  (if_expression)
  (case_expression)
  (lambda_expression)
] @indent

(function_declaration "end" @end)
(struct_declaration "end" @end)
(enum_declaration "end" @end)
(service_declaration "end" @end)
(actor_declaration "end" @end)
(api_declaration "end" @end)
(scope_clause "end" @end)
(test_declaration "end" @end)
(promise_block "end" @end)
(examples_block "end" @end)
(for_statement "end" @end)
(while_statement "end" @end)
(if_expression "end" @end)
(case_expression "end" @end)
(lambda_expression "end" @end)

(else_clause "else" @outdent)

("(" @start ")" @end) @indent
("[" @start "]" @end) @indent
("{" @start "}" @end) @indent
