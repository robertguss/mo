module Map
expose update

## Sets the key to f of its value, or of default when it has none.
fn update(entries: Map(K, V), key: K, default: V, f: fn(V) : V) : Map(K, V)
  examples
    {"a": 1}.update("a", 0, fn(n) n + 1 end) == {"a": 2}
    {"a": 1}.update("b", 10, fn(n) n + 1 end) == {"a": 1, "b": 11}
  end

  current = entries.get(key) or default
  entries.set(key, f(current))
end
