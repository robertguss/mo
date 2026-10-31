module HttpBody
expose Phase(..), State(..), Bounds(..), Step(..), fixed, chunked, feed
use Http

enum Phase
  Fixed(Int)
  SizeLine(List(Byte))
  Data(Int)
  DataCr
  DataLf
  TrailerLine(List(Byte))
  Complete
end

struct State
  phase: Phase
  encoded: Int
  decoded: Int
  chunks: Int
  trailer_bytes: Int
  trailer_fields: Int
end

struct Bounds
  body: Int
  ingress: Int
  chunks: Int
  line: Int
  trailer_bytes: Int
  trailer_fields: Int
end

struct Step
  state: State
  consumed: Int
  payload: List(Http.Span)
end

fn fixed(length: Int) : State
  State(
    phase: if length == 0: Complete else: Fixed(length),
    encoded: 0,
    decoded: 0,
    chunks: 0,
    trailer_bytes: 0,
    trailer_fields: 0
  )
end

fn chunked() : State
  State(phase: SizeLine([]), encoded: 0, decoded: 0, chunks: 0, trailer_bytes: 0, trailer_fields: 0)
end

fn line_byte(line: List(Byte), value: Byte, limit: Int) : Result(List(Byte), Int)
  c = value.to_int
  previous = case line.last
    Some(last): last.to_int
    None: -1
  end
  return Error(400) if (previous == 13 and c != 10) or (c == 10 and previous != 13)
  return Error(400) if c == 0 or c == 127 or (c < 32 and c != 9 and c != 10 and c != 13)
  if c != 10 and c != 13
    return Error(413) if line.size >= limit
  end
  Ok(line.append(value))
end

fn feed(state: State, bytes: List(Byte), bounds: Bounds) : Result(Step, Int)
  var phase = state.phase
  var encoded = state.encoded
  var decoded = state.decoded
  var chunks = state.chunks
  var trailer_bytes = state.trailer_bytes
  var trailer_fields = state.trailer_fields
  var at = 0
  var payload = []
  while at < bytes.size
    break if phase == Complete
    return Error(413) if encoded >= bounds.ingress
    case phase
      Fixed(remaining) | Data(remaining):
        return Error(400) if remaining < 0
        return Error(413) if remaining > bounds.body - decoded
        count = if remaining < bytes.size - at: remaining else: bytes.size - at
        return Error(413) if count > bounds.ingress - encoded
        if count > 0
          payload = payload.append(Http.Span(start: at, size: count))
        end
        at += count
        encoded += count
        decoded += count
        case phase
          Fixed(_): phase = if count == remaining: Complete else: Fixed(remaining - count)
          Data(_): phase = if count == remaining: DataCr else: Data(remaining - count)
          SizeLine(_) | DataCr | DataLf | TrailerLine(_) | Complete: return Error(400)
        end
      SizeLine(line):
        value = try bytes.get(at).or_fail(400)
        read = try line_byte(line, value, bounds.line)
        at += 1
        encoded += 1
        if value.to_int == 10
          size = case Http.chunk_size(read, bounds.body - decoded, bounds.line)
            Ok(value_size): value_size
            Error(Http.TooLarge): return Error(413)
            Error(_): return Error(400)
          end
          if size == 0
            phase = TrailerLine([])
          else
            return Error(413) if chunks >= bounds.chunks
            chunks += 1
            phase = Data(size)
          end
        else
          phase = SizeLine(read)
        end
      DataCr:
        value = try bytes.get(at).or_fail(400)
        return Error(400) if value.to_int != 13
        at += 1
        encoded += 1
        phase = DataLf
      DataLf:
        value = try bytes.get(at).or_fail(400)
        return Error(400) if value.to_int != 10
        at += 1
        encoded += 1
        phase = SizeLine([])
      TrailerLine(line):
        return Error(413) if trailer_bytes >= bounds.trailer_bytes
        value = try bytes.get(at).or_fail(400)
        read = try line_byte(line, value, bounds.line)
        at += 1
        encoded += 1
        trailer_bytes += 1
        if value.to_int == 10
          if read.size == 2
            phase = Complete
          else
            return Error(413) if trailer_fields >= bounds.trailer_fields
            finish = case Http.line_end(read, 0, bounds.line)
              Ok(value_finish): value_finish
              Error(Http.TooLarge): return Error(413)
              Error(_): return Error(400)
            end
            case Http.field(read, 0, finish)
              Ok(_): Unit
              Error(_): return Error(400)
            end
            trailer_fields += 1
            phase = TrailerLine([])
          end
        else
          phase = TrailerLine(read)
        end
      Complete: break
    end
  end
  Ok(
    Step(
      state: State(
        phase: phase,
        encoded: encoded,
        decoded: decoded,
        chunks: chunks,
        trailer_bytes: trailer_bytes,
        trailer_fields: trailer_fields
      ),
      consumed: at,
      payload: payload
    )
  )
end
