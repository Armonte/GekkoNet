#pragma once

#include <vector>

#include "gekko_types.h"
#include "input.h"

namespace Gekko {

	class SyncSystem {
	public:
		SyncSystem();

		void Init(u8 num_players, u32 input_size, u32 buffer_size = InputBuffer::DEFAULT_BUFF_SIZE);

		void AddLocalInput(Handle player, u8* input);

		void AddRemoteInput(Handle player, u8* input, Frame frame);

		void IncrementFrame();

		bool GetCurrentInputs(std::unique_ptr<u8[]>& inputs, Frame& frame);

		void SetRunaheadMode(bool running_ahead);

		bool GetSpectatorInputs(std::unique_ptr<u8[]>& inputs, Frame frame);

		bool GetLocalInput(Handle player, std::unique_ptr<u8[]>& input, Frame frame);

		void SetLocalDelay(Handle player, u8 delay);
		
		u8 GetLocalDelay(Handle player);

		void SetInputPredictionWindow(Handle player, u8 input_window);

		Frame GetCurrentFrame() const;

		void SetCurrentFrame(Frame frame);

		Frame GetMinIncorrectFrame();

		Frame GetMinReceivedFrame();

        Frame GetLastReceivedFrom(Handle player);
        // [PovertyCaster #83] THE ADVANCE GATE'S OWN STATE. GetCurrentInputs() returns false -- and the
        // session cannot advance -- when ANY player's buffer has no input at _current_frame. Everything
        // else is downstream of that one fact, and three hypotheses were argued without ever reading it.
        Frame StallCurrentFrame() const { return _current_frame; }
        Frame StallLastReceived(u8 player) const {
            return player < _num_players ? _input_buffers[player].LastReceived() : (Frame)-1;
        }
        u8    StallNumPlayers() const { return _num_players; }

        void ClearIncorrectFramesUpTo(Frame clear_limit);

	private:
		u8 _num_players;

		u32 _input_size;

		Frame _current_frame;

		std::unique_ptr<InputBuffer[]> _input_buffers;
	};
}
