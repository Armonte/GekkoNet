/*
BSD 2-Clause License

Copyright (c) 2024-2026, Jamie Meyer

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
#pragma once

#ifdef __cplusplus
extern "C" {
#else
#include <stdbool.h>
#endif

#ifdef _WIN32
#ifdef GEKKONET_STATIC
    // Static library - no import/export needed
#define GEKKONET_API
#else
#ifdef GEKKONET_DLL_EXPORT
    // Building the DLL
#define GEKKONET_API __declspec(dllexport)
#else
    // Using the DLL
#define GEKKONET_API __declspec(dllimport)
#endif
#endif
#else
    // Non-Windows platforms don't need special export macros
#define GEKKONET_API
#endif

// GekkoNet is mostly self contained memory wise using internal memory buffers.
// Technically the user shouldnt need to free or create any memory (look at the examples).
// The only case the user needs to create memory is when slotting in their own GekkoNetAdapter
typedef struct GekkoSession GekkoSession;

typedef enum GekkoSessionType {
    GekkoGameSession, // session for an active player.
    GekkoStressSession, // session to test your local simulation for state desyncs.
    GekkoSpectateSession, // session for spectators watching an active player.
    GekkoReplaySession, // session which plays back a recorded replay.
} GekkoSessionType;

typedef struct GekkoConfig {
    unsigned char num_players;
    unsigned char max_spectators;
    unsigned char input_prediction_window;
    unsigned int spectator_delay;
    unsigned int input_size;
    unsigned int state_size;
    bool limited_saving;
    bool desync_detection;
    unsigned int check_distance;
} GekkoConfig;

typedef enum GekkoPlayerType {
    GekkoLocalPlayer,
    GekkoRemotePlayer,
    GekkoSpectator
} GekkoPlayerType;

typedef struct GekkoNetAddress {
    void* data;
    unsigned int size;
} GekkoNetAddress;

typedef struct GekkoNetResult {
    GekkoNetAddress addr;
    unsigned int data_len;
    void* data;
} GekkoNetResult;

typedef struct GekkoNetAdapter {
    // send data to another peer
    void (*send_data)(GekkoNetAddress* addr, const char* data, int length);
    // receive all packets accumulated between the last frame and now
    // the results within the array will be freed after use using the free_data function
    // the array itself won't be touched
    GekkoNetResult** (*receive_data)(int* length);
    // free data function so gekkonet can cleanup data that the user created.
    void (*free_data)(void* data_ptr);
} GekkoNetAdapter;

typedef enum GekkoGameEventType {
    GekkoEmptyGameEvent = -1,
    GekkoAdvanceEvent,
    GekkoSaveEvent,
    GekkoLoadEvent
} GekkoGameEventType;

typedef struct GekkoGameEvent {
    GekkoGameEventType type;

    union GekkoEventData {
        // events 
        struct GekkoAdvance {
            int frame;
            unsigned int input_len;
            unsigned char* inputs;
            bool rolling_back;
            bool running_ahead;
        } adv;
        struct GekkoSave {
            int frame;
            unsigned int* checksum;
            unsigned int* state_len;
            unsigned char* state;
        } save;
        struct GekkoLoad {
            int frame;
            unsigned int state_len;
            unsigned char* state;
        } load;
    } data;
} GekkoGameEvent;

typedef enum GekkoSessionEventType {
    GekkoEmptySessionEvent = -1,
    GekkoPlayerSyncing,
    GekkoPlayerConnected,
    GekkoPlayerDisconnected,
    GekkoSessionStarted,
    GekkoSpectatorPaused,
    GekkoSpectatorUnpaused,
    GekkoDesyncDetected,
    GekkoReplayFinished
} GekkoSessionEventType;

typedef struct GekkoSessionEvent {
    GekkoSessionEventType type;

    union GekkoSessionData {
        struct GekkoSyncing {
            int handle;
            unsigned char current;
            unsigned char max;
        } syncing;
        struct GekkoConnected {
            int handle;
        } connected;
        struct GekkoDisconnected {
            int handle;
        } disconnected;
        struct GekkoDesynced {
            int frame;
            unsigned int local_checksum;
            unsigned int remote_checksum;
            int remote_handle;
        } desynced;
    } data;
} GekkoSessionEvent;

typedef struct GekkoNetworkStats {
    float kb_sent;
    float kb_received;
    unsigned short last_ping;
    float avg_ping;
    float jitter;
} GekkoNetworkStats;

// [PovertyCaster #233] CROSS-PEER HEALTH-CHECK COVERAGE.
// The desync signal is an EVENT, raised only when two peers' checksums for a frame DISAGREE. There was
// no way to ask "how many frames were actually compared and AGREED", so a session that compared
// thousands of frames and one that compared NOTHING both looked identical from the outside: no desync
// event either way. That makes every clean verdict unfalsifiable. These counters close that hole.
//
// compares_matched counts (frame, remote-peer) PAIRS, not frames: with N peers a single frame can
// contribute up to N-1 matches. Both counters are monotonic for the life of the session.
typedef struct GekkoHealthStats {
    // GATE ON compares_matched. It counts only frames where BOTH peers had a real opinion, so it is
    // the true verification count. A frame where either side reported 0 (pc::kNoChecksum, "no opinion")
    // is NOT verification and lands in one of the abstain buckets instead — counting those as matches
    // was the original bug here and would have made a run that sat in a menu look fully verified.
    unsigned int compares_matched;      // BOTH sides had an opinion and they AGREED  <- real coverage
    unsigned int compares_mismatched;   // BOTH had an opinion and they DISAGREED (raised a desync)
    unsigned int abstained_both;        // neither side had an opinion — nothing to compare
    unsigned int abstained_one_sided;   // exactly one side did; per #112 that is itself a divergence
} GekkoHealthStats;

// Public Facing API

// creates a session of the given type. the handle has to be empty.
GEKKONET_API bool gekko_create(GekkoSession** session, GekkoSessionType session_type);

// destroys the session and clears the handle.
GEKKONET_API bool gekko_destroy(GekkoSession** session);
/* [PovertyCaster #231] copy up to max u32s of recent health-attest records (5 u32s per record:
   frame, checksum, min_received, min_incorrect, stale_from); returns the record count. */
GEKKONET_API int gekko_attest_log(GekkoSession* session, unsigned int* out, int max);

// applies the config to the session, call this before adding any actors.
GEKKONET_API void gekko_start(GekkoSession* session, GekkoConfig* config);

// sets the adapter the session sends and receives its packets with.
GEKKONET_API void gekko_net_adapter_set(GekkoSession* session, GekkoNetAdapter* adapter);

// adds a player or spectator and returns its handle, or -1 when it doesnt fit the session.
// remote actors require an address, local ones dont.
// adding a spectator after the session starts also requires a nonzero state_size.
GEKKONET_API int gekko_add_actor(GekkoSession* session, GekkoPlayerType player_type, GekkoNetAddress* addr);

// disconnects an actor without destroying the session.
// disconnecting a remote actor drops every actor which shares its address.
// disconnecting a local actor means leaving the session and drops every remote actor and spectator.
GEKKONET_API bool gekko_disconnect_actor(GekkoSession* session, int actor);

// sets the time in ms without received packets before an actor is disconnected automatically.
// a timeout of 0 disables automatic disconnecting. defaults to 5000ms.
GEKKONET_API void gekko_set_disconnect_timeout(GekkoSession* session, unsigned int timeout);

// holds a local players input back by the given amount of frames before the
// session uses it, trading input lag for less mispredictions.
GEKKONET_API void gekko_set_local_delay(GekkoSession* session, int player, unsigned char delay);

// simulates the given amount of frames ahead every update and rewinds them
// on the next one, which hides local input delay.
GEKKONET_API void gekko_set_runahead(GekkoSession* session, unsigned char runahead);

// hands the input of a local player to the session, once per player per frame.
GEKKONET_API void gekko_add_local_input(GekkoSession* session, int player, void* input);

// advances the session and returns the events the game has to handle in order.
GEKKONET_API GekkoGameEvent** gekko_update_session(GekkoSession* session, int* count);

// returns the events the last update raised, like actors connecting or a replay finishing.
GEKKONET_API GekkoSessionEvent** gekko_session_events(GekkoSession* session, int* count);

// [PovertyCaster #233] Cross-peer checksum coverage. Returns FALSE when this session type does no
// cross-peer health checking at all (spectator / stress / replay), so "unsupported" is distinguishable
// from "supported and zero" — the whole point of the counter is that a silent zero must not read as proof.
GEKKONET_API bool gekko_health_stats(GekkoSession* session, GekkoHealthStats* stats);

// the average amount of frames this session runs ahead of its remote players.
GEKKONET_API float gekko_frames_ahead(GekkoSession* session);

// copies the network stats gathered for a remote actor or spectator.
GEKKONET_API void gekko_network_stats(GekkoSession* session, int player, GekkoNetworkStats* stats);

// sends and receives packets without advancing the session.
GEKKONET_API void gekko_network_poll(GekkoSession* session);

// starts recording the confirmed inputs of a session. replay sessions cannot record.
// when save_initial_state is set the session stores the gamestate the recording starts at,
// issuing a save event whenever it doesnt hold one already, so playback can start from there.
// the recorded inputs are packed unless disable_compression is set.
GEKKONET_API bool gekko_start_recording(GekkoSession* session, bool save_initial_state, bool disable_compression);

// stops the recording and returns the serialized replay.
// the returned memory is owned by the session and stays valid
// until the session records again or gets destroyed.
GEKKONET_API const unsigned char* gekko_stop_recording(GekkoSession* session, unsigned int* length);

// loads a serialized replay into a replay session.
// the config is stored within the replay so calling gekko_start is not needed.
GEKKONET_API bool gekko_load_replay(GekkoSession* session, const unsigned char* replay_data, unsigned int length);

#ifndef GEKKONET_NO_ASIO

// creates the built in UDP adapter bound to the given port.
// calling this again closes the socket the previous call opened.
GEKKONET_API GekkoNetAdapter* gekko_default_adapter(unsigned short port);

// closes the socket the built in adapter uses.
GEKKONET_API bool gekko_default_adapter_destroy();

#endif // GEKKONET_NO_ASIO

#ifdef __cplusplus  
}
#endif
