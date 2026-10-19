/*
**	Command & Conquer Generals Zero Hour(tm)
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

/*
** MilesStub.h
**
** Stub definitions for Miles Sound System types when compiling on Linux with OpenAL or MiniAudio.
**
** TheSuperHackers @build 15/12/2024
** GeneralsX @feature fbraz 11/06/2026 Updated to also work with MiniAudio backend
** This header provides minimal type definitions to allow WWAudio.h and related
** files to compile without the Miles SDK headers. These are only used when
** SAGE_USE_OPENAL or SAGE_USE_MINIAUDIO is defined (Linux/macOS builds).
*/

#pragma once

#if (defined(SAGE_USE_OPENAL) || defined(SAGE_USE_MINIAUDIO)) && !defined(_MILES_STUB_H)
#define _MILES_STUB_H

// VC++ calling convention stubs
#if !defined(_WIN32)
    #define __stdcall
    #define __cdecl
    #define AILCALLBACK
#else
    #define AILCALLBACK
#endif

// Windows type stubs
// GeneralsX @build Qoder 18/10/2026 The 64-bit Windows MiniAudio build routes WWAudio through this
// stub as well, and its TUs do not otherwise include windows.h, so CRITICAL_SECTION/HANDLE must be
// provided here. WIN32_LEAN_AND_MEAN is defined for all core targets, so this does not pull
// mmsystem/mmreg and cannot clash with the WAVEFORMAT stub below.
#if !defined(_WIN32)
    #define HANDLE void*
#else
    #include <windows.h>
#endif

// GeneralsX @build win64 11/10/2026 The WW integral typedefs (U8..F64) were historically only
// defined for the !defined(_WIN32) OpenAL/MiniAudio stub path, because every Windows build used
// the real Miles SDK (mss.h) which provided them. Now that a 64-bit Windows build also routes
// through this stub (mss.h is not included), define them unconditionally. Redefining a typedef to
// the same underlying type is legal in C++11, so this stays safe even if another header agrees.
typedef unsigned char  U8;
typedef signed char    S8;
typedef unsigned short U16;
typedef signed short   S16;
typedef unsigned int   U32;
typedef signed int     S32;
typedef float          F32;
typedef double         F64;

// Miles Sound System type stubs (not including callback typedefs - those are defined in AudioEvents.h)
// GeneralsX @build Qoder 20/10/2026 WWAudioClass::Open_2D_Device reads m_Driver2D->emulated_ds, so the
// driver handle cannot stay a void*; this mirrors the DIG_DRIVER layout declared by mss.h.
typedef struct _DIG_DRIVER {
    char pad[168];
    int emulated_ds;
} DIG_DRIVER;
typedef DIG_DRIVER* HDIGDRIVER;
typedef void* HPROVIDER;
typedef void* HSAMPLE;
typedef void* H3DSAMPLE;
typedef void* H3DPOBJECT;
// GeneralsX @build Qoder 20/10/2026 mss.h declares HTIMER as an integer handle (S32), and
// WWAudioClass::Shutdown assigns -1 to it; keeping the old void* made that assignment fail.
typedef S32 HTIMER;

// Wave format stub
// GeneralsX @build win64 11/10/2026 WWAudio.h references LPWAVEFORMAT in a public method signature
// (Open_2D_Device), so the stub must exist on every platform that routes through MilesStub.h, now
// including 64-bit Windows. The win64 build compiles with WIN32_LEAN_AND_MEAN, so the SDK does not
// pull mmsystem/mmreg and there is no WAVEFORMAT redefinition clash. TUs that do include miniaudio.h
// (which owns the real WAVEFORMAT) never include this stub header.
typedef struct {
    U16 wFormatTag;
    U16 nChannels;
    U32 nSamplesPerSec;
    U32 nAvgBytesPerSec;
    U16 nBlockAlign;
    U16 wBitsPerSample;
} WAVEFORMAT;
typedef WAVEFORMAT* LPWAVEFORMAT;

// Driver info stub
struct DRIVER_INFO_STRUCT {
    char name[256];
    int capabilities;
    void* handle;
};

// GeneralsX @build Qoder 18/10/2026 MMSLockClass calls these unconditionally; with the Miles SDK
// replaced, there is no global Miles lock to take.
inline void AIL_lock() {}
inline void AIL_unlock() {}

// GeneralsX @build Qoder 20/10/2026 The stub used to cover only the 2D sample path, so the twelve
// WWAudio translation units that still speak Miles (stream handles, 3D handles, provider/filter
// enumeration, the file-callback bridge) failed to compile in the win64vk MiniAudio build. Everything
// the audio backends actually consume is MiniAudio; these declarations exist only so that the dead
// WWAudio Miles layer remains type-correct. Signatures mirror Dependencies/Miles/mss/mss.h.
// Parameters are left unnamed on purpose: the project builds with warnings-as-errors, so an unused
// named parameter would trip C4100.

typedef struct _STREAM* HSTREAM;
typedef U32 HPROENUM;
typedef S32 M3DRESULT;

typedef struct {
    S32 format;
    const void* data_ptr;
    U32 data_len;
    U32 rate;
    S32 bits;
    S32 channels;
    U32 samples;
    U32 block_size;
    const void* initial_ptr;
} AILSOUNDINFO;

typedef enum {
    DP_ASI_DECODER = 0,
    DP_FILTER,
    DP_MERGE,
    N_SAMPLE_STAGES,
    SAMPLE_ALL_STAGES
} SAMPLESTAGE;

// WIN32_LEAN_AND_MEAN keeps mmsystem.h out of the build, so the wave-out handle that Miles uses in
// its AIL_waveOutOpen signature has to be spelled out here.
#if !defined(_MMSYSTEM)
typedef void* HWAVEOUT;
#endif
typedef HWAVEOUT* LPHWAVEOUT;

typedef struct PCMWAVEFORMAT {
    WAVEFORMAT wf;
    U16 wBitsPerSample;
} PCMWAVEFORMAT;

typedef U32(__stdcall* AIL_file_open_callback)(const char*, void**);
typedef void(__stdcall* AIL_file_close_callback)(void*);
typedef S32(__stdcall* AIL_file_seek_callback)(void*, S32, U32);
typedef U32(__stdcall* AIL_file_read_callback)(void*, void*, U32);

#define WAVE_FORMAT_PCM 0x0001
#define WAVE_FORMAT_IMA_ADPCM 0x11
#define DIG_USE_WAVEOUT 15
#define AIL_LOCK_PROTECTION 18
#define ENVIRONMENT_GENERIC 0
#define HPROENUM_FIRST 0
#define AIL_NO_ERROR 0
#define AIL_FILE_SEEK_BEGIN 0
#define AIL_FILE_SEEK_CURRENT 1
#define AIL_FILE_SEEK_END 2
#define AIL_3D_2_SPEAKER 0
#define M3D_NOERR 0

// mss.h pairs these with the same include guards; WWAudio.cpp passes NO to AIL_set_preference.
#ifndef YES
#define YES 1
#endif
#ifndef NO
#define NO 0
#endif

#define AIL_set_3D_object_user_data AIL_set_3D_user_data
#define AIL_3D_object_user_data AIL_3D_user_data
#define AIL_3D_open_listener AIL_open_3D_listener

inline H3DPOBJECT AIL_open_3D_listener(HPROVIDER) { return 0; }
inline H3DSAMPLE AIL_allocate_3D_sample_handle(HPROVIDER) { return 0; }
inline HPROVIDER AIL_set_sample_processor(HSAMPLE, SAMPLESTAGE, HPROVIDER) { return 0; }
inline HSAMPLE AIL_allocate_sample_handle(HDIGDRIVER) { return 0; }
inline HSTREAM AIL_open_stream(HDIGDRIVER, const char*, S32) { return 0; }
inline M3DRESULT AIL_open_3D_provider(HPROVIDER) { return 0; }
inline S32 AIL_3D_sample_playback_rate(H3DSAMPLE) { return 0; }
inline F32 AIL_3D_sample_volume(H3DSAMPLE) { return 0.0f; }
inline S32 AIL_3D_user_data(H3DPOBJECT, U32) { return 0; }
inline S32 AIL_WAV_info(const void*, AILSOUNDINFO*) { return 0; }
inline S32 AIL_enumerate_3D_providers(HPROENUM*, HPROVIDER*, char**) { return 0; }
inline S32 AIL_enumerate_filters(HPROENUM*, HPROVIDER*, char**) { return 0; }
inline S32 AIL_sample_loop_count(HSAMPLE) { return 0; }
inline S32 AIL_sample_playback_rate(HSAMPLE) { return 0; }
inline S32 AIL_sample_user_data(HSAMPLE, U32) { return 0; }
inline S32 AIL_set_3D_sample_file(H3DSAMPLE, const void*) { return 0; }
inline S32 AIL_set_named_sample_file(HSAMPLE, const char*, const void*, S32, S32) { return 0; }
inline S32 AIL_set_preference(U32, S32) { return 0; }
inline S32 AIL_startup(void) { return 0; }
inline S32 AIL_stream_loop_count(HSTREAM) { return 0; }
inline S32 AIL_stream_playback_rate(HSTREAM) { return 0; }
inline S32 AIL_waveOutOpen(HDIGDRIVER*, LPHWAVEOUT*, S32, LPWAVEFORMAT) { return 0; }
inline U32 AIL_3D_sample_length(H3DSAMPLE) { return 0; }
inline U32 AIL_3D_sample_loop_count(H3DSAMPLE) { return 0; }
inline U32 AIL_3D_sample_offset(H3DSAMPLE) { return 0; }
inline char* AIL_last_error(void) { return nullptr; }
inline void AIL_close_3D_provider(HPROVIDER) { }
inline void AIL_close_stream(HSTREAM) { }
inline void AIL_end_3D_sample(H3DSAMPLE) { }
inline void AIL_end_sample(HSAMPLE) { }
inline void AIL_init_sample(HSAMPLE) { }
inline void AIL_pause_stream(HSTREAM, S32) { }
inline void AIL_release_3D_sample_handle(H3DSAMPLE) { }
inline void AIL_release_sample_handle(HSAMPLE) { }
inline void AIL_release_timer_handle(HTIMER) { }
inline void AIL_resume_3D_sample(H3DSAMPLE) { }
inline void AIL_resume_sample(HSAMPLE) { }
inline void AIL_sample_ms_position(HSAMPLE, S32*, S32*) { }
inline void AIL_sample_volume_pan(HSAMPLE, F32*, F32*) { }
inline void AIL_set_3D_orientation(H3DPOBJECT, F32, F32, F32, F32, F32, F32) { }
inline void AIL_set_3D_position(H3DPOBJECT, F32, F32, F32) { }
inline void AIL_set_3D_sample_distances(H3DSAMPLE, F32, F32) { }
inline void AIL_set_3D_sample_effects_level(H3DSAMPLE, F32) { }
inline void AIL_set_3D_sample_loop_count(H3DSAMPLE, U32) { }
inline void AIL_set_3D_sample_offset(H3DSAMPLE, U32) { }
inline void AIL_set_3D_sample_playback_rate(H3DSAMPLE, S32) { }
inline void AIL_set_3D_sample_volume(H3DSAMPLE, F32) { }
inline void AIL_set_3D_speaker_type(HPROVIDER, S32) { }
inline void AIL_set_3D_user_data(H3DPOBJECT, U32, S32) { }
inline void AIL_set_3D_velocity_vector(H3DPOBJECT, F32, F32, F32) { }
inline void AIL_set_file_callbacks(AIL_file_open_callback, AIL_file_close_callback, AIL_file_seek_callback, AIL_file_read_callback) { }
inline void AIL_set_filter_sample_preference(HSAMPLE, const char*, const void*) { }
inline void AIL_set_sample_loop_count(HSAMPLE, S32) { }
inline void AIL_set_sample_ms_position(HSAMPLE, S32) { }
inline void AIL_set_sample_playback_rate(HSAMPLE, S32) { }
inline void AIL_set_sample_user_data(HSAMPLE, U32, S32) { }
inline void AIL_set_sample_volume_pan(HSAMPLE, F32, F32) { }
inline void AIL_set_stream_loop_block(HSTREAM, S32, S32) { }
inline void AIL_set_stream_loop_count(HSTREAM, S32) { }
inline void AIL_set_stream_ms_position(HSTREAM, S32) { }
inline void AIL_set_stream_playback_rate(HSTREAM, S32) { }
inline void AIL_set_stream_volume_pan(HSTREAM, F32, F32) { }
inline void AIL_shutdown(void) { }
inline void AIL_start_3D_sample(H3DSAMPLE) { }
inline void AIL_start_sample(HSAMPLE) { }
inline void AIL_start_stream(HSTREAM) { }
inline void AIL_stop_3D_sample(H3DSAMPLE) { }
inline void AIL_stop_sample(HSAMPLE) { }
inline void AIL_stop_timer(HTIMER) { }
inline void AIL_stream_ms_position(HSTREAM, S32*, S32*) { }
inline void AIL_stream_volume_pan(HSTREAM, F32*, F32*) { }
inline void AIL_waveOutClose(HDIGDRIVER) { }

#endif


