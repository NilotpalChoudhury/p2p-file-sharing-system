# P2P File Sharing System

A peer-to-peer file sharing system (mini BitTorrent) in C++, using raw POSIX
sockets. Implements the complete system: **User & Group Management**,
**Multi-Tracker Synchronization**, and **File Sharing** (piece chunking,
SHA1 integrity, multi-peer downloads).

## Compilation and Execution

A single top-level `Makefile` builds both binaries by delegating into each
component's own Makefile:

```bash
make          # builds ./tracker/tracker and ./client/client
make clean    # removes both
```

Each component can also be built directly if only one binary is needed:
```bash
cd tracker && make
cd ../client && make
```

Built and tested with `g++ -std=c++17 -Wall -Wextra -pthread`, zero warnings.

**Run a tracker:**
```bash
./tracker <tracker_info.txt> <tracker_no>
```
`tracker_info.txt` lists exactly two "IP:PORT" lines; `tracker_no` (1 or 2)
selects which line is this process. Type `quit` at the console to shut down.

**Run a client:**
```bash
./client <tracker_ip>:<tracker_port> <tracker_info.txt>
```
Drops into a command REPL supporting every command from spec 4.1 and 4.2:
```text
create_user <user_id> <password>          list_groups
login <user_id> <password>                list_requests <group_id>
logout                                    accept_request <group_id> <user_id>
create_group <group_id>                   upload_file <group_id> <file_path>
join_group <group_id>                     list_files <group_id>
leave_group <group_id>                    download_file <group_id> <file_name> <dest_path>
                                           show_downloads
                                           stop_share <group_id> <file_name>
```
`tracker_info.txt` is actively used for client-side failover: if the given
tracker is unreachable (at startup or mid-session), the client automatically
retries against the other address.

## Architecture

Three kinds of TCP connection, per spec 3.2:
```
 client ──────► tracker      (user/group commands, file metadata)
 client ◄─────► client        (direct file piece transfer)
 tracker ◄────► tracker       (state synchronization)
```
Each tracker handles every client on its own thread and keeps one
`TrackerState` (users, groups, file metadata) behind a mutex, plus one
persistent `PeerLink` to the other tracker for real-time replication. Only
the lower-numbered tracker dials the other; the higher one just accepts.

Each client runs three things concurrently: the REPL thread, a background
seeding thread (accepts incoming peer connections and answers piece
requests), and — while a download is active — a small pool of worker
threads pulling pieces from one or more peers. The tracker never touches
file bytes; it only ever exchanges metadata.

```
common/    protocol.h, network.h/.cpp, sha1.h/.cpp, file_pieces.h/.cpp
tracker/   tracker_state.h/.cpp (state + rules), tracker.cpp (networking, dispatch, sync)
client/    client.cpp (REPL, seeding side, downloading side)
```

## Key Algorithms

**Message framing.** TCP has no message boundaries, so every message is a
fixed 8-byte header (payload length, command, status) followed by exactly
that many bytes. `send_all`/`recv_all` loop until the full amount has
transferred, since one `send()`/`recv()` call isn't guaranteed to move
everything at once.

**Multi-tracker synchronization** uses two mechanisms together: real-time
replication of every mutating command (including `upload_file` and seeder
changes) over the persistent tracker-tracker link, and a full snapshot
exchange whenever that link (re)connects — the snapshot is what actually
guarantees recovery; replication is an optimization on top of it. Three
merge rules handle three different kinds of conflict:
- **Group membership** merges per-user via add/remove sequence counters
  (an OR-Set), so two trackers editing different members of the same
  group while partitioned both survive a reconnect.
- **Naming conflicts** (`create_user`, `create_group`, `upload_file` all
  independently claiming the same identifier) resolve by earliest
  creation timestamp wins outright, tie-broken by comparing the
  colliding value as a string — deterministic on both sides.
- **A file's seeder set** (Day 4) is unioned across trackers rather than
  merged per-element, since it's an unbounded, ownerless set with no
  natural per-item counter.

**Owner succession**: when an owner leaves, the group isn't deleted — the
active member with the earliest join sequence becomes the new owner.

**Idempotent replay**: every mutating method takes an `is_replay` flag,
so re-applying an already-accepted operation (via replication or a
snapshot) succeeds silently instead of erroring, since neither
replication nor snapshot merge guarantees exactly-once delivery.

**File chunking and integrity**: files split into fixed 512KB pieces
(final piece smaller if needed), read via raw `open`/`read` syscalls
through one reused buffer so memory use stays constant regardless of
file size. SHA1 is implemented from scratch (FIPS 180-1, no external
crypto library), verified against known test vectors and cross-checked
against `sha1sum`.

**Multi-peer downloads**: the tracker hands out every current seeder for
a file, not just the uploader. A small worker pool (capped at 4 peers)
pulls from one shared queue of needed pieces; each verified piece is
written with `pwrite()` at its exact offset, so disjoint writes need no locking. A
completed download re-verifies the whole file and registers this client
as a new seeder, growing the pool over time.

**Corrupted-piece exclusion**: a hash mismatch requeues the piece and
records which peer failed it (`failed_peers[index]`), so a retry is
*structurally* guaranteed to land on a different peer, not just likely
to — and a peer disconnecting mid-transfer is handled by the identical
requeue path, since both look the same at the protocol layer.

**Client-side tracker failover**: every client is given the full
`tracker_info.txt`. A failed request triggers a reconnect to the other
tracker and a silent re-login (session state is per-connection, not
synchronized), then a transparent retry of the original request.

## Data Structures and Rationale

- **`User`** — flat record; a creation timestamp resolves naming
  conflicts on merge.
- **`Group`** — per-user add/remove counter maps (not a plain set) for
  the membership merge above; a `files` map for that group's shared files.
- **`FileInfo`** — name, size, whole-file hash, piece hashes, the
  original uploader (kept for display), and the *live* `seeder_addresses`
  list a downloader actually gets handed — kept separate so the seeder
  set can grow/shrink without touching the permanent catalog entry.
- **`TrackerState`** — owns all of the above behind one mutex; no
  networking knowledge, so it's unit-testable in isolation.
- **`PeerLink`** — the tracker-tracker socket behind a mutex, so
  concurrent replicate() calls can't interleave on the wire.
- **`DownloadState`** (client-side) — one per download: the shared
  piece queue, the per-peer failure-exclusion set, and completion state.
  One instance per download is what lets several run concurrently with
  no cross-download locking.
- **`SeedingFiles`** / **`SeederRegistrations`** (client-side) — what
  this client actually answers piece requests for, and what it's told
  the tracker it seeds, respectively; `stop_share`/`logout` clear both.

## Network Protocol Design and Message Formats

Every message: 8-byte header (`payload_length` u32, `command` u16,
`status` u16, sent as separate fields) + payload. `status` is `STATUS_OK`
(0) or `STATUS_ERROR` (1) on responses. Lists (piece hashes, seeder
addresses) travel as one comma-joined field, `-` for empty.

**Client → tracker:**

| Command | Payload |
|---|---|
| `create_user` / `login` | `<user_id> <password>` |
| `logout` | *(empty)* — also stops sharing every file this client seeds |
| `create_group` / `join_group` / `leave_group` | `<group_id>` |
| `list_groups` | *(empty)* |
| `list_requests` | `<group_id>` |
| `accept_request` | `<group_id> <user_id>` |
| `upload_file` | `<group_id> <filename> <file_size> <file_hash> <uploader_address> <piece_hashes_csv>` |
| `list_files` | `<group_id>` |
| `MSG_GET_FILE_INFO` (internal) | `<group_id> <filename>` → `<file_size> <file_hash> <peer_addresses_csv> <piece_hashes_csv>` |
| `MSG_REGISTER_SEEDER` (internal) | `<group_id> <filename> <address>` |
| `stop_share` | `<group_id> <filename> <address>` |

**Tracker ↔ tracker:** `MSG_TRACKER_HELLO` (handshake), `MSG_SYNC_OP`
(replicate one operation, e.g. `UPLOAD_FILE ...`, `ADD_SEEDER ...`),
`MSG_SYNC_SNAPSHOT` (full state, exchanged on every reconnect).

**Client ↔ client:** `MSG_PEER_PIECE_REQUEST` — `<filename> <piece_index>`
→ the piece's raw bytes on success, a text reason on failure. The only
message type that never touches a tracker.

**Snapshot format** (one record per line):
```
USER <user_id> <password> <created_at_ms>
GROUP <group_id> <creator_id> <owner_id> <created_at_ms> <next_seq> <member_add_csv> <member_remove_csv> <pending_add_csv> <pending_remove_csv>
FILE <group_id> <filename> <uploader_id> <uploader_address> <file_size> <file_hash> <uploaded_at_ms> <piece_hashes_csv> <seeder_addresses_csv>
```

## Assumptions Made

- Commands use underscores (`create_user`), per the rendered PDF image
  rather than its text extraction, which had dropped them.
- `list_groups`, `list_files`, and `download_file` don't require login —
  treated as public reads, like a torrent tracker's listing; only
  `upload_file` requires group membership, per spec 2.1.
- Piece selection is sequential out of one shared queue, not
  rarest-first — left open by the spec, and sufficient for correctness.
- A completed download automatically becomes a new seed (spec 2.2
  describes every client as both downloader and seeder).
- Re-uploading identical content (same hash) under an existing filename
  is treated as re-registering as a seeder, not an error — this is what
  lets a file recover once every seeder has stopped sharing it, without
  allowing a name to silently change content.
- Passwords are stored in plaintext; nothing in the spec asks for hashing.
- A login session is local to one TCP connection, not synchronized
  between trackers (spec 7 only requires syncing users/groups/file
  sharing info); a client that fails over re-establishes its own session.
- One seed address per client process for its whole lifetime, not per
  login — so `logout`'s "stop sharing" cleanup covers everything that
  process has ever shared, not just the ending session's own uploads.

## Known Limitations

- No persistence to disk — if both trackers die at once, all state is lost.
- A naming conflict during a partition can't be prevented, only resolved
  deterministically afterward (inherent to staying available during a
  partition, per CAP).
- A file's seeder-set merge is add-only-safe: a `stop_share` issued
  during a partition can be resurrected by the other tracker's
  still-cached copy until reissued after reconnecting.
- The tracker-to-tracker socket has no send timeout; tested adversarially
  against a dead/paused peer with a realistically-sized message without
  reproducing an actual hang, but the theoretical gap remains for a
  message larger than this system ever generates.

## Testing

Verified directly against the real binaries (two trackers, up to three
concurrent clients): full user/group command coverage including error
paths and owner succession; cross-tracker sync and full-state recovery
after a tracker crash and restart; file upload/download across the
spec's size boundaries (under 512KB, exact multiples, many-piece files up
to ~950MB) and file types (text, binary, image, archive), checked
byte-for-byte via `sha1sum`; multi-peer downloads with a measured speedup
from using more than one peer; corrupted-piece recovery and mid-transfer
peer disconnects, both via a scripted peer for deterministic
reproduction; tracker failover mid-session and at startup; and
`logout`/`stop_share` correctly removing a client as a seeder, tracker-side
and locally. Concurrent independent edits and naming conflicts during a
partition were verified directly against `TrackerState` in isolation,
since real network-partition timing is impractical to script reliably.
`script.sh` automates an end-to-end run of most of the above against the
real binaries.
