## Reads a playlist from standard input and prints its length and its longest track.
module Playlist
expose Track(..), parse_track, parse_duration, format_duration, summary, main

struct Track
  artist: Text
  title: Text
  seconds: Int
end

## Reads a duration such as `3:07`: minutes, a colon, and two digits of seconds below 60.
fn parse_duration(text: Text) : Option(Int)
  examples
    parse_duration("3:07") == Some(187)
    parse_duration("12:00") == Some(720)
    parse_duration("3:7") == None
    parse_duration("3:60") == None
  end

  case text.split(":")
    [minutes, seconds] if seconds.char_count == 2:
      m = try minutes.to_int
      s = try seconds.to_int
      return None if m < 0 or s < 0 or s > 59
      Some(m * 60 + s)
    _: None
  end
end

## Writes seconds as `M:SS`, or `H:MM:SS` from an hour up.
fn format_duration(seconds: Int) : Text
  examples
    format_duration(187) == "3:07"
    format_duration(59) == "0:59"
    format_duration(3600) == "1:00:00"
    format_duration(4321) == "1:12:01"
  end

  minutes = seconds / 60
  two = fn(n) "#{n}".pad_left(2).replace(" ", "0") end
  if minutes < 60
    "#{minutes}:#{two(seconds % 60)}"
  else
    "#{minutes / 60}:#{two(minutes % 60)}:#{two(seconds % 60)}"
  end
end

## Reads a line such as `Nina Simone - Feeling Good (2:53)`.
fn parse_track(line: Text) : Option(Track)
  examples
    parse_track("Nina Simone - Feeling Good (2:53)") ==
      Some(Track(artist: "Nina Simone", title: "Feeling Good", seconds: 173))
    parse_track("Feeling Good (2:53)") == None
    parse_track("Nina Simone - Feeling Good") == None
  end

  open = line.trim.drop_suffix(")")
  pieces = open.split(" (")
  return None if pieces.size < 2
  seconds = try parse_duration(pieces.last or "")
  name = pieces.take(pieces.size - 1).join(" (")
  parts = name.split(" - ")
  return None if parts.size < 2
  artist = (try parts.get(0)).trim
  title = parts.drop(1).join(" - ").trim
  return None if artist.empty? or title.empty?
  Some(Track(artist: artist, title: title, seconds: seconds))
end

## The playlist's summary lines: how many tracks, their total length, and the longest.
fn summary(tracks: List(Track)) : List(Text)
  examples
    summary([]) == ["0 tracks, 0:00"]
  end

  total = tracks.reduce(0, fn(sum, track) sum + track.seconds end)
  noun = if tracks.size == 1: "track" else: "tracks"
  var lines = ["#{tracks.size} #{noun}, #{format_duration(total)}"]
  case tracks.sort_by_desc(fn(track) track.seconds end).first
    Some(longest): lines = lines.append(
      "longest: #{longest.title} by #{longest.artist} (#{format_duration(longest.seconds)})"
    )
    None:
  end
  lines
end

fn main(console: Console) : Int
  var tracks = []
  var status = 0
  while console.read_line is Some(line)
    continue if line.trim.empty?
    case parse_track(line)
      Some(track): tracks = tracks.append(track)
      None:
        console.error("skipped: #{line}")
        status = 1
    end
  end
  for line in summary(tracks)
    console.print(line)
  end
  status
end
