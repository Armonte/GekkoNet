#pragma once

#include "gekkonet.h"
#include "gekko_types.h"

// define GekkoSession internally
struct GekkoSession {
    virtual void Init(GekkoConfig* config) = 0;
    virtual void SetLocalDelay(i32 player, u8 delay) {}
    virtual void SetRunahead(u8 runahead) {}
    virtual void SetNetAdapter(GekkoNetAdapter* adapter) {}
    virtual i32 AddActor(GekkoPlayerType type, GekkoNetAddress* addr) { return -1; }
    virtual bool DisconnectActor(i32 actor) { return false; }
    virtual void SetDisconnectTimeout(u32 timeout) {}
    virtual void AddLocalInput(i32 player, void* input) {}
    // [PovertyCaster #83] Inputs discarded from the send queue that a connected peer had not acked.
    // Default 0 for session kinds with no remote send queue (stress); GameSession overrides.
    virtual unsigned DiscardedUnacked() { return 0; }
    // [PovertyCaster #83] Fill `out` with the advance gate's own state: [0]=current frame, then one
    // last-received frame per player. Returns the count written. THE question at a stall is which player's
    // buffer is short, and nothing exposed it.
    virtual int StallInfo(int* /*out*/, int /*max*/) { return 0; }
    virtual GekkoGameEvent** UpdateSession(i32* count) = 0;
    virtual GekkoSessionEvent** Events(i32* count) = 0;
    virtual f32 FramesAhead() { return 0.f; }
    virtual void NetworkStats(i32 player, GekkoNetworkStats* stats) {}
    // [PovertyCaster #233] PURE, deliberately — and it stays pure across the 2026-09 upstream sync even
    // though upstream defaulted every other virtual on this ABC: a defaulted "0 matches" on the base would
    // be a silent zero that reads exactly like "compared nothing" — the bug this counter exists to expose.
    // Every session type must state whether it cross-peer health-checks at all (ReplaySession: no).
    virtual bool HealthStats(GekkoHealthStats* stats) = 0;
    // [PovertyCaster #231] copy up to `max` recent health attestations into out (5 u32s each:
    // frame, checksum, min_received, min_incorrect, stale_from). Default: none (spectators etc).
    virtual i32 AttestLog(u32* out, i32 max) { (void)out; (void)max; return 0; }
    virtual void NetworkPoll() {}
    virtual bool StartRecording(bool save_initial_state, bool disable_compression) { return false; }
    virtual const u8* StopRecording(u32& length) { return nullptr; }
    virtual bool LoadReplay(const u8* replay_data, u32 length) { return false; }
    virtual ~GekkoSession() = default;
};
