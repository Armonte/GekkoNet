#pragma once

#include <vector>
#include <map>
#include <memory>

#include "gekkonet.h"
#include "gekko_types.h"
#include "backend.h"
#include "event.h"
#include "sync.h"
#include "storage.h"

// define GekkoSession internally
struct GekkoSession {
    virtual void Init(GekkoConfig* config) = 0;
    virtual void SetLocalDelay(i32 player, u8 delay) = 0;
    virtual void SetRunahead(u8 runahead) = 0;
    virtual void SetNetAdapter(GekkoNetAdapter* adapter) = 0;
    virtual i32 AddActor(GekkoPlayerType type, GekkoNetAddress* addr) = 0;
    virtual void AddLocalInput(i32 player, void* input) = 0;
    virtual GekkoGameEvent** UpdateSession(i32* count) = 0;
    virtual GekkoSessionEvent** Events(i32* count) = 0;
    virtual f32 FramesAhead() = 0;
    virtual void NetworkStats(i32 player, GekkoNetworkStats* stats) = 0;
    // [PovertyCaster #233] PURE, deliberately: a defaulted "0 matches" on the base would be a silent
    // zero that reads exactly like "compared nothing" — the bug this counter exists to expose. Every
    // session type must state whether it cross-peer health-checks at all.
    virtual bool HealthStats(GekkoHealthStats* stats) = 0;
    // [PovertyCaster #231] copy up to `max` recent health attestations into out (5 u32s each:
    // frame, checksum, min_received, min_incorrect, stale_from). Default: none (spectators etc).
    virtual i32 AttestLog(u32* out, i32 max) { (void)out; (void)max; return 0; }
    virtual void NetworkPoll() = 0;
    virtual ~GekkoSession() = default;
};

namespace Gekko {

	class GameSession : public GekkoSession {
    public:
        GameSession();

        void Init(GekkoConfig* config) override;

        void SetLocalDelay(i32 player, u8 delay) override;

        void SetRunahead(u8 runahead) override;

        void SetNetAdapter(GekkoNetAdapter* adapter) override;

        i32 AddActor(GekkoPlayerType type, GekkoNetAddress* addr) override;

        void AddLocalInput(i32 player, void* input) override;

        GekkoGameEvent** UpdateSession(i32* count) override;

        GekkoSessionEvent** Events(i32* count) override;

        f32 FramesAhead() override;

        void NetworkStats(i32 player, GekkoNetworkStats* stats) override;
        bool HealthStats(GekkoHealthStats* stats) override;   // [PovertyCaster #233]

        void NetworkPoll() override;

	private:
		void Poll();

		bool AllActorsValid();

		void HandleReceivedInputs();

		void SendLocalInputs();

		u8 GetMinLocalDelay();

		bool IsPlayingLocally();

        bool IsLockstepActive() const;

		void AddDisconnectedPlayerInputs();

		void SendSpectatorInputs();

		void HandleRollback();

		void HandleSavingConfirmedFrame();

		void HandleRunahead();

		void RewindRunahead();

		bool RollbackPending();

		bool ConfirmedSaveDue();

        void SendSessionHealthCheck();

        void SendNetworkHealthCheck();

        void SessionIntegrityCheck();

	private:
		bool _started;

		Frame _last_saved_frame;

        Frame _last_sent_healthcheck;
        // [PovertyCaster #231] Frames at/above this were re-simulated by a rollback QUEUED THIS POLL and
        // their storage is stale until the app executes the queued save events. Attesting them now would
        // send a checksum from the speculative save. Reset each poll.
        Frame _attest_stale_from;
        // [PovertyCaster #231] The last 8 attestations, with the sync state AT ATTEST TIME. A real 4P catch
        // showed one peer attesting a checksum matching NEITHER its first nor its last save of the frame --
        // a mid-rollback-chain value -- and no post-hoc instrument can reconstruct the ordering that allowed
        // it. This ring records it as it happens; the app prints it beside the desync report.
    public:
        struct AttestRec { Frame frame; u32 checksum; Frame min_received; Frame min_incorrect; Frame stale_from; };
        static constexpr int kAttestRing = 8;
        i32 AttestLog(u32* out, i32 max) override {
            i32 n = _attest_count < kAttestRing ? _attest_count : kAttestRing;
            if (n > max / 5) n = max / 5;
            // oldest-first so the printout reads chronologically
            for (i32 k = 0; k < n; ++k) {
                const AttestRec& r = _attest_ring[(_attest_count - n + k) % kAttestRing];
                out[k*5+0] = (u32)r.frame; out[k*5+1] = r.checksum;
                out[k*5+2] = (u32)r.min_received; out[k*5+3] = (u32)r.min_incorrect;
                out[k*5+4] = (u32)r.stale_from;
            }
            return n;
        }
    private:
        AttestRec _attest_ring[kAttestRing] = {};
        int _attest_count = 0;
        // [PovertyCaster #233] cross-peer coverage counters — see GekkoHealthStats in gekkonet.h.
        u32 _health_matched = 0;
        u32 _health_mismatched = 0;
        u32 _health_abstain_both = 0;   // [#233] both sides kNoChecksum — nothing to compare
        u32 _health_abstain_one  = 0;   // [#233] exactly one side had an opinion (itself a divergence, #112)

		Frame _runahead_start_frame;

		u8 _runahead_frames;

		std::unique_ptr<u8[]> _disconnected_input;

		GekkoConfig _config;

		SyncSystem _sync;

        GekkoNetAdapter* _host;

		MessageSystem _msg;

		StateStorage _storage;

        GameEventSystem _game_events;
	};

	class SpectatorSession : public GekkoSession {
    public:
        SpectatorSession();

        void Init(GekkoConfig* config) override;

        void SetLocalDelay(i32 player, u8 delay) override;

        void SetRunahead(u8 runahead) override {}

        void SetNetAdapter(GekkoNetAdapter* adapter) override;

        i32 AddActor(GekkoPlayerType type, GekkoNetAddress* addr) override;

        void AddLocalInput(i32 player, void* input) override;

        GekkoGameEvent** UpdateSession(i32* count) override;

        GekkoSessionEvent** Events(i32* count) override;

        f32 FramesAhead() override;

        void NetworkStats(i32 player, GekkoNetworkStats* stats) override;
        bool HealthStats(GekkoHealthStats* stats) override;   // [PovertyCaster #233]

        void NetworkPoll() override;

	private:
		void Poll();

		bool AllActorsValid();

		void HandleReceivedInputs();

        bool ShouldDelaySpectator();

	private:
		bool _started;

        bool _delay_spectator;

		Frame _last_saved_frame;

		GekkoConfig _config;

		SyncSystem _sync;

        GekkoNetAdapter* _host;

		MessageSystem _msg;

        GameEventSystem _game_events;
	};

    class StressSession : public GekkoSession {
    public:
        StressSession();

        void Init(GekkoConfig* config) override;

        void SetLocalDelay(i32 player, u8 delay) override;

        void SetRunahead(u8 runahead) override {}

        void SetNetAdapter(GekkoNetAdapter* adapter) override;

        i32 AddActor(GekkoPlayerType type, GekkoNetAddress* addr) override;

        void AddLocalInput(i32 player, void* input) override;

        GekkoGameEvent** UpdateSession(i32* count) override;

        GekkoSessionEvent** Events(i32* count) override;

        f32 FramesAhead() override;

        void NetworkStats(i32 player, GekkoNetworkStats* stats) override;
        bool HealthStats(GekkoHealthStats* stats) override;   // [PovertyCaster #233]

        void NetworkPoll() override;

    private:
        void HandleRollback();

        void CheckForDesyncs(Frame check_frame);

    private:
        GekkoConfig _config;

        SyncSystem _sync;

        StateStorage _storage;

        SessionEventSystem _session_events;

        GameEventSystem _game_events;

        std::vector<Player> _locals;

        u32 _check_distance;

        std::map<Frame, u32> _checksum_history;
    };
}
