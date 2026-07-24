#pragma once

#include "gekkonet.h"
#include "gekko_types.h"
#include "session.h"
#include "event.h"
#include "sync.h"
#include "replay.h"

namespace Gekko {

    class ReplaySession : public GekkoSession {
    public:
        ReplaySession();

        bool LoadReplay(const u8* replay_data, u32 length);

        void Init(GekkoConfig* config) override;

        void SetLocalDelay(i32 player, u8 delay) override {}

        void SetRunahead(u8 runahead) override {}

        void SetNetAdapter(GekkoNetAdapter* adapter) override {}

        i32 AddActor(GekkoPlayerType type, GekkoNetAddress* addr) override;

        bool DisconnectActor(i32 actor) override;

        void SetDisconnectTimeout(u32 timeout) override {}

        void AddLocalInput(i32 player, void* input) override {}

        GekkoGameEvent** UpdateSession(i32* count) override;

        GekkoSessionEvent** Events(i32* count) override;

        f32 FramesAhead() override;

        void NetworkStats(i32 player, GekkoNetworkStats* stats) override {}

        void NetworkPoll() override {}

    private:
        void FeedInputs();

    private:
        bool _started;

        GekkoConfig _config;

        SyncSystem _sync;

        ReplaySystem _replay;

        SessionEventSystem _session_events;

        GameEventSystem _game_events;
    };
}
