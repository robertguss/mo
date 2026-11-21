## Stateful BEAM actor coordinating multi-step coding-agent turns, token cost budgets,
## and subagent delegations with machine-checked invariants across crashes and restarts.
module Coordinator
expose TaskCoordinator,
RunStatus(..),
BudgetError(..),
SubagentRecord(..),
CoordinatorView(..),
token_cost

enum RunStatus
  Running
  Completed
  Stopped(reason: Text)
end

enum BudgetError
  InvalidLimits
  InvalidTokens
  StepLimitReached(max_steps: Int)
  BudgetExceeded(spent_usd: Decimal, budget_usd: Decimal)
  SubagentLimitReached(max_subagents: Int)
  RunNotActive
end

struct SubagentRecord
  id: Int
  role: Text
  summary: Text
end

struct CoordinatorView
  steps: Int
  max_steps: Int
  spent_usd: Decimal
  budget_usd: Decimal
  subagents: List(SubagentRecord)
  status: RunStatus
end

struct State
  steps: Int
  max_steps: Int
  spent_usd: Decimal
  budget_usd: Decimal
  subagents: List(SubagentRecord)
  status: RunStatus
end

fn initial_state() : State
  State(steps: 0, max_steps: 12, spent_usd: 0.00, budget_usd: 5.00, subagents: [], status: Running)
end

## Exact 128-bit Decimal USD cost ($3/1M input tokens, $15/1M output tokens) for bounded token counts.
fn token_cost(input_tokens: Int, output_tokens: Int) : Result(Decimal, BudgetError)
  promise "token costs are nonnegative and bounded for valid token counts"
    result is Ok(cost) implies cost >= 0.00 and cost <= 180.00
    (input_tokens < 0 or output_tokens < 0) implies result == Error(InvalidTokens)
  end

  if input_tokens < 0 or
    input_tokens > 10_000_000 or
    output_tokens < 0 or
    output_tokens > 10_000_000
    return Error(InvalidTokens)
  end
  cost = input_tokens.to_decimal * 0.000003 + output_tokens.to_decimal * 0.000015
  Ok(cost)
end

actor TaskCoordinator
  state State = initial_state()
  calls set_limits, record_turn, record_subagent, finish, view

  promise
    after.steps >= before.steps
    after.steps <= after.max_steps
    after.spent_usd >= before.spent_usd
    after.spent_usd <= after.budget_usd
    after.subagents.size >= before.subagents.size
  end

  promise on record_turn
    reply is Ok(_) implies after.steps == before.steps + 1
    reply is Error(_) implies after == before
  end
end

fn to_view(s: State) : CoordinatorView
  CoordinatorView(
    steps: s.steps,
    max_steps: s.max_steps,
    spent_usd: s.spent_usd,
    budget_usd: s.budget_usd,
    subagents: s.subagents,
    status: s.status
  )
end

## Tightens or configures step and cent budgets without invalidating already-committed work.
fn set_limits(s: State, _now: Time, max_steps: Int, budget_cents: Int) : Result(
  (State, CoordinatorView),
  BudgetError
)
  if max_steps < 1 or max_steps > 1000 or max_steps < s.steps
    return Error(InvalidLimits)
  end
  if budget_cents < 1 or budget_cents > 1_000_000
    return Error(InvalidLimits)
  end
  next_budget = budget_cents.to_decimal / 100
  if next_budget < s.spent_usd
    return Error(InvalidLimits)
  end
  next = State(
    steps: s.steps,
    max_steps: max_steps,
    spent_usd: s.spent_usd,
    budget_usd: next_budget,
    subagents: s.subagents,
    status: s.status
  )
  Ok((next, to_view(next)))
end

## Commits one LLM/tool turn if active and within both step and exact Decimal USD budgets.
fn record_turn(s: State, _now: Time, input_tokens: Int, output_tokens: Int) : Result(
  (State, CoordinatorView),
  BudgetError
)
  return Error(RunNotActive) if not (s.status is Running)
  cost = try token_cost(input_tokens, output_tokens)
  if s.steps >= s.max_steps
    return Error(StepLimitReached(max_steps: s.max_steps))
  end
  next_spent = s.spent_usd + cost
  if next_spent > s.budget_usd
    return Error(BudgetExceeded(spent_usd: next_spent, budget_usd: s.budget_usd))
  end
  next = State(
    steps: s.steps + 1,
    max_steps: s.max_steps,
    spent_usd: next_spent,
    budget_usd: s.budget_usd,
    subagents: s.subagents,
    status: s.status
  )
  Ok((next, to_view(next)))
end

## Records a scoped subagent delegation report (capped at 8 subagents per task).
fn record_subagent(s: State, _now: Time, role: Text, summary: Text) : Result(
  (State, SubagentRecord),
  BudgetError
)
  return Error(RunNotActive) if not (s.status is Running)
  if s.subagents.size >= 8
    return Error(SubagentLimitReached(max_subagents: 8))
  end
  rec = SubagentRecord(id: s.subagents.size + 1, role: role, summary: summary)
  next = State(
    steps: s.steps,
    max_steps: s.max_steps,
    spent_usd: s.spent_usd,
    budget_usd: s.budget_usd,
    subagents: s.subagents.append(rec),
    status: s.status
  )
  Ok((next, rec))
end

## Marks the run Completed or Stopped.
fn finish(s: State, _now: Time, completed: Bool, reason: Text) : (State, CoordinatorView)
  next_status = if completed: Completed else: Stopped(reason: reason)
  next = State(
    steps: s.steps,
    max_steps: s.max_steps,
    spent_usd: s.spent_usd,
    budget_usd: s.budget_usd,
    subagents: s.subagents,
    status: next_status
  )
  (next, to_view(next))
end

## Reads an immutable snapshot of the coordinator state.
fn view(s: State, _now: Time) : CoordinatorView
  to_view(s)
end
