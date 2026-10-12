/-
  Mo Core: the small internal language every feature of Mo lowers to.
  See README.md for the layout, the theorems and how to build.
-/
-- The language
import MoCore.Syntax
import MoCore.Value
import MoCore.Support.Num
import MoCore.Support.Text
import MoCore.Support.Time
import MoCore.Support.Equiv
import MoCore.Prim
import MoCore.Eval
-- Typing and the proofs
import MoCore.Typing
import MoCore.Safety
import MoCore.PrimSafety
import MoCore.Theorems
import MoCore.Check
import MoCore.Fuel
-- Writing and running Core programs
import MoCore.DSL
import MoCore.Lib
import MoCore.Testing
-- Checks of the built-in types the samples use little
import MoCore.Builtins
-- The twelve samples
import MoCore.Examples.Greet
import MoCore.Examples.Wc
import MoCore.Examples.Find
import MoCore.Examples.Expenses
import MoCore.Examples.JsonQuery
import MoCore.Examples.Todo
import MoCore.Examples.Account
import MoCore.Examples.RateLimit
import MoCore.Examples.JobQueue
import MoCore.Examples.Kv
import MoCore.Examples.NotesTool
import MoCore.Examples.Inventory
-- The text interchange contract and its round-trip checks
import MoCore.Text.Run
import MoCore.TextChecks
