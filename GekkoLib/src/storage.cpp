#include "storage.h"

Gekko::StateStorage::StateStorage()
{
	_max_num_states = 0;
}


void Gekko::StateStorage::Init(u32 num_states, u32 state_size, bool limited)
{
	const u32 num = limited ? 2 : num_states + 2;
	_max_num_states = num;

	// [PovertyCaster #219 — DIVERGES FROM HeatXD UPSTREAM. Both allocations below were
	// std::make_unique<u8[]>(state_size), which VALUE-initialises: it zero-fills the whole buffer and so
	// TOUCHES EVERY PAGE. With num = num_states + 2 (11 at prediction 8, plus the runahead state) that made
	// 11 x state_size RESIDENT physical memory at session start, whether or not the game ever writes those
	// bytes. Measured on qoh99, whose state_size is a 4,772,716 B upper bound (FXSAVE + regions + a 4 MB
	// heap budget) against a ~1.4 MB typical save: 4 peers at ~170 MB peak working set each.
	//
	// unique_ptr<u8[]>(new u8[n]) DEFAULT-initialises — no fill, no touch — so untouched tail pages are
	// never faulted in and physical memory tracks the state the game ACTUALLY writes. It also means every
	// byte later removed from a game's real state converts to RAM saved with no further tuning of
	// state_size, which stays the worst-case bound because GekkoNet's ring slot must fit one.
	//
	// SAFE BECAUSE NOTHING READS PAST state_len (audited across every consumer): game_session.cpp and
	// stress_session.cpp read only frame/checksum, and the checksum is produced by the GAME in the save
	// event, never computed over this buffer. The stale/uninitialised tail is unreachable.
	//
	// ORDERING THAT MADE THIS SAFE — do not reintroduce the fill to "be careful", and do not port this to a
	// tree that has not done the same: until PovertyCaster's #218, the padding between what save() WROTE and
	// state_size was copied into replay tapes and sent to spectators, and was harmless ONLY because this
	// zero-fill made it zeros. Landing this first would have put uninitialised process heap into files and
	// on the wire, and made recordings non-reproducible run to run.
	for (u32 i = 0; i < _max_num_states; i++) {
		_states.push_back(std::make_unique<StateEntry>());
        _states.back().get()->state = std::unique_ptr<u8[]>(new u8[state_size]);
		_states.back().get()->state_len = state_size;
	}

	_runahead_state.state = std::unique_ptr<u8[]>(new u8[state_size]);
	_runahead_state.state_len = state_size;
	_runahead_state.frame = GameInput::NULL_FRAME;
}

Gekko::StateEntry* Gekko::StateStorage::GetRunaheadState()
{
	return &_runahead_state;
}

Gekko::StateEntry* Gekko::StateStorage::GetState(Frame frame)
{
	frame = frame < 0 ? frame + _max_num_states : frame;
	return _states[frame % _max_num_states].get();
}
