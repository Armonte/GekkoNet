#pragma once

#include <memory>

#include "gekkonet.h"
#include "gekko_types.h"
#include "session.h"
#include "backend.h"
#include "event.h"
#include "replay.h"
#include "sync.h"
#include "storage.h"

namespace Gekko {

	class GameSession : public GekkoSession {
    public:
        GameSession();

        void Init(GekkoConfig* config) override;

        void SetLocalDelay(i32 player, u8 delay) override;

        void SetForcedRollback(u8 depth) override;

        void SetRunahead(u8 runahead) override;

        void SetInputPredictionWindow(u8 window) override;

        void SetNetAdapter(GekkoNetAdapter* adapter) override;

        i32 AddActor(GekkoPlayerType type, GekkoNetAddress* addr) override;

        bool DisconnectActor(i32 actor) override;

        void SetDisconnectTimeout(u32 timeout) override;

        void AddLocalInput(i32 player, void* input) override;
        unsigned DiscardedUnacked() override { return _msg.DiscardedUnacked(); }
        int StallInfo(int* out, int max) override {
            if (!out || max < 1) return 0;
            int n = 0;
            out[n++] = (int)_sync.StallCurrentFrame();
            const u8 np = _sync.StallNumPlayers();
            for (u8 i = 0; i < np && n < max; i++) out[n++] = (int)_sync.StallLastReceived(i);
            return n;
        }

        // Hinokakera resilience patch: see gekko_prediction_depth.
        i32 PredictionDepth() override;


        GekkoGameEvent** UpdateSession(i32* count) override;

        GekkoSessionEvent** Events(i32* count) override;

        f32 FramesAhead() override;

        i32 ConfirmedFrame() override;

        void NetworkStats(i32 player, GekkoNetworkStats* stats) override;

        void NetworkPoll() override;

        bool StartRecording(bool save_initial_state, bool disable_compression) override;

        const u8* StopRecording(u32& length) override;

        const u8* PeekRecording(u32& length) override;

        bool RecordChecksums(u32 interval) override;

        bool SetReplayUserData(const u8* data, u32 length) override;

	private:
		void Poll();

        void UpdateRecording();

        void FlushRecording();

        void RecordInitialState();

        void RecordConfirmedChecksums();

        void CaptureChecksumState(Frame frame);

		bool AllActorsValid();

		void HandleReceivedInputs();

		void SendLocalInputs();

		u8 GetMinLocalDelay();

		bool IsPlayingLocally();

        bool IsLockstepActive() const;

		void AddDisconnectedPlayerInputs();

		void SendSpectatorInputs();

        void PrepareSpectatorStates();

        void CaptureSpectatorState();

		void HandleRollback();

        // Harness: synthesize the depth-N rollback transaction (see GekkoConfig::forced_rollback_depth).
        void HandleForcedRollback();

        // true when this update's live advance of the current frame will run
        bool CanAdvanceFrontier();

		void HandleSavingConfirmedFrame();

		void HandleRunahead();

		void RewindRunahead();

		bool RollbackPending();

		bool ConfirmedSaveDue();

		Frame GetConfirmedFrame();

		bool ShouldStallAdvance();

        Frame RemotePredictionDepth();


        void SendSessionHealthCheck();

        void CaptureHealthState(Frame frame);

        void SkipUnreachableHealthFrame();

        void SendNetworkHealthCheck();

        void SessionIntegrityCheck();

	private:
		bool _started;

		Frame _last_saved_frame;

        u8 _forced_rollback_depth;      // 0 = off
        u8 _forced_rollback_max;        // what the storage can hold

        Frame _last_sent_healthcheck;

		Frame _runahead_start_frame;

		u8 _runahead_frames;

		u8 _input_prediction_window;

		Frame _health_frame;

		bool _health_captured;

		std::unique_ptr<u8[]> _disconnected_input;

		GekkoConfig _config;

		SyncSystem _sync;

        GekkoNetAdapter* _host;

		MessageSystem _msg;

		StateStorage _storage;

        GameEventSystem _game_events;

        ReplaySystem _replay;

        StateEntry _spectator_state;

        StateEntry _health_state;

        static const Frame HEALTH_CHECK_INTERVAL = 16;

        // with limited saving, the confirmed frames a recording wants the checksum of.
        std::vector<std::unique_ptr<StateEntry>> _checksum_states;

        u32 _checksum_captures;

        Frame _last_checksum_capture;
	};
}
