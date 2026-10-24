# SAR proofs (`.sarp`)

A SAR proof records the same information as a Source demo, without making the
engine record a demo. It exists because demos cost time and change the game:

- **Demo delay.** In single player the engine normally copies entity state from
  the server to the client directly (the "local network backdoor").
  `CL_CheckClientState` turns the backdoor off whenever a demo is recording,
  so starting a demo (or recording through a load) means a full network update
  and network serialization of every entity from then on. `record` also asks
  for a full update itself, and every demo start sends `demorestart`, which
  makes the server reset the HUD (`ForceClientDllUpdate`) and restart ambient
  sounds.
- **Gameplay impact.** With the backdoor off the client runs on networked,
  quantized values instead of the server's own; the `demorestart` side effects
  above happen at every load.
- **Not customizable.** The demo format has no room for anything else.

A proof recording never touches any of that: the backdoor stays on, nothing is
requested from the server, and the game's state is never modified.

## Using it

| Command | |
|---|---|
| `sar_proof_record <name>` | Record `<game>/<name>.sarp`. Keeps recording across loads and level changes until stopped. Can start in the menu (recording begins at the next load) or in the middle of a map. |
| `sar_proof_stop` | Stop and finalize the file. |
| `sar_proof_replace_demos 1` | Make `record`/`stop` record proofs, so existing binds and configs keep working. |
| `sar_proof_play <file> [segment]` | Convert to temporary demos in `<game>/sarproof_play` and play them in order. |
| `sar_proof_export <file> [base]` | Convert to `<base>.dem`, `<base>_2.dem`, ... (one demo per segment). |
| `sar_proof_info <file>` | Verify the file and list its segments. |

`tools/sarproof` builds a standalone `sarproof.exe` with `info` and `convert`
(and a `roundtrip` self-test that wraps demos as proofs and converts them
back), so proofs can be checked and converted without the game.
`tools/sarproof/sarpdump.py` is a small reference reader that lists a proof's
records.

SAR's own demo data (timer, splits, checksums, cvars, inputs, ...) is written
into proofs the same way it is written into demos.

## How it records

| What | Where it comes from |
|---|---|
| Network packets | `CDemoRecorder::RecordMessages`/`RecordPacket`. The client's net channel always calls these (whether or not a demo is recording), so every packet the client processes is captured bit for bit. |
| Entities (single player) | Hooks on `PackEntities_NetworkBackDoor` and `CLocalNetworkBackdoor::EndEntityStateUpdate`. After the backdoor copies a snapshot, the recorder reads back which entities it created, changed, made dormant or deleted, and encodes them with the engine's own `SendTable_Encode`, `SendTable_CalcDelta`, `CullPropsFromProxies` and `SendTable_WritePropList`, exactly as `SV_WriteEnterPVS`/`SV_WriteDeltaEntity` would. The result is the body of an `svc_PacketEntities` message. |
| Entities (co-op, or with the backdoor off) | Already in the network packets. |
| String tables (single player) | The backdoor copies server string table changes straight into the client's tables (`CNetworkStringTableContainer::DirectUpdate`). Before each copy, the recorder records the difference between each changed server table and its client mirror. |
| Instance baselines | Entering entities are encoded against their class baseline. The backdoor never needs baselines, so the server only has them for classes that existed at map load; for other classes the proof carries one synthesized from the entity's state (what `SV_EnsureInstanceBaseline` does when an entity is networked). The game's tables are never changed. |
| Signon data | Cached for every map, recording or not, so a recording started mid-map still has it. |
| User commands | `CInput::CreateMove`, encoded with `CInput::EncodeUserCmdToBuffer` like `RecordUserInput`. |
| Console commands | `CDemoRecorder::RecordCommand` (also always called). |
| View info | `CDemoRecorder::GetClientCmdInfo`, like a demo. |
| Data tables | `DataTable_WriteSendTablesBuffer`/`WriteClassInfosBuffer`, once per file. |

A recording started mid-map in single player starts with a keyframe: every
entity the client has (from the backdoor) and a copy of every string table, so
it needs no full update at all. In co-op there is no backdoor to read from, so
a mid-map start requests a full update like a demo start does.

Everything on the game thread is a memory copy or the engine's own encoder
(about 0.1 ms per snapshot); compression and disk writes run on a background
thread.

## Playback

Proofs play back by converting them to ordinary demos, one per segment (a
segment is one map load). The converter puts the recorded packets back into
demo frames, replaces the `svc_PacketEntities` messages the client ignored
(backdoor active) with the recorded entity snapshots, inserts the string table
updates as `svc_UpdateStringTable` messages, and writes the data tables,
custom data table, user commands, console commands and SAR data at the right
ticks. The output is a normal demo, so playback, rendering and every demo tool
work unchanged. Conversion is deterministic.

The converted demos start with complete state: there are no "broken" ticks at
the start of each segment, since the stream is captured from the signon on
instead of from a forced full update that arrives a few ticks late.

## File format

All integers are little-endian. `varint` is LEB128; `string` and `blob` are a
varint length followed by the bytes.

```
FileHeader (64 bytes)
  char[8]  "SARPROOF"
  u32      format version (1)
  u32      header size (64)
  u64      creation time (unix seconds)
  ...      zero

Chunk (repeated)
  u32      "CHNK"
  u32      chunk index (0, 1, 2, ...)
  u32      raw size
  u32      stored size
  u32      flags (1 = zlib deflate)
  u32      CRC-32 of the stored bytes
  stored bytes

Trailer (written by a clean stop)
  u32      "SEND"
  u32      chunk count
  u8[64]   final hash
  u8[64]   ed25519 signature of the final hash (zero without a key)
  u8[32]   public key (zero without a key)
```

The raw bytes of a chunk are whole records: `u8 type, varint length, payload`.
Chunks are flushed about twice a second, so a recording cut short by a crash is
readable up to its last chunk.

Integrity: `hash0 = SHA-512(file header)`, `hash[i] = SHA-512(hash[i-1] || raw
chunk i)`. The trailer stores the final hash, signed with SAR's demo signing
key in release builds.

### Records

| Type | Name | Payload |
|---|---|---|
| 0x01 | FILE_INFO | varint n, n × (string key, string value) |
| 0x02 | DATATABLES | `dem_datatables` blob |
| 0x03 | CUSTOMDATA_TABLE | varint n, n × string (demo custom data callback names) |
| 0x10 | SEGMENT_BEGIN | varint index, u8 flags (1 = started mid-map, 2 = multiplayer clock), string map, string game dir, string server, string client, i32 network protocol, f32 interval per tick, varint max clients, varint server class count |
| 0x11 | SEGMENT_END | i32 clock, string reason |
| 0x12 | SYNC | i32 clock of demo tick 0 |
| 0x20 | PACKET | u8 flags (1 = signon, 2 = backdoor active), i32 clock, u8[152] democmdinfo, i32 in sequence, i32 out sequence ack, varint bit count, message bits |
| 0x21 | ENTITIES | i32 server tick, u8 flags (1 = delta from the previous snapshot), varint max entries, varint updated entries, varint bit count, `svc_PacketEntities` entity data |
| 0x22 | STRINGTABLE | i32 tick (-1), string table, varint n, n × (varint index, u8 flags (1 = string, 2 = data), [string], [blob]) |
| 0x23 | USERCMD | i32 clock, i32 command number, u8 slot, blob encoded command |
| 0x24 | CONSOLECMD | i32 clock, u8 flags (1 = FCVAR_DEMO value at segment start), string |
| 0x25 | CUSTOMDATA | i32 clock, i32 callback, blob (SAR's data uses callback 0) |
| 0x27 | EVENT | u8 kind, i32 clock, string |
| 0x7F | END | empty |

The clock is the value `GetRecordingTick` counts from: the client tick count,
or `net_time` in ticks with more than one player.

## Verification

Tested on Portal 2 (and Portal Stories: Mel; the signatures also match the
Aperture Tag and Thinking with Time Machine engines):

- Every converted demo decodes completely with an independent parser: every
  entity property of every snapshot, and every bit consumed.
- `sarproof roundtrip` reproduces six real demos (single player and co-op,
  ~19 000 packets) byte for byte.
- Entity state during playback compared with the live game at every server
  tick (`sar_proof_debug_dump`): the same entities on every tick, with origins
  and angles within the network encoding's precision. The only differences
  are ones an ordinary demo of the same moment has as well (client-side
  smoothing of the local player while landing or turning, and a static entity
  whose cell-relative origin decodes one cell off).
- No delay: after a load, frame times and server ticks with a proof recording
  match not recording at all; recording a demo through the same loads cost an
  extra ~25 ms frame and two server ticks per load on the test machine.
- Size: about 5-8 KB/s, against 15-25 KB/s for demos (a proof is around a
  third of the size of the demos it converts to).
- Killed mid-recording, a proof stays readable and convertible up to its last
  chunk.

## Limitations

- Windows only for now (the engine signatures are Windows ones).
- Playback goes through the engine's demo player, so it has the same
  client-side behaviour as demo playback.
- Converted demos don't carry SAR's demo signature; the proof itself is the
  signed artifact.
