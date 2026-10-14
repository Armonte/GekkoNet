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

5. **Connect-race guard in `GameSession::Poll`** (`game_session.cpp`, from `Armonte/GekkoNet ymir`
   `610080d`): `CheckStatusActors()` now runs only once the session `_started`. Unguarded, a peer could
   mark its remotes connected and start while they were still syncing, then start far ahead and roll
   back continuously. Before the start `AllActorsValid()` already drives the handshake; late spectators
   need the per-poll handshake only once running. Measured on the fm2k-port 2-peer loopback as a
   persistent one-sided asymmetry (one peer ~222 rollbacks, the other 0); guard removes it.

Also on `fm2k` (canonical convergence, 2026-10-14; ymir fork supplies the middle layer):

6. **`GekkoConfig::nonportable_saves`** (`gekkonet.h`, `game_session.cpp`; ymir `8285ff5`): a session whose
   save event hands out a process-local buffer (a work-RAM delta against a local base) cannot serve a late
   spectator from the rollback storage. With the flag set, `PrepareSpectatorStates` uses only the on-demand
   state captured with `portable = true`.
7. **Capture late spectator states from the frame before the confirmed one** (`game_session.cpp`; ymir
   `932a4e3`): without limited saving the latest save is already past the confirmed frame, so
   `CaptureSpectatorState` returned without capturing; re-simulate from the stored frame just before the
   confirmed one and capture the portable state there.
8. **Input: bridge a local-input gap instead of wedging** (`input.cpp`, `backend.*`, `gekkonet.*`; PC
   `bef8a8b`): `AddLocalInput` can only write at `frame + _input_delay` and `AddInput` accepts only
   `_last_received_input + 1`, so a local buffer one frame behind the session wedges forever (every later
   push dropped). Fill the gap with the last input, unconditionally. Plus `gekko_stall_info` and
   `discarded_unacked` diagnostics.

Not yet folded (L3, PC `a5d9181`): the Aquat1c bundle -- `forced_rollback_depth`, `gekko_prediction_depth`,
`gekko_add_local_input_ahead`, disconnect/interrupt/input-retry timeouts, new event enum values. It is 393
lines across 14 files and its storage sizing (`max(prediction_window, forced_depth)`) couples the forced
harness to upstream frames-ahead; split and gate before pinning.

9. **Forced rollback: `GekkoConfig::forced_rollback_depth` + `gekko_set_forced_rollback` +
   `gekko_prediction_depth`** (`gekkonet.h`, `game_session.*`, `event.*`, `session.h`; PC `a5d9181`,
   split). Synthesizes a rollback transaction of the configured depth on every advanced frontier (load
   the state `depth` frames back, replay the recorded inputs, re-save each), giving a peer that would
   never otherwise re-simulate the rollback path. Load events carry `forced = true`. State storage stays
   sized to the prediction window (upstream); the forced depth is CLAMPED to it, so it does not couple to
   upstream frames-ahead. Public delta: 2 symbols -- the rest of `a5d9181`'s Aquat1c bundle (liveness
   timeouts, Interrupted/Resumed, `gekko_add_local_input_ahead`, the 128->1800 queue bump) is deferred to
   L3b and has no consumer on the chain today.
