#include "session/game_session.h"

#include <algorithm>
#include <cassert>
#include <cstring>

Gekko::GameSession::GameSession()
{
    _host = nullptr;
    _started = false;
    _last_saved_frame = GameInput::NULL_FRAME - 1;
    _disconnected_input = nullptr;
    _last_sent_healthcheck = GameInput::NULL_FRAME;
    _attest_stale_from = GameInput::NULL_FRAME;   // [PovertyCaster #231]
    _runahead_start_frame = GameInput::NULL_FRAME;
    _runahead_frames = 0;
    _config = GekkoConfig();
}

void Gekko::GameSession::Init(GekkoConfig* config)
{
    _host = nullptr;

    _started = false;

    // get given configs
    std::memcpy(&_config, config, sizeof(GekkoConfig));

    // setup input buffer for the players
    _sync.Init(_config.num_players, _config.input_size);

    // setup message system.
    // Hinokakera resilience patch: plumb the configured liveness timeouts (0 = defaults).
    _msg.Init(_config.num_players, _config.input_size, _config.state_size, false,
        _config.disconnect_timeout_ms, _config.interrupt_timeout_ms, _config.input_retry_ms);

    // setup game event system
    _game_events.Init(_config.input_size * _config.num_players);

    // setup state storage (large enough for the forced rollback depth too)
    _forced_rollback_depth = _config.forced_rollback_depth;
    _forced_rollback_max = _config.forced_rollback_depth;
    {
        u32 states = _config.input_prediction_window;
        if (_config.forced_rollback_depth > states) states = _config.forced_rollback_depth;
        _storage.Init(states, _config.state_size, _config.limited_saving);
    }

    _spectator_state.state = std::make_unique<u8[]>(_config.state_size);
    _spectator_state.state_len = _config.state_size;

    // setup disconnected input for disconnected player within the session
    _disconnected_input = std::make_unique<u8[]>(_config.input_size);
    std::memset(_disconnected_input.get(), 0, _config.input_size);

    // we only detect desyncs whenever we are not limited saving for now.
    _config.desync_detection = _config.limited_saving ? false : _config.desync_detection;
}

void Gekko::GameSession::SetRunahead(u8 runahead)
{
    _runahead_frames = runahead;
}

void Gekko::GameSession::SetForcedRollback(u8 depth)
{
    _forced_rollback_depth = depth > _forced_rollback_max ? _forced_rollback_max : depth;
}

void Gekko::GameSession::SetLocalDelay(i32 player, u8 delay)
{
    for (u32 i = 0; i < _msg.locals.size(); i++) {
        if (_msg.locals[i]->handle == player) {
            _sync.SetLocalDelay(player, delay);
        }
    }
}

void Gekko::GameSession::SetNetAdapter(GekkoNetAdapter* adapter)
{
    _host = adapter;
}

i32 Gekko::GameSession::AddActor(GekkoPlayerType type, GekkoNetAddress* addr)
{
    const i32 ERR = -1;
    std::unique_ptr<NetAddress> address;

    if (addr) {
        address = std::make_unique<NetAddress>(addr->data, addr->size);
    }

    if (type == GekkoSpectator) {
        if (_started && _config.state_size == 0) {
            return ERR;
        }

        // Disconnected spectators no longer occupy a slot. Their remaining
        // disconnect retries are retained outside the active routing records.
        _msg.ReclaimDisconnectedSpectators();

        if (_msg.spectators.size() >= _config.max_spectators) {
            return ERR;
        }

        Handle new_handle = _config.num_players;
        const Handle end_handle = _config.num_players + _config.max_spectators;
        for (; new_handle < end_handle; new_handle++) {
            const bool in_use = std::any_of(
                _msg.spectators.begin(),
                _msg.spectators.end(),
                [new_handle](const std::unique_ptr<Player>& player) {
                    return player->handle == new_handle;
                }
            );

            if (!in_use) {
                break;
            }
        }

        auto spectator = std::make_unique<Player>(new_handle, type, address.get());
        spectator->requires_spectator_state = _started;
        spectator->spectator_state_acked = !_started;
        _msg.spectators.push_back(std::move(spectator));

        return new_handle;
    }
    else {
        if (_started || _msg.locals.size() + _msg.remotes.size() >= _config.num_players) {
            return ERR;
        }

        u32 new_handle = (u32)(_msg.locals.size() + _msg.remotes.size());

        if (type == GekkoLocalPlayer) {
            _msg.locals.push_back(std::make_unique<Player>(new_handle, type, address.get()));
        }
        else {
            // require an address when specifing a remote player
            if (addr == nullptr) {
                return ERR;
            }

            _msg.remotes.push_back(std::make_unique<Player>(new_handle, type, address.get()));
            _sync.SetInputPredictionWindow(new_handle, _config.input_prediction_window);
        }

        return new_handle;
    }
}

bool Gekko::GameSession::DisconnectActor(i32 actor)
{
    if (!_msg.DisconnectActor(actor)) {
        return false;
    }

    // flush right away so the disconnect gets sent even
    // when the session isnt updated after this call.
    if (_host) {
        _msg.SendPendingOutput(_host);
    }

    return true;
}

void Gekko::GameSession::SetDisconnectTimeout(u32 timeout)
{
    _msg.SetDisconnectTimeout(timeout);
}

void Gekko::GameSession::AddLocalInput(i32 player, void* input)
{
    u8* inp = (u8*)input;

    for (u32 i = 0; i < _msg.locals.size(); i++) {
        if (_msg.locals[i]->handle == player) {
            _sync.AddLocalInput(player, inp);
            break;
        }
    }
}

bool Gekko::GameSession::AddLocalInputAhead(i32 player, void* input, i32 max_lead)
{
    // Hinokakera resilience patch: input production that survives a stalled simulation.
    if (!input) {
        return false;
    }

    bool is_local = false;
    for (auto& local : _msg.locals) {
        if (local->handle == player) {
            is_local = true;
            break;
        }
    }

    if (!is_local) {
        return false;
    }

    u8* inp = (u8*)input;
    const Frame before = _sync.GetLastReceivedFrom(player);

    // nothing produced yet: the regular path also fills the delay prefix.
    if (before == GameInput::NULL_FRAME) {
        _sync.AddLocalInput(player, inp);
        return _sync.GetLastReceivedFrom(player) != before;
    }

    const Frame current = _sync.GetCurrentFrame();
    const Frame regular = current + (Frame)_sync.GetLocalDelay(player);
    const Frame target = before + 1;

    // beyond the regular slot only while stalled on remote input and within the lead cap.
    if (target > regular) {
        if (!IsStalledOnRemoteInput()) {
            return false;
        }

        Frame lead = max_lead < 0 ? 0 : (Frame)max_lead;
        lead = std::min(lead, (Frame)MAX_INPUT_AHEAD_LEAD);

        if (target > current + lead) {
            return false;
        }
    }

    _sync.AddLocalInputAt(player, inp, target);
    return _sync.GetLastReceivedFrom(player) == target;
}

i32 Gekko::GameSession::PredictionDepth()
{
    // Hinokakera resilience patch: simulated frames resting on predicted remote input.
    if (!_started || _msg.remotes.empty()) {
        return 0;
    }

    const Frame depth = RemotePredictionDepth();
    return depth > 0 ? depth : 0;
}

Frame Gekko::GameSession::RemotePredictionDepth()
{
    // (last simulated frame) - (lowest last received remote frame), may be negative.
    Frame min_received = INT32_MAX;
    for (auto& remote : _msg.remotes) {
        min_received = std::min(min_received, _sync.GetLastReceivedFrom(remote->handle));
    }

    return (_sync.GetCurrentFrame() - 1) - min_received;
}

bool Gekko::GameSession::IsStalledOnRemoteInput()
{
    // the next frame can neither use a received remote input nor predict one:
    // the prediction window is used up (lockstep: the remote input is missing).
    if (!_started || _msg.remotes.empty()) {
        return false;
    }

    return RemotePredictionDepth() >= (Frame)_config.input_prediction_window;
}

GekkoGameEvent** Gekko::GameSession::UpdateSession(i32* count)
{
    // reset session events
    _msg.session_events.Reset();

    // connection Handling
    Poll();

    // clear GameEvents
    _game_events.Clear();

    // gameplay
    if (AllActorsValid()) {
        // reset the game event buffer before doing anything else
        _game_events.Reset();

        // add inputs so we can continue the session.
        AddDisconnectedPlayerInputs();

        // rewind any runahead frames from the previous tick
        RewindRunahead();

        // store the frames that went by for the replay
        UpdateRecording();

        // check if we need to rollback
        HandleRollback();

        // harness: the synthesized per-frame rollback (after real corrections)
        HandleForcedRollback();

        // check if we need to save the confirmed frame
        HandleSavingConfirmedFrame();

        // send a healthcheck if applicable
        SendSessionHealthCheck();

        // check if the session is still doing alright.
        SessionIntegrityCheck();

        // Sessions which do not hold a recent enough rollback save create one
        // on demand when a spectator joins late.
        CaptureSpectatorState();

        // then advance the session
        if (!ShouldStallAdvance() && _game_events.AddAdvanceEvent(_sync, false, _runahead_frames > 0)) {
            if (!_config.limited_saving) {
                _game_events.AddSaveEvent(_sync, _storage, &_last_saved_frame);
            }
            _sync.IncrementFrame();
        }

        // run ahead if configured
        HandleRunahead();
    }

    *count = _game_events.Count();
    return _game_events.Data();
}

GekkoSessionEvent** Gekko::GameSession::Events(i32* count)
{
    *count = (i32)_msg.session_events.GetRecentEvents().size();
    return _msg.session_events.GetRecentEvents().data();
}

f32 Gekko::GameSession::FramesAhead()
{
    if (!_started) {
        return 0.f;
    }

    f32 sum = 0.f;
    i32 count = 0;
    for (auto& remote : _msg.remotes) {
        if (remote->GetStatus() == Connected) {
            sum += remote->adv_history.GetAverageAdvantage();
            count++;
        }
    }
    return count > 0 ? sum / (f32)count : 0.f;
}

// [PovertyCaster #233] This session DOES cross-peer health checking -> true, with real counts.
bool Gekko::GameSession::HealthStats(GekkoHealthStats* stats)
{
    if (!stats) {
        return false;
    }
    stats->compares_matched = _health_matched;
    stats->compares_mismatched = _health_mismatched;
    stats->abstained_both = _health_abstain_both;
    stats->abstained_one_sided = _health_abstain_one;
    return _config.desync_detection;   // false when the feature is OFF: no comparisons are even attempted
}

void Gekko::GameSession::NetworkStats(i32 player, GekkoNetworkStats* stats)
{
    std::vector<std::unique_ptr<Player>>* current = &_msg.remotes;

    for (u32 i = 0; i < 2; i++)
    {
        if (i == 1) {
            current = &_msg.spectators;
        }

        for (auto& actor : *current) {
            if (actor->handle == player) {
                actor->stats.UpdateBandwidth();
                stats->kb_sent = actor->stats.kb_sent_per_sec;
                stats->kb_received = actor->stats.kb_received_per_sec;
                stats->last_ping = actor->stats.LastRTT();
                stats->jitter = actor->stats.CalculateJitter();
                stats->avg_ping = actor->stats.CalculateAvgRTT();
                return;
            }
        }
    }
}

void Gekko::GameSession::NetworkPoll()
{
    Poll();
}

bool Gekko::GameSession::StartRecording(bool save_initial_state, bool disable_compression)
{
    return _replay.StartRecording(_config, _sync.GetCurrentFrame(), save_initial_state, disable_compression);
}

const u8* Gekko::GameSession::StopRecording(u32& length)
{
    return _replay.StopRecording(length);
}

void Gekko::GameSession::UpdateRecording()
{
    if (!_replay.IsRecording()) {
        return;
    }

    if (_replay.NeedsState()) {
        RecordInitialState();
    }

    _replay.RecordInputs(_sync);
}

void Gekko::GameSession::RecordInitialState()
{
    const Frame current = _sync.GetCurrentFrame();
    const Frame confirmed = GetConfirmedFrame();
    const Frame incorrect = _sync.GetMinIncorrectFrame();

    Frame stored = confirmed;

    if (incorrect != GameInput::NULL_FRAME) {
        stored = std::min(stored, incorrect - 1);
    }

    if (_config.limited_saving) {
        stored = std::min(stored, _last_saved_frame);
    }

    if (stored >= 0) {
        auto saved = _storage.GetState(stored);

        if (saved->frame == stored && saved->state_len > 0) {
            _replay.RecordState(saved->state.get(), saved->state_len, stored);
            return;
        }
    }

    if (current - 1 > confirmed || incorrect != GameInput::NULL_FRAME) {
        return;
    }

    _game_events.AddStateSaveEvent(current - 1, _replay.PendingState());
}

void Gekko::GameSession::HandleSavingConfirmedFrame()
{
    if (!ConfirmedSaveDue()) {
        return;
    }

    const Frame confirmed_frame = GetConfirmedFrame();
    const Frame current = _sync.GetCurrentFrame();

    assert(_last_saved_frame < confirmed_frame);

    const Frame sync_frame = _last_saved_frame;
    const Frame frame_to_save = std::min(current - 1, confirmed_frame);

    _sync.SetCurrentFrame(sync_frame);
    _game_events.AddLoadEvent(_sync, _storage);
    _sync.IncrementFrame();

    for (Frame frame = sync_frame + 1; frame < current; frame++) {
        _game_events.AddAdvanceEvent(_sync, true);
        if (frame == frame_to_save) {
            _game_events.AddSaveEvent(_sync, _storage, &_last_saved_frame);
        }
        _sync.IncrementFrame();
    }

    // make sure that we are back where we started.
    assert(_sync.GetCurrentFrame() == current);
}

void Gekko::GameSession::SendSessionHealthCheck()
{
    if (!_config.desync_detection) {
        return;
    }

    const Frame current = _sync.GetCurrentFrame();
    Frame confirmed = (current - _config.input_prediction_window) - 1;

    // [PovertyCaster #231] TWO GUARDS, both measured from a real 4P catch (detection f=994, ring dumps
    // byte-identical, checksums mismatched -- a FALSE desync that kills the match all the same):
    //
    // 1. THE ARITHMETIC WINDOW IS NOT CONFIRMATION. current - window - 1 assumes every remote input that
    //    old has ARRIVED. Under jitter/loss an input can arrive later than the window; the save for that
    //    frame was made with a PREDICTED input and its checksum is speculative. Cap attestation at
    //    GetMinReceivedFrame(): a frame is attestable only when every player's real input for it exists.
    const Frame min_received = _sync.GetMinReceivedFrame();
    if (min_received != GameInput::NULL_FRAME && confirmed > min_received) {
        confirmed = min_received;
    }
    // 2. A ROLLBACK QUEUED THIS POLL HAS NOT EXECUTED YET. HandleRollback only queues load/advance/save
    //    events; the app runs them after this poll returns. Storage for frames >= _attest_stale_from is
    //    stale RIGHT NOW, so defer -- next poll the corrected save is in place and attestation resumes.
    if (_attest_stale_from != GameInput::NULL_FRAME && confirmed >= _attest_stale_from) {
        return;
    }

    if (confirmed <= GameInput::NULL_FRAME) {
        return;
    }

    if (confirmed <= _last_sent_healthcheck) {
        return;
    }

    auto sav = _storage.GetState(confirmed);

    assert(sav->frame == confirmed);

    _last_sent_healthcheck = confirmed;

    _msg.local_health[confirmed] = sav->checksum;

    _msg.SendSessionHealth(confirmed, sav->checksum);
    {   // [PovertyCaster #231] record the attestation with the sync state that allowed it
        AttestRec& r = _attest_ring[_attest_count % kAttestRing];
        r.frame = confirmed; r.checksum = sav->checksum;
        r.min_received = min_received; r.min_incorrect = _sync.GetMinIncorrectFrame();
        r.stale_from = _attest_stale_from;
        ++_attest_count;
    }

    for (auto iter = _msg.local_health.begin();
        iter != _msg.local_health.end(); ) {
        if (iter->first < (confirmed - 100)) {
            iter = _msg.local_health.erase(iter);
        }
        else {
            ++iter;
        }
    }
}

void Gekko::GameSession::SendNetworkHealthCheck()
{
    _msg.SendNetworkHealth();
}

void Gekko::GameSession::SessionIntegrityCheck()
{
    if (!_config.desync_detection) {
        return;
    }

    for (auto iter = _msg.local_health.begin();
        iter != _msg.local_health.end(); ) {

        for (auto& player : _msg.remotes) {
            if (player->session_health.count(iter->first)) {
                // [PovertyCaster #233] Record BOTH outcomes. Only the mismatch used to be recorded (as
                // an event), so "compared and agreed" was indistinguishable from "never compared" —
                // both produced silence. Counting the agreements is what makes a clean run falsifiable.
                //
                // *** CORRECTED 2026-08-13, and this correction is the whole point of the counter. ***
                // The first version of this was a raw `local == remote`, which counts TWO ABSTAINING
                // PEERS AS A VERIFIED MATCH: an adapter with no opinion about a frame reports
                // pc::kNoChecksum (0), so 0 == 0 compares equal and incremented `matched`. That made
                // `chk` structurally incapable of counting the thing it appears to count — the exact
                // disease as the pre-existing `abst` counter, and worse, because chk is what the
                // harnesses gate on. pc/Checksum.hpp already models this correctly:
                //     checksumsComparable(a,b) := a != kNoChecksum && b != kNoChecksum
                // Caught by melty-session reading the diff, NOT by any of my own tests — a clean 4P
                // qoh99 run reported chk=16017/0 and looked perfect either way. MBAACC would have shown
                // it first: it abstains on every non-battle frame, so menus/CSS/loading would have
                // inflated chk while verifying nothing.
                //
                // ONE-SIDED still raises the desync event exactly as before: pc::SessionDriver
                // classifies it via abstainKind() into _abstainOneSided, and #112 established a
                // one-sided abstain IS itself a state divergence. Do not "simplify" that away.
                const u32 _local  = iter->second;
                const u32 _remote = player->session_health[iter->first];
                const bool _comparable = (_local != 0u) && (_remote != 0u);   // 0 == pc::kNoChecksum
                if (!_comparable) {
                    if (_local == 0u && _remote == 0u) {
                        _health_abstain_both++;      // nothing to compare; NOT verification
                    }
                    else {
                        _health_abstain_one++;
                        _msg.session_events.AddDesyncDetectedEvent(   // preserved: drives oneSidedAbstains
                            iter->first, player->handle, _local, _remote);
                    }
                }
                else if (_local == _remote) {
                    _health_matched++;               // a REAL verification: both sides had an opinion
                }
                else {
                    _health_mismatched++;
                    _msg.session_events.AddDesyncDetectedEvent(
                        iter->first,
                        player->handle,
                        iter->second,
                        player->session_health[iter->first]
                    );
                }
                player->session_health.erase(iter->first);
            }
        }

        ++iter;
    }
}

void Gekko::GameSession::AddDisconnectedPlayerInputs()
{
    const Frame current = _sync.GetCurrentFrame();

    for (auto& player : _msg.remotes) {
        if (player->GetStatus() != Disconnected) {
            continue;
        }

        const Handle handle = player->handle;
        const Frame disc_frame = player->disconnect_frame;

        auto& input_q = _msg.GetNetPlayerQueue(handle);
        const Frame last_added = _msg.GetLastAddedInputFrom(handle);
        const Frame oldest = last_added - (Frame)input_q.size() + 1;

        // when a claim raised the agreed frame, replace the neutral inputs the
        // session already used with the real ones so a rollback corrects it.
        if (player->applied_disconnect_frame != INT32_MAX &&
            player->applied_disconnect_frame < disc_frame) {
            const Frame received = _sync.GetLastReceivedFrom(handle);
            const Frame raised_up_to = std::min(disc_frame, received);
            for (Frame frame = player->applied_disconnect_frame + 1; frame <= raised_up_to; frame++) {
                if (frame >= oldest && frame <= last_added) {
                    _sync.OverwriteInput(handle, input_q[frame - oldest].get(), frame);
                }
            }
        }
        player->applied_disconnect_frame = disc_frame;

        // use the inputs we hold up to the agreed frame, neutral input afterwards.
        // include the current frame itself, lockstep cant predict its way past it.
        const Frame last_recv = _sync.GetLastReceivedFrom(handle) + 1;

        for (Frame frame = last_recv; frame <= current; frame++) {
            if (frame <= disc_frame && frame >= oldest && frame <= last_added) {
                _sync.AddRemoteInput(handle, input_q[frame - oldest].get(), frame);
            }
            else {
                _sync.AddRemoteInput(handle, _disconnected_input.get(), frame);
            }
        }
    }
}

void Gekko::GameSession::SendSpectatorInputs()
{
    const Frame current = _msg.GetLastAddedInput(true) + 1;
    const Frame confirmed = GetConfirmedFrame();

    std::unique_ptr<u8[]> inputs;
    for (Frame frame = current; frame <= confirmed; frame++) {
        if (!_sync.GetSpectatorInputs(inputs, frame)) {
            break;
        }
        _msg.AddSpectatorInput(frame, inputs.get());
    }
}

void Gekko::GameSession::PrepareSpectatorStates()
{
    if (!_started || _config.state_size == 0) {
        return;
    }

    const Frame oldest_input = _msg.GetOldestSpectatorInput();

    for (auto& spectator : _msg.spectators) {
        if (spectator->GetStatus() != Connected ||
            !spectator->requires_spectator_state || spectator->spectator_state_acked) {
            continue;
        }

        const bool state_too_old = spectator->spectator_state_configured &&
            spectator->spectator_state_frame + 1 < oldest_input;
        if (spectator->spectator_state_configured && !state_too_old) {
            continue;
        }

        Frame state_frame = _config.limited_saving ? _last_saved_frame : GetConfirmedFrame();
        const Frame oldest_state = _config.limited_saving
            ? state_frame
            : std::max((Frame)-1, state_frame - (Frame)_config.input_prediction_window - 1);

        StateEntry* state = nullptr;
        for (Frame frame = state_frame; frame >= oldest_state; frame--) {
            auto candidate = _storage.GetState(frame);
            if (candidate->frame == frame && candidate->state_len > 0 &&
                candidate->state_len <= _config.state_size) {
                state_frame = frame;
                state = candidate;
                break;
            }
        }

        const Frame confirmed = GetConfirmedFrame();
        if (_spectator_state.frame >= oldest_state &&
            _spectator_state.frame <= confirmed && _spectator_state.state_len > 0 &&
            _spectator_state.state_len <= _config.state_size &&
            (!state || _spectator_state.frame > state_frame)) {
            state_frame = _spectator_state.frame;
            state = &_spectator_state;
        }

        if (!state || state_frame + 1 < oldest_input ||
            (spectator->spectator_state_configured &&
            state_frame <= spectator->spectator_state_frame)) {
            continue;
        }

        _msg.SetSpectatorState(
            spectator->handle,
            state_frame,
            state->state.get(),
            state->state_len
        );
    }
}

void Gekko::GameSession::CaptureSpectatorState()
{
    if (_config.state_size == 0) {
        return;
    }

    const Frame oldest_input = _msg.GetOldestSpectatorInput();
    bool state_needed = false;
    for (auto& spectator : _msg.spectators) {
        if (spectator->GetStatus() != Connected ||
            !spectator->requires_spectator_state || spectator->spectator_state_acked) {
            continue;
        }

        if (!spectator->spectator_state_configured ||
            spectator->spectator_state_frame + 1 < oldest_input) {
            state_needed = true;
            break;
        }
    }

    const Frame current = _sync.GetCurrentFrame();
    const Frame state_frame = std::min(current - 1, GetConfirmedFrame());
    if (!state_needed || state_frame + 1 < oldest_input ||
        state_frame <= _spectator_state.frame) {
        return;
    }

    // Lockstep and local sessions are already sitting on the confirmed state.
    if (state_frame == current - 1) {
        _game_events.AddStateSaveEvent(state_frame, &_spectator_state, true);
        return;
    }

    // A rollback session may be ahead of its confirmed frame. Re-simulate from
    // its latest persistent save, capture the confirmed state along the way,
    // and finish back at the state where this update started.
    const Frame sync_frame = _last_saved_frame;
    if (sync_frame >= state_frame) {
        return;
    }

    auto saved = _storage.GetState(sync_frame);
    if (saved->frame != sync_frame || saved->state_len == 0) {
        return;
    }

    _sync.SetCurrentFrame(sync_frame);
    _game_events.AddLoadEvent(_sync, _storage);
    _sync.IncrementFrame();

    for (Frame frame = sync_frame + 1; frame < current; frame++) {
        _game_events.AddAdvanceEvent(_sync, true);
        if (frame == state_frame) {
            _game_events.AddStateSaveEvent(frame, &_spectator_state, true);
        }
        _sync.IncrementFrame();
    }

    assert(_sync.GetCurrentFrame() == current);
}

void Gekko::GameSession::HandleRollback()
{
    Frame current = _sync.GetCurrentFrame();
    if (_last_saved_frame == GameInput::NULL_FRAME - 1) {
        _sync.SetCurrentFrame(current - 1);
        _game_events.AddSaveEvent(_sync, _storage, &_last_saved_frame);
        _sync.IncrementFrame();
    }

    _attest_stale_from = GameInput::NULL_FRAME;   // [PovertyCaster #231] reset each poll
    if (!RollbackPending()) {
        return;
    }

    current = _sync.GetCurrentFrame();
    const Frame min = _sync.GetMinIncorrectFrame();

    // [PovertyCaster #231] Everything from the rollback's resim start upward is about to be RE-SAVED by
    // events the app has not executed yet. SendSessionHealthCheck runs later in THIS SAME poll and reads
    // _storage directly, so without this marker it attests the speculative save of any such frame.
    _attest_stale_from = min;

    const Frame sync_frame = _config.limited_saving ? _last_saved_frame : min - 1;
    // never keep a save beyond the confirmed frame, a disconnect claim may
    // still change inputs past it and the save would bake in the wrong ones.
    const Frame frame_to_save = std::min(std::min(current - 1, min), GetConfirmedFrame());

    // load the sync frame
    _sync.SetCurrentFrame(sync_frame);
    _game_events.AddLoadEvent(_sync, _storage);
    _sync.IncrementFrame();

    for (Frame frame = sync_frame + 1; frame < current; frame++) {
        _game_events.AddAdvanceEvent(_sync, true);
        if (!_config.limited_saving || frame == frame_to_save) {
            _game_events.AddSaveEvent(_sync, _storage, &_last_saved_frame);
        }
        _sync.IncrementFrame();
    }

    // clear the marked mispredictions up to this point in the input buffer
    _sync.ClearIncorrectFramesUpTo(current);

    // make sure that we are back where we started.
    assert(_sync.GetCurrentFrame() == current);
}

// [PovertyCaster] DELIBERATELY DOES NOT SET _attest_stale_from, unlike HandleRollback. Read this before
// "fixing" the asymmetry — it is load-bearing in both directions:
//
//   * WHY IT IS SAFE. #231's marker exists because a REAL rollback re-simulates with CORRECTED inputs, so
//     the save events queued here will differ from what _storage holds right now, and SendSessionHealthCheck
//     (later in this same poll, reading _storage directly) would attest a speculative value. A FORCED
//     transaction replays the IDENTICAL input history, so the re-save is expected to be byte-identical and
//     what _storage holds is already the confirmed-correct value. Attesting it is correct.
//   * WHY SETTING IT WOULD BE HARMFUL. This runs on every advanced frontier, so _attest_stale_from would be
//     current - depth every poll, capping attestation at confirmed <= current - depth - 1. At the depths this
//     harness is used at (30) that is far below GetConfirmedFrame(), so cross-peer health checking would go
//     SILENT for the whole run — disabling desync detection in exactly the runs that exist to prove there is
//     none, and doing it invisibly (no events, just no comparisons).
//
// The two mechanisms are independent: a forced replay that does NOT reproduce the state is caught by the
// app-side replay verification, not by attestation.
void Gekko::GameSession::HandleForcedRollback()
{
    if (_forced_rollback_depth == 0 || !_started || _config.state_size == 0 || _config.limited_saving) {
        return;
    }
    // the initial save (HandleRollback) must exist before anything can be loaded
    if (_last_saved_frame == GameInput::NULL_FRAME - 1) {
        return;
    }
    // exactly one transaction per advanced frontier: run it only in the update whose live
    // advance of `current` follows, so it always replays the final history of the frontier
    // (a stalled update runs none; a correction in a later update precedes the one cycle).
    if (!CanAdvanceFrontier()) {
        return;
    }
    const Frame current = _sync.GetCurrentFrame();
    // load the state after frame `sync_frame` and replay sync_frame+1 .. current-1
    // (depth frames). Early in the session the depth is clamped to what exists:
    // the first save is labelled -1 (the state before frame 0).
    Frame sync_frame = current - 1 - (Frame)_forced_rollback_depth;
    if (sync_frame < -1) {
        sync_frame = -1;
    }
    if (sync_frame >= current - 1) {
        return;   // nothing to replay yet
    }
    StateEntry* entry = _storage.GetState(sync_frame);
    if (entry == nullptr || entry->frame != sync_frame) {
        return;   // not saved (evicted or never written)
    }

    _sync.SetCurrentFrame(sync_frame);
    _game_events.AddLoadEvent(_sync, _storage, true);
    _sync.IncrementFrame();

    for (Frame frame = sync_frame + 1; frame < current; frame++) {
        _game_events.AddAdvanceEvent(_sync, true);
        _game_events.AddSaveEvent(_sync, _storage, &_last_saved_frame);
        _sync.IncrementFrame();
    }

    assert(_sync.GetCurrentFrame() == current);
}

bool Gekko::GameSession::CanAdvanceFrontier()
{
    // the predicate UpdateSession's live advance uses (ShouldStallAdvance, then the input
    // fetch inside AddAdvanceEvent). Nothing between here and the advance touches the input
    // buffers, so the prediction this fetch may create is the one the advance reuses.
    if (ShouldStallAdvance()) {
        return false;
    }
    std::unique_ptr<u8[]> inputs;
    Frame frame = GameInput::NULL_FRAME;
    return _sync.GetCurrentInputs(inputs, frame);
}

void Gekko::GameSession::Poll()
{
    // return if no host is defined.
    if (!_host) {
        return;
    }

    // fetch data from network
    int length = 0;
    auto data = _host->receive_data(&length);

    // process the data we received
    _msg.HandleData(_host, data, length);

    // Existing sessions continue handshaking newly added spectators.
    _msg.CheckStatusActors();

    // handle received inputs
    HandleReceivedInputs();

    // add local input for the network
    SendLocalInputs();

    // send inputs to spectators
    SendSpectatorInputs();

    // Late spectators start from a confirmed state and receive only later inputs.
    PrepareSpectatorStates();

    // send network health update
    SendNetworkHealthCheck();

    // now send data
    _msg.SendPendingOutput(_host);
}

bool Gekko::GameSession::AllActorsValid()
{
    if (!_started) {
        if (!_msg.CheckStatusActors()) {
            return false;
        }

        // if none returned that the session is ready!
        _msg.session_events.AddSessionStartedEvent();

        _started = true;

        return true;
    }

    return true;
}

void Gekko::GameSession::HandleReceivedInputs()
{
    for (auto& remote : _msg.remotes) {
        if (remote->GetStatus() != Connected) continue;

        {
            auto handle = remote->handle;
            const Frame last_recv = _sync.GetLastReceivedFrom(handle) + 1;
            const Frame last_added = _msg.GetLastAddedInputFrom(handle);

            // Hinokakera resilience patch: 128 -> 1800 to match MAX_INPUT_QUEUE_SIZE, a resume
            // burst after a long interruption can deliver more than 128 frames in one poll.
            assert(last_added - last_recv <= 1800); // more then 1800 frames behind sounds incorrect.

            auto& input_q = _msg.GetNetPlayerQueue(handle);
            const Frame min_frame = last_added - (i32)input_q.size() + 1;
            const Frame current_frame = _sync.GetCurrentFrame();
            const Frame local_delay = (Frame)GetMinLocalDelay();
            for (int i = last_recv; i <= last_added; i++) {
                if (i >= min_frame) {
                    int current_idx = i - min_frame;
                    u8* input = input_q[current_idx].get();
                    _sync.AddRemoteInput(handle, input, i);
                    const i8 local_adv = (i8)(current_frame - i - local_delay);
                    _msg.SendInputAck(handle, i, local_adv);
                }
            }
        }
    }
}

void Gekko::GameSession::SendLocalInputs()
{
    if (!_msg.locals.empty() && _started) {
        const Frame current = _msg.GetLastAddedInput(false) + 1;
        const Frame delay = GetMinLocalDelay();

        auto input = std::make_unique<u8[]>(_config.input_size);
        // Hinokakera resilience patch: also forward inputs produced ahead of the regular
        // slot (gekko_add_local_input_ahead); the loop still stops at the first missing input.
        for (Frame frame = current; frame <= current + delay + MAX_INPUT_AHEAD_LEAD; frame++) {
            for (auto& player : _msg.locals) {
                if (!_sync.GetLocalInput(player->handle, input, frame)) {
                    return;
                }
                _msg.AddInput(frame, player->handle, input.get());
            }
            // Record per-peer advantage snapshot once per actual game frame
            if (frame == current) {
                const Frame current_frame = _sync.GetCurrentFrame();
                for (auto& remote : _msg.remotes) {
                    if (remote->GetStatus() == Connected) {
                        const i8 local_adv = (i8)(current_frame - _sync.GetLastReceivedFrom(remote->handle) - (Frame)delay);
                        remote->adv_history.SetLocalAdvantage(local_adv);
                        remote->adv_history.Update(frame);
                    }
                }
            }
        }
    }
}

u8 Gekko::GameSession::GetMinLocalDelay()
{
    u8 min = UINT8_MAX;
    for (auto& player : _msg.locals) {
        min = std::min(_sync.GetLocalDelay(player->handle), min);
    }
    return min;
}

bool Gekko::GameSession::IsPlayingLocally()
{
    return _msg.remotes.empty() && !_msg.locals.empty();
}

bool Gekko::GameSession::IsLockstepActive() const
{
    return _config.input_prediction_window == 0;
}

bool Gekko::GameSession::RollbackPending()
{
    if (IsLockstepActive() || IsPlayingLocally()) {
        return false;
    }

    return _sync.GetMinIncorrectFrame() != GameInput::NULL_FRAME;
}

bool Gekko::GameSession::ConfirmedSaveDue()
{
    if (IsLockstepActive() || !_config.limited_saving || IsPlayingLocally()) {
        return false;
    }

    const Frame diff = _sync.GetCurrentFrame() - (_last_saved_frame + 1);
    return diff > _config.input_prediction_window;
}

Frame Gekko::GameSession::GetConfirmedFrame()
{
    // hold back confirmation while a disconnected players inputs may still grow.
    return std::min(_msg.GetDisconnectHoldFrame(), _sync.GetMinReceivedFrame());
}

bool Gekko::GameSession::ShouldStallAdvance()
{
    // while the claims for a disconnected player are settling a peer may still
    // carry more inputs, dont outrun what the session can roll back to.
    const Frame hold = _msg.GetDisconnectHoldFrame();

    if (hold == INT32_MAX) {
        return false;
    }

    return _sync.GetCurrentFrame() - hold > (Frame)_config.input_prediction_window;
}

void Gekko::GameSession::RewindRunahead()
{
    if (_runahead_start_frame == GameInput::NULL_FRAME) {
        return;
    }

    _runahead_start_frame = GameInput::NULL_FRAME;

    // a rollback or confirmed save will load+resim this frame, so dont load twice.
    if (RollbackPending() || ConfirmedSaveDue()) {
        return;
    }

    _game_events.AddRunaheadLoadEvent(_storage);
}

void Gekko::GameSession::HandleRunahead()
{
    if ((IsLockstepActive() && !IsPlayingLocally()) || _runahead_frames == 0) {
        return;
    }

    _runahead_start_frame = _sync.GetCurrentFrame();
    _game_events.AddRunaheadSaveEvent(_sync, _storage);

    _sync.SetRunaheadMode(true);
    for (u8 i = 0; i < _runahead_frames; i++) {
        const bool is_display_frame = (i == _runahead_frames - 1);
        if (!_game_events.AddAdvanceEvent(_sync, false, !is_display_frame)) {
            break;
        }
        _sync.IncrementFrame();
    }
    _sync.SetRunaheadMode(false);

    // Reset back to the real frame so AddLocalInput and network logic see the correct frame
    _sync.SetCurrentFrame(_runahead_start_frame);
}
