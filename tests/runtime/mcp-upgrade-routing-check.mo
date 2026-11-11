module McpUpgradeRoutingCheck
use McpViews
use McpFetch
use Json

fn main() : Int
  if McpViews.names().get(12) != Some("mo-future-view")
    stop "fixture did not append its view"
  end
  if McpViews.upgrade_kind(12) != 0
    stop "appended ordinary view routed as upgrade"
  end
  if McpViews.upgrade_kind(9) != 1 or
    McpViews.upgrade_kind(10) != 2 or
    McpViews.upgrade_kind(11) != 3
    stop "upgrade names lost their routing"
  end
  var seen_future = false
  for tool in McpViews.catalog()
    fields = case tool
      Json.Object(value): value
      Json.Null | Json.Boolean(_) | Json.Number(_) | Json.String(_) | Json.Array(_):
        stop "catalog tool"
    end
    name = fields.get("name")
    annotations = fields.get("annotations")
    if name == Some(Json.String("mo-future-view"))
      seen_future = true
      if annotations !=
        Some(Json.Object({"readOnlyHint": Json.Boolean(true), "idempotentHint": Json.Boolean(true)}))
        stop "appended view lost readOnlyHint"
      end
    else if name == Some(Json.String("mo-upgrade-open")) or
      name == Some(Json.String("mo-upgrade-page"))
      if annotations !=
        Some(
          Json.Object({
            "readOnlyHint": Json.Boolean(false),
            "destructiveHint": Json.Boolean(false),
            "idempotentHint": Json.Boolean(false),
            "openWorldHint": Json.Boolean(false)
          })
        )
        stop "upgrade tool annotations"
      end
    else if name == Some(Json.String("mo-upgrade-commit"))
      if annotations !=
        Some(
          Json.Object({
            "readOnlyHint": Json.Boolean(false),
            "destructiveHint": Json.Boolean(true),
            "idempotentHint": Json.Boolean(false),
            "openWorldHint": Json.Boolean(false)
          })
        )
        stop "commit annotations"
      end
    end
  end
  if not seen_future
    stop "fixture did not catalog its view"
  end
  for effect in ["none", "applied", "unknown"]
    body = Json.Object({
      "jsonrpc": Json.String("2.0"),
      "id": Json.int(1),
      "result": Json.Object({
        "isError": Json.Boolean(true),
        "structuredContent": Json.Object({
          "error": Json.Object({
            "kind": Json.String("runtime"),
            "code": Json.String("rate_limited"),
            "effect": Json.String(effect)
          })
        })
      })
    }).to_text
    wire = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: #{body.byte_count}\r\n\r\n#{body}"
    expected = if effect == "none": ["rate_limited"] else: []
    if McpFetch.upgrade(wire) != expected
      stop "only no-effect rate refusal permits retry"
    end
  end
  37
end
