import json
import sys

body, folder = sys.argv[1], sys.argv[2]
document = json.load(open(body, encoding="utf8"))


def escaped(value):
    if isinstance(value, str):
        return value + ' "q" \n\t\\ é'
    if isinstance(value, list):
        return [escaped(item) for item in value]
    if isinstance(value, dict):
        return {key: escaped(item) for key, item in value.items()}
    return value


def write(name, text):
    open(f"{folder}/{name}.json", "w", encoding="utf8").write(text)


write("pretty", json.dumps(document, indent=2, ensure_ascii=False))
write("escapes", json.dumps(escaped(document), ensure_ascii=True, separators=(",", ":")))
write("numbers", json.dumps([[i, i * 1.5, -i, i / 7] for i in range(30000)], separators=(",", ":")))
