#include "replay.h"

#include <cstdio>
#include <cstring>

#include "compression.h"
#include "zpp/zpp_bits.h"

void Gekko::ReplaySystem::StartRecording(GekkoConfig config, const u8* initial_state)
{
    Reset();

    _replay.config = config;

    if (initial_state && config.state_size > 0) {
        _replay.initial_state.assign(initial_state, initial_state + config.state_size);
    }

    _mode = Recording;
}

const u8* Gekko::ReplaySystem::StopRecording(u32& length)
{
    length = 0;

    if (_mode != Recording) return nullptr;

    const u32 block = InputSize();

    std::vector<u8> packed;
    if (block > 0 && !_replay.inputs.empty()) {
        auto delta = Compression::DeltaEncode(_replay.inputs.data(), (u32)_replay.inputs.size(), block);
        packed = Compression::RLEEncode(delta.data(), (u32)delta.size());
    }

    const bool use_packed = !packed.empty() && packed.size() < _replay.inputs.size();
    if (use_packed) {
        _replay.inputs.swap(packed);
        _replay.codec = ReplayBlob::DeltaRle;
    }

    _bin_buffer.clear();
    zpp::bits::out out(_bin_buffer);
    const bool bad = failure(out(_replay));

    if (use_packed) {
        _replay.inputs.swap(packed);
        _replay.codec = ReplayBlob::Raw;
    }

    if (bad) {
        printf("failed to serialize replay data\n");
        return nullptr;
    }

    _mode = None;

    length = (u32)_bin_buffer.size();

    return _bin_buffer.data();
}

void Gekko::ReplaySystem::RecordInput(Frame frame, const u8* input)
{
    if (_mode != Recording || !input) return;

    const u32 size = InputSize();
    if (size == 0) return;

    if (_replay.inputs.empty()) {
        _start_frame = frame;
    }

    if (frame < _start_frame) return;

    const u64 index = (u64)(frame - _start_frame);
    const u64 needed = (index + 1) * size;
    if (_replay.inputs.size() < needed) {
        _replay.inputs.resize(needed);
    }

    std::memcpy(_replay.inputs.data() + index * size, input, size);
}

bool Gekko::ReplaySystem::LoadReplay(const u8* replay_data, u32 length)
{
    Reset();

    if (!replay_data || length == 0) return false;

    _bin_buffer.assign(replay_data, replay_data + length);
    zpp::bits::in in(_bin_buffer);

    if (failure(in(_replay))) {
        printf("failed to deserialize replay data\n");
        Reset();
        return false;
    }

    if (_replay.header != ReplayBlob::MAGIC) {
        printf("invalid replay header\n");
        Reset();
        return false;
    }

    if (_replay.version != ReplayBlob::FORMAT_VERSION) {
        printf("unsupported replay version %u\n", _replay.version);
        Reset();
        return false;
    }

    if (_replay.codec == ReplayBlob::DeltaRle) {
        auto delta = Compression::RLEDecode(_replay.inputs.data(), (u32)_replay.inputs.size());
        _replay.inputs = Compression::DeltaDecode(delta.data(), (u32)delta.size(), InputSize());
        _replay.codec = ReplayBlob::Raw;
    }

    _current_frame = 0;
    _mode = Replaying;

    return true;
}

const u8* Gekko::ReplaySystem::ReplayState()
{
    if (_replay.initial_state.empty()) return nullptr;

    return _replay.initial_state.data();
}

bool Gekko::ReplaySystem::NextReplayInput(u8* input)
{
    if (_mode != Replaying || !input) return false;

    const u32 size = InputSize();
    if (size == 0) return false;

    const u64 offset = (u64)_current_frame * size;
    if (offset + size > _replay.inputs.size()) return false;

    std::memcpy(input, _replay.inputs.data() + offset, size);
    _current_frame++;

    return true;
}

GekkoConfig Gekko::ReplaySystem::Config()
{
    return _replay.config;
}

bool Gekko::ReplaySystem::IsRecording() const
{
    return _mode == Recording;
}

bool Gekko::ReplaySystem::IsReplaying() const
{
    return _mode == Replaying;
}

void Gekko::ReplaySystem::Reset()
{
    _mode = None;
    _start_frame = 0;
    _current_frame = 0;

    _bin_buffer.clear();

    _replay = {};
}

u32 Gekko::ReplaySystem::InputSize() const
{
    return _replay.config.input_size * _replay.config.num_players;
}
