/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "voice_capture.h"

#include <base/log.h>
#include <base/math.h>
#include <base/mem.h>

#include <SDL.h>

#include <atomic>
#include <mutex>
#include <vector>

class CVoiceCapture : public IVoiceCapture
{
	SDL_AudioDeviceID m_Device = 0;
	bool m_Open = false;

	// Ring buffer: RING_FRAMES * FRAME_SAMPLES short samples
	short m_aRingBuffer[RING_FRAMES * FRAME_SAMPLES];
	std::atomic<int> m_WritePos{0}; // written by SDL callback (audio thread)
	std::atomic<int> m_ReadPos{0}; // read by game thread

	// Device name cache populated on Init
	std::vector<std::string> m_vDeviceNames;

	static void SdlCaptureCallback(void *pUser, Uint8 *pStream, int Len)
	{
		CVoiceCapture *pSelf = static_cast<CVoiceCapture *>(pUser);
		const int NumSamples = Len / (int)sizeof(short);
		const short *pIn = reinterpret_cast<const short *>(pStream);

		// Write samples into the ring buffer, one FRAME_SAMPLES chunk at a time.
		int Remaining = NumSamples;
		int InOff = 0;
		while(Remaining >= FRAME_SAMPLES)
		{
			int Write = pSelf->m_WritePos.load(std::memory_order_relaxed);
			int NextWrite = (Write + 1) % RING_FRAMES;
			int Read = pSelf->m_ReadPos.load(std::memory_order_acquire);
			if(NextWrite == Read)
			{
				// Buffer full – drop oldest frame
				pSelf->m_ReadPos.store((Read + 1) % RING_FRAMES, std::memory_order_release);
			}

			mem_copy(&pSelf->m_aRingBuffer[Write * FRAME_SAMPLES], pIn + InOff, FRAME_SAMPLES * sizeof(short));

			// publish write
			pSelf->m_WritePos.store(NextWrite, std::memory_order_release);

			InOff += FRAME_SAMPLES;
			Remaining -= FRAME_SAMPLES;
		}
	}

public:
	CVoiceCapture()
	{
		mem_zero(m_aRingBuffer, sizeof(m_aRingBuffer));
		RefreshDeviceList();
	}

	void RefreshDeviceList()
	{
		m_vDeviceNames.clear();
		const int Count = SDL_GetNumAudioDevices(1 /* capture */);
		for(int i = 0; i < Count; i++)
		{
			const char *pName = SDL_GetAudioDeviceName(i, 1);
			if(pName)
				m_vDeviceNames.emplace_back(pName);
		}
	}

	bool Open(const char *pDeviceName) override
	{
		if(m_Open)
			Close();

		if(SDL_InitSubSystem(SDL_INIT_AUDIO) < 0)
		{
			log_error("voice_capture", "SDL audio init failed: %s", SDL_GetError());
			return false;
		}

		SDL_AudioSpec Want, Got;
		SDL_zero(Want);
		Want.freq = 48000;
		Want.format = AUDIO_S16SYS;
		Want.channels = 1;
		Want.samples = FRAME_SAMPLES;
		Want.callback = SdlCaptureCallback;
		Want.userdata = this;

		const char *pDevice = (pDeviceName && pDeviceName[0] != '\0') ? pDeviceName : nullptr;
		m_Device = SDL_OpenAudioDevice(pDevice, 1 /* capture */, &Want, &Got, 0);
		if(m_Device == 0)
		{
			log_error("voice_capture", "Failed to open capture device '%s': %s",
				pDevice ? pDevice : "<default>", SDL_GetError());
			return false;
		}

		m_Open = true;
		m_WritePos.store(0, std::memory_order_relaxed);
		m_ReadPos.store(0, std::memory_order_relaxed);

		SDL_PauseAudioDevice(m_Device, 0); // start capturing
		log_info("voice_capture", "Opened capture device '%s'", pDevice ? pDevice : "<default>");
		return true;
	}

	void Close() override
	{
		if(!m_Open)
			return;

		SDL_PauseAudioDevice(m_Device, 1);
		SDL_CloseAudioDevice(m_Device);
		m_Device = 0;
		m_Open = false;
	}

	bool IsOpen() const override { return m_Open; }

	int AvailableFrames() const override
	{
		const int Write = m_WritePos.load(std::memory_order_acquire);
		const int Read = m_ReadPos.load(std::memory_order_relaxed);
		return (Write - Read + RING_FRAMES) % RING_FRAMES;
	}

	bool ReadFrame(short *pOut) override
	{
		const int Write = m_WritePos.load(std::memory_order_acquire);
		int Read = m_ReadPos.load(std::memory_order_relaxed);
		if(Read == Write)
			return false;

		mem_copy(pOut, &m_aRingBuffer[Read * FRAME_SAMPLES], FRAME_SAMPLES * sizeof(short));
		m_ReadPos.store((Read + 1) % RING_FRAMES, std::memory_order_release);
		return true;
	}

	int GetDeviceCount() const override { return (int)m_vDeviceNames.size(); }

	const char *GetDeviceName(int Index) const override
	{
		if(Index < 0 || Index >= (int)m_vDeviceNames.size())
			return "";
		return m_vDeviceNames[Index].c_str();
	}

	void Shutdown() override
	{
		Close();
	}
};

IVoiceCapture *CreateVoiceCapture()
{
	return new CVoiceCapture();
}
