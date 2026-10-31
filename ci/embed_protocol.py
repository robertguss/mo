import json
import pathlib
import sys

out = pathlib.Path(sys.argv[1])
parts = []
sources = []
for index, name in enumerate(sys.argv[2:]):
    data = pathlib.Path(name).read_bytes()
    symbol = f"mo_protocol_source_{index}"
    parts.append(f"static const unsigned char {symbol}[] = {{\n")
    for start in range(0, len(data), 24):
        parts.append("    " + ",".join(str(b) for b in data[start:start + 24]) + ",\n")
    parts.append("    0\n};\n")
    sources.append(f"    {{{json.dumps(name)}, {symbol}, {len(data)}}},\n")
parts.append("static const struct {\n    const char *path;\n"
             "    const unsigned char *bytes;\n    size_t size;\n"
             "} mo_protocol_sources[] = {\n")
parts.extend(sources)
parts.append("};\n")
data = "".join(parts).encode("ascii")
out.parent.mkdir(parents=True, exist_ok=True)
if not out.exists() or out.read_bytes() != data:
    out.write_bytes(data)
