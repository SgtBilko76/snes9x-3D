#pragma once

#include <cstdint>

namespace audio {

constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;

bool Start();
void Stop();

// Appends interleaved stereo samples.  `count` is the number of int16 values,
// not frames.  Drops the tail if the ring is full rather than blocking.
void Push(const int16_t *samples, int count);

// Blocks until the ring has room for roughly one SNES frame of audio.  This is
// what paces the emulation thread.
void WaitForRoom();

// Releases anyone blocked in WaitForRoom, for shutdown.
void WakeAll();

} // namespace audio
