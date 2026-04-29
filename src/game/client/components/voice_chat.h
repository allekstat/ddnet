/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_VOICE_CHAT_H
#define GAME_CLIENT_COMPONENTS_VOICE_CHAT_H

#include <engine/shared/protocol.h>
#include <engine/sound.h>
#include <engine/voice_capture.h>

#include <game/client/component.h>

#include <deque>
#include <memory>
#include <vector>

struct OpusEncoder;
struct OpusDecoder;
class CUnpacker;

/*
 * CVoiceChat – client-side proximity voice chat component.
 *
 * Responsibilities:
 *  - Opens the microphone via IVoiceCapture.
 *  - Encodes captured PCM with Opus and sends NETMSG_VOICEDATA to the server.
 *  - Receives NETMSG_VOICERELAY, decodes Opus and plays via ISound::CHN_VOICECHAT.
 *  - Implements per-player mute and volume settings.
 *  - Tracks which players are currently speaking (for HUD indicator and scoreboard icon).
 */
class CVoiceChat : public CComponent
{
public:
	// Maximum compressed Opus frame bytes (mono, 48kHz, 10ms @128kbps)
	static constexpr int MAX_OPUS_PACKET = 400;
	// PCM frame size: 10ms at 48kHz
	static constexpr int FRAME_SAMPLES = 480;
	// Jitter buffer capacity (in frames, per sender)
	static constexpr int JITTER_MAX = 32;

	// Per-client state kept locally (not sent over network)
	struct CClientVoiceState
	{
		bool m_Muted = false;
		float m_Volume = 1.0f; // 0.0–2.0
		int64_t m_LastSpeakTime = 0; // time_get() when last frame arrived
		bool m_Speaking = false;
	};

	struct CJitterFrame
	{
		int m_Sequence;
		short m_aPCM[FRAME_SAMPLES];
		int m_NumSamples;
		// For tracking sample lifetime
		int m_SampleId = -1;
	};

	CVoiceChat();
	~CVoiceChat();

	int Sizeof() const override { return sizeof(*this); }

	void OnConsoleInit() override;
	void OnInit() override;
	void OnShutdown() override;
	void OnStateChange(int NewState, int OldState) override;
	void OnRender() override;
	void OnMessage(int MsgType, void *pRawMsg) override;

	// Called from CGameClient::OnMessage for UUID messages (NETMSG_VOICERELAY)
	void OnVoiceRelayMessage(CUnpacker *pUnpacker);

	// Returns true if the local player is currently transmitting voice
	bool IsLocalSpeaking() const { return m_LocalSpeaking; }

	// Returns true if the given client is currently speaking
	bool IsSpeaking(int ClientId) const;

	// Per-client mute / volume accessors
	bool IsMuted(int ClientId) const;
	void SetMuted(int ClientId, bool Muted);
	float GetVolume(int ClientId) const;
	void SetVolume(int ClientId, float Volume);

	// Get list of currently-speaking client IDs (for HUD)
	const std::vector<int> &SpeakingClients() const { return m_vSpeakingClients; }

	// Reload capture device (called from settings UI)
	void ReloadCaptureDevice();

private:
	// Opus encoder/decoder
	OpusEncoder *m_pEncoder = nullptr;
	OpusDecoder *m_aDecoders[MAX_CLIENTS] = {};

	// Capture device
	IVoiceCapture *m_pCapture = nullptr;

	// Per-client state
	CClientVoiceState m_aClientState[MAX_CLIENTS];

	// Per-sender jitter buffer (small sorted queue by sequence number)
	std::deque<CJitterFrame> m_aJitterBuffer[MAX_CLIENTS];
	int m_aNextExpectedSeq[MAX_CLIENTS] = {};

	// Outgoing sequence counter
	uint8_t m_SendSequence = 0;

	// Pending sample IDs to unload once they finish playing
	struct CPendingSample
	{
		int m_SampleId;
		int64_t m_DeadlineTime; // time after which we unload (time_get())
	};
	std::vector<CPendingSample> m_vPendingSamples;

	// Encode and send one frame
	void SendVoiceFrame(const short *pPCM);

	// Decode and play one received frame
	void PlayDecodedFrame(int SenderClientId, float Distance, uint8_t Sequence,
		const unsigned char *pData, int DataSize);

	// Process jitter buffer for a sender: play frames that are due
	void FlushJitterBuffer(int SenderClientId);

	// Cleanup pending samples that have finished playing
	void CleanupPendingSamples();

	// Voice Activity Detection: returns true if the frame has enough energy
	bool VadActive(const short *pPCM, int NumSamples) const;

	// Whether the local player is currently transmitting
	bool m_LocalSpeaking = false;

	// Cached list of speaking client IDs (refreshed each render)
	std::vector<int> m_vSpeakingClients;

	static void ConVoiceMute(IConsole::IResult *pResult, void *pUserData);
	static void ConVoiceUnmute(IConsole::IResult *pResult, void *pUserData);
	static void ConVoiceVolume(IConsole::IResult *pResult, void *pUserData);
};

#endif // GAME_CLIENT_COMPONENTS_VOICE_CHAT_H
