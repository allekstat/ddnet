/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_VOICE_CAPTURE_H
#define ENGINE_VOICE_CAPTURE_H

#include <engine/kernel.h>

/*
 * IVoiceCapture: Interface for capturing audio from the microphone.
 *
 * The implementation opens an SDL audio capture device and writes
 * captured PCM frames into a ring buffer.  The game loop reads frames
 * from the ring buffer and encodes them with Opus.
 *
 * All public methods are safe to call from the main thread.
 * The SDL capture callback runs on a separate audio thread and only
 * accesses the ring buffer via atomic operations / a lock.
 */
class IVoiceCapture : public IInterface
{
	MACRO_INTERFACE("voicecapture")
public:
	// Maximum Opus frame size in samples at 48 kHz (60 ms)
	static constexpr int OPUS_MAX_FRAME_SIZE = 2880;
	// Voice chat frame size: 10 ms at 48 kHz
	static constexpr int FRAME_SAMPLES = 480;
	// Ring buffer capacity in frames
	static constexpr int RING_FRAMES = 64;

	virtual ~IVoiceCapture() = default;

	// Open the capture device.  Returns true on success.
	// pDeviceName: UTF-8 device name, or empty/null for the system default.
	virtual bool Open(const char *pDeviceName) = 0;

	// Close the capture device.
	virtual void Close() = 0;

	// Returns true if the device is currently open.
	virtual bool IsOpen() const = 0;

	// Returns the number of complete frames (each FRAME_SAMPLES samples) available in the ring buffer.
	virtual int AvailableFrames() const = 0;

	// Copy one frame (FRAME_SAMPLES s16 samples) from the ring buffer into pOut.
	// Returns true if a frame was available.
	virtual bool ReadFrame(short *pOut) = 0;

	// Enumerate available capture device names.
	// pNames: array of const char * to fill, MaxNames: max entries.
	// Returns the number of devices found.
	virtual int GetDeviceCount() const = 0;
	virtual const char *GetDeviceName(int Index) const = 0;
};

extern IVoiceCapture *CreateVoiceCapture();

#endif // ENGINE_VOICE_CAPTURE_H
