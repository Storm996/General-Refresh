/*
**	Command & Conquer Generals(tm)
**	Copyright 2026 Stephan Vedder
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  (c) 2001-2003 Electronic Arts Inc.                                       //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

// FILE: MiniAudioManager.cpp
/*---------------------------------------------------------------------------*/
/* Project:    RTS3                                                          */
/* File name:  MiniAudioManager.cpp                                          */
/* Created:    Stephan Vedder (feliwir), June 2026                           */
/*             Adapted for GeneralsX Core architecture                       */
/* Desc:       Implementation for the MiniAudioManager, interfacing with    */
/*             the miniaudio Sound System.                                   */
/* Reference:  https://github.com/feliwir/CnC_Generals_Zero_Hour            */
/*---------------------------------------------------------------------------*/

#include "Lib/BaseType.h"
#define MINIAUDIO_IMPLEMENTATION
#include "MiniAudioDevice/MiniAudioManager.h"
#include "MiniAudioDevice/MiniAudioStream.h"

#include "Common/AudioAffect.h"
#include "Common/AudioHandleSpecialValues.h"
#include "Common/AudioRequest.h"
#include "Common/AudioSettings.h"
#include "Common/AsciiString.h"
#include "Common/AudioEventInfo.h"
#include "Common/FileSystem.h"
#include "Common/GameCommon.h"
#include "Common/GameSounds.h"
#include "Common/CRCDebug.h"
#include "Common/GlobalData.h"

#include "GameClient/DebugDisplay.h"
#include "GameClient/Drawable.h"
#include "GameClient/GameClient.h"
#include "GameClient/VideoPlayer.h"
#include "GameClient/View.h"

#include "GameLogic/GameLogic.h"
#include "GameLogic/TerrainLogic.h"
// GeneralsX @bugfix fbraz3 24/06/2026 Save and restore FPU precision mode when calling audio manager entrypoints.
#include "GameLogic/FPUControl.h"

#include "Common/file.h"
// GeneralsX @build win64 11/10/2026 decodeFileToPcmSound uses std::vector<uint8_t> and fixed-width
// int types unconditionally (it is the no-FFmpeg path). Previously <vector> was only pulled in via
// the FFmpeg-gated block, so include it here to keep the win64 minimal build self-contained.
#include <vector>
#include <cstdint>
#include <chrono>

// GeneralsX diag: wall-clock timestamps (ms since process start) for audio lifecycle logging.
// Temporary diagnostic aid; remove together with the [MAUDIO-T]/[MAUDIO-*] stderr probes.
static double MAudioNowMs(void)
{
	static const auto s_epoch = std::chrono::steady_clock::now();
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s_epoch).count();
}
// GeneralsX @build win64 11/10/2026 FFmpeg is only pulled in when the build actually has it.
// Without SAGE_HAVE_FFMPEG (minimal win64) MiniAudioManager decodes via miniaudio's native
// WAV/MP3/FLAC/Vorbis decoders instead (see playAudioEvent).
#ifdef SAGE_HAVE_FFMPEG
#include "VideoDevice/FFmpeg/FFmpegFile.h"

extern "C" {
#include <libavcodec/avcodec.h>
}
#endif

#ifdef _INTERNAL
//#pragma optimize("", off)
#endif

enum { INFINITE_LOOP_COUNT = 1000000 };

// Callback functions for miniaudio's virtual file system
static ma_result vfsFileOpen(ma_vfs *pVFS, const char *filename, ma_uint32 mode, ma_vfs_file *pFile);
static ma_result vfsFileClose(ma_vfs *pVFS, ma_vfs_file file);
static ma_result vfsFileInfo(ma_vfs *pVFS, ma_vfs_file file, ma_file_info *pInfo);
static ma_result vfsFileRead(ma_vfs *pVFS, ma_vfs_file file, void *pBuffer, size_t bytesToRead, size_t *pBytesRead);
static ma_result vfsFileTell(ma_vfs *pVFS, ma_vfs_file file, ma_int64 *pCursor);
static ma_result vfsFileSeek(ma_vfs *pVFS, ma_vfs_file file, ma_int64 offset, ma_seek_origin origin);

//-------------------------------------------------------------------------------------------------
MiniAudioManager::MiniAudioManager() :
	m_playbackDeviceCount(0),
	m_selectedPlaybackDevice(PROVIDER_ERROR),
	m_selectedSpeakerType(0),
	m_lastSelectedPlaybackDevice(PROVIDER_ERROR),
	m_binkHandle(NULL),
	m_pref3DProvider(AsciiString::TheEmptyString),
	m_prefSpeaker(AsciiString::TheEmptyString)
{
}

//-------------------------------------------------------------------------------------------------
MiniAudioManager::~MiniAudioManager()
{
	DEBUG_ASSERTCRASH(m_binkHandle == NULL, ("Leaked a Bink handle. Chuybregts"));
	releaseHandleForBink();
	closeDevice();

	DEBUG_ASSERTCRASH(this == TheAudio, ("Umm...\n"));
	TheAudio = NULL;
}

//-------------------------------------------------------------------------------------------------
#if defined(_DEBUG) || defined(_INTERNAL)
AudioHandle MiniAudioManager::addAudioEvent(const AudioEventRTS *eventToAdd)
{
	ScopedFPUGuard fpuGuard;
	if (TheGlobalData->m_preloadReport) {
		if (!eventToAdd->getEventName().isEmpty()) {
			m_allEventsLoaded.insert(eventToAdd->getEventName());
		}
	}

	return AudioManager::addAudioEvent(eventToAdd);
}
#endif

#if defined(_DEBUG) || defined(_INTERNAL)
//-------------------------------------------------------------------------------------------------
void MiniAudioManager::audioDebugDisplay(DebugDisplayInterface *dd, void *, FILE *fp)
{
	std::list<PlayingAudio *>::iterator it;

	Coord3D lookPos = TheTacticalView->getPosition();
	lookPos.z = TheTerrainLogic->getGroundHeight(lookPos.x, lookPos.y);
	const Coord3D *mikePos = TheAudio->getListenerPosition();
	Coord3D distanceVector = TheTacticalView->get3DCameraPosition();
	distanceVector.sub(*mikePos);

	if (dd)
	{
		dd->printf("MiniAudio version: %s    ", ma_version_string());
		dd->printf("Sound: %s    ", (isOn(AudioAffect_Sound) ? "Yes" : "No"));
		dd->printf("3DSound: %s    ", (isOn(AudioAffect_Sound3D) ? "Yes" : "No"));
		dd->printf("Speech: %s    ", (isOn(AudioAffect_Speech) ? "Yes" : "No"));
		dd->printf("Music: %s\n", (isOn(AudioAffect_Music) ? "Yes" : "No"));
		dd->printf("Channels Available: ");
		// GeneralsX @build d3d9 port 11/10/2026 - Miles SoundManager::getAvailableSamples has no
		// MiniAudio equivalent; report the active playback count so this debug line compiles.
		dd->printf("%d Sounds    ", (Int)m_playingSounds.size());
		dd->printf("Volume: ");
		dd->printf("Sound: %d    ", REAL_TO_INT(m_soundVolume * 100.0f));
		dd->printf("3DSound: %d    ", REAL_TO_INT(m_sound3DVolume * 100.0f));
		dd->printf("Speech: %d    ", REAL_TO_INT(m_speechVolume * 100.0f));
		dd->printf("Music: %d\n", REAL_TO_INT(m_musicVolume * 100.0f));
		dd->printf("Current 3D Provider: %s    ", TheAudio->getProviderName(getSelectedProvider()).str());
		dd->printf("Current Speaker Type: %s\n", TheAudio->translateUnsignedIntToSpeakerType(TheAudio->getSpeakerType()).str());
		dd->printf("Looking at: (%d,%d,%d) -- Microphone at: (%d,%d,%d)\n",
			(Int)lookPos.x, (Int)lookPos.y, (Int)lookPos.z, (Int)mikePos->x, (Int)mikePos->y, (Int)mikePos->z);
		dd->printf("Camera distance from microphone: %d -- Zoom Volume: %d%%\n",
			(Int)distanceVector.length(), (Int)(TheAudio->getZoomVolume() * 100.0f));
		dd->printf("-----------------------------------------------------------\n");
		dd->printf("Playing Audio:\n");

		int i = 0;
		for (it = m_playingSounds.begin(); it != m_playingSounds.end(); ++it) {
			PlayingAudio *playing = *it;
			if (!playing || !playing->m_audioEventRTS) continue;
			AsciiString fn = playing->m_audioEventRTS->getFilename();
			fn = fn.reverseFind('\\') + 1;
			Real volume = 100.0f * getEffectiveVolume(playing->m_audioEventRTS.Peek());
			dd->printf("%2d: %-20s - (%s) Volume: %d\n", i++,
				playing->m_audioEventRTS->getEventName().str(), fn.str(), REAL_TO_INT(volume));
		}
	}

	if (fp)
	{
		fprintf(fp, "MiniAudio version: %s\n", ma_version_string());
	}
}
#endif

//-------------------------------------------------------------------------------------------------
void MiniAudioManager::init()
{
	ScopedFPUGuard fpuGuard;
	AudioManager::init();
#ifdef INTENSE_DEBUG
	return;
#endif
	openDevice();
}

//-------------------------------------------------------------------------------------------------
void MiniAudioManager::postProcessLoad()
{
	AudioManager::postProcessLoad();
}

//-------------------------------------------------------------------------------------------------
void MiniAudioManager::reset()
{
#if defined(_DEBUG) || defined(_INTERNAL)
	dumpAllAssetsUsed();
	m_allEventsLoaded.clear();
#endif

	AudioManager::reset();
	fprintf(stderr, "[MAUDIO-T %.0fms RESET\n", MAudioNowMs()); fflush(stderr);
	stopAllAudioImmediately();
	// GeneralsX @bugfix coolswood 18/07/2026 Restart sound groups in reset() to fix silent audio after loadGame.
	// stopAllAudioImmediately() releases individual PlayingAudio entries but does not touch the four
	// ma_sound_group nodes. If a prior pauseAudio() had stopped them (e.g. via the ESC menu used to pick
	// "Load"), they remain stopped across loadGame(). New sounds attach to the stopped groups via
	// ma_sound_init_from_data_source() and are then silently reaped by processPlayingList() because
	// ma_sound_is_playing() returns false — the user observes "no sound until ESC toggled", since the
	// pause/unpause cycle finally calls resumeAudio() -> ma_sound_group_start(). Restart the groups here
	// so audio works immediately after a save load. Guarded by the per-type *On flags: if the device
	// never opened (openDevice bailed out), the groups were never ma_sound_group_init'd, so touching
	// them would be unsafe.
	if (m_soundOn || m_sound3DOn || m_speechOn || m_musicOn) {
		resumeAudio(AudioAffect_All);
	}
}

//-------------------------------------------------------------------------------------------------
void MiniAudioManager::update()
{
	ScopedFPUGuard fpuGuard;

	AudioManager::update();
	setDeviceListenerPosition();
	processRequestList();
	processPlayingList();
	processFadingList();
	processStoppedList();
}

//-------------------------------------------------------------------------------------------------
void MiniAudioManager::stopAudio(AudioAffect which)
{
	// GeneralsX @bugfix meerzulee 19/07/2026 FP env guard for audio entry point (see #215)
	ScopedFPUGuard fpuGuard;
	fprintf(stderr, "[MAUDIO-T %.0fms GROUPSTOP which=%d\n", MAudioNowMs(), (int)which); fflush(stderr);
	if (BitIsSet(which, AudioAffect_Sound))
		ma_sound_group_stop(&m_soundGroup);
	if (BitIsSet(which, AudioAffect_Sound3D))
		ma_sound_group_stop(&m_sound3DGroup);
	if (BitIsSet(which, AudioAffect_Speech))
		ma_sound_group_stop(&m_speechGroup);
	if (BitIsSet(which, AudioAffect_Music))
		ma_sound_group_stop(&m_musicGroup);
}

//-------------------------------------------------------------------------------------------------
void MiniAudioManager::pauseAudio(AudioAffect which)
{
	// GeneralsX @bugfix meerzulee 19/07/2026 FP env guard for audio entry point (see #215)
	ScopedFPUGuard fpuGuard;
	fprintf(stderr, "[MAUDIO-T %.0fms GROUPPAUSE which=%d\n", MAudioNowMs(), (int)which); fflush(stderr);
	if (BitIsSet(which, AudioAffect_Sound))
		ma_sound_group_stop(&m_soundGroup);
	if (BitIsSet(which, AudioAffect_Sound3D))
		ma_sound_group_stop(&m_sound3DGroup);
	if (BitIsSet(which, AudioAffect_Speech))
		ma_sound_group_stop(&m_speechGroup);
	if (BitIsSet(which, AudioAffect_Music))
		ma_sound_group_stop(&m_musicGroup);
}

//-------------------------------------------------------------------------------------------------
void MiniAudioManager::resumeAudio(AudioAffect which)
{
	// GeneralsX @bugfix meerzulee 19/07/2026 FP env guard for audio entry point (see #215)
	ScopedFPUGuard fpuGuard;
	fprintf(stderr, "[MAUDIO-T %.0fms GROUPRESUME which=%d\n", MAudioNowMs(), (int)which); fflush(stderr);
	if (BitIsSet(which, AudioAffect_Sound))
		ma_sound_group_start(&m_soundGroup);
	if (BitIsSet(which, AudioAffect_Sound3D))
		ma_sound_group_start(&m_sound3DGroup);
	if (BitIsSet(which, AudioAffect_Speech))
		ma_sound_group_start(&m_speechGroup);
	if (BitIsSet(which, AudioAffect_Music))
		ma_sound_group_start(&m_musicGroup);
}

//-------------------------------------------------------------------------------------------------
void MiniAudioManager::pauseAmbient(Bool shouldPause)
{
}

//-------------------------------------------------------------------------------------------------
void MiniAudioManager::stopAllAmbientsBy(Object *obj)
{
}

//-------------------------------------------------------------------------------------------------
void MiniAudioManager::stopAllAmbientsBy(Drawable *draw)
{
}

//-------------------------------------------------------------------------------------------------
// GeneralsX @bugfix Antigravity 11/10/2026 win64 (no-FFmpeg) sound/music decode.
// The minimal win64 build has no FFmpeg, so playAudioEvent previously decoded every sample through
// miniaudio's native *streaming* file decoder via ma_sound_init_from_file() over the game's BIG VFS.
// That streaming path is exactly what Linux/macOS avoid by pre-decoding with FFmpeg: on MP3 it stalls
// partway through the track (music suddenly goes silent) and leaves a stuck decoder worker whose
// thread ma_engine_uninit() then joins at shutdown, hanging the exit. Here we read the whole file
// into memory through the engine VFS and decode it to PCM with miniaudio's synchronous in-memory
// decoder (ma_decoder_init_memory), then wrap that PCM in an audio buffer backed by a data-source
// sound -- no background decoder thread. This mirrors the FFmpeg branch above and fixes both the
// mid-track music dropout and the freeze-on-exit. Returns TRUE with *outSound/*outBuffer populated.
static Bool decodeFileToPcmSound(ma_engine *engine, const char *path, ma_uint32 flags,
	ma_sound_group *group, ma_sound **outSound, ma_audio_buffer **outBuffer)
{
	*outSound = NULL;
	*outBuffer = NULL;

	File *file = TheFileSystem->openFile(path);
	fprintf(stderr, "[MAUDIO-T %.0fms DECBEG '%s'\n", MAudioNowMs(), path);
	fflush(stderr);
	if (!file) {
		fprintf(stderr, "[MAUDIO-T %.0fms DECOPENFAIL '%s'\n", MAudioNowMs(), path);
		fflush(stderr);
		DEBUG_LOG(("MiniAudio: decodeFileToPcmSound - failed to open '%s'\n", path));
		return FALSE;
	}

	// readEntireAndClose() allocates a buffer for the whole entry, reads it, and closes the File. BIG
	// archive entries are returned as RAMFiles created with deleteOnClose(), so close() also deletes the
	// File object -- the caller must NOT delete it again (matches SDL3Mouse/DataChunk usage). The
	// returned char* buffer is caller-owned and freed with delete[].
	Int fsize = file->size();
	char *raw = (fsize > 0) ? file->readEntireAndClose() : NULL;
	if (!raw) {
		file->close();
		DEBUG_LOG(("MiniAudio: decodeFileToPcmSound - empty/unreadable '%s'\n", path));
		return FALSE;
	}

	// Force s16 output; 0 channels/rate tells the decoder to keep the file's native values.
	ma_decoder decoder;
	ma_decoder_config dcfg = ma_decoder_config_init(ma_format_s16, 0, 0);
	if (ma_decoder_init_memory(raw, (size_t)fsize, &dcfg, &decoder) != MA_SUCCESS) {
		delete[] raw;
		DEBUG_LOG(("MiniAudio: decodeFileToPcmSound - decoder init failed for '%s'\n", path));
		return FALSE;
	}

	ma_uint32 ch   = decoder.outputChannels;
	ma_uint32 rate = decoder.outputSampleRate;

	std::vector<uint8_t> pcmData;
	// Read in fixed-size frame chunks. The buffer MUST be sized framesPerRead * channels samples,
	// because ma_decoder_read_pcm_frames() counts its request in FRAMES (1 frame = channels s16
	// samples). A stack buffer of only `framesPerRead` int16_t would overflow 2x for stereo (the
	// common case for WAV/MP3) and trip the /GS stack cookie -> STATUS_STACK_BUFFER_OVERRUN (0xc0000409).
	const ma_uint32 framesPerRead = 4096;
	std::vector<int16_t> frameBuf((size_t)framesPerRead * (ch ? ch : 2));
	ma_result lastRr = MA_SUCCESS;
	ma_uint64 lastRead = 0;
	for (;;) {
		ma_uint64 framesRead = 0;
		ma_result rr = ma_decoder_read_pcm_frames(&decoder, frameBuf.data(), framesPerRead, &framesRead);
		if (framesRead > 0) {
			const uint8_t *bytes = (const uint8_t *)frameBuf.data();
			size_t n = (size_t)framesRead * ch * sizeof(int16_t);
			pcmData.insert(pcmData.end(), bytes, bytes + n);
		}
		lastRr = rr; lastRead = framesRead;
		if (rr != MA_SUCCESS || framesRead == 0) break;
	}
	ma_decoder_uninit(&decoder);
	delete[] raw;

	if (pcmData.empty() || ch == 0 || rate == 0) {
		fprintf(stderr, "[MAUDIO-DEC] FAIL '%s' fsize=%d ch=%u rate=%u bytes=%zu lastRr=%d lastRead=%llu\n",
			path, fsize, ch, rate, pcmData.size(), (int)lastRr, (unsigned long long)lastRead);
		fflush(stderr);
		DEBUG_LOG(("MiniAudio: decodeFileToPcmSound - no PCM decoded for '%s'\n", path));
		return FALSE;
	}

	ma_audio_buffer *audioBuffer = (ma_audio_buffer *)malloc(sizeof(ma_audio_buffer));
	ma_audio_buffer_config abConfig = ma_audio_buffer_config_init(ma_format_s16, ch,
		pcmData.size() / ma_get_bytes_per_frame(ma_format_s16, ch),
		pcmData.data(), NULL);
	if (ma_audio_buffer_init_copy(&abConfig, audioBuffer) != MA_SUCCESS) {
		free(audioBuffer);
		DEBUG_LOG(("MiniAudio: decodeFileToPcmSound - buffer init failed for '%s'\n", path));
		return FALSE;
	}
	audioBuffer->ref.sampleRate = rate;

	ma_sound *sound = (ma_sound *)malloc(sizeof(ma_sound));
	if (ma_sound_init_from_data_source(engine, audioBuffer, flags, group, sound) != MA_SUCCESS) {
		ma_audio_buffer_uninit(audioBuffer);
		free(audioBuffer);
		free(sound);
		DEBUG_LOG(("MiniAudio: decodeFileToPcmSound - sound init failed for '%s'\n", path));
		return FALSE;
	}

	// GeneralsX diag: fires when miniaudio itself considers the sound finished (data source
	// exhausted or engine-stopped). Lets us distinguish "miniaudio ended it" from "we released it".
	ma_sound_set_end_callback(sound, [](void *, ma_sound *) {
		fprintf(stderr, "[MAUDIO-T %.0fms ENDCALLBACK\n", MAudioNowMs());
		fflush(stderr);
	}, NULL);

	*outSound = sound;
	*outBuffer = audioBuffer;
	{
		ma_uint64 frames = pcmData.size() / ma_get_bytes_per_frame(ma_format_s16, ch);
		// Diagnostics: compare PCM energy at several offsets. If early frames are loud but later
		// windows are ~0, miniaudio's built-in MP3 decoder emitted silence past a point even though
		// it produced a full-length frame count (the cursor advances, but there is nothing to hear).
		const int16_t *sm = (const int16_t *)pcmData.data();
		size_t totalSamples = pcmData.size() / sizeof(int16_t);
		auto meanAbs = [&](ma_uint64 frameStart, ma_uint64 frameCount)->double {
			size_t a = (size_t)frameStart * ch;
			size_t b = a + (size_t)frameCount * ch;
			if (b > totalSamples) b = totalSamples;
			if (a >= b) return -1.0;
			double acc = 0.0; size_t n = 0;
			for (size_t i = a; i < b; i++) { acc += (sm[i] < 0 ? -sm[i] : sm[i]); n++; }
			return n ? acc / (double)n : 0.0;
		};
		double e0 = meanAbs(0, rate * 8);
		double eMid = (frames > rate * 68) ? meanAbs(rate * 60, rate * 8) : -2.0;
		double eLate = (frames > rate * 158) ? meanAbs(rate * 150, rate * 8) : -2.0;
		fprintf(stderr, "[MAUDIO-DEC] OK '%s' fsize=%d ch=%u rate=%u frames=%llu dur=%.2fs energy[0-8s]=%.1f [60-68s]=%.1f [150-158s]=%.1f\n",
			path, fsize, ch, rate, (unsigned long long)frames, rate ? (float)frames / (float)rate : 0.0f, e0, eMid, eLate);
		fflush(stderr);
	}
	return TRUE;
}

//-------------------------------------------------------------------------------------------------
void MiniAudioManager::playAudioEvent(AudioRequest *req)
{
	AudioEventRTS *event = req->m_pendingEvent.Peek();
	if (!event) {
		return;
	}

	const AudioEventInfo *info = event->getAudioEventInfo();
	if (!info) {
		return;
	}

	AsciiString fileToPlay = event->getFilename();
	DEBUG_LOG(("MINIAUDIO: playAudioEvent '%s' type=%d file='%s'\n",
		event->getEventName().str(), info->m_soundType, fileToPlay.str()));

	std::list<PlayingAudio *>::iterator it;

	AudioHandle handleToKill = event->getHandleToKill();
	PlayingAudio *audio = allocatePlayingAudio();
	audio->m_audioEventRTS = req->m_pendingEvent;

	// Kill existing sound if handleToKill is set
	if (handleToKill) {
		for (it = m_playingSounds.begin(); it != m_playingSounds.end(); ++it) {
			PlayingAudio *playing = (*it);
			if (!playing) continue;
			if (playing->m_audioEventRTS && playing->m_audioEventRTS->getPlayingHandle() == handleToKill)
			{
				fprintf(stderr, "[MAUDIO-T %.0fms KILLHANDLE victim='%s' (new='%s')\n", MAudioNowMs(),
					playing->m_audioEventRTS->getEventName().str(), event->getEventName().str());
				fflush(stderr);
				releasePlayingAudio(playing);
				m_playingSounds.erase(it);
				break;
			}
		}
	}

	ma_sound_group *groupToUse = NULL;
	ma_uint32 flags = MA_SOUND_FLAG_NO_SPATIALIZATION | MA_SOUND_FLAG_NO_PITCH;
	switch (info->m_soundType)
	{
	case AT_Music:
		groupToUse = &m_musicGroup;
		break;
	case AT_Streaming:
		groupToUse = &m_speechGroup;
		break;
	case AT_SoundEffect:
		if (event->isPositionalAudio()) {
			groupToUse = &m_sound3DGroup;
			flags = 0; // Allow spatialization for 3D
		}
		else {
			groupToUse = &m_soundGroup;
		}
		break;
	}

	// Decode-and-play. When FFmpeg is available (Linux/macOS presets) we decode the file into PCM
	// ourselves and feed miniaudio, because miniaudio's built-in decoders were observed to hang on
	// MP3 through the BIG VFS there. Builds without FFmpeg (the minimal win64 build) take the #else
	// branch below and let miniaudio decode the game's WAV samples/speech and MP3 music natively.
#ifdef SAGE_HAVE_FFMPEG
	// Use FFmpeg to decode the file into PCM, then feed to miniaudio.
	// This avoids miniaudio's built-in decoders which hang on MP3 via VFS.
	File *file = TheFileSystem->openFile(fileToPlay.str());
	if (!file) {
		DEBUG_LOG(("MiniAudio: Failed to open file: %s\n", fileToPlay.str()));
		releasePlayingAudio(audio);
		return;
	}

	FFmpegFile *ffmpegFile = NEW FFmpegFile();
	if (!ffmpegFile->open(file)) {
		DEBUG_LOG(("MiniAudio: Failed to open FFmpeg file: %s\n", fileToPlay.str()));
		delete ffmpegFile;
		releasePlayingAudio(audio);
		return;
	}

	// Decode all audio frames into a buffer
	std::vector<uint8_t> pcmData;
	int sampleRate = 0;
	int channels = 0;
	int bytesPerSample = 0;

	auto onFrame = [&](AVFrame *frame, int stream_idx, int stream_type, void *user_data) {
		if (stream_type != AVMEDIA_TYPE_AUDIO) return;

		sampleRate = frame->sample_rate;
		channels = frame->ch_layout.nb_channels;
		bytesPerSample = av_get_bytes_per_sample(static_cast<AVSampleFormat>(frame->format));

		const int frameSize = av_samples_get_buffer_size(NULL, channels, frame->nb_samples,
			static_cast<AVSampleFormat>(frame->format), 1);
		if (frameSize <= 0) return;

		if (av_sample_fmt_is_planar(static_cast<AVSampleFormat>(frame->format))) {
			// Interleave planar audio
			for (int s = 0; s < frame->nb_samples; s++) {
				for (int c = 0; c < channels; c++) {
					const uint8_t *src = frame->data[c] + s * bytesPerSample;
					pcmData.insert(pcmData.end(), src, src + bytesPerSample);
				}
			}
		} else {
			pcmData.insert(pcmData.end(), frame->data[0], frame->data[0] + frameSize);
		}
	};

	ffmpegFile->setFrameCallback(onFrame);
	ffmpegFile->setUserData(NULL);

	while (ffmpegFile->decodePacket()) {}

	// Always delete ffmpegFile after decoding is complete
	delete ffmpegFile;
	ffmpegFile = NULL;

	if (pcmData.empty() || sampleRate == 0 || bytesPerSample == 0) {
		DEBUG_LOG(("MiniAudio: No audio data decoded from: %s\n", fileToPlay.str()));
		releasePlayingAudio(audio);
		return;
	}

	// Map FFmpeg bytes-per-sample to miniaudio format
	ma_format maFmt = ma_format_unknown;
	if (bytesPerSample == 1) maFmt = ma_format_u8;
	else if (bytesPerSample == 2) maFmt = ma_format_s16;
	else if (bytesPerSample == 4) maFmt = ma_format_f32;

	if (maFmt == ma_format_unknown) {
		DEBUG_LOG(("MiniAudio: Unsupported format (%d bps) for: %s\n", bytesPerSample, fileToPlay.str()));
		releasePlayingAudio(audio);
		return;
	}

	// Create audio buffer — init_copy copies data into internal storage
	// so it's safe when pcmData goes out of scope after this function returns.
	ma_audio_buffer *audioBuffer = (ma_audio_buffer *)malloc(sizeof(ma_audio_buffer));
	ma_audio_buffer_config abConfig = ma_audio_buffer_config_init(maFmt, channels,
		pcmData.size() / ma_get_bytes_per_frame(maFmt, channels),
		pcmData.data(), NULL);

	ma_result result = ma_audio_buffer_init_copy(&abConfig, audioBuffer);
	if (result != MA_SUCCESS) {
		DEBUG_LOG(("MiniAudio: Failed to create audio buffer: %d for '%s'\n", result, fileToPlay.str()));
		free(audioBuffer);
		releasePlayingAudio(audio);
		return;
	}
	audioBuffer->ref.sampleRate = sampleRate;

	ma_sound *sound = (ma_sound *)malloc(sizeof(ma_sound));
	result = ma_sound_init_from_data_source(&m_engine, audioBuffer, flags, groupToUse, sound);
	if (result != MA_SUCCESS) {
		DEBUG_LOG(("MiniAudio: Failed to init sound: %d for '%s'\n", result, fileToPlay.str()));
		ma_audio_buffer_uninit(audioBuffer);
		free(audioBuffer);
		free(sound);
		releasePlayingAudio(audio);
		return;
	}
#else
	// No FFmpeg: read the whole file through the engine VFS and decode it to PCM with miniaudio's
	// synchronous in-memory decoder (see decodeFileToPcmSound). We deliberately avoid the native
	// streaming file decoder here because it stalls on MP3 over the BIG VFS (music cuts out) and
	// leaves a stuck decoder worker that hangs ma_engine_uninit() at shutdown (freeze on exit).
	// Handles the game's WAV samples/speech and MP3 music with miniaudio's built-in decoders and no
	// external dependency. m_audioBuffer is owned by the PlayingAudio and released via releasePlayingAudio.
	ma_audio_buffer *audioBuffer = NULL;
	ma_sound *sound = NULL;
	if (!decodeFileToPcmSound(&m_engine, fileToPlay.str(), flags, groupToUse, &sound, &audioBuffer)) {
		DEBUG_LOG(("MiniAudio: Failed to decode sound: '%s'\n", fileToPlay.str()));
		releasePlayingAudio(audio);
		return;
	}
	ma_result result = MA_SUCCESS;
#endif

	// Assign to PlayingAudio before any early returns below
	audio->m_sound = sound;
	audio->m_audioBuffer = audioBuffer;

	switch (info->m_soundType)
	{
	case AT_Music:
	case AT_Streaming:
	{
		if ((info->m_soundType == AT_Streaming) && event->getUninterruptible()) {
			stopAllSpeech();
		}
		audio->m_type = PAT_Stream;
		if ((info->m_soundType == AT_Streaming) && event->getUninterruptible()) {
			setDisallowSpeech(TRUE);
		}
		break;
	}

	case AT_SoundEffect:
	{
		if (event->isPositionalAudio()) {
			audio->m_type = PAT_3DSample;
			if (event->getAudioEventInfo()->m_type & ST_GLOBAL) {
				ma_sound_set_min_distance(sound, TheAudio->getAudioSettings()->m_globalMinRange);
				ma_sound_set_max_distance(sound, TheAudio->getAudioSettings()->m_globalMaxRange);
			}
			else {
				ma_sound_set_min_distance(sound, event->getAudioEventInfo()->m_minDistance);
				ma_sound_set_max_distance(sound, event->getAudioEventInfo()->m_maxDistance);
			}
			const Coord3D *pos = event->getCurrentPosition();
			if (pos) {
				ma_sound_set_position(sound, pos->x, pos->y, pos->z);
			}

		}
		else {
			audio->m_type = PAT_Sample;

		}
		break;
	}
	}

	// GeneralsX @bugfix Mr. Meeseeks 19/06/2026 - Initialize volume using adjustPlayingVolume
	adjustPlayingVolume(audio);

	result = ma_sound_start(sound);
	if (result != MA_SUCCESS) {
		DEBUG_LOG(("MiniAudio: Failed to start sound: %d for '%s'\n", result, fileToPlay.str()));
		releasePlayingAudio(audio);
		return;
	}

	fprintf(stderr, "[MAUDIO-T %.0fms STARTED '%s' type=%d file='%s' handle=%d\n", MAudioNowMs(),
		event->getEventName().str(), (int)info->m_soundType, fileToPlay.str(),
		(int)event->getPlayingHandle());
	fflush(stderr);

	m_playingSounds.push_back(audio);
}

//-------------------------------------------------------------------------------------------------
void MiniAudioManager::stopAudioEvent(AudioHandle handle)
{
	// GeneralsX @bugfix meerzulee 19/07/2026 FP env guard for audio entry point (see #215)
	ScopedFPUGuard fpuGuard;
	std::list<PlayingAudio *>::iterator it;

	// Handle special music stop commands
	if (handle == AHSV_StopTheMusic || handle == AHSV_StopTheMusicFade) {
		for (it = m_playingSounds.begin(); it != m_playingSounds.end(); ++it) {
			PlayingAudio *audio = (*it);
			if (!audio || !audio->m_audioEventRTS) continue;

			const AudioEventInfo *stopInfo = audio->m_audioEventRTS->getAudioEventInfo();
			if (stopInfo && stopInfo->m_soundType == AT_Music)
			{
				fprintf(stderr, "[MAUDIO-T %.0fms STOPMUSIC stopAudioEvent(%s) releasing '%s'\n", MAudioNowMs(),
					handle == AHSV_StopTheMusicFade ? "AHSV_StopTheMusicFade" : "AHSV_StopTheMusic",
					audio->m_audioEventRTS->getEventName().str());
				fflush(stderr);
				if (handle == AHSV_StopTheMusicFade)
					m_fadingAudio.push_back(audio);
				else
					releasePlayingAudio(audio);
				m_playingSounds.erase(it);
				break;
			}
		}
	}

	// Find and mark the audio event for stopping
	for (it = m_playingSounds.begin(); it != m_playingSounds.end(); ++it) {
		PlayingAudio *audio = (*it);
		if (!audio || !audio->m_audioEventRTS) continue;
		if (audio->m_audioEventRTS->getPlayingHandle() == handle) {
			audio->m_requestStop = true;
			break;
		}
	}
}

//-------------------------------------------------------------------------------------------------
void MiniAudioManager::killAudioEventImmediately(AudioHandle audioEvent)
{
	// GeneralsX @bugfix meerzulee 19/07/2026 FP env guard for audio entry point (see #215)
	ScopedFPUGuard fpuGuard;
	// First look for it in the request list.
	std::list<AudioRequest *>::iterator ait;
	for (ait = m_audioRequests.begin(); ait != m_audioRequests.end(); ait++)
	{
		AudioRequest *req = (*ait);
		if (req && req->m_request == AR_Play && req->m_handleToInteractOn == audioEvent)
		{
			deleteInstance(req);
			ait = m_audioRequests.erase(ait);
			return;
		}
	}

	std::list<PlayingAudio *>::iterator it;
	for (it = m_playingSounds.begin(); it != m_playingSounds.end(); it++)
	{
		PlayingAudio *audio = (*it);
		if (!audio) continue;
		if (audio->m_audioEventRTS && audio->m_audioEventRTS->getPlayingHandle() == audioEvent)
		{
			fprintf(stderr, "[MAUDIO-T %.0fms KILLIMM handle=%d victim='%s'\n", MAudioNowMs(),
				(int)audioEvent, audio->m_audioEventRTS->getEventName().str()); fflush(stderr);
			releasePlayingAudio(audio);
			m_playingSounds.erase(it);
			return;
		}
	}
}

//-------------------------------------------------------------------------------------------------
void MiniAudioManager::pauseAudioEvent(AudioHandle handle)
{
	// GeneralsX @bugfix meerzulee 19/07/2026 FP env guard for audio entry point (see #215)
	ScopedFPUGuard fpuGuard;
	// TODO: implement individual sound pause
}

//-------------------------------------------------------------------------------------------------
PlayingAudio *MiniAudioManager::allocatePlayingAudio(void)
{
	PlayingAudio *aud = NEW PlayingAudio;
	return aud;
}

//-------------------------------------------------------------------------------------------------
void MiniAudioManager::releaseMiniAudioHandles(PlayingAudio *release)
{
	// Uninit sound BEFORE audio buffer — sound holds reference to buffer
	if (release->m_sound) {
		ma_sound_uninit(release->m_sound);
		free(release->m_sound);
		release->m_sound = NULL;
	}
	if (release->m_audioBuffer) {
		ma_audio_buffer_uninit(release->m_audioBuffer);
		free(release->m_audioBuffer);
		release->m_audioBuffer = NULL;
	}
	release->m_type = PAT_INVALID;
}

//-------------------------------------------------------------------------------------------------
void MiniAudioManager::releasePlayingAudio(PlayingAudio *release)
{
	if (!release) return;

	// GeneralsX diag: single choke point for EVERY removal path. cursor tells us how far the
	// sound had played when it was released; playing tells us whether miniaudio still considered
	// it live at that moment.
	{
		const char *nm = release->m_audioEventRTS ? release->m_audioEventRTS->getEventName().str() : "(null)";
		float cur = -1.0f;
		int pl = -1;
		if (release->m_sound) {
			ma_sound_get_cursor_in_seconds(release->m_sound, &cur);
			pl = (int)ma_sound_is_playing(release->m_sound);
		}
		fprintf(stderr, "[MAUDIO-T %.0fms RELEASE '%s' type=%d cursor=%.2fs playing=%d\n",
			MAudioNowMs(), nm, (int)release->m_type, cur, pl);
		fflush(stderr);
	}

	const AudioEventInfo *releaseInfo = release->m_audioEventRTS
		? release->m_audioEventRTS->getAudioEventInfo() : nullptr;

	if (releaseInfo && releaseInfo->m_soundType == AT_SoundEffect && release->m_sound) {
		if (release->m_type == PAT_Sample) {

		}
		else if (release->m_type == PAT_3DSample) {

		}
		// PAT_Stream and PAT_INVALID don't notify
	}

	releaseMiniAudioHandles(release);
	delete release;
}

//-------------------------------------------------------------------------------------------------
void MiniAudioManager::stopAllAudioImmediately(void)
{
	// GeneralsX @bugfix meerzulee 19/07/2026 FP env guard for audio entry point (see #215)
	ScopedFPUGuard fpuGuard;
	std::list<PlayingAudio *>::iterator it;
	PlayingAudio *playing;
	fprintf(stderr, "[MAUDIO-T %.0fms STOPALL playing=%zu fading=%zu stopped=%zu\n", MAudioNowMs(),
		m_playingSounds.size(), m_fadingAudio.size(), m_stoppedAudio.size()); fflush(stderr);

	for (it = m_playingSounds.begin(); it != m_playingSounds.end(); ) {
		playing = *it;
		it = m_playingSounds.erase(it); // advance before release
		if (playing) releasePlayingAudio(playing);
	}

	for (it = m_fadingAudio.begin(); it != m_fadingAudio.end(); ) {
		playing = *it;
		it = m_fadingAudio.erase(it);
		if (playing) releasePlayingAudio(playing);
	}

	for (it = m_stoppedAudio.begin(); it != m_stoppedAudio.end(); ) {
		playing = *it;
		it = m_stoppedAudio.erase(it);
		if (playing) releasePlayingAudio(playing);
	}
}

//-------------------------------------------------------------------------------------------------
void MiniAudioManager::freeAllMiniAudioHandles(void)
{
	// GeneralsX @bugfix meerzulee 19/07/2026 FP env guard for audio entry point (see #215)
	ScopedFPUGuard fpuGuard;
	ma_engine_stop(&m_engine);
}

//-------------------------------------------------------------------------------------------------
void MiniAudioManager::adjustPlayingVolume(PlayingAudio *audio)
{
	if (!audio || !audio->m_audioEventRTS || !audio->m_sound) return;

	Real desiredVolume = audio->m_audioEventRTS->getVolume() * audio->m_audioEventRTS->getVolumeShift();
	if (audio->m_type == PAT_Sample) {
		ma_sound_set_volume(audio->m_sound, m_soundVolume * desiredVolume);
	}
	else if (audio->m_type == PAT_3DSample) {
		ma_sound_set_volume(audio->m_sound, m_sound3DVolume * desiredVolume);
	}
	else if (audio->m_type == PAT_Stream) {
		const AudioEventInfo *info = audio->m_audioEventRTS->getAudioEventInfo();
		if (info && info->m_soundType == AT_Music)
			ma_sound_set_volume(audio->m_sound, m_musicVolume * desiredVolume);
		else
			ma_sound_set_volume(audio->m_sound, m_speechVolume * desiredVolume);
	}
}

//-------------------------------------------------------------------------------------------------
void MiniAudioManager::stopAllSpeech(void)
{
	// GeneralsX @bugfix meerzulee 19/07/2026 FP env guard for audio entry point (see #215)
	ScopedFPUGuard fpuGuard;
	std::list<PlayingAudio *>::iterator it;
	for (it = m_playingSounds.begin(); it != m_playingSounds.end(); ) {
		PlayingAudio *playing = (*it);
		if (!playing) { ++it; continue; }

		if (playing->m_audioEventRTS && playing->m_audioEventRTS->getAudioEventInfo() &&
			playing->m_audioEventRTS->getAudioEventInfo()->m_soundType == AT_Streaming) {
			fprintf(stderr, "[MAUDIO-T %.0fms STOPALLSPEECH victim='%s'\n", MAudioNowMs(),
				playing->m_audioEventRTS->getEventName().str()); fflush(stderr);
			it = m_playingSounds.erase(it);
			releasePlayingAudio(playing);
		}
		else {
			++it;
		}
	}
}

//-------------------------------------------------------------------------------------------------
AsciiString MiniAudioManager::nextMusicTrack(void)
{
	AsciiString trackName;
	for (auto it = m_playingSounds.begin(); it != m_playingSounds.end(); ++it) {
		PlayingAudio *playing = *it;
		if (playing && playing->m_audioEventRTS && playing->m_audioEventRTS->getAudioEventInfo() &&
			playing->m_audioEventRTS->getAudioEventInfo()->m_soundType == AT_Music) {
			trackName = playing->m_audioEventRTS->getEventName();
		}
	}
	TheAudio->removeAudioEvent(AHSV_StopTheMusic);
	trackName = nextTrackName(trackName);
	AudioEventRTS newTrack(trackName);
	TheAudio->addAudioEvent(&newTrack);
	return trackName;
}

//-------------------------------------------------------------------------------------------------
AsciiString MiniAudioManager::prevMusicTrack(void)
{
	AsciiString trackName;
	for (auto it = m_playingSounds.begin(); it != m_playingSounds.end(); ++it) {
		PlayingAudio *playing = *it;
		if (playing && playing->m_audioEventRTS && playing->m_audioEventRTS->getAudioEventInfo() &&
			playing->m_audioEventRTS->getAudioEventInfo()->m_soundType == AT_Music) {
			trackName = playing->m_audioEventRTS->getEventName();
		}
	}
	TheAudio->removeAudioEvent(AHSV_StopTheMusic);
	trackName = prevTrackName(trackName);
	AudioEventRTS newTrack(trackName);
	TheAudio->addAudioEvent(&newTrack);
	return trackName;
}

//-------------------------------------------------------------------------------------------------
Bool MiniAudioManager::isMusicPlaying(void) const
{
	return ma_sound_group_is_playing(&m_musicGroup) == MA_TRUE;
}

//-------------------------------------------------------------------------------------------------
Bool MiniAudioManager::hasMusicTrackCompleted(const AsciiString &trackName, Int numberOfTimes) const
{
	return FALSE;
}

//-------------------------------------------------------------------------------------------------
AsciiString MiniAudioManager::getMusicTrackName(void) const
{
	for (auto ait = m_audioRequests.begin(); ait != m_audioRequests.end(); ++ait) {
		if ((*ait)->m_request != AR_Play || !(*ait)->m_pendingEvent) continue;
		if ((*ait)->m_pendingEvent->getAudioEventInfo()->m_soundType == AT_Music)
			return (*ait)->m_pendingEvent->getEventName();
	}
	for (auto it = m_playingSounds.begin(); it != m_playingSounds.end(); ++it) {
		PlayingAudio *playing = *it;
		if (playing && playing->m_audioEventRTS && playing->m_audioEventRTS->getAudioEventInfo() &&
			playing->m_audioEventRTS->getAudioEventInfo()->m_soundType == AT_Music) {
			return playing->m_audioEventRTS->getEventName();
		}
	}
	return AsciiString::TheEmptyString;
}

//-------------------------------------------------------------------------------------------------
void MiniAudioManager::openDevice(void)
{
	if (!TheGlobalData->m_audioOn) {
		return;
	}

	ma_result result;
	ma_resource_manager_config resourceManagerConfig;
	ma_context_config contextConfig;
	ma_engine_config engineConfig;
	static ma_vfs_callbacks vfs = {};
	vfs.onOpen = vfsFileOpen;
	vfs.onClose = vfsFileClose;
	vfs.onInfo = vfsFileInfo;
	vfs.onRead = vfsFileRead;
	vfs.onTell = vfsFileTell;
	vfs.onSeek = vfsFileSeek;

	resourceManagerConfig = ma_resource_manager_config_init();
	resourceManagerConfig.pVFS = (ma_vfs *)&vfs;
	result = ma_resource_manager_init(&resourceManagerConfig, &m_resourceManager);
	if (result != MA_SUCCESS) {
		DEBUG_LOG(("MiniAudio: Failed to initialize resource manager: %d\n", result));
		setOn(false, AudioAffect_All);
		return;
	}

	result = ma_log_init(NULL, &m_log);
	if (result != MA_SUCCESS) {
		DEBUG_LOG(("MiniAudio: Failed to initialize log: %d\n", result));
		setOn(false, AudioAffect_All);
		return;
	}

	auto on_log = [](void *pUserData, ma_uint32 logLevel, const char *message) {
		DEBUG_LOG(("miniaudio [%s]: %s", ma_log_level_to_string(logLevel), message));
	};
	ma_log_register_callback(&m_log, ma_log_callback_init(on_log, NULL));

	contextConfig = ma_context_config_init();
	contextConfig.pLog = &m_log;

	result = ma_context_init(NULL, 0, &contextConfig, &m_context);
	if (result != MA_SUCCESS) {
		DEBUG_LOG(("MiniAudio: Failed to initialize context: %d\n", result));
		setOn(false, AudioAffect_All);
		return;
	}

	result = ma_context_get_devices(&m_context, &m_playbackDevices, &m_playbackDeviceCount, NULL, NULL);
	if (result != MA_SUCCESS) {
		DEBUG_LOG(("MiniAudio: Failed to enumerate devices: %d\n", result));
		ma_context_uninit(&m_context);
		setOn(false, AudioAffect_All);
		return;
	}

	engineConfig = ma_engine_config_init();
	engineConfig.pResourceManager = &m_resourceManager;

	result = ma_engine_init(&engineConfig, &m_engine);
	if (result != MA_SUCCESS) {
		DEBUG_LOG(("MiniAudio: Failed to initialize engine: %d\n", result));
		setOn(false, AudioAffect_All);
		return;
	}

	ma_sound_group_init(&m_engine, 0, NULL, &m_musicGroup);
	ma_sound_group_init(&m_engine, 0, NULL, &m_soundGroup);
	ma_sound_group_init(&m_engine, 0, NULL, &m_sound3DGroup);
	ma_sound_group_init(&m_engine, 0, NULL, &m_speechGroup);

	fprintf(stderr, "AUDIO: MiniAudio backend loaded - version %s, device: %s, playback devices: %d\n",
		ma_version_string(),
		m_playbackDeviceCount > 0 ? m_playbackDevices[0].name : "default",
		m_playbackDeviceCount);

	TheAudio->refreshCachedVariables();
}

//-------------------------------------------------------------------------------------------------
void MiniAudioManager::closeDevice(void)
{
	// GeneralsX @bugfix meerzulee 19/07/2026 FP env guard for audio entry point (see #215)
	ScopedFPUGuard fpuGuard;
	fprintf(stderr, "[MAUDIO-CLOSE] enter closeDevice\n"); fflush(stderr);
	// Stop all audio first to prevent use-after-free in audio callbacks
	stopAllAudioImmediately();
	fprintf(stderr, "[MAUDIO-CLOSE] stopAll done, uninit groups\n"); fflush(stderr);

	ma_sound_group_uninit(&m_speechGroup);
	ma_sound_group_uninit(&m_sound3DGroup);
	ma_sound_group_uninit(&m_soundGroup);
	ma_sound_group_uninit(&m_musicGroup);
	fprintf(stderr, "[MAUDIO-CLOSE] pre ma_engine_uninit\n"); fflush(stderr);
	ma_engine_uninit(&m_engine);
	fprintf(stderr, "[MAUDIO-CLOSE] post ma_engine_uninit\n"); fflush(stderr);
	ma_resource_manager_uninit(&m_resourceManager);
	ma_context_uninit(&m_context);
	ma_log_uninit(&m_log);
	fprintf(stderr, "[MAUDIO-CLOSE] leave closeDevice\n"); fflush(stderr);
}

//-------------------------------------------------------------------------------------------------
Bool MiniAudioManager::isCurrentlyPlaying(AudioHandle handle)
{
	for (auto it = m_playingSounds.begin(); it != m_playingSounds.end(); ++it) {
		PlayingAudio *playing = *it;
		if (playing && playing->m_audioEventRTS && playing->m_audioEventRTS->getPlayingHandle() == handle)
			return true;
	}
	for (auto ait = m_audioRequests.begin(); ait != m_audioRequests.end(); ++ait) {
		AudioRequest *req = *ait;
		if (req && req->m_pendingEvent && req->m_pendingEvent->getPlayingHandle() == handle)
			return true;
	}
	return false;
}

//-------------------------------------------------------------------------------------------------
void MiniAudioManager::notifyOfAudioCompletion(UnsignedInt audioCompleted, UnsignedInt flags)
{
	// GeneralsX @bugfix meerzulee 19/07/2026 FP env guard for audio entry point (see #215)
	ScopedFPUGuard fpuGuard;
	PlayingAudio *playing = findPlayingAudioFrom(audioCompleted, flags);
	if (!playing) return;
	if (!playing->m_audioEventRTS || !playing->m_audioEventRTS->getAudioEventInfo()) return;

	if (getDisallowSpeech() && playing->m_audioEventRTS->getAudioEventInfo()->m_soundType == AT_Streaming)
		setDisallowSpeech(FALSE);

	if (playing->m_audioEventRTS->getAudioEventInfo()->m_control & AC_LOOP) {
		if (playing->m_audioEventRTS->getNextPlayPortion() == PP_Attack)
			playing->m_audioEventRTS->setNextPlayPortion(PP_Sound);
		if (playing->m_audioEventRTS->getNextPlayPortion() == PP_Sound) {
			playing->m_audioEventRTS->decreaseLoopCount();
			if (startNextLoop(playing)) return;
		}
	}
}

//-------------------------------------------------------------------------------------------------
PlayingAudio *MiniAudioManager::findPlayingAudioFrom(UnsignedInt audioCompleted, UnsignedInt flags)
{
	ma_sound *sound = (ma_sound *)audioCompleted;
	for (auto it = m_playingSounds.begin(); it != m_playingSounds.end(); ++it) {
		PlayingAudio *playing = *it;
		if (playing && playing->m_sound == sound)
			return playing;
	}
	return NULL;
}

//-------------------------------------------------------------------------------------------------
UnsignedInt MiniAudioManager::getProviderCount(void) const
{
	return m_playbackDeviceCount;
}

//-------------------------------------------------------------------------------------------------
AsciiString MiniAudioManager::getProviderName(UnsignedInt providerNum) const
{
	if (isOn(AudioAffect_Sound3D) && providerNum < m_playbackDeviceCount)
		return m_playbackDevices[providerNum].name;
	return AsciiString::TheEmptyString;
}

//-------------------------------------------------------------------------------------------------
UnsignedInt MiniAudioManager::getProviderIndex(AsciiString providerName) const
{
	for (UnsignedInt i = 0; i < m_playbackDeviceCount; ++i) {
		if (providerName == m_playbackDevices[i].name)
			return i;
	}
	return PROVIDER_ERROR;
}

//-------------------------------------------------------------------------------------------------
void MiniAudioManager::selectProvider(UnsignedInt providerNdx) {}
void MiniAudioManager::unselectProvider(void) {}

//-------------------------------------------------------------------------------------------------
UnsignedInt MiniAudioManager::getSelectedProvider(void) const
{
	return m_selectedPlaybackDevice;
}

//-------------------------------------------------------------------------------------------------
void MiniAudioManager::setSpeakerType(UnsignedInt speakerType) {}

//-------------------------------------------------------------------------------------------------
UnsignedInt MiniAudioManager::getSpeakerType(void)
{
	return 0;
}

//-------------------------------------------------------------------------------------------------
void *MiniAudioManager::getHandleForBink(void)
{
	if (!m_binkHandle) {
		m_binkHandle = NEW MiniAudioStream;
		// Give the stream access to the engine
		static_cast<MiniAudioStream *>(m_binkHandle)->setEngine(&m_engine);
	}
	return m_binkHandle;
}

//-------------------------------------------------------------------------------------------------
void MiniAudioManager::releaseHandleForBink(void)
{
	if (m_binkHandle) {
		delete (MiniAudioStream *)m_binkHandle;
		m_binkHandle = NULL;
	}
}

//-------------------------------------------------------------------------------------------------
UnsignedInt MiniAudioManager::getNum2DSamples(void) const { return 0; }
UnsignedInt MiniAudioManager::getNum3DSamples(void) const { return 0; }
UnsignedInt MiniAudioManager::getNumStreams(void) const { return 0; }

//-------------------------------------------------------------------------------------------------
Bool MiniAudioManager::doesViolateLimit(AudioEventRTS *event) const
{
	Int limit = event->getAudioEventInfo()->m_limit;
	if (limit == 0) return false;

	Int totalCount = 0;
	Int totalRequestCount = 0;

	for (auto it = m_playingSounds.begin(); it != m_playingSounds.end(); ++it) {
		if (!(*it)->m_audioEventRTS) continue;
		if ((*it)->m_audioEventRTS->getEventName() == event->getEventName()) {
			if (totalCount == 0)
				event->setHandleToKill((*it)->m_audioEventRTS->getPlayingHandle());
			++totalCount;
		}
	}
	for (auto arIt = m_audioRequests.begin(); arIt != m_audioRequests.end(); ++arIt) {
		AudioRequest *req = (*arIt);
		if (req && req->m_pendingEvent &&
			req->m_pendingEvent->getEventName() == event->getEventName()) {
			totalRequestCount++;
			totalCount++;
		}
	}

	if (event->getAudioEventInfo()->m_control & AC_INTERRUPT) {
		if (totalRequestCount < limit) {
			Int totalPlayingCount = totalCount - totalRequestCount;
			if (totalRequestCount + totalPlayingCount < limit) {
				event->setHandleToKill(0);
				return false;
			}
			return false;
		}
	}

	if (totalCount < limit) {
		event->setHandleToKill(0);
		return false;
	}
	return true;
}

//-------------------------------------------------------------------------------------------------
Bool MiniAudioManager::isPlayingAlready(AudioEventRTS *event) const
{
	for (auto it = m_playingSounds.begin(); it != m_playingSounds.end(); ++it) {
		if (!(*it)->m_audioEventRTS) continue;
		if ((*it)->m_audioEventRTS->getEventName() == event->getEventName())
			return true;
	}
	return false;
}

//-------------------------------------------------------------------------------------------------
Bool MiniAudioManager::isObjectPlayingVoice(UnsignedInt objID) const
{
	if (objID == 0) return false;
	for (auto it = m_playingSounds.begin(); it != m_playingSounds.end(); ++it) {
		if (!(*it)->m_audioEventRTS) continue;
		const AudioEventInfo *info = (*it)->m_audioEventRTS->getAudioEventInfo();
		if (info && (*it)->m_audioEventRTS->getObjectID() == objID && (info->m_type & ST_VOICE))
			return true;
	}
	return false;
}

//-------------------------------------------------------------------------------------------------
AudioEventRTS *MiniAudioManager::findLowestPrioritySound(AudioEventRTS *event)
{
	AudioPriority priority = event->getAudioEventInfo()->m_priority;
	if (priority == AP_LOWEST) return NULL;

	AudioEventRTS *lowestPriorityEvent = NULL;
	AudioPriority lowestPriority = AP_LOWEST;

	for (auto it = m_playingSounds.begin(); it != m_playingSounds.end(); ++it) {
		AudioEventRTS *itEvent = (*it)->m_audioEventRTS.Peek();
		if (!itEvent) continue;
		const AudioEventInfo *itInfo = itEvent->getAudioEventInfo();
		if (!itInfo) continue;
		// GeneralsX @bugfix Antigravity 12/10/2026 Only consider 2D/3D sound effects as preemption
		// victims. In the MiniAudio backend music (AT_Music) and speech (AT_Streaming) share
		// m_playingSounds with the effects (Miles/OpenAL keep them in separate pools), so without
		// this filter a crowded effect pool would kill the currently playing music or voiceline
		// via handleToKill (music cutting out after ~8s in the main menu and mid-speech in-game).
		if (itInfo->m_soundType != AT_SoundEffect) continue;
		AudioPriority itPriority = itInfo->m_priority;
		if (itPriority < priority) {
			if (!lowestPriorityEvent || lowestPriority > itPriority) {
				lowestPriorityEvent = itEvent;
				lowestPriority = itPriority;
				if (lowestPriority == AP_LOWEST) return lowestPriorityEvent;
			}
		}
	}
	return lowestPriorityEvent;
}

//-------------------------------------------------------------------------------------------------
Bool MiniAudioManager::isPlayingLowerPriority(AudioEventRTS *event) const
{
	AudioPriority priority = event->getAudioEventInfo()->m_priority;
	if (priority == AP_LOWEST) return false;

	for (auto it = m_playingSounds.begin(); it != m_playingSounds.end(); ++it) {
		if (!(*it)->m_audioEventRTS) continue;
		const AudioEventInfo *info = (*it)->m_audioEventRTS->getAudioEventInfo();
		// GeneralsX @bugfix Antigravity 12/10/2026 Only preempt 2D/3D sound effects, never music
		// or speech. GameSounds::canPlayNow() calls this whenever the 2D/3D sample pools are full,
		// and the returned handleToKill is then killed in playAudioEvent(). Because music and
		// speech live in the same m_playingSounds list here (unlike the Miles/OpenAL backends),
		// any higher-priority effect request used to kill the active music track or voiceline,
		// which is why music cut out after ~8s in the menus and speech died mid-sentence in-game.
		if (info && info->m_soundType == AT_SoundEffect && info->m_priority < priority) {
			event->setHandleToKill((*it)->m_audioEventRTS->getPlayingHandle());
			return true;
		}
	}
	return false;
}

//-------------------------------------------------------------------------------------------------
Bool MiniAudioManager::killLowestPrioritySoundImmediately(AudioEventRTS *event)
{
	// GeneralsX @bugfix meerzulee 19/07/2026 FP env guard for audio entry point (see #215)
	ScopedFPUGuard fpuGuard;
	AudioEventRTS *lowestPriorityEvent = findLowestPrioritySound(event);
	if (!lowestPriorityEvent) return FALSE;

	for (auto it = m_playingSounds.begin(); it != m_playingSounds.end(); ++it) {
		PlayingAudio *playing = (*it);
		if (!playing) continue;
		if (playing->m_audioEventRTS && playing->m_audioEventRTS.Peek() == lowestPriorityEvent) {
			releasePlayingAudio(playing);
			m_playingSounds.erase(it);
			return TRUE;
		}
	}
	return FALSE;
}

//-------------------------------------------------------------------------------------------------
void MiniAudioManager::adjustVolumeOfPlayingAudio(AsciiString eventName, Real newVolume)
{
	// GeneralsX @bugfix meerzulee 19/07/2026 FP env guard for audio entry point (see #215)
	ScopedFPUGuard fpuGuard;
	for (auto it = m_playingSounds.begin(); it != m_playingSounds.end(); ++it) {
		PlayingAudio *playing = *it;
		if (playing && playing->m_audioEventRTS && playing->m_audioEventRTS->getEventName() == eventName) {
			playing->m_audioEventRTS->setVolume(newVolume);
			adjustPlayingVolume(playing);
		}
	}
}

//-------------------------------------------------------------------------------------------------
void MiniAudioManager::removePlayingAudio(AsciiString eventName)
{
	// GeneralsX @bugfix meerzulee 19/07/2026 FP env guard for audio entry point (see #215)
	ScopedFPUGuard fpuGuard;
	fprintf(stderr, "[MAUDIO-T %.0fms REMOVE '%s'\n", MAudioNowMs(), eventName.str()); fflush(stderr);
	for (auto it = m_playingSounds.begin(); it != m_playingSounds.end(); ) {
		PlayingAudio *playing = *it;
		if (playing && playing->m_audioEventRTS && playing->m_audioEventRTS->getEventName() == eventName) {
			it = m_playingSounds.erase(it);
			releasePlayingAudio(playing);
		}
		else { ++it; }
	}
}

//-------------------------------------------------------------------------------------------------
void MiniAudioManager::removeAllDisabledAudio(void)
{
	// GeneralsX @bugfix meerzulee 19/07/2026 FP env guard for audio entry point (see #215)
	ScopedFPUGuard fpuGuard;
	fprintf(stderr, "[MAUDIO-T %.0fms REMOVEDISABLED\n", MAudioNowMs()); fflush(stderr);
	for (auto it = m_playingSounds.begin(); it != m_playingSounds.end(); ) {
		PlayingAudio *playing = *it;
		if (playing && playing->m_audioEventRTS && playing->m_audioEventRTS->getVolume() == 0.0f) {
			fprintf(stderr, "[MAUDIO-T %.0fms REMOVEDISABLED victim='%s'\n", MAudioNowMs(),
				playing->m_audioEventRTS->getEventName().str()); fflush(stderr);
			it = m_playingSounds.erase(it);
			releasePlayingAudio(playing);
		}
		else { ++it; }
	}
}

//-------------------------------------------------------------------------------------------------
void MiniAudioManager::processRequestList(void)
{
	for (auto it = m_audioRequests.begin(); it != m_audioRequests.end(); ) {
		AudioRequest *req = (*it);
		if (req == NULL) { continue; }

		if (!shouldProcessRequestThisFrame(req)) {
			adjustRequest(req);
			++it;
			continue;
		}

		if (!req->m_requiresCheckForSample || checkForSample(req))
			processRequest(req);

		deleteInstance(req);
		it = m_audioRequests.erase(it);
	}
}

//-------------------------------------------------------------------------------------------------
void MiniAudioManager::processPlayingList(void)
{
	for (auto it = m_playingSounds.begin(); it != m_playingSounds.end(); ) {
		PlayingAudio *playing = (*it);
		if (!playing) {
			it = m_playingSounds.erase(it);
			continue;
		}

		// GeneralsX diag: periodically sample long-lived playback to find what cuts it off.
		{
			static unsigned long s_tick = 0;
			if (playing->m_sound) {
				float cur = 0.0f;
				ma_sound_get_cursor_in_seconds(playing->m_sound, &cur);
				Bool qualify = (playing->m_type == PAT_Stream) || (cur > 1.5f);
				if (qualify && ((s_tick++ % 60) == 0)) {
					float vol = ma_sound_get_volume(playing->m_sound);
					int pl = (int)ma_sound_is_playing(playing->m_sound);
					int gpl = (int)ma_sound_group_is_playing(&m_musicGroup);
					AsciiString nm = playing->m_audioEventRTS ? playing->m_audioEventRTS->getEventName() : AsciiString::TheEmptyString;
					fprintf(stderr, "[MAUDIO-T %.0fms LONG tick=%lu type=%d name='%s' cursor=%.2fs playing=%d vol=%.3f musicGroup=%d\n",
						MAudioNowMs(), s_tick, (int)playing->m_type, nm.str(), cur, pl, vol, gpl);
					fflush(stderr);
				}
			}
		}

		// Check if sound was explicitly marked to stop
		if (playing->m_requestStop) {
			if (playing->m_sound) {
				if (playing->m_type == PAT_Stream) {
					float cur = 0.0f;
					ma_sound_get_cursor_in_seconds(playing->m_sound, &cur);
					AsciiString nm = playing->m_audioEventRTS ? playing->m_audioEventRTS->getEventName() : AsciiString::TheEmptyString;
					fprintf(stderr, "[MAUDIO-T %.0fms STOPREQ stream '%s' requestStop at cursor=%.2fs\n", MAudioNowMs(), nm.str(), cur);
					fflush(stderr);
				}
				ma_sound_stop(playing->m_sound);
			}
			it = m_playingSounds.erase(it);
			releasePlayingAudio(playing);
			continue;
		}

		// Check if sound has finished playing
		if (playing->m_sound && !ma_sound_is_playing(playing->m_sound)) {
			if (playing->m_type == PAT_Stream) {
				float cur = 0.0f;
				ma_sound_get_cursor_in_seconds(playing->m_sound, &cur);
				AsciiString nm = playing->m_audioEventRTS ? playing->m_audioEventRTS->getEventName() : AsciiString::TheEmptyString;
				fprintf(stderr, "[MAUDIO-T %.0fms REAP stream '%s' ended at cursor=%.2fs\n", MAudioNowMs(), nm.str(), cur);
				fflush(stderr);
			}
			it = m_playingSounds.erase(it);
			releasePlayingAudio(playing);
			continue;
		}

		if (m_volumeHasChanged) {
			adjustPlayingVolume(playing);
		}

		// Update 3D position for positional sounds
		if (playing->m_type == PAT_3DSample && playing->m_audioEventRTS) {
			const Coord3D *pos = getCurrentPositionFromEvent(playing->m_audioEventRTS.Peek());
			if (pos && playing->m_sound) {
				ma_sound_set_position(playing->m_sound, pos->x, pos->y, pos->z);
			}
		}

		++it;
	}

	if (m_volumeHasChanged) {
		m_volumeHasChanged = false;

		// GeneralsX @bugfix Push speech volume changes because movie audio bypasses the audio mixer
		if (TheVideoPlayer) {
			TheVideoPlayer->setVolume(getVolume(AudioAffect_Speech));
		}
	}
}

//-------------------------------------------------------------------------------------------------
void MiniAudioManager::processFadingList(void)
{
	for (auto it = m_fadingAudio.begin(); it != m_fadingAudio.end(); ) {
		PlayingAudio *playing = *it;
		if (!playing) {
			it = m_fadingAudio.erase(it);
			continue;
		}

		if (playing->m_framesFaded >= getAudioSettings()->m_fadeAudioFrames) {
			fprintf(stderr, "[MAUDIO-T %.0fms FADEEND '%s' frames=%d\n", MAudioNowMs(),
				playing->m_audioEventRTS ? playing->m_audioEventRTS->getEventName().str() : "(null)",
				(int)playing->m_framesFaded); fflush(stderr);
			playing->m_requestStop = true;
			it = m_fadingAudio.erase(it);
			releasePlayingAudio(playing);
			continue;
		}

		++playing->m_framesFaded;
		Real volume = getEffectiveVolume(playing->m_audioEventRTS.Peek());
		volume *= (1.0f - 1.0f * playing->m_framesFaded / getAudioSettings()->m_fadeAudioFrames);

		if (playing->m_sound)
			ma_sound_set_volume(playing->m_sound, volume);

		++it;
	}
}

//-------------------------------------------------------------------------------------------------
void MiniAudioManager::processStoppedList(void)
{
	for (auto it = m_stoppedAudio.begin(); it != m_stoppedAudio.end(); ) {
		PlayingAudio *playing = *it;
		it = m_stoppedAudio.erase(it);
		if (playing) releasePlayingAudio(playing);
	}
}

//-------------------------------------------------------------------------------------------------
Bool MiniAudioManager::shouldProcessRequestThisFrame(AudioRequest *req) const
{
	if (!req->m_pendingEvent) return true;
	if (req->m_pendingEvent->getDelay() < MSEC_PER_LOGICFRAME_REAL) return true;
	return false;
}

//-------------------------------------------------------------------------------------------------
void MiniAudioManager::adjustRequest(AudioRequest *req)
{
	if (!req->m_pendingEvent) return;
	req->m_pendingEvent->decrementDelay(MSEC_PER_LOGICFRAME_REAL);
	req->m_requiresCheckForSample = true;
}

//-------------------------------------------------------------------------------------------------
Bool MiniAudioManager::checkForSample(AudioRequest *req)
{
	if (!req->m_pendingEvent) return true;
	if (req->m_pendingEvent->getAudioEventInfo() == NULL)
		getInfoForAudioEvent(req->m_pendingEvent.Peek());
	if (req->m_pendingEvent->getAudioEventInfo()->m_type != AT_SoundEffect)
		return true;
	return m_sound->canPlayNow(req->m_pendingEvent.Peek());
}

//-------------------------------------------------------------------------------------------------
void MiniAudioManager::setHardwareAccelerated(Bool accel)
{
	AudioManager::setHardwareAccelerated(accel);
}

//-------------------------------------------------------------------------------------------------
void MiniAudioManager::setSpeakerSurround(Bool surround)
{
	AudioManager::setSpeakerSurround(surround);
}

//-------------------------------------------------------------------------------------------------
Real MiniAudioManager::getFileLengthMS(AsciiString strToLoad) const
{
	// GeneralsX @bugfix meerzulee 19/07/2026 FP env guard for audio entry point (see #215)
	ScopedFPUGuard fpuGuard;
	if (strToLoad.isEmpty()) return 0.0f;

	static ma_vfs_callbacks vfs = {};
	vfs.onOpen = vfsFileOpen;
	vfs.onClose = vfsFileClose;
	vfs.onInfo = vfsFileInfo;
	vfs.onRead = vfsFileRead;
	vfs.onTell = vfsFileTell;
	vfs.onSeek = vfsFileSeek;

	ma_decoder decoder;
	ma_decoder_config config = ma_decoder_config_init_default();
	ma_result result = ma_decoder_init_vfs((ma_vfs *)&vfs, strToLoad.str(), &config, &decoder);
	if (result != MA_SUCCESS) return 0.0f;

	ma_uint64 lengthInFrames;
	result = ma_decoder_get_length_in_pcm_frames(&decoder, &lengthInFrames);
	if (result != MA_SUCCESS) {
		ma_decoder_uninit(&decoder);
		return 0.0f;
	}

	ma_uint32 sampleRate = decoder.outputSampleRate;
	if (sampleRate == 0) sampleRate = 44100;
	float lengthMS = (float)lengthInFrames / (float)sampleRate * 1000.0f;

	ma_decoder_uninit(&decoder);
	return lengthMS;
}

//-------------------------------------------------------------------------------------------------
void MiniAudioManager::closeAnySamplesUsingFile(const void *fileToClose)
{
	// GeneralsX @bugfix meerzulee 19/07/2026 FP env guard for audio entry point (see #215)
	ScopedFPUGuard fpuGuard;
	// miniaudio manages file lifecycle internally after decoding
}


//-------------------------------------------------------------------------------------------------
Bool MiniAudioManager::startNextLoop(PlayingAudio *looping)
{
	if (looping->m_requestStop) return false;
	if (!looping->m_audioEventRTS->hasMoreLoops()) return false;

	looping->m_audioEventRTS->generateFilename();

	if (looping->m_audioEventRTS->getDelay() > MSEC_PER_LOGICFRAME_REAL) {
		looping->m_requestStop = true;

		AudioRequest *req = allocateAudioRequest();
		req->m_pendingEvent = looping->m_audioEventRTS;
		req->m_requiresCheckForSample = true;
		appendAudioRequest(req);
		return true;
	}

	// Re-initialize the sound from the new file
	if (looping->m_sound) {
		ma_sound_uninit(looping->m_sound);
	}

	AsciiString fileToPlay = looping->m_audioEventRTS->getFilename();
	ma_uint32 flags = MA_SOUND_FLAG_NO_SPATIALIZATION | MA_SOUND_FLAG_NO_PITCH;
	if (looping->m_type == PAT_3DSample) flags = 0;

	ma_result result = ma_sound_init_from_file(&m_engine, fileToPlay.str(), flags,
		(looping->m_type == PAT_3DSample) ? &m_sound3DGroup : &m_soundGroup,
		NULL, looping->m_sound);

	if (result == MA_SUCCESS) {
		// GeneralsX @bugfix Mr. Meeseeks 19/06/2026 - Initialize loop volume using adjustPlayingVolume
		adjustPlayingVolume(looping);
		ma_sound_start(looping->m_sound);
		return true;
	}
	return false;
}

//-------------------------------------------------------------------------------------------------
void MiniAudioManager::playStream(AudioEventRTS *event, ma_sound *sound)
{
	ma_sound_start(sound);
}

//-------------------------------------------------------------------------------------------------
void MiniAudioManager::setDeviceListenerPosition(void)
{
	ma_engine_listener_set_position(&m_engine, 0,
		m_listenerPosition.x, m_listenerPosition.y, m_listenerPosition.z);
	ma_engine_listener_set_direction(&m_engine, 0,
		m_listenerOrientation.x, m_listenerOrientation.y, m_listenerOrientation.z);
}

//-------------------------------------------------------------------------------------------------
const Coord3D *MiniAudioManager::getCurrentPositionFromEvent(AudioEventRTS *event)
{
	if (!event->isPositionalAudio()) return NULL;
	return event->getCurrentPosition();
}

//-------------------------------------------------------------------------------------------------
Bool MiniAudioManager::isOnScreen(const Coord3D *pos) const
{
	static ICoord2D dummy;
	return TheTacticalView->worldToScreen(pos, &dummy);
}

//-------------------------------------------------------------------------------------------------
Real MiniAudioManager::getEffectiveVolume(AudioEventRTS *event) const
{
	if (!event) return 0.0f;
	Real volume = event->getVolume() * event->getVolumeShift();
	const AudioEventInfo *evInfo = event->getAudioEventInfo();
	if (evInfo && evInfo->m_soundType == AT_Music)           volume *= m_musicVolume;
	else if (evInfo && evInfo->m_soundType == AT_Streaming)  volume *= m_speechVolume;
	else if (event->isPositionalAudio())                     volume *= m_sound3DVolume;
	else                                                     volume *= m_soundVolume;
	return volume;
}

//-------------------------------------------------------------------------------------------------
void MiniAudioManager::buildProviderList(void) {}
void MiniAudioManager::createListener(void) {}
void MiniAudioManager::initDelayFilter(void) {}

//-------------------------------------------------------------------------------------------------
Bool MiniAudioManager::isValidProvider(void)
{
	return (m_selectedPlaybackDevice < m_playbackDeviceCount);
}

//-------------------------------------------------------------------------------------------------
void MiniAudioManager::initSamplePools(void) {}

//-------------------------------------------------------------------------------------------------
void MiniAudioManager::processRequest(AudioRequest *req)
{
	switch (req->m_request)
	{
	case AR_Play:   playAudioEvent(req); break;
	case AR_Pause:  pauseAudioEvent(req->m_handleToInteractOn); break;
	case AR_Stop:   stopAudioEvent(req->m_handleToInteractOn); break;
	}
}

//-------------------------------------------------------------------------------------------------
void MiniAudioManager::friend_forcePlayAudioEventRTS(const AudioEventRTS *eventToPlay)
{
	if (!eventToPlay->getAudioEventInfo()) {
		getInfoForAudioEvent(eventToPlay);
		if (!eventToPlay->getAudioEventInfo()) {
			DEBUG_CRASH(("MiniAudio: No info for forced audio event '%s'\n", eventToPlay->getEventName().str()));
			return;
		}
	}

	switch (eventToPlay->getAudioEventInfo()->m_soundType)
	{
	case AT_Music:     if (!isOn(AudioAffect_Music))  return; break;
	case AT_SoundEffect:
		if (!isOn(AudioAffect_Sound) || !isOn(AudioAffect_Sound3D)) return;
		break;
	case AT_Streaming: if (!isOn(AudioAffect_Speech)) return; break;
	}

	AudioRequest *req = allocateAudioRequest();
	req->m_pendingEvent.Assign_No_Add_Ref(newInstance(DynamicAudioEventRTS)(*eventToPlay));
	req->m_pendingEvent->generateFilename();
	req->m_pendingEvent->generatePlayInfo();

	for (auto it = m_adjustedVolumes.begin(); it != m_adjustedVolumes.end(); ++it) {
		if (it->first == req->m_pendingEvent->getEventName()) {
			req->m_pendingEvent->setVolume(it->second);
			break;
		}
	}

	playAudioEvent(req);
	releaseAudioRequest(req);
}

#if defined(_DEBUG) || defined(_INTERNAL)
//-------------------------------------------------------------------------------------------------
void MiniAudioManager::dumpAllAssetsUsed()
{
	if (!TheGlobalData->m_preloadReport) return;

	FILE *logfile = fopen("PreloadedAssets.txt", "a+");
	if (!logfile) return;

	fprintf(logfile, "\nAudio Asset Report - BEGIN\n");
	{
		std::list<AsciiString> missingEvents;
		std::list<AsciiString> usedFiles;

		for (auto it = m_allEventsLoaded.begin(); it != m_allEventsLoaded.end(); ++it) {
			AsciiString astr = *it;
			AudioEventInfo *aei = findAudioEventInfo(astr);
			if (!aei) { missingEvents.push_back(astr); continue; }

			for (auto asIt = aei->m_attackSounds.begin(); asIt != aei->m_attackSounds.end(); ++asIt)
				usedFiles.push_back(*asIt);
			for (auto asIt = aei->m_sounds.begin(); asIt != aei->m_sounds.end(); ++asIt)
				usedFiles.push_back(*asIt);
			for (auto asIt = aei->m_decaySounds.begin(); asIt != aei->m_decaySounds.end(); ++asIt)
				usedFiles.push_back(*asIt);
			if (!aei->m_filename.isEmpty())
				usedFiles.push_back(aei->m_filename);
		}

		fprintf(logfile, "\nMissing events - BEGIN\n");
		for (auto &s : missingEvents) fprintf(logfile, "%s\n", s.str());
		fprintf(logfile, "\nMissing events - END\n");

		fprintf(logfile, "\nFiles Used - BEGIN\n");
		for (auto &s : usedFiles) fprintf(logfile, "%s\n", s.str());
		fprintf(logfile, "\nFiles Used - END\n");
	}
	fprintf(logfile, "\nAudio Asset Report - END\n");
	fclose(logfile);
}
#endif

//-------------------------------------------------------------------------------------------------
// VFS callbacks — allow miniaudio to load files from the game's virtual file system
//-------------------------------------------------------------------------------------------------

struct VFSFileHandle { File *file; };

static ma_result vfsFileOpen(ma_vfs *pVFS, const char *filename, ma_uint32 mode, ma_vfs_file *pFile)
{
	VFSFileHandle *handle = new VFSFileHandle;
	handle->file = TheFileSystem->openFile(filename);
	if (!handle->file) {
		delete handle;
		return MA_ERROR;
	}
	*pFile = (ma_vfs_file)handle;
	return MA_SUCCESS;
}

static ma_result vfsFileClose(ma_vfs *pVFS, ma_vfs_file file)
{
	VFSFileHandle *handle = (VFSFileHandle *)file;
	if (handle) {
		if (handle->file) handle->file->close();
		delete handle;
	}
	return MA_SUCCESS;
}

static ma_result vfsFileInfo(ma_vfs *pVFS, ma_vfs_file file, ma_file_info *pInfo)
{
	VFSFileHandle *handle = (VFSFileHandle *)file;
	if (!handle || !handle->file) return MA_ERROR;
	pInfo->sizeInBytes = handle->file->size();
	return MA_SUCCESS;
}

static ma_result vfsFileRead(ma_vfs *pVFS, ma_vfs_file file, void *pBuffer, size_t bytesToRead, size_t *pBytesRead)
{
	VFSFileHandle *handle = (VFSFileHandle *)file;
	if (!handle || !handle->file) return MA_ERROR;
	Int bytesRead = handle->file->read(pBuffer, (Int)bytesToRead);
	if (pBytesRead) *pBytesRead = (size_t)(bytesRead < 0 ? 0 : bytesRead);
	return MA_SUCCESS;
}

static ma_result vfsFileTell(ma_vfs *pVFS, ma_vfs_file file, ma_int64 *pCursor)
{
	VFSFileHandle *handle = (VFSFileHandle *)file;
	if (!handle || !handle->file) return MA_ERROR;
	*pCursor = (ma_int64)handle->file->position();
	return MA_SUCCESS;
}

static ma_result vfsFileSeek(ma_vfs *pVFS, ma_vfs_file file, ma_int64 offset, ma_seek_origin origin)
{
	VFSFileHandle *handle = (VFSFileHandle *)file;
	if (!handle || !handle->file) return MA_ERROR;

	File::seekMode seekType;
	switch (origin) {
	case ma_seek_origin_start:   seekType = File::START;   break;
	case ma_seek_origin_current: seekType = File::CURRENT; break;
	case ma_seek_origin_end:     seekType = File::END;     break;
	default: return MA_ERROR;
	}

	handle->file->seek((Int)offset, seekType);
	return MA_SUCCESS;
}

//-------------------------------------------------------------------------------------------------
UnsignedInt MiniAudioManager::getNumAvailable2DSamples() const
{
	// GeneralsX @bugfix Mr. Meeseeks 27/06/2026 Fix available samples calculation to prevent voicelines culling
	UnsignedInt max2D = getAudioSettings()->m_sampleCount2D;
	UnsignedInt playing = 0;
	for (auto it = m_playingSounds.begin(); it != m_playingSounds.end(); ++it) {
		if ((*it) && (*it)->m_type == PAT_Sample) {
			playing++;
		}
	}
	return (max2D > playing) ? (max2D - playing) : 0;
}

//-------------------------------------------------------------------------------------------------
UnsignedInt MiniAudioManager::getNumAvailable3DSamples() const
{
	// GeneralsX @bugfix Mr. Meeseeks 01/07/2026 Fix available samples calculation to only count 3D samples
	UnsignedInt max3D = getAudioSettings()->m_sampleCount3D;
	UnsignedInt playing = 0;
	for (auto it = m_playingSounds.begin(); it != m_playingSounds.end(); ++it) {
		if ((*it) && (*it)->m_type == PAT_3DSample) {
			playing++;
		}
	}
	return (max3D > playing) ? (max3D - playing) : 0;
}
