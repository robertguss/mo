## Tests of the Playlist module.
module PlaylistTest
use Playlist{Track}

test "durations are minutes and two digits of seconds"
  assert Playlist.parse_duration("3:07") == Some(187)
  assert Playlist.parse_duration("0:00") == Some(0)
  assert Playlist.parse_duration("75:30") == Some(4530)
  assert Playlist.parse_duration("3:7") == None
  assert Playlist.parse_duration("3:60") == None
  assert Playlist.parse_duration("-1:00") == None
  assert Playlist.parse_duration("3") == None
  assert Playlist.parse_duration("1:02:03") == None
end

test "durations are written back with an hour when there is one"
  assert Playlist.format_duration(0) == "0:00"
  assert Playlist.format_duration(187) == "3:07"
  assert Playlist.format_duration(3599) == "59:59"
  assert Playlist.format_duration(3600) == "1:00:00"
  assert Playlist.format_duration(36061) == "10:01:01"
end

test "a track is an artist, a title and a duration"
  assert Playlist.parse_track("Nina Simone - Feeling Good (2:53)") ==
    Some(Track(artist: "Nina Simone", title: "Feeling Good", seconds: 173))
  assert Playlist.parse_track("  Can - Vitamin C (3:32)  ") ==
    Some(Track(artist: "Can", title: "Vitamin C", seconds: 212))
end

test "only the first dash ends the artist, and only the last bracket starts the duration"
  assert Playlist.parse_track("Sly - Thank You - Live (Remastered) (6:01)") ==
    Some(Track(artist: "Sly", title: "Thank You - Live (Remastered)", seconds: 361))
end

test "a line without an artist, a title or a duration isn't a track"
  assert Playlist.parse_track("Feeling Good (2:53)") == None
  assert Playlist.parse_track("Nina Simone - Feeling Good") == None
  assert Playlist.parse_track("Nina Simone - (2:53)") == None
  assert Playlist.parse_track("Nina Simone - Feeling Good (2:5)") == None
end

test "the summary counts the tracks and names the longest"
  tracks = [
    Track(artist: "A", title: "One", seconds: 100),
    Track(artist: "B", title: "Two", seconds: 300),
    Track(artist: "C", title: "Three", seconds: 300)
  ]
  assert Playlist.summary(tracks) == ["3 tracks, 11:40", "longest: Two by B (5:00)"]
  assert Playlist.summary([Track(artist: "A", title: "One", seconds: 61)]) ==
    ["1 track, 1:01", "longest: One by A (1:01)"]
  assert Playlist.summary([]) == ["0 tracks, 0:00"]
end

test "main skips lines it can't read"
  console = Console.fake_input(["A - One (1:00)", "", "nothing here", "B - Two (59:00)"])
  assert Playlist.main(console) == 1
  assert console.printed == ["2 tracks, 1:00:00", "longest: Two by B (59:00)"]
  assert console.printed_errors == ["skipped: nothing here"]
end
