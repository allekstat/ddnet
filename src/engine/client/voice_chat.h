/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_CLIENT_VOICE_CHAT_H
#define ENGINE_CLIENT_VOICE_CHAT_H

#include <base/lock.h>

#include <SDL_audio.h>

#include <atomic>
#include <deque>
#include <vector>

// Forward declarations for Opus encoder/decoder types.
struct OpusEncoder;
struct OpusDecoder;

// Maximum size of a single Opus-encoded packet per RFC 6716 §3.2.1.
static constexpr int VOICE_OPUS_MAX_PACKET_BYTES = 1275;
// PCM frame size fed to the Opus encoder per encode call (20 ms @ 48 kHz).
static constexpr int VOICE_OPUS_FRAME_SIZE = 960;
// Maximum number of simultaneous incoming voice streams (one per client slot).
static constexpr int VOICE_MAX_CLIENTS = 64;

// Holds one ready-to-send chunk of Opus-compressed voice data produced by
// the capture callback and consumed by the game-client network tick.
struct CVoicePacket
{
	unsigned char m_aData[VOICE_OPUS_MAX_PACKET_BYTES];
	int m_Size; // actual encoded byte count
};

// Ring buffer of decoded PCM samples queued for playback for one remote player.
struct CVoicePlaybackBuffer
{
	bool m_Active = false; // true while the remote player is speaking
	std::deque<short> m_Samples; // interleaved stereo 48 kHz PCM
	CLock m_Lock;
};

// CVoiceChat manages microphone capture, Opus encoding, and the per-player
// decode/playback buffers required for proximity voice chat.
//
// Lifecycle:
//   Init()       — call once after SDL audio is already initialised.
//   Update()     — call once per game tick; drains m_CaptureQueue into
//                  m_aEncodedPackets (available via TakeEncodedPackets).
//   Shutdown()   — call once before the sound system is destroyed.
//
// Receiving voice from another player:
//   PushEncodedAudio(ClientId, pData, Size) — decode into playback buffer.
//   ReadPlaybackPCM(ClientId, pOut, Frames) — mix into the output stream
//                                             (called from the SDL callback).
class CVoiceChat
{
public:
	CVoiceChat();
	~CVoiceChat();

	// Initialise capture device and Opus encoder/decoders.
	// Returns 0 on success, negative on failure.
	int Init();

	// Process pending captured PCM — encode and queue for sending.
	// Call once per game tick before reading TakeEncodedPackets.
	void Update();

	// Release all resources.
	void Shutdown();

	// True if the capture device was successfully opened.
	bool IsCaptureActive() const { return m_CaptureDevice != 0; }

	// Start/stop push-to-talk capture.
	void StartCapture();
	void StopCapture();
	bool IsCapturing() const { return m_Capturing.load(std::memory_order_relaxed); }

	// Drain the queue of encoded packets ready to send.
	// The caller takes ownership of the returned vector.
	std::vector<CVoicePacket> TakeEncodedPackets();

	// Decode an incoming Opus packet from a remote player and append its
	// PCM samples to that player's playback buffer.
	void PushEncodedAudio(int ClientId, const unsigned char *pData, int Size);

	// Read decoded PCM from a player's playback buffer (called from the SDL
	// audio callback to mix voice into the output stream).
	// pOut must point to (Frames * 2) shorts (stereo interleaved).
	// Returns the number of stereo frames actually written (may be < Frames).
	int ReadPlaybackPCM(int ClientId, short *pOut, int Frames);

	// Called by the SDL capture callback — appends raw PCM to m_RawCaptureBuf.
	void OnCaptureCallback(const Uint8 *pStream, int Len);

private:
	// SDL capture device handle (0 = not open).
	SDL_AudioDeviceID m_CaptureDevice = 0;

	// Opus encoder (48 kHz, mono, VOIP application).
	OpusEncoder *m_pEncoder = nullptr;

	// Per-client Opus decoders and playback ring buffers.
	OpusDecoder *m_apDecoders[VOICE_MAX_CLIENTS] = {};
	CVoicePlaybackBuffer m_aPlayback[VOICE_MAX_CLIENTS];

	// Raw PCM accumulated by the SDL capture callback (mono, 16-bit, 48 kHz).
	// Protected by m_CaptureLock; encoder drains this in Update().
	CLock m_CaptureLock;
	std::deque<short> m_RawCaptureBuf GUARDED_BY(m_CaptureLock);

	// Encoded packets ready for the network tick.
	CLock m_PacketLock;
	std::vector<CVoicePacket> m_aEncodedPackets GUARDED_BY(m_PacketLock);

	// Whether push-to-talk is currently active.
	std::atomic<bool> m_Capturing{false};

	// Encode one VOICE_OPUS_FRAME_SIZE-frame chunk from m_RawCaptureBuf and
	// append result to m_aEncodedPackets.  Call only from Update().
	void EncodeFrame(const short *pPCM);

	// Allocate (or reuse) an Opus decoder for the given client slot.
	bool EnsureDecoder(int ClientId);
};

#endif // ENGINE_CLIENT_VOICE_CHAT_H
