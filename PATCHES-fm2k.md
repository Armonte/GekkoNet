# fm2k branch — the FM2K port's GekkoNet patches

Base: upstream `HeatXD/GekkoNet` `0d491a4` (`v20261009095145`, 2026-10-09). This branch stays as
close to upstream as possible: rebase onto a newer upstream tip rather than merging, and keep the
delta small and surgical.

Applied here:

1. **`gekko_confirmed_frame()`** (`gekkonet.h`, `gekkonet.cpp`, and `ConfirmedFrame()` on the session
   interface + game/spectator/stress sessions). Highest frame whose inputs from ALL players were
   actually received, never predicted. Gates replay / spectator recording on confirmed inputs.
2. **Stale-ack guard in `MessageSystem::OnInputAck`** (`backend.cpp`): reject an ack that covers
   frames we never sent (plus the spectator queue). A stale cross-session ack otherwise poisons
   `last_acked_frame`, makes `SendInputsToPeer`'s "peer is caught up" gate permanently true, and this
   side silently stops sending inputs forever while ping/health keep flowing. Provenance: local
   wallclock/ack notes taken from the Avatar Fighting Legends RealMatch netcode.
3. **Silent magic-mismatch drop** (`backend.cpp`): upstream `printf("dropped packet!")` fires during
   every session start (each peer holds its own `_session_magic` until SyncRequest/Response reconcile
   them) and reads as packet loss. Drop it silently.
4. **Health-check slot validation** (`game_session.cpp`): `StateStorage::GetState` is a bare ring
   index with no validation; once `confirmed` falls further behind than the ring is deep (packet loss,
   high delay) the slot holds a newer frame and its plausible non-zero checksum would be sent labelled
   `confirmed`, tearing down a healthy session. Skip a slot whose `frame != confirmed`.

Deliberately NOT here:

- **Late spectator join** — upstreamed (#56, `mid-session-join`); the old wanwan `input_history_size`
  patch is gone.
- **Candidate D (advantage mirror after a stale-ack episode)** — pending an A/B against upstream's
  newer delay-advantage handling (`0d491a4`), which likely supersedes it. Its diagnostic counters may
  land separately.
- **`CMakeLists.txt` local edits** (C++17, include dirs) from wanwan — not needed; upstream builds.

Convergence target and verification: `fm2k-port/docs/GEKKONET_AUDIT.md`.
