/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "voice_chat.h"

#include <base/log.h>
#include <base/math.h>

extern "C" {
#include <opus/opus.h>
}

#include <SDL.h>

// 48 kHz, mono, 16-bit signed — matches DDNet's mixing rate.
static constexpr int VOICE_SAMPLE_RATE = 48000;
// Maximum PCM samples buffered per player before old data is discarded.
static constexpr int VOICE_PLAYBACK_MAX_SAMPLES = VOICE_SAMPLE_RATE * 2; // 2 s

// SDL capture callback — forwarded to CVoiceChat.
static void SdlCaptureCallback(void *pUser, Uint8 *pStream, int Len)
{
	CVoiceChat *pVoice = static_cast<CVoiceChat *>(pUser);
	pVoice->OnCaptureCallback(pStream, Len);
}

CVoiceChat::CVoiceChat() = default;
CVoiceChat::~CVoiceChat()
{
	Shutdown();
}

int CVoiceChat::Init()
{
	// Create Opus encoder: 48 kHz, mono, VOIP.
	int OpusError = 0;
	m_pEncoder = opus_encoder_create(VOICE_SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &OpusError);
	if(!m_pEncoder || OpusError != OPUS_OK)
	{
		log_error("voice_chat", "Failed to create Opus encoder: %s", opus_strerror(OpusError));
		m_pEncoder = nullptr;
		return -1;
	}
	// Low-latency encoding for real-time voice.
	opus_encoder_ctl(m_pEncoder, OPUS_SET_BITRATE(24000));
	opus_encoder_ctl(m_pEncoder, OPUS_SET_COMPLEXITY(5));
	opus_encoder_ctl(m_pEncoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

	// Open the default capture device (microphone).
	SDL_AudioSpec Want, Got;
	SDL_zero(Want);
	Want.freq = VOICE_SAMPLE_RATE;
	Want.format = AUDIO_S16SYS;
	Want.channels = 1; // mono input
	Want.samples = VOICE_OPUS_FRAME_SIZE;
	Want.callback = SdlCaptureCallback;
	Want.userdata = this;

	// iscapture = 1
	m_CaptureDevice = SDL_OpenAudioDevice(nullptr, 1, &Want, &Got, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
	if(m_CaptureDevice == 0)
	{
		log_error("voice_chat", "Failed to open capture device: %s", SDL_GetError());
		// Non-fatal: the game still works without voice input.
		return 0;
	}

	log_info("voice_chat", "Capture device opened at %d Hz", Got.freq);
	// Device starts paused; capture begins in StartCapture().
	return 0;
}

void CVoiceChat::Update()
{
	if(!m_pEncoder)
		return;

	// Drain raw capture buffer in VOICE_OPUS_FRAME_SIZE chunks.
	while(true)
	{
		short aBuf[VOICE_OPUS_FRAME_SIZE];
		{
			const CLockScope LockScope(m_CaptureLock);
			if((int)m_RawCaptureBuf.size() < VOICE_OPUS_FRAME_SIZE)
				break;
			for(int i = 0; i < VOICE_OPUS_FRAME_SIZE; ++i)
			{
				aBuf[i] = m_RawCaptureBuf.front();
				m_RawCaptureBuf.pop_front();
			}
		}
		EncodeFrame(aBuf);
	}
}

void CVoiceChat::Shutdown()
{
	StopCapture();

	if(m_CaptureDevice)
	{
		SDL_CloseAudioDevice(m_CaptureDevice);
		m_CaptureDevice = 0;
	}

	if(m_pEncoder)
	{
		opus_encoder_destroy(m_pEncoder);
		m_pEncoder = nullptr;
	}

	for(int i = 0; i < VOICE_MAX_CLIENTS; ++i)
	{
		if(m_apDecoders[i])
		{
			opus_decoder_destroy(m_apDecoders[i]);
			m_apDecoders[i] = nullptr;
		}
	}
}

void CVoiceChat::StartCapture()
{
	if(m_CaptureDevice && !m_Capturing.load(std::memory_order_relaxed))
	{
		m_Capturing.store(true, std::memory_order_relaxed);
		SDL_PauseAudioDevice(m_CaptureDevice, 0); // 0 = unpause
	}
}

void CVoiceChat::StopCapture()
{
	if(m_CaptureDevice && m_Capturing.load(std::memory_order_relaxed))
	{
		SDL_PauseAudioDevice(m_CaptureDevice, 1); // 1 = pause
		m_Capturing.store(false, std::memory_order_relaxed);
	}
}

std::vector<CVoicePacket> CVoiceChat::TakeEncodedPackets()
{
	const CLockScope LockScope(m_PacketLock);
	std::vector<CVoicePacket> Out;
	Out.swap(m_aEncodedPackets);
	return Out;
}

void CVoiceChat::PushEncodedAudio(int ClientId, const unsigned char *pData, int Size)
{
	if(ClientId < 0 || ClientId >= VOICE_MAX_CLIENTS)
		return;
	if(!EnsureDecoder(ClientId))
		return;

	// Decode into a temporary PCM buffer (mono, 48 kHz).
	short aDecoded[VOICE_OPUS_FRAME_SIZE * 6]; // up to 120 ms worth
	const int Frames = opus_decode(m_apDecoders[ClientId], pData, Size, aDecoded, VOICE_OPUS_FRAME_SIZE * 6, 0);
	if(Frames <= 0)
	{
		log_error("voice_chat", "opus_decode error for client %d: %s", ClientId, opus_strerror(Frames));
		return;
	}

	// Upmix mono → stereo and append to playback buffer.
	CVoicePlaybackBuffer &PlayBuf = m_aPlayback[ClientId];
	const CLockScope PlayLockScope(PlayBuf.m_Lock);
	PlayBuf.m_Active = true;
	for(int i = 0; i < Frames; ++i)
	{
		PlayBuf.m_Samples.push_back(aDecoded[i]); // L
		PlayBuf.m_Samples.push_back(aDecoded[i]); // R
	}
	// Prevent unbounded growth: discard oldest data if buffer is too large.
	while((int)PlayBuf.m_Samples.size() > VOICE_PLAYBACK_MAX_SAMPLES * 2)
	{
		PlayBuf.m_Samples.pop_front();
		PlayBuf.m_Samples.pop_front();
	}
}

int CVoiceChat::ReadPlaybackPCM(int ClientId, short *pOut, int Frames)
{
	if(ClientId < 0 || ClientId >= VOICE_MAX_CLIENTS)
		return 0;

	CVoicePlaybackBuffer &PlayBuf = m_aPlayback[ClientId];
	const CLockScope LockScope(PlayBuf.m_Lock);
	if(!PlayBuf.m_Active || PlayBuf.m_Samples.empty())
		return 0;

	const int Available = (int)PlayBuf.m_Samples.size() / 2; // stereo frames
	const int ToWrite = minimum(Frames, Available);
	for(int i = 0; i < ToWrite * 2; ++i)
	{
		pOut[i] = PlayBuf.m_Samples.front();
		PlayBuf.m_Samples.pop_front();
	}
	if(PlayBuf.m_Samples.empty())
		PlayBuf.m_Active = false;
	return ToWrite;
}

void CVoiceChat::OnCaptureCallback(const Uint8 *pStream, int Len)
{
	if(!m_Capturing.load(std::memory_order_relaxed))
		return;

	const short *pSamples = reinterpret_cast<const short *>(pStream);
	const int NumSamples = Len / (int)sizeof(short);

	const CLockScope LockScope(m_CaptureLock);
	for(int i = 0; i < NumSamples; ++i)
		m_RawCaptureBuf.push_back(pSamples[i]);
}

void CVoiceChat::EncodeFrame(const short *pPCM)
{
	CVoicePacket Packet;
	const int Bytes = opus_encode(m_pEncoder, pPCM, VOICE_OPUS_FRAME_SIZE, Packet.m_aData, VOICE_OPUS_MAX_PACKET_BYTES);
	if(Bytes <= 0)
	{
		log_error("voice_chat", "opus_encode error: %s", opus_strerror(Bytes));
		return;
	}
	Packet.m_Size = Bytes;

	const CLockScope LockScope(m_PacketLock);
	m_aEncodedPackets.push_back(Packet);
}

bool CVoiceChat::EnsureDecoder(int ClientId)
{
	if(m_apDecoders[ClientId])
		return true;

	int OpusError = 0;
	m_apDecoders[ClientId] = opus_decoder_create(VOICE_SAMPLE_RATE, 1, &OpusError);
	if(!m_apDecoders[ClientId] || OpusError != OPUS_OK)
	{
		log_error("voice_chat", "Failed to create Opus decoder for client %d: %s", ClientId, opus_strerror(OpusError));
		m_apDecoders[ClientId] = nullptr;
		return false;
	}
	return true;
}
