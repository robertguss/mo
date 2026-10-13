module Main
fn main() : Int
_entry = Entry(name: "a", kind: File).to_text
if NotFound(path: "a") == Failed(path: "a", reason: "b"): 1 else: 0
end
