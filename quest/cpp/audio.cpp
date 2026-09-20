#include "audio.h"
#include "log.h"

#include <aaudio/AAudio.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>

namespace audio {
namespace {

// ~250 ms of headroom.  Must be a power of two.
constexpr int kRingSize = 32768;
constexpr int kRingMask = kRingSize - 1;
constexpr int kFrameSamples = (kSampleRate / 60) * kChannels;

int16_t g_ring[kRingSize];
std::atomic<uint32_t> g_write{0};   // emulation thread
std::atomic<uint32_t> g_read{0};    // AAudio callback thread

std::mutex g_wake_mutex;
std::condition_variable g_wake;
std::atomic<bool> g_shutdown{false};

AAudioStream *g_stream = nullptr;
int16_t g_last_sample[kChannels] = {0, 0};

int Available()
{
	return static_cast<int>(g_write.load(std::memory_order_acquire) -
	                        g_read.load(std::memory_order_acquire));
}

aaudio_data_callback_result_t DataCallback(AAudioStream *, void *,
                                           void *audio_data, int32_t num_frames)
{
	int16_t *dest = static_cast<int16_t *>(audio_data);
	int wanted = num_frames * kChannels;
	int available = Available();
	int copy = wanted < available ? wanted : available;

	uint32_t read = g_read.load(std::memory_order_relaxed);
	for (int i = 0; i < copy; i++)
		dest[i] = g_ring[(read + i) & kRingMask];

	g_read.store(read + copy, std::memory_order_release);

	if (copy >= kChannels)
		memcpy(g_last_sample, &dest[copy - kChannels], sizeof(g_last_sample));

	// Underrun: hold the last sample instead of clicking to silence.
	for (int i = copy; i < wanted; i++)
		dest[i] = g_last_sample[i % kChannels];

	g_wake.notify_one();
	return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void ErrorCallback(AAudioStream *, void *, aaudio_result_t error)
{
	LOGW("audio: stream error %d", error);
}

} // namespace

bool Start()
{
	AAudioStreamBuilder *builder = nullptr;
	aaudio_result_t result = AAudio_createStreamBuilder(&builder);
	if (result != AAUDIO_OK)
	{
		LOGE("audio: cannot create stream builder (%d)", result);
		return false;
	}

	AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
	AAudioStreamBuilder_setChannelCount(builder, kChannels);
	AAudioStreamBuilder_setSampleRate(builder, kSampleRate);
	AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
	AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
	AAudioStreamBuilder_setDataCallback(builder, DataCallback, nullptr);
	AAudioStreamBuilder_setErrorCallback(builder, ErrorCallback, nullptr);

	result = AAudioStreamBuilder_openStream(builder, &g_stream);
	AAudioStreamBuilder_delete(builder);

	if (result != AAUDIO_OK)
	{
		LOGE("audio: cannot open stream (%d)", result);
		return false;
	}

	AAudioStream_setBufferSizeInFrames(
		g_stream, AAudioStream_getFramesPerBurst(g_stream) * 4);

	result = AAudioStream_requestStart(g_stream);
	if (result != AAUDIO_OK)
	{
		LOGE("audio: cannot start stream (%d)", result);
		return false;
	}

	LOGI("audio: %d Hz, burst %d frames", AAudioStream_getSampleRate(g_stream),
	     AAudioStream_getFramesPerBurst(g_stream));
	return true;
}

void Stop()
{
	WakeAll();
	if (!g_stream)
		return;

	AAudioStream_requestStop(g_stream);
	AAudioStream_close(g_stream);
	g_stream = nullptr;
}

void Push(const int16_t *samples, int count)
{
	int room = kRingSize - Available();
	if (count > room)
		count = room;

	uint32_t write = g_write.load(std::memory_order_relaxed);
	for (int i = 0; i < count; i++)
		g_ring[(write + i) & kRingMask] = samples[i];

	g_write.store(write + count, std::memory_order_release);
}

void WaitForRoom()
{
	// Keep roughly four frames of audio queued: enough to ride out a slow
	// frame, short enough that input latency stays low.
	constexpr int kTarget = kFrameSamples * 4;

	std::unique_lock<std::mutex> lock(g_wake_mutex);
	while (!g_shutdown.load(std::memory_order_relaxed) && Available() > kTarget)
		g_wake.wait_for(lock, std::chrono::milliseconds(2));
}

void WakeAll()
{
	g_shutdown.store(true);
	g_wake.notify_all();
}

} // namespace audio
