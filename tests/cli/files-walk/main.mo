## Walks tree/ in the folder it is started in; ci/check_cli.py builds the tree, with a link and
## a folder it can't read, in a fresh temporary folder.
module Main

fn main(console: Console, files: Files)
  case files.walk("tree")
    Ok(entries):
      for entry in entries
        kind = case entry.kind
          File: "file"
          Folder: "folder"
          Link: "link"
          Other: "other"
        end
        console.print("#{entry.name} #{kind}")
      end
    Error(failure): console.print("error: #{failure}")
  end
end
