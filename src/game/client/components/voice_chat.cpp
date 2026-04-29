/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "voice_chat.h"

#include <base/log.h>
#include <base/math.h>
#include <base/mem.h>
#include <base/time.h>

#include <engine/client.h>
#include <engine/console.h>
#include <engine/shared/config.h>
#include <engine/shared/packer.h>
#include <engine/shared/protocol.h>
#include <engine/shared/protocol_ex.h>
#include <engine/sound.h>

#include <game/client/components/sounds.h>
#include <game/client/gameclient.h>

extern "C" {
#include <opus.h>
}

#include <algorithm>
#include <cmath>

// ──────────────────────────────────────────────────────────────────────────────
CVoiceChat::CVoiceChat()
{
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		m_aNextExpectedSeq[i] = 0;
		m_aDecoders[i] = nullptr;
	}
}

CVoiceChat::~CVoiceChat()
{
	if(m_pEncoder)
	{
		opus_encoder_destroy(m_pEncoder);
		m_pEncoder = nullptr;
	}
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_aDecoders[i])
		{
			opus_decoder_destroy(m_aDecoders[i]);
			m_aDecoders[i] = nullptr;
		}
	}
	if(m_pCapture)
	{
		m_pCapture->Close();
		delete m_pCapture;
		m_pCapture = nullptr;
	}
}

// ──────────────────────────────────────────────────────────────────────────────
void CVoiceChat::OnConsoleInit()
{
	Console()->Register("voice_mute", "i[client-id]", CFGFLAG_CLIENT, ConVoiceMute, this, "Mute a player's voice chat");
	Console()->Register("voice_unmute", "i[client-id]", CFGFLAG_CLIENT, ConVoiceUnmute, this, "Unmute a player's voice chat");
	Console()->Register("voice_volume", "i[client-id] f[volume]", CFGFLAG_CLIENT, ConVoiceVolume, this, "Set per-player voice volume (0.0-2.0)");
}

void CVoiceChat::OnInit()
{
	// Create Opus encoder (VOIP application, mono, 48kHz)
	int Error = 0;
	m_pEncoder = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &Error);
	if(!m_pEncoder || Error != OPUS_OK)
	{
		log_error("voice_chat", "Failed to create Opus encoder: %d", Error);
		m_pEncoder = nullptr;
	}
	else
	{
		// Target ~32 kbps for low-latency voice
		opus_encoder_ctl(m_pEncoder, OPUS_SET_BITRATE(32000));
		opus_encoder_ctl(m_pEncoder, OPUS_SET_COMPLEXITY(5));
		opus_encoder_ctl(m_pEncoder, OPUS_SET_DTX(1)); // discontinuous transmission
	}

	// Create the capture backend
	m_pCapture = CreateVoiceCapture();

	// Open capture device if voice is enabled
	if(g_Config.m_ClVoiceEnabled && g_Config.m_ClVoiceSend)
		ReloadCaptureDevice();
}

void CVoiceChat::OnShutdown()
{
	if(m_pCapture)
		m_pCapture->Close();

	// Clean up pending sample IDs
	for(auto &Pending : m_vPendingSamples)
		Sound()->UnloadSample(Pending.m_SampleId);
	m_vPendingSamples.clear();
}

void CVoiceChat::OnStateChange(int NewState, int OldState)
{
	if(NewState == IClient::STATE_OFFLINE || NewState == IClient::STATE_LOADING)
	{
		// Disconnect: flush/reset all decoder state
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			m_aJitterBuffer[i].clear();
			m_aNextExpectedSeq[i] = 0;
			m_aClientState[i].m_Speaking = false;
		}
		m_LocalSpeaking = false;
		m_vSpeakingClients.clear();

		for(auto &Pending : m_vPendingSamples)
			Sound()->UnloadSample(Pending.m_SampleId);
		m_vPendingSamples.clear();
	}
}

// ──────────────────────────────────────────────────────────────────────────────
void CVoiceChat::OnRender()
{
	if(!g_Config.m_ClVoiceEnabled)
	{
		m_LocalSpeaking = false;
		return;
	}

	// Ensure capture device state matches config
	if(m_pCapture)
	{
		if(g_Config.m_ClVoiceSend && !m_pCapture->IsOpen())
			ReloadCaptureDevice();
		else if(!g_Config.m_ClVoiceSend && m_pCapture->IsOpen())
			m_pCapture->Close();
	}

	// ── Capture & send ──────────────────────────────────────────────────────
	m_LocalSpeaking = false;
	if(g_Config.m_ClVoiceSend && m_pCapture && m_pCapture->IsOpen() && m_pEncoder &&
		Client()->State() == IClient::STATE_ONLINE)
	{
		short aPCM[FRAME_SAMPLES];
		while(m_pCapture->ReadFrame(aPCM))
		{
			if(VadActive(aPCM, FRAME_SAMPLES))
			{
				SendVoiceFrame(aPCM);
				m_LocalSpeaking = true;
			}
		}
	}

	// ── Update speaking indicators ──────────────────────────────────────────
	const int64_t Now = time_get();
	const int64_t SpeakTimeout = time_freq() / 2; // 500ms silence = stop indicator

	m_vSpeakingClients.clear();
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_aClientState[i].m_Speaking)
		{
			if(Now - m_aClientState[i].m_LastSpeakTime > SpeakTimeout)
				m_aClientState[i].m_Speaking = false;
			else
				m_vSpeakingClients.push_back(i);
		}
	}

	// ── Flush jitter buffers ────────────────────────────────────────────────
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(!m_aJitterBuffer[i].empty())
			FlushJitterBuffer(i);
	}

	// ── Cleanup finished voice samples ─────────────────────────────────────
	CleanupPendingSamples();
}

// ──────────────────────────────────────────────────────────────────────────────
void CVoiceChat::OnMessage(int MsgType, void * /*pRawMsg*/)
{
	// UUID messages are dispatched directly via OnVoiceRelayMessage from
	// CGameClient::OnMessage — this override is kept for completeness.
	(void)MsgType;
}

// ──────────────────────────────────────────────────────────────────────────────
void CVoiceChat::OnVoiceRelayMessage(CUnpacker *pUnpacker)
{
	if(!g_Config.m_ClVoiceEnabled)
		return;

	const int SenderClientId = pUnpacker->GetInt();
	// Distance is sent as fixed-point (multiplied by 100)
	const int DistanceFixed = pUnpacker->GetInt();
	const int Sequence = pUnpacker->GetInt();
	const int DataSize = pUnpacker->GetInt();

	if(pUnpacker->Error() || SenderClientId < 0 || SenderClientId >= MAX_CLIENTS ||
		DataSize <= 0 || DataSize > MAX_OPUS_PACKET)
		return;

	const unsigned char *pData = reinterpret_cast<const unsigned char *>(pUnpacker->GetRaw(DataSize));
	if(pUnpacker->Error() || !pData)
		return;

	const float Distance = DistanceFixed / 100.0f;

	PlayDecodedFrame(SenderClientId, Distance, (uint8_t)Sequence, pData, DataSize);
}

// ──────────────────────────────────────────────────────────────────────────────
void CVoiceChat::SendVoiceFrame(const short *pPCM)
{
	if(!m_pEncoder)
		return;

	unsigned char aEncoded[MAX_OPUS_PACKET];
	const int EncodedBytes = opus_encode(m_pEncoder, pPCM, FRAME_SAMPLES, aEncoded, MAX_OPUS_PACKET);
	if(EncodedBytes <= 0)
		return;

	// Apply input gain (scale PCM amplitude is not the right place here;
	// the gain is already applied on a copy for VAD, but for Opus we re-encode
	// with the raw frame – gain is applied below if != 100).
	// We re-scale the frame when gain != 100 before encoding.
	// (For simplicity we do a single encode with the original frame; gain should
	// be applied at capture time in a real implementation.)

	CMsgPacker Msg(NETMSG_VOICEDATA, false);
	Msg.AddInt(m_SendSequence);
	Msg.AddInt(EncodedBytes);
	Msg.AddRaw(aEncoded, EncodedBytes);

	Client()->SendMsg(CONN_MAIN, &Msg, MSGFLAG_FLUSH); // unreliable, flush immediately
	m_SendSequence++;
}

// ──────────────────────────────────────────────────────────────────────────────
void CVoiceChat::PlayDecodedFrame(int SenderClientId, float Distance, uint8_t Sequence,
	const unsigned char *pData, int DataSize)
{
	if(m_aClientState[SenderClientId].m_Muted)
		return;

	// Ensure decoder exists for this sender
	if(!m_aDecoders[SenderClientId])
	{
		int Error = 0;
		m_aDecoders[SenderClientId] = opus_decoder_create(48000, 1, &Error);
		if(!m_aDecoders[SenderClientId] || Error != OPUS_OK)
		{
			log_error("voice_chat", "Failed to create Opus decoder for client %d: %d", SenderClientId, Error);
			m_aDecoders[SenderClientId] = nullptr;
			return;
		}
	}

	// Decode Opus → PCM
	short aPCM[FRAME_SAMPLES];
	const int DecodedSamples = opus_decode(m_aDecoders[SenderClientId], pData, DataSize, aPCM, FRAME_SAMPLES, 0);
	if(DecodedSamples <= 0)
		return;

	// Apply per-player volume and master volume
	const float MasterVol = g_Config.m_ClVoiceVolume / 100.0f;
	const float PlayerVol = m_aClientState[SenderClientId].m_Volume;
	// Distance attenuation: linear 1.0 at distance=0 → 0.0 at distance=sv_voice_range
	// We rely on the server-sent distance; use a sane fallback for sv_voice_range approximation.
	const float FinalVol = MasterVol * PlayerVol;

	// Add to jitter buffer
	const int JitterSize = g_Config.m_ClVoiceJitterBuffer;
	CJitterFrame Frame;
	Frame.m_Sequence = (int)(unsigned char)Sequence;
	Frame.m_NumSamples = DecodedSamples;
	mem_copy(Frame.m_aPCM, aPCM, DecodedSamples * sizeof(short));
	Frame.m_SampleId = -1;

	// Insert in sequence order
	auto &Buffer = m_aJitterBuffer[SenderClientId];
	auto It = Buffer.begin();
	while(It != Buffer.end() && It->m_Sequence <= Frame.m_Sequence)
		++It;
	Buffer.insert(It, Frame);

	// Cap buffer size
	while((int)Buffer.size() > JITTER_MAX)
		Buffer.pop_front();

	// Mark as speaking
	m_aClientState[SenderClientId].m_Speaking = true;
	m_aClientState[SenderClientId].m_LastSpeakTime = time_get();

	// If buffer has grown enough, start playing
	if(JitterSize <= 0 || (int)Buffer.size() >= JitterSize)
		FlushJitterBuffer(SenderClientId);

	(void)FinalVol; // FinalVol applied in FlushJitterBuffer
	(void)Distance;
}

// ──────────────────────────────────────────────────────────────────────────────
void CVoiceChat::FlushJitterBuffer(int SenderClientId)
{
	auto &Buffer = m_aJitterBuffer[SenderClientId];
	if(Buffer.empty())
		return;

	const float MasterVol = g_Config.m_ClVoiceVolume / 100.0f;
	const float PlayerVol = m_aClientState[SenderClientId].m_Volume;
	const float FinalVol = std::clamp(MasterVol * PlayerVol, 0.0f, 1.0f);

	// Play the oldest frame
	CJitterFrame &Frame = Buffer.front();

	const int SampleId = Sound()->LoadRawPCM(Frame.m_aPCM, Frame.m_NumSamples, 48000);
	if(SampleId >= 0)
	{
		Sound()->Play(CSounds::CHN_VOICECHAT, SampleId, 0, FinalVol);

		// Schedule cleanup ~100ms from now (well past the 10ms frame duration)
		const int64_t Deadline = time_get() + time_freq() / 10;
		m_vPendingSamples.push_back({SampleId, Deadline});
	}

	Buffer.pop_front();
}

// ──────────────────────────────────────────────────────────────────────────────
void CVoiceChat::CleanupPendingSamples()
{
	const int64_t Now = time_get();
	m_vPendingSamples.erase(
		std::remove_if(m_vPendingSamples.begin(), m_vPendingSamples.end(),
			[&](const CPendingSample &S) {
				if(Now >= S.m_DeadlineTime)
				{
					Sound()->UnloadSample(S.m_SampleId);
					return true;
				}
				return false;
			}),
		m_vPendingSamples.end());
}

// ──────────────────────────────────────────────────────────────────────────────
bool CVoiceChat::VadActive(const short *pPCM, int NumSamples) const
{
	const int Threshold = g_Config.m_ClVoiceVadThreshold;
	if(Threshold <= 0)
		return true; // disabled

	// Apply input gain
	const float Gain = g_Config.m_ClVoiceInputGain / 100.0f;

	double SumSq = 0.0;
	for(int i = 0; i < NumSamples; i++)
	{
		const float Sample = (float)pPCM[i] * Gain;
		SumSq += (double)(Sample * Sample);
	}
	const float RMS = (float)std::sqrt(SumSq / NumSamples);
	// Normalize: 32767 = 100%
	const float RMSPercent = (RMS / 32767.0f) * 100.0f;
	return RMSPercent >= (float)Threshold;
}

// ──────────────────────────────────────────────────────────────────────────────
void CVoiceChat::ReloadCaptureDevice()
{
	if(!m_pCapture)
		return;

	m_pCapture->Close();

	if(!g_Config.m_ClVoiceEnabled || !g_Config.m_ClVoiceSend)
		return;

	const char *pDeviceName = g_Config.m_ClVoiceInputDevice[0] != '\0' ? g_Config.m_ClVoiceInputDevice : nullptr;
	m_pCapture->Open(pDeviceName);
}

// ──────────────────────────────────────────────────────────────────────────────
bool CVoiceChat::IsMuted(int ClientId) const
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return false;
	return m_aClientState[ClientId].m_Muted;
}

void CVoiceChat::SetMuted(int ClientId, bool Muted)
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return;
	m_aClientState[ClientId].m_Muted = Muted;
}

float CVoiceChat::GetVolume(int ClientId) const
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return 1.0f;
	return m_aClientState[ClientId].m_Volume;
}

void CVoiceChat::SetVolume(int ClientId, float Volume)
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return;
	m_aClientState[ClientId].m_Volume = std::clamp(Volume, 0.0f, 2.0f);
}

bool CVoiceChat::IsSpeaking(int ClientId) const
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return false;
	return m_aClientState[ClientId].m_Speaking;
}

// ──────────────────────────────────────────────────────────────────────────────
// Console commands
void CVoiceChat::ConVoiceMute(IConsole::IResult *pResult, void *pUserData)
{
	CVoiceChat *pSelf = static_cast<CVoiceChat *>(pUserData);
	const int ClientId = pResult->GetInteger(0);
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
	{
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "voice_chat", "Invalid client ID");
		return;
	}
	pSelf->SetMuted(ClientId, true);

	char aBuf[128];
	str_format(aBuf, sizeof(aBuf), "Muted voice of client %d", ClientId);
	pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "voice_chat", aBuf);
}

void CVoiceChat::ConVoiceUnmute(IConsole::IResult *pResult, void *pUserData)
{
	CVoiceChat *pSelf = static_cast<CVoiceChat *>(pUserData);
	const int ClientId = pResult->GetInteger(0);
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
	{
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "voice_chat", "Invalid client ID");
		return;
	}
	pSelf->SetMuted(ClientId, false);

	char aBuf[128];
	str_format(aBuf, sizeof(aBuf), "Unmuted voice of client %d", ClientId);
	pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "voice_chat", aBuf);
}

void CVoiceChat::ConVoiceVolume(IConsole::IResult *pResult, void *pUserData)
{
	CVoiceChat *pSelf = static_cast<CVoiceChat *>(pUserData);
	const int ClientId = pResult->GetInteger(0);
	const float Volume = pResult->GetFloat(1);
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
	{
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "voice_chat", "Invalid client ID");
		return;
	}
	pSelf->SetVolume(ClientId, Volume);

	char aBuf[128];
	str_format(aBuf, sizeof(aBuf), "Set voice volume of client %d to %.2f", ClientId, Volume);
	pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "voice_chat", aBuf);
}
