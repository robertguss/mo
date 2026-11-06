Defect fixtures for `ci/check_offline_core.py`, which checks that `mo replay` and Lean's `mocore`
stop the same way on the same handler inputs. Each folder holds a service program, `main.mo`, and
`case.txt`, one `key: value` line per field (`#` lines are comments):

| Key | Value |
| --- | --- |
| `handler` | the tool that fails, as the source names it |
| `call` | the MCP arguments, as JSON, that make it fail in a real `mo serve` |
| `state` | the state it fails on, as a Core value; the service's start value |
| `arrival` | an arrival time, as a Core value; the handlers do not read it |
| `arguments` | the call's arguments as Core values, in order |
| `stop` | the stop Lean must report, as a Core `Bug` |
| `passing` | arguments, all `(int N)`, on which the handler returns |
| `returns` | what Lean must return for them, in its structural spelling |

Every value is written by hand from the program, never copied from a `mo` printer. The check
needs fixtures stopping with `stop`, `overflow` and `divByZero`; work, memory and state-size
failures are out of scope, since Lean's fuel, heap and saved encoding are not the VM's.
