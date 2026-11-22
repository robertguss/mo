## Ash/Ecto-style validation accumulator producing `Map(Text, List(Text))` and Inertia
## `useForm().errors` (`Record<string, string>`) JSON payloads.
module Arvo.Changeset
expose Changeset(..),
new,
add_error,
check,
require_non_empty,
require_length,
require_positive_int,
require_non_negative_int,
require_email,
require_one_of,
valid?,
first_errors,
errors_json,
error_map_json,
inertia_errors_json,
inertia_error_map_json,
finish
use Json

## Accumulates field-keyed validation errors alongside candidate `data`.
struct Changeset(T)
  data: T
  errors: Map(Text, List(Text))
end

## Starts a valid `Changeset` wrapping `data` with zero errors.
fn new(data: T) : Changeset(T)
  Changeset(data: data, errors: {})
end

## Appends `message` under `field` in `cs.errors`.
fn add_error(cs: Changeset(T), field: Text, message: Text) : Changeset(T)
  prev = cs.errors.get(field) or []
  Changeset(data: cs.data, errors: cs.errors.set(field, prev.append(message)))
end

## Records `message` under `field` when `ok?` is `false`.
fn check(cs: Changeset(T), field: Text, ok?: Bool, message: Text) : Changeset(T)
  if ok?
    cs
  else
    add_error(cs, field, message)
  end
end

## Requires `value` to contain at least one non-whitespace character.
fn require_non_empty(cs: Changeset(T), field: Text, value: Text) : Changeset(T)
  check(cs, field, not value.trim.empty?, "can't be blank")
end

## Requires `value.char_count` to fall within `[min, max]`.
fn require_length(cs: Changeset(T), field: Text, value: Text, min: Int, max: Int) : Changeset(T)
  n = value.char_count
  if n < min
    add_error(cs, field, "must be at least #{min} characters")
  else if n > max
    add_error(cs, field, "must be at most #{max} characters")
  else
    cs
  end
end

## Requires `value > 0`.
fn require_positive_int(cs: Changeset(T), field: Text, value: Int) : Changeset(T)
  check(cs, field, value > 0, "must be greater than 0")
end

## Requires `value >= 0`.
fn require_non_negative_int(cs: Changeset(T), field: Text, value: Int) : Changeset(T)
  check(cs, field, value >= 0, "must be 0 or greater")
end

fn valid_domain_label?(label: Text) : Bool
  return false if label.empty? or label.byte_count > 63
  return false if label.starts_with?("-") or label.ends_with?("-")
  label.chars.all?(fn(c) c.ascii_letter? or c.ascii_digit? or c == '-' end)
end

fn plausible_email?(raw: Text) : Bool
  s = raw.trim
  return false if s.empty? or s.byte_count > 254
  return false if s.chars.any?(fn(c) c.code <= 32 or c.code == 127 end)
  parts = s.split("@")
  return false if parts.size != 2
  local = parts.get(0) or ""
  domain = parts.get(1) or ""
  return false if local.empty? or local.byte_count > 64
  return false if local.starts_with?(".") or local.ends_with?(".") or local.contains?("..")
  labels = domain.split(".")
  return false if labels.size < 2
  tld = labels.last or ""
  return false if tld.byte_count < 2
  labels.all?(valid_domain_label?)
end

## Requires `value` to be a well-formed email address (`local@domain.tld`).
fn require_email(cs: Changeset(T), field: Text, value: Text) : Changeset(T)
  check(cs, field, plausible_email?(value), "must be a valid email address")
end

## Requires `value` to be an exact member of `allowed`.
fn require_one_of(cs: Changeset(T), field: Text, value: Text, allowed: List(Text)) : Changeset(T)
  check(cs, field, allowed.contains?(value), "must be one of: #{allowed.join(", ")}")
end

## Whether `cs` has accumulated zero validation errors.
fn valid?(cs: Changeset(T)) : Bool
  cs.errors.empty?
end

## Projects each field's error list down to its first message (`Record<string, string>`).
fn first_errors(errors: Map(Text, List(Text))) : Map(Text, Text)
  var flat = {}
  for (k, msgs) in errors
    if msgs.first is Some(first_msg)
      flat = flat.set(k, first_msg)
    end
  end
  flat
end

## Encodes a full `Map(Text, List(Text))` error bag as `Json`.
fn error_map_json(errors: Map(Text, List(Text))) : Json
  errors.to_json
end

## Encodes `cs.errors` as a full `Map(Text, List(Text))` `Json` object.
fn errors_json(cs: Changeset(T)) : Json
  error_map_json(cs.errors)
end

## Encodes a `Map(Text, List(Text))` error bag as a flat `Record<string, string>` `Json` object
## matching `@inertiajs/react` `useForm().errors`.
fn inertia_error_map_json(errors: Map(Text, List(Text))) : Json
  first_errors(errors).to_json
end

## Encodes `cs.errors` as a flat `Record<string, string>` `Json` object matching
## `@inertiajs/react` `useForm().errors`.
fn inertia_errors_json(cs: Changeset(T)) : Json
  inertia_error_map_json(cs.errors)
end

## Returns `Ok(cs.data)` when `cs` is valid, or `Error(cs.errors)` when any check failed.
fn finish(cs: Changeset(T)) : Result(T, Map(Text, List(Text)))
  promise "finish returns Ok iff valid? is true"
    (result is Ok(_)) == valid?(cs)
  end

  if cs.errors.empty?
    Ok(cs.data)
  else
    Error(cs.errors)
  end
end

struct SampleForm
  email: Text
  name: Text
  amount_cents: Int
  kind: Text
end

test "changeset accumulates multiple field errors and formats Inertia error maps"
  good = SampleForm(email: "ada@example.com", name: "Groceries", amount_cents: 2500, kind: "debit")
  cs_ok = new(good).require_email("email", good.email).require_non_empty("name", good.name)
    .require_length("name", good.name, 2, 40).require_positive_int(
      "amount_cents",
      good.amount_cents
    ).require_non_negative_int("amount_cents", 0).require_one_of(
      "kind",
      good.kind,
      ["debit", "credit"]
    )
  assert cs_ok.valid?
  assert finish(cs_ok) == Ok(good)
  assert cs_ok.inertia_errors_json == Json.Object({})

  bad = SampleForm(email: "not-an-email", name: " ", amount_cents: 0, kind: "wire")
  cs_bad = new(bad).require_email("email", bad.email).require_non_empty("name", bad.name)
    .require_length("name", bad.name, 3, 20).require_positive_int("amount_cents", bad.amount_cents)
    .require_non_negative_int("balance", -5).require_one_of("kind", bad.kind, ["debit", "credit"])
  assert not cs_bad.valid?
  assert finish(cs_bad) is Error(errs)
  assert errs.get("name") == Some(["can't be blank", "must be at least 3 characters"])
  assert cs_bad.errors_json.list_at("name") != None
  flat = cs_bad.inertia_errors_json
  assert flat.text_at("name") == Some("can't be blank")
  assert flat.text_at("email") == Some("must be a valid email address")
  assert flat.text_at("amount_cents") == Some("must be greater than 0")
  assert flat.text_at("balance") == Some("must be 0 or greater")
  assert flat.text_at("kind") == Some("must be one of: debit, credit")
end
