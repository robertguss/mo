## Agent-Native Architecture Review

Mo exposes operator capabilities directly through its authenticated MCP catalog and CLI; no separate interactive UI creates a parity requirement. Retained failure discovery, refusal inspection, and paged private exports have matching agent-callable primitives. Independent state/failure pins and explicit refusal output support composition. Known pagination, signature, and omitted-count fixes assigned by the caller are excluded here.

### Finding

- **P2 — Remove the obsolete unavailability claim from the embedded agent guide.** `docs/agent-guide.md:280-281` still says: “Failure capture and failure-kind export remain unavailable.” The same diff updates line 27 to advertise capture, `mo-failures`, and `mo fetch --export failure ID`. Agents reading the dedicated fetch instructions receive directly contradictory capability guidance and may skip the supported failure-artifact workflow. Update the fetch section with the supported failure selector and retain only the U5 offline limitation. Confidence: 100; autofix: gated_auto; owner: downstream-resolver; pre-existing: false (the old limitation becomes false through U4); requires verification: false.

### Capability map

| Capability | Agent surface | Assessment |
| --- | --- | --- |
| Discover retained failures | `mo-failures`, authenticated tools/list | Accessible |
| Inspect capture refusals | `mo-failures`, `mo-supervisor` | Accessible |
| Read complete private failure bytes | `mo-export` with failure selector | Accessible |
| Save verified private artifact | `mo fetch --export failure ID` | Accessible |

Static review only; no builds or tests run. Verdict: NEEDS WORK for the conflicting guide text; no additional agent-parity findings.
