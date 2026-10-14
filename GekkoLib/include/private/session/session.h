#pragma once

#include "gekkonet.h"
#include "gekko_types.h"

// define GekkoSession internally
struct GekkoSession {
    virtual void Init(GekkoConfig* config) = 0;
    virtual void SetLocalDelay(i32 player, u8 delay) {}
    virtual void SetForcedRollback(u8 depth) {}
    virtual void SetRunahead(u8 runahead) {}
    virtual void SetInputPredictionWindow(u8 window) {}
    virtual void SetNetAdapter(GekkoNetAdapter* adapter) {}
    virtual i32 AddActor(GekkoPlayerType type, GekkoNetAddress* addr) { return -1; }
    virtual bool DisconnectActor(i32 actor) { return false; }
    virtual void SetDisconnectTimeout(u32 timeout) {}
    virtual void AddLocalInput(i32 player, void* input) {}
    // [hinokakera] Queue a local input allowed to run up to `max_lead` frames ahead of the
    // confirmed frame; false when the lead would be exceeded. Used by the forced-rollback harness.
    virtual bool AddLocalInputAhead(i32 player, void* input, i32 max_lead) { return false; }
    // [hinokakera] Frames currently predicted beyond the last confirmed frame.
    virtual i32 PredictionDepth() { return 0; }
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
    // FM2K patch: highest frame for which inputs from ALL players have actually been RECEIVED
    // (no predictions). NULL_FRAME until inputs flow. Lets a host gate replay / spectator
    // recording on confirmed-only inputs.
    virtual i32 ConfirmedFrame() { return 0; }
    virtual i32 SpectatorBufferedFrames() { return 0; }
    virtual void NetworkStats(i32 player, GekkoNetworkStats* stats) {}
    virtual void NetworkPoll() {}
    virtual bool StartRecording(bool save_initial_state, bool disable_compression) { return false; }
    virtual const u8* StopRecording(u32& length) { return nullptr; }
    virtual const u8* PeekRecording(u32& length) { return nullptr; }
    virtual bool RecordChecksums(u32 interval) { return false; }
    virtual bool SetReplayUserData(const u8* data, u32 length) { return false; }
    virtual bool LoadReplay(const u8* replay_data, u32 length) { return false; }
    virtual const u8* ReplayUserData(u32& length) { length = 0; return nullptr; }
    virtual ~GekkoSession() = default;

    // lazy checksum (gekko_set_checksum_callback); null = use the checksum written by the save event.
    GekkoChecksumFn checksum_fn = nullptr;
    void* checksum_user = nullptr;
};
