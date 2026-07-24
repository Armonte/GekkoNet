#pragma once

#include <vector>

#include "gekko_types.h"
#include "gekkonet.h"

namespace Gekko {
    struct ReplayBlob {
        static constexpr u32 MAGIC = 0x474B5250; // GKRP
        static constexpr u32 FORMAT_VERSION = 1;

        enum Codec : u8 { Raw, DeltaRle };

        u32 header = MAGIC;
        u32 version = FORMAT_VERSION;
        Codec codec = Raw;

        GekkoConfig config = {};

        std::vector<u8> inputs;
        std::vector<u8> initial_state;
    };

    struct ReplaySystem {
        void StartRecording(GekkoConfig config, const u8* initial_state = nullptr);
        const u8* StopRecording(u32& length);
        void RecordInput(Frame frame, const u8* input);

        bool LoadReplay(const u8* replay_data, u32 length);
        const u8* ReplayState();
        bool NextReplayInput(u8* input);

        GekkoConfig Config();

        bool IsRecording() const;
        bool IsReplaying() const;

    private:
        void Reset();

        u32 InputSize() const;

        enum Mode {
            None,
            Recording,
            Replaying
        } _mode = None;

        Frame _start_frame = 0;
        Frame _current_frame = 0;

        std::vector<u8> _bin_buffer;

        ReplayBlob _replay;
    };
}
