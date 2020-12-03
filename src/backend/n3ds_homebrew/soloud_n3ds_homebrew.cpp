/*
SoLoud Nintendo 3DS homebrew output backend
Copyright (c) 2020 Lauren Kelly

Based on the Vita homebrew output backend
Copyright (c) 2017 Ilya Zhuravlev

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

   1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.

   2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.

   3. This notice may not be removed or altered from any source
   distribution.
*/

#include <atomic>
#include <string.h>

#include "soloud.h"
// #include "soloud_thread.h"

#include <3ds.h>
#include <stdio.h>

#if !defined(WITH_N3DS_HOMEBREW)

namespace SoLoud
{
	result n3ds_homebrew_init(Soloud *aSoloud, unsigned int aFlags, unsigned int aSamplerate, unsigned int aBuffer)
	{
		return NOT_IMPLEMENTED;
	}
};

#else

namespace SoLoud
{
	struct N3Data {
		// Signal to wake the thread when data is ready
		LightEvent event;
		std::atomic<bool> done;
		unsigned int samplesPerWavebuf;
		unsigned int numChannels;
		Soloud *soloud;
		::Thread tid;
		int16_t *audioBuffer;
		ndspWaveBuf *wavebufs[2];
	};

	// NDSP audio frame callback
	// This signals the audioThread to decode more things
	// once NDSP has played a sound frame, meaning that there should be
	// one or more available waveBufs to fill with more data.
	static void n3ds_audioCallback(void *const data_) {
		N3Data *const data = (N3Data*)data_;
		// (void)nul_;  // Unused

		if(data->done) { // Quit flag
			return;
		}
		
		LightEvent_Signal(&data->event);
	}

	static void n3ds_cleanup(Soloud *aSoloud) {
		if (!aSoloud->mBackendData)
			return;

		N3Data *data = (N3Data*)aSoloud->mBackendData;

		// Signal the thread to quit
		data->done = true;
		LightEvent_Signal(&data->event);

		threadJoin(data->tid, UINT64_MAX);

		// sceKernelWaitThreadEnd(data->tid, NULL, NULL);
		// sceKernelDeleteThread(data->tid);
		// sceAudioOutReleasePort(data->port);

		free(data->wavebufs);
		free(data->audioBuffer);

		delete data;
		aSoloud->mBackendData = NULL;
	}

	static void n3ds_thread(void *const data_) {
		N3Data *const data = (N3Data*)data_;

		int buf_id = 0;

		while (!data->done) {
			ndspWaveBuf *buffer = data->wavebufs[buf_id];
			data->soloud->mixSigned16(
				(int16_t *)(data->wavebufs[buf_id]->data_vaddr),
				data->samplesPerWavebuf);
			// sceAudioOutOutput(data->port, data->buffer[buf_id]);
			buffer->nsamples = data->samplesPerWavebuf;
			ndspChnWaveBufAdd(0, buffer);
			DSP_FlushDataCache(buffer->data_pcm16,
				data->samplesPerWavebuf * data->numChannels * sizeof(int16_t));

			buf_id ^= 1;

			// 3DS uses cooperative threading, so yield until ndsp is ready for more
			LightEvent_Wait(&data->event);
		}
	}

	result n3ds_homebrew_init(Soloud *aSoloud, unsigned int aFlags, unsigned int aSamplerate, unsigned int aBuffer, unsigned int aChannels)
	{
		// TODO: Add support for multiple sample rates, more than 2 channels
		if (aSamplerate != 44100 || aChannels != 2)
			return INVALID_PARAMETER;
		
		// Initialise NDSP
		if(!ndspInit())
			return UNKNOWN_ERROR;

		// Initialise audio channel
		ndspChnReset(0);
		ndspSetOutputMode(NDSP_OUTPUT_STEREO);
		ndspChnSetInterp(0, NDSP_INTERP_POLYPHASE);
		ndspChnSetRate(0, aSamplerate);
		ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);

		// Allocate a linear block of memory
		N3Data *data = new N3Data;
		memset(data, 0, sizeof(*data)); //TODO: "Using 'memset' on struct that contains a 'std::atomic'"

		// Allocate a linear block of memory for the audio buffer
		const size_t wavebufSize = aChannels * aBuffer * sizeof(int16_t);
		const size_t bufferSize = wavebufSize * 2; // 2 wavebufs

		data->audioBuffer = (int16_t *)linearAlloc(bufferSize);

		data->samplesPerWavebuf = aBuffer;
		data->numChannels = aChannels;
		data->soloud = aSoloud;

		// Initialise wavebufs
		auto myBuffer = data->audioBuffer;
		for(auto &waveBuf : data->wavebufs) {
			waveBuf->data_vaddr = myBuffer;
			waveBuf->status 	= NDSP_WBUF_DONE;

			myBuffer += wavebufSize / sizeof(myBuffer[0]);
		}

		aSoloud->mBackendData = data;
		aSoloud->mBackendCleanupFunc = n3ds_cleanup;

		aSoloud->postinit_internal(aSamplerate, data->samplesPerWavebuf * aChannels, aFlags, aChannels);
		
		// Set ndsp sound frame callback, which signals our audio thread to continue
		ndspSetCallback(n3ds_audioCallback, data);

		// Spawn audio thread

		// Set the thread priority to the main thread's priority ...
		int32_t priority = 0x30;
		svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
		// ... then subtract 1, as lower number => higher actual priority ...
		priority -= 1;
		// ... finally, clamp it between 0x18 and 0x3F to guarantee that it's valid.
		priority = priority < 0x18 ? 0x18 : priority;
		priority = priority > 0x3F ? 0x3F : priority;

		data->tid = threadCreate(n3ds_thread, data,
								 32*1024, priority,
								 -1, false);

		return 0;
	}
};

#endif
