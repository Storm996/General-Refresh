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

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

// FILE: W3DDisplay.cpp ///////////////////////////////////////////////////////
//
// W3D Implementation for the Game Display which is responsible for creating
// and maintaining the entire visual display
//
// Author: Colin Day, April 2001
//
///////////////////////////////////////////////////////////////////////////////

static void drawFramerateBar();

// SYSTEM INCLUDES ////////////////////////////////////////////////////////////
#include <numeric>
#include <stdlib.h>
#include <windows.h>
// GeneralsX @bugfix BenderAI 13/02/2026 - io.h is Windows-specific, use unistd.h on Linux
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h> // access() for file existence checks
#include <SDL3/SDL.h> // For SDL_ShowWindow() on Linux
#endif
#include <time.h>
#include <vector>

// USER INCLUDES //////////////////////////////////////////////////////////////
#include "Common/FramePacer.h"
#include "Common/ThingFactory.h"
#include "Common/GlobalData.h"
#include "Common/OptionPreferences.h"	// GeneralsX @feature PS3.0-P3-25a 14/10/2026 VSync present interval
#include "Common/PerfTimer.h"
#include "Common/FileSystem.h"
#include "Common/LocalFileSystem.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/ThingTemplate.h"
#include "Common/GameLOD.h"
#include "Common/DrawModule.h"
#include "GameLogic/AIPathfind.h"
#include "GameLogic/Module/PhysicsUpdate.h"

#include "GameClient/Drawable.h"
#include "GameClient/GameText.h"
#include "GameClient/GraphDraw.h"
#include "GameClient/Line2D.h"
#include "GameClient/Mouse.h"
#include "GameClient/GlobalLanguage.h"
#include "GameClient/Water.h"
#include "GameClient/WeatherFX.h"		// GeneralsX @feature PS3.0 Weather W1 16/10/2026 weather->lighting coupler

#include "GameNetwork/NetworkInterface.h"
#include "Common/ModelState.h"
#include "Lib/BaseType.h"
#include "W3DDevice/Common/W3DConvert.h"
#include "W3DDevice/GameClient/W3DAssetManager.h"
#include "W3DDevice/GameClient/W3DGameClient.h"
#include "W3DDevice/GameClient/W3DFileSystem.h"
#include "W3DDevice/GameClient/W3DDynamicLight.h"
#include "W3DDevice/GameClient/W3DProfilerFrameCapture.h"
#include "W3DDevice/GameClient/HeightMap.h"
#include "W3DDevice/GameClient/W3DView.h"	// GeneralsX @feature 26/10/2026 fog camera-to-world snapshot
#include "W3DDevice/GameClient/WorldHeightMap.h"
#include "W3DDevice/GameClient/W3DScene.h"
#include "W3DDevice/GameClient/W3DTerrainTracks.h"
#include "W3DDevice/GameClient/W3DWater.h"
#include "W3DDevice/GameClient/W3DVideoBuffer.h"
#include "W3DDevice/GameClient/W3DShaderManager.h"
#include "W3DDevice/GameClient/W3DDebugDisplay.h"
#include "W3DDevice/GameClient/W3DProjectedShadow.h"
#include "W3DDevice/GameClient/W3DShadow.h"	// GeneralsX @feature Qoder 20/10/2026 native sun shadow: TheW3DShadowManager light direction
#include "W3DDevice/GameClient/W3DSunShadowMap.h"	// GeneralsX @feature PS3.0 T6 (base game links the inline no-op stub)
#include "W3DDevice/GameClient/W3DScreenshot.h"
#include "W3DDevice/GameClient/W3DShroud.h"
#include "W3DDevice/GameClient/W3DRenderLighting.h"
#include "W3DDevice/GameClient/W3DRenderGraph.h"
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
#include "WW3D2/Backend/VulkanDraw.h"	// GeneralsX @feature Qoder 20/10/2026 native sun shadow entry point
#endif
#include "WWMath/wwmath.h"
#include "WWLib/registry.h"
#include "WW3D2/ww3d.h"
#include "WW3D2/predlod.h"
#include "WW3D2/part_emt.h"
#include "WW3D2/part_ldr.h"
#include "WW3D2/dx8caps.h"
#include "WW3D2/dx8normalmap.h"
#include "WW3D2/ww3dformat.h"
#include "WW3D2/agg_def.h"
#include "WW3D2/render2dsentence.h"
#include "WW3D2/sortingrenderer.h"
#include "WW3D2/textureloader.h"
#include "WW3D2/dx8webbrowser.h"
#include "WW3D2/mesh.h"
#include "WW3D2/hlod.h"
#include "WW3D2/meshmatdesc.h"
#include "WW3D2/meshmdl.h"
#include "WW3D2/rddesc.h"
#include "WWLib/TARGA.h"

#include "GameLogic/ScriptEngine.h"		// For TheScriptEngine - jkmcd
#include "GameLogic/GameLogic.h"
#ifdef DUMP_PERF_STATS
#include "GameLogic/PartitionManager.h"
#endif

#include "WinMain.h"


// DEFINE AND ENUMS ///////////////////////////////////////////////////////////

#define no_SAMPLE_DYNAMIC_LIGHT	1
#ifdef SAMPLE_DYNAMIC_LIGHT
static W3DDynamicLight * theDynamicLight = nullptr;
static Real theLightXOffset = 0.1f;
static Real theLightYOffset = 0.07f;
static Int theFlashCount = 0;
#endif

// GeneralsX @debug 28/10/2026: the game layer (InGameUI::addMessageText) cannot see the Vulkan backend
// symbols, so a forensic shot window is armed through this device-layer shim. No-op off Vulkan.
#if defined(SAGE_USE_VULKAN_CORE)
void VKCore_Shot_Arm(int frames);
void GX_VK_Shot_Arm(int frames) { VKCore_Shot_Arm(frames); }
#else
void GX_VK_Shot_Arm(int) {}
#endif

// GeneralsX @feature PS3.0-4A agent 16/10/2026 forward decl of the shared pulse transport
// (defined next to createLightPulse; used earlier by the W3DDisplay::update test leg).
static void applyLightPulse(RTS3DScene* scene, const Coord3D *pos, const RGBColor *color,
                            Real innerRadius, Real attenuationWidth,
                            UnsignedInt increaseFrameTime, UnsignedInt decayFrameTime);

//*****************************************************************************************
//*****************************************************************************************
//**** Start Statistical Dump *************************************************************
//*****************************************************************************************

#ifdef DUMP_PERF_STATS

#include <cstdarg>

class StatDumpClass
{
public:
	StatDumpClass( const char *fname );
	~StatDumpClass();
	void dumpStats( Bool brief = FALSE, Bool flagSpikes = FALSE );

protected:
	FILE *m_fp;
};

//=============================================================================
//Open the file once at the beginning of the game -- everything appends to it.
//=============================================================================
StatDumpClass::StatDumpClass( const char *fname )
{
	char buffer[ _MAX_PATH ];
	GetModuleFileName( nullptr, buffer, sizeof( buffer ) );
	if (char *pEnd = strrchr(buffer, '\\'))
	{
		*pEnd = 0;
	}
	// TheSuperHackers @fix Caball009 03/06/2025 Don't use AsciiString here anymore because its memory allocator may not have been initialized yet.
	const std::string fullPath = std::string(buffer) + "\\" + fname;
	m_fp = fopen(fullPath.c_str(), "wt");
}

//=============================================================================
//Close the file at the end of the application
//=============================================================================
StatDumpClass::~StatDumpClass()
{
	if( m_fp )
	{
		fclose( m_fp );
	}
}

static const char *getCurrentTimeString()
{
	time_t aclock;
	time(&aclock);
	struct tm *newtime = localtime(&aclock);
	return asctime(newtime);
}

//=============================================================================
//Dump the stats
//=============================================================================


static Bool s_notFirstDump = FALSE;

void StatDumpClass::dumpStats( Bool brief, Bool flagSpikes )
{
	if( !m_fp )
	{
		return;
	}


  Bool beBrief = brief & s_notFirstDump;
  s_notFirstDump = TRUE;

	fprintf( m_fp, "----------------------------------------------------------------\n" );
	fprintf( m_fp, "Performance Statistical Dump -- Frame %d\n", TheGameLogic->getFrame() );
  if ( ! beBrief )
  {
	  //static char buf[1024];
	  fprintf( m_fp, "Time:\t%s", getCurrentTimeString() );
	  fprintf( m_fp, "Map:\t%s\n", TheGlobalData->m_mapName.str());
	  fprintf( m_fp, "Side:\t%s\n", ThePlayerList->getLocalPlayer()->getSide().str());
	  fprintf( m_fp, "----------------------------------------------------------------\n" );
  }

	//FPS
	Real fps = TheDisplay->getAverageFPS();
	fprintf( m_fp, "Average FPS: %.1f (%.5f msec)\n", fps, 1000.0f / fps );
  if ( flagSpikes && fps<20.0f )
  	fprintf( m_fp, "                                                                      FPS OUT OF TOLERANCE\n" );


	//Rendering stats
	fprintf( m_fp, "Draws: %d \nSkins: %d \nSortedPolys: %d \nSkinPolys: %d\n",(Int)Debug_Statistics::Get_Draw_Calls(),
		(Int)Debug_Statistics::Get_DX8_Skin_Renders(),
		(Int)Debug_Statistics::Get_Sorting_Polygons(), (Int)Debug_Statistics::Get_DX8_Skin_Polygons());

	Int onScreenParticleCount = TheParticleSystemManager->getOnScreenParticleCount();

  if ( flagSpikes )
  {
    if ( Debug_Statistics::Get_Draw_Calls()>2000 )
  	  fprintf( m_fp, "                                                                      DRAWS OUT OF TOLERANCE(2000)\n" );
    if ( Debug_Statistics::Get_Sorting_Polygons() > (onScreenParticleCount*2) + 300 )
  	  fprintf( m_fp, "                                                                      NON-PARTICLE-SORTS OUT OF TOLERANCE(300)\n" );
    if ( Debug_Statistics::Get_DX8_Skin_Renders()>100 )
  	  fprintf( m_fp, "                                                                      SKINS OUT OF TOLERANCE(100)\n" );
  }


	//Object stats
	UnsignedInt objCount = TheGameLogic->getObjectCount();
	UnsignedInt objScreenCount = TheGameClient->getRenderedObjectCount();
	fprintf( m_fp, "Objects: %d in world (%d onscreen)\n", objCount, objScreenCount );
  if ( flagSpikes && objCount > 800 )
  	fprintf( m_fp, "                                                                      OBJS OUT OF TOLERANCE(800)\n" );

	//AI stats
	UnsignedInt numAI, numMoving, numAttacking, numWaitingForPath, overallFailedPathfinds;
	TheGameLogic->getAIMetricsStatistics( &numAI, &numMoving, &numAttacking, &numWaitingForPath, &overallFailedPathfinds );
	fprintf( m_fp, "\n" );
	fprintf( m_fp, "AI Statistics:\n" );
	fprintf( m_fp, "  Total AI Objects: %d\n", numAI );
	fprintf( m_fp, "    -moving: %d\n", numMoving );
	fprintf( m_fp, "    -attacking: %d\n", numAttacking );
	fprintf( m_fp, "    -waiting for path: %d\n", numWaitingForPath );
	fprintf( m_fp, "  Total failed pathfinds: %d\n", overallFailedPathfinds );
  if ( flagSpikes && overallFailedPathfinds > 0 )
  	fprintf( m_fp, "                                                                      FAILEDPATHFINDS OUT OF TOLERANCE(0)\n" );
	fprintf( m_fp, "\n" );

	// Script stats
	Real timeLastFrame, slowScript1, slowScript2;
	AsciiString slowScripts = TheScriptEngine->getStats(&timeLastFrame, &slowScript1, &slowScript2);
	fprintf( m_fp, "\n" );
	fprintf( m_fp, "Script Engine Statistics:\n" );
	fprintf( m_fp, "  Total time last frame: %.5f msec\n", timeLastFrame*1000 );
	fprintf( m_fp, "    -Slowest 2 scripts      %s\n", slowScripts.str() );
	fprintf( m_fp, "    -Slowest 2 script times %.5f msec, %.5f msec \n", slowScript1*1000, slowScript2*1000 );
  if ( flagSpikes && slowScript1*1000 > 0.2f || slowScript2*1000 > 0.2f )
  	fprintf( m_fp, "                                                                      SLOW SCRIPT OUT OF TOLERANCE(0.2)\n" );
	fprintf( m_fp, "\n" );



	//PartitionMgr stats
	double gcoTimeThisFrameTotal, gcoTimeThisFrameAvg;
	ThePartitionManager->getPMStats(gcoTimeThisFrameTotal, gcoTimeThisFrameAvg);
	fprintf(m_fp, "Partition Manager Statistics:\n");
	fprintf(m_fp, "  Total time for object scans this frame is %.5f msec\n", gcoTimeThisFrameTotal);
	fprintf(m_fp, "  Avg time per object scan this frame is %.5f msec\n", gcoTimeThisFrameAvg);
	fprintf( m_fp, "\n" );

	// setup texture stats
	Debug_Statistics::Record_Texture_Mode(Debug_Statistics::RECORD_TEXTURE_SIMPLE/*RECORD_TEXTURE_NONE*/);

	fprintf( m_fp, "Video Statistics:\n" );
	//Particle system stats
	fprintf( m_fp, "  Particle Systems: %d\n", TheParticleSystemManager->getParticleSystemCount() );
	Int totalParticles = TheParticleSystemManager->getParticleCount();
	fprintf( m_fp, "  Particles: %d in world (%d onscreen)\n", totalParticles, onScreenParticleCount );

  if ( flagSpikes && totalParticles > TheGlobalData->m_maxParticleCount - 10 )
  	fprintf( m_fp, "                                                                      PARTICLES OUT OF TOLERANCE(CAP-10)\n" );
  if ( flagSpikes && onScreenParticleCount > TheGlobalData->m_maxParticleCount - 10 )
  	fprintf( m_fp, "                                                                      ON_SCREEN_PARTICLES OUT OF TOLERANCE(CAP-10)\n" );


	// polygons this frame
	Int polyPerFrame = Debug_Statistics::Get_DX8_Polygons();
	Int polyPerSecond = (Int)(polyPerFrame * fps);
	fprintf( m_fp, "  Polygons: %d per frame (%d per second)\n", polyPerFrame, polyPerSecond );

	// vertices this frame
	fprintf( m_fp, "  Vertices: %d\n", Debug_Statistics::Get_DX8_Vertices() );

	//
	// I'm adjusting the texture memory usage counter by subtracting
	// out the terrain alpha texture (since it's really == terrain texture).
	//
	fprintf( m_fp, "  Video RAM: %d\n", Debug_Statistics::Get_Record_Texture_Size() - 1376256 );

	// terrain stats
	fprintf( m_fp, "  3-Way Blends: %d/%d, \n Shoreline Blends: %d/%d\n", TheTerrainRenderObject->getNumExtraBlendTiles(TRUE),TheTerrainRenderObject->getNumExtraBlendTiles(FALSE), TheTerrainRenderObject->getNumShoreLineTiles(TRUE),TheTerrainRenderObject->getNumShoreLineTiles(FALSE));
  if ( flagSpikes && TheTerrainRenderObject->getNumExtraBlendTiles(TRUE) > 2000 )
  	fprintf( m_fp, "                                                                      3-WAYS OUT OF TOLERANCE(2000)\n" );
  if ( flagSpikes && TheTerrainRenderObject->getNumShoreLineTiles(TRUE) > 2000 )
  	fprintf( m_fp, "                                                                      SHORELINES OUT OF TOLERANCE(2000)\n" );

	fprintf( m_fp, "\n" );

#if defined(RTS_DEBUG)
  if ( ! beBrief )
  {
    TheAudio->audioDebugDisplay( nullptr, nullptr, m_fp );
	  fprintf( m_fp, "\n" );
  }
#endif

#ifdef MEMORYPOOL_DEBUG
	//Report memory usage.
	TheMemoryPoolFactory->debugMemoryReport( REPORT_FACTORYINFO | REPORT_POOLINFO, 0, 0, m_fp );
#else
	fprintf( m_fp, "Memory Report -- unavailable \n(build doesn't have MEMORYPOOL_DEBUG defined)\n" );
#endif
	fprintf( m_fp, "\n" );

	fprintf( m_fp, "%s", TheSubsystemList->dumpTimesForAll().str());

  if ( ! beBrief )
  {
	  fprintf( m_fp, "----------------------------------------------------------------\n" );
	  fprintf( m_fp, "END -- Frame %d\n", TheGameLogic->getFrame() );
	  fprintf( m_fp, "----------------------------------------------------------------\n" );
  }
	fprintf( m_fp, "\n\n" );
	fflush(m_fp);
}

StatDumpClass TheStatDump("StatisticsDump.txt");

#endif //DUMP_PERF_STATS

//*****************************************************************************************
//**** End Statistical Dump ***************************************************************
//*****************************************************************************************
//*****************************************************************************************



///////////////////////////////////////////////////////////////////////////////
// DEFINITIONS ////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

//=============================================================================
RTS3DScene *W3DDisplay::m_3DScene = nullptr;
RTS2DScene *W3DDisplay::m_2DScene = nullptr;
RTS3DInterfaceScene *W3DDisplay::m_3DInterfaceScene = nullptr;
W3DAssetManager *W3DDisplay::m_assetManager = nullptr;

//=============================================================================
	// note, can't use the ones from PerfTimer.h 'cuz they are currently
	// only valid when "-vtune" is used... (srj)
// GeneralsX @build BenderAI 13/02/2026 Add platform-specific performance counter wrappers
inline Int64 getPerformanceCounter()
{
	Int64 tmp;
#ifdef _WIN32
	QueryPerformanceCounter((LARGE_INTEGER*)&tmp);
#else
	// Linux: Use time_compat.h wrapper which calls clock_gettime(CLOCK_MONOTONIC)
	QueryPerformanceCounter(&tmp);
#endif
	return tmp;
}

inline Int64 getPerformanceCounterFrequency()
{
	Int64 tmp;
#ifdef _WIN32
	QueryPerformanceFrequency((LARGE_INTEGER*)&tmp);
#else
	// Linux: Use time_compat.h wrapper which returns 10 million (100-nanosecond intervals)
	QueryPerformanceFrequency(&tmp);
#endif
	return tmp;
}

// W3DDisplay::W3DDisplay =====================================================
/** */
//=============================================================================
// GeneralsX @refactor 23/10/2026: the first shared render-lighting snapshot. Filled once per
// time-of-day/weather change; tree/grass consume it now, and the remaining families move over in
// later phases. Keeping it here (rather than in WW3D2) preserves the platform-isolation rule: the
// backend stays game-agnostic, the game layer owns GlobalData.
static W3DRenderLightingState s_renderLighting = {
	{ 0.0f, 0.0f, 1.0f },
	{ 1.0f, 1.0f, 1.0f },
	{ 0.0f, 0.0f, 0.0f },
	{ 0.0f, 0.0f, 1.0f },
	{ 0.0f, 0.0f, 0.0f },
	{ 0.0f, 0.0f, 0.0f },
	{ 0.0f, 0.0f, 0.0f },
	{ 0.72f, 0.78f, 0.84f, 0.0f },
	{ 0.0f, 0.0f, 0.0f, 0.0f },
	{ 0.0f, 0.0f },
	0.0f,
	1.0f,
	0u
};

const W3DRenderLightingState& W3D_GetRenderLighting()
{
	return s_renderLighting;
}

// GeneralsX @feature Qoder 24/10/2026 P1: one hash for the three lighting families to compare in QA.
// It covers the values that must agree (sun, ambient, fill, fog), not the revision counter.
unsigned W3D_LightingStateHash()
{
	unsigned h = 2166136261u;
	const float* groups[5] = {
		s_renderLighting.sunDir, s_renderLighting.sunColor, s_renderLighting.ambient,
		s_renderLighting.fillDir, s_renderLighting.fillColor
	};
	for (int g = 0; g < 5; ++g) {
		for (int i = 0; i < 3; ++i) {
			unsigned u = 0;
			memcpy(&u, &groups[g][i], sizeof(u));
			h = (h ^ u) * 16777619u;
		}
	}
	for (int i = 0; i < 4; ++i) {
		unsigned u = 0;
		memcpy(&u, &s_renderLighting.fogColor[i], sizeof(u));
		h = (h ^ u) * 16777619u;
	}
	return h;
}

void W3D_SetCloudOffset(float x, float y)
{
	// Per-frame scroll; deliberately does not bump revision (that would rebake baked families every
	// frame). Terrain/road texture matrices and future sky clouds read this one pair.
	s_renderLighting.cloudOffset[0] = x;
	s_renderLighting.cloudOffset[1] = y;
}

static void NightCoolTint(float& r, float& g, float& b, bool ambient);



// GeneralsX @feature 26/10/2026 HDR: display-space colour -> linear. Values above 1 are light
// intensities (sun gain, point lights), so extend the sRGB curve with the same power instead of
// clamping; a colour and its intensity stay separable.
static inline float W3D_SrgbToLinearExt(float v)
{
	if (v <= 0.0f) return 0.0f;
	if (v <= 1.0f)
		return (v <= 0.04045f) ? (v / 12.92f) : powf((v + 0.055f) / 1.055f, 2.4f);
	return powf(v, 2.2f);
}

// GeneralsX @feature 27/10/2026 P1-2 rig chromaticity (04 T2.6): the map's light colours are
// display-authored. Decoding them per channel is space-correct but over-reddens a warm sun; 04 T2.6
// authored the linear rig hues directly (warm sun, cool sky, warm ground). This helper replaces the
// map's display CHROMA with the display-encoded T2.6 hue while preserving the map's display luminance,
// so TOD/weather intensity still drives the rig and the single decode produces the intended linear hue.
// GeneralsX @tweak 28/10/2026: default OFF. Once the light decode was corrected (objects no longer
// squared, sun at 2x, lights luma-preserved) the forced cool sky hue is what turned the HDR frame
// cold: on flat ground the hemisphere ambient is the dominant fill, and T2.6 replaces the map's
// warm-neutral ambient with a blue one (measured: turning it off lifts ground red ~7 levels at
// unchanged luma). The authored map chroma is the art direction; the rig only carries intensity.
// GENERALSX_VK_RIG_T26=1 re-arms the 04 hue for an A/B. Global rig change, never per family.
enum { W3D_RIG_SUN = 0, W3D_RIG_SKY = 1, W3D_RIG_GROUND = 2 };

static inline bool W3D_RigT26()
{
	static int cached = -1;
	if (cached < 0) {
		const char* e = getenv("GENERALSX_VK_RIG_T26");
		cached = (e != nullptr && *e != 0) ? ((atoi(e) != 0) ? 1 : 0) : 0;
	}
	return cached != 0;
}

static inline float W3D_LinearToSrgbExt(float v)
{
	if (v <= 0.0f) return 0.0f;
	if (v <= 0.0031308f) return v * 12.92f;
	if (v <= 1.0f) return 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
	return powf(v, 1.0f / 2.2f);
}

static inline void W3D_ApplyRigHue(float& r, float& g, float& b, int kind)
{
	static const float sunL[3] = { 1.00f, 0.84f, 0.66f };   // 04 T2.6 linear hues
	static const float skyL[3] = { 0.32f, 0.42f, 0.58f };
	static const float gndL[3] = { 0.24f, 0.19f, 0.15f };
	const float* h = (kind == W3D_RIG_SUN) ? sunL : ((kind == W3D_RIG_GROUND) ? gndL : skyL);
	const float dr = W3D_LinearToSrgbExt(h[0]);
	const float dg = W3D_LinearToSrgbExt(h[1]);
	const float db = W3D_LinearToSrgbExt(h[2]);
	const float yd = 0.2126f * r + 0.7152f * g + 0.0722f * b;
	const float yh = 0.2126f * dr + 0.7152f * dg + 0.0722f * db;
	if (yh <= 1e-6f) return;
	const float k = yd / yh;
	r = dr * k; g = dg * k; b = db * k;
}

// GeneralsX @tweak 28/10/2026 luma-preserving light/veil decode. A light colour (sun, ambient, fill,
// fog in-scatter) is an authored hue plus an intensity, not three independent albedo channels: decoding
// each channel separately raises the chroma of every SUM (sky blue/red 1.8 -> 3.5), which is what turned
// the ground/road cold while the albedo products cancelled. Decode the luminance, scale the authored
// RGB by that gain: magnitude moves into linear, the authored chroma survives. Albedo-like colours
// (textures, vertex tint, material diffuse) stay per-channel - there the decode IS the transfer.
static inline void W3D_LightToLinear(float& r, float& g, float& b)
{
	const float yd = 0.2126f * r + 0.7152f * g + 0.0722f * b;
	if (yd <= 1e-6f) { r = g = b = 0.0f; return; }
	const float k = W3D_SrgbToLinearExt(yd) / yd;
	r *= k; g *= k; b *= k;
}

// GeneralsX @feature 26/10/2026 HDR: light colours go through the same sRGB decode as the albedo they
// multiply. Fixed-camera A/B: decoding both matches the retail luminance (ACES k=0.65, luma 0.2367 vs
// 0.2314); skipping the light decode lifts the scene far above it (linear-only 0.368) and washes the
// shadows. GENERALSX_VK_LIN_LIGHT_SRGB=0 is the QA leg that treats light colours as linear multipliers.
static inline bool W3D_LightColorsAreSrgb()
{
	static int cached = -1;
	if (cached < 0) {
		const char* e = getenv("GENERALSX_VK_LIN_LIGHT_SRGB");
		cached = (e != nullptr && *e != 0 && atoi(e) == 0) ? 0 : 1;
	}
	return cached != 0;
}

void W3D_RefreshRenderLighting()
{
	if (!TheGlobalData) {
		return;
	}
	TimeOfDay tod = W3DDisplay::getRenderTimeOfDay();
	if ((Int)tod < 0 || (Int)tod >= TIME_OF_DAY_COUNT) {
		tod = TheGlobalData->m_timeOfDay;
	}
	const GlobalData::TerrainLighting &slot = TheGlobalData->m_terrainObjectsLighting[tod][0];
	const WeatherRenderState &wx = weatherRenderState();

	// GeneralsX @feature 26/10/2026 volumetric fog tier. 0 keeps the legacy per-material haze; 1 is the
	// analytic height-fog post pass, 2 adds 8 sun-shadow samples per pixel and 3 sixteen. The env override
	// is read once per process so the four QA legs stay comparable; the Options.ini row will seed it when
	// the settings page lands.
	// Settings-page rows (Options.ini VolumetricFog*), with GENERALSX_VK_FOG* still winning inside the
	// OptionPreferences getters for QA legs. Read once per process: the launcher saves while the game is
	// closed, so a change takes effect on the next launch (the page marks the rows as restart-scoped).
	static int s_vkFogMode = -1;
	static int s_vkFogDensityTick = -1;
	static int s_vkFogHeight = -1;
	static int s_vkFogStart = -1;
	static int s_vkFogSunTick = -1;
	if (s_vkFogMode < 0) {
		OptionPreferences prefs;
		s_vkFogMode = prefs.getVolumetricFogMode();
		s_vkFogDensityTick = prefs.getVolumetricFogDensity();
		s_vkFogHeight = prefs.getVolumetricFogHeight();
		s_vkFogStart = prefs.getVolumetricFogStart();
		s_vkFogSunTick = prefs.getVolumetricFogSun();
		fprintf(stderr, "[VKFOG] mode=%d densityTick=%d height=%d start=%d sunTick=%d "
			"(Options.ini VolumetricFog*; GENERALSX_VK_FOG* wins)\n",
			s_vkFogMode, s_vkFogDensityTick, s_vkFogHeight, s_vkFogStart, s_vkFogSunTick);
		fflush(stderr);
	}
	// The post pass draws the whole atmosphere in modes 1..3. Suppressing the legacy ramp here (a flag,
	// not the player's DistanceHaze value) is what prevents the old per-material haze from adding a
	// second layer on top of the new one.
	W3DShaderManager::setVolumetricFogSuppression(s_vkFogMode > 0);

	// GeneralsX @tweak 24/10/2026: the extra 0.5 ambient multiplier is gone. It was a leftover debug
	// value, and it cost the ground roughly half of the retail ambient: the object path was halved
	// while terrain/road/tree had been reading the raw INI value, and this snapshot then handed the
	// halved number to all of them. Ambient is now the map's raw slot0 value scaled only by weather.
	static Real s_keyGain = -1.0f;
	if (s_keyGain < 0.0f) {
		const char* ev = getenv("GENERALSX_KEY_GAIN");
		s_keyGain = (ev != nullptr && *ev != 0) ? (Real)atof(ev) : 1.0f;
		if (s_keyGain < 1.0f) s_keyGain = 1.0f;
		if (s_keyGain > 3.0f) s_keyGain = 3.0f;
	}

	Vector3 dir(-slot.lightPos.x, -slot.lightPos.y, -slot.lightPos.z);
	const Real len = dir.Length();
	if (len > 1e-6f) {
		dir.X /= len;
		dir.Y /= len;
		dir.Z /= len;
	} else {
		dir.Set(0.0f, 0.0f, 1.0f);
	}
	s_renderLighting.sunDir[0] = dir.X;
	s_renderLighting.sunDir[1] = dir.Y;
	s_renderLighting.sunDir[2] = dir.Z;

	// GeneralsX @feature Qoder 24/10/2026 P2 T2.6: low sun is warmer, sky ambient is cooler. This is
	// a data tint on the shared snapshot, not a per-shader hack, so every family sees the same grade.
	Real warm = 0.0f;
	if (tod != TIME_OF_DAY_NIGHT) {
		Real elev = dir.Z;
		if (elev < 0.0f) elev = 0.0f;
		if (elev > 1.0f) elev = 1.0f;
		warm = 1.0f - elev;
	}

	float sr = slot.diffuse.red * wx.m_keyScale * s_keyGain;
	float sg = slot.diffuse.green * wx.m_keyScale * s_keyGain;
	float sb = slot.diffuse.blue * wx.m_keyScale * s_keyGain;
	if (sr > 1.0f) sr = 1.0f;
	if (sg > 1.0f) sg = 1.0f;
	if (sb > 1.0f) sb = 1.0f;
	if (tod == TIME_OF_DAY_NIGHT) {
		NightCoolTint(sr, sg, sb, false);
	} else {
		sg *= 1.0f - 0.16f * warm;
		sb *= 1.0f - 0.32f * warm;
	}
	// GeneralsX @tweak 25/10/2026 P35: user request - every level reads a little dark, raise the KEY SUN
	// by 20% on all three channels and leave everything else (ambient, fill, tint, weather) untouched.
	// Applied after the clamp/tint so daylight (raw 1.0) also gains: 1.0 -> 1.2. The value can exceed 1,
	// which the shaders accept (LDR targets clip, which is the expected +20% look). This is the single
	// snapshot that terrain, water, FF and GN all read, and T1.6 below publishes it back into
	// m_myLight[0], so the object/FF key light moves with it instead of only the terrain.
	{
		static const Real kSunGain = 1.2f;
		sr *= kSunGain;
		sg *= kSunGain;
		sb *= kSunGain;
	}
	s_renderLighting.sunColor[0] = sr;
	s_renderLighting.sunColor[1] = sg;
	s_renderLighting.sunColor[2] = sb;

	// Object path: scene ambient = the map's raw slot0 ambient * weather ambient (no extra scale).
	float ar = slot.ambient.red * wx.m_ambientScale;
	float ag = slot.ambient.green * wx.m_ambientScale;
	float ab = slot.ambient.blue * wx.m_ambientScale;
	if (tod == TIME_OF_DAY_NIGHT) {
		NightCoolTint(ar, ag, ab, true);
	} else {
		ar *= 1.0f - 0.08f * warm;
		ab *= 1.0f + 0.20f * warm;
	}
	s_renderLighting.ambient[0] = ar;
	s_renderLighting.ambient[1] = ag;
	s_renderLighting.ambient[2] = ab;

	// GeneralsX @feature Qoder 24/10/2026 P3 T3.1: split the flat ambient into a simple sky/ground
	// hemisphere. The upper hemisphere picks up a little more blue, the lower one a little more warm
	// bounce; terrain/road blend by the surface normal instead of using one constant.
	s_renderLighting.skyColor[0] = ar * 0.92f;
	s_renderLighting.skyColor[1] = ag * 1.00f;
	s_renderLighting.skyColor[2] = ab * 1.18f;
	s_renderLighting.groundColor[0] = ar * 1.08f;
	s_renderLighting.groundColor[1] = ag * 0.98f;
	s_renderLighting.groundColor[2] = ab * 0.86f;

	// Map fill (slot1): objects get raw fill * weather ambient; slot2/3 are ambient-scaled later.
	// The synthetic L1 fill rig, when enabled, overwrites slot1 with a derived colour; state keeps the
	// data value for now, and P2 unifies the synthetic fill through this same snapshot.
	const GlobalData::TerrainLighting &fill = TheGlobalData->m_terrainObjectsLighting[tod][1];
	Vector3 fdir(-fill.lightPos.x, -fill.lightPos.y, -fill.lightPos.z);
	const Real flen = fdir.Length();
	if (flen > 1e-6f) {
		fdir.X /= flen; fdir.Y /= flen; fdir.Z /= flen;
	} else {
		fdir.Set(0.0f, 0.0f, 1.0f);
	}
	float fr = fill.diffuse.red * wx.m_ambientScale;
	float fg = fill.diffuse.green * wx.m_ambientScale;
	float fb = fill.diffuse.blue * wx.m_ambientScale;
	if (fr > 1.0f) fr = 1.0f;
	if (fg > 1.0f) fg = 1.0f;
	if (fb > 1.0f) fb = 1.0f;
	s_renderLighting.fillDir[0] = fdir.X;
	s_renderLighting.fillDir[1] = fdir.Y;
	s_renderLighting.fillDir[2] = fdir.Z;
	s_renderLighting.fillColor[0] = fr;
	s_renderLighting.fillColor[1] = fg;
	s_renderLighting.fillColor[2] = fb;

	// Same haze recipe terrain/road/tree and GN already use; one snapshot means water, terrain and
	// the weather veil cannot disagree about start/length/strength.
	float haze[4] = { 0.72f, 0.78f, 0.84f, 0.0f };
	float ramp[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	W3DShaderManager::getWeatherFogRamp(haze, ramp);
	s_renderLighting.fogColor[0] = haze[0];
	s_renderLighting.fogColor[1] = haze[1];
	s_renderLighting.fogColor[2] = haze[2];
	s_renderLighting.fogColor[3] = haze[3];
	s_renderLighting.fogRamp[0] = ramp[0];
	s_renderLighting.fogRamp[1] = ramp[1];
	s_renderLighting.fogRamp[2] = ramp[2];
	s_renderLighting.fogRamp[3] = ramp[3];

	// GeneralsX @feature 26/10/2026 volumetric fog: resolve the same TOD/weather snapshot into the post
	// pass's POD. The legacy fogColor above intentionally keeps its shipped rgb (the sky/water tint read
	// it even at strength 0); this block is the new atmosphere's own colour, darker and bluer at night.
	{
		VKAtmosphereFogParams fog = {};
		fog.mode = s_vkFogMode;
		float scale = wx.m_fogScale;
		if (scale < 1.0f) scale = 1.0f;
		if (scale > 2.4f) scale = 2.4f;
		// Ticks come from the settings page (VolumetricFog*); weather still scales the extinction.
		fog.density = (Real)s_vkFogDensityTick * 0.0001f * scale;
		fog.heightFalloff = 1.0f / (Real)s_vkFogHeight;
		// Anchor the density field to the ground under the camera pivot, not to world z=0. Maps in this game
		// routinely sit several hundred units above zero, and with a 150-unit scale height a z=0 anchor buries
		// the whole layer underground: measured camZ 631 / groundZ 506 gave tau 0.05 and an invisible fog.
		// The pivot's own z is the reliable ground height here (getTerrainHeightAtPivot stays 0 until the
		// camera is fully updated); terrain-relative height also lets valleys hold mist and ridges clear it.
		fog.baseHeight = (TheTacticalView != nullptr) ? (float)TheTacticalView->getPosition().z : 0.0f;
		if (!(fog.baseHeight == fog.baseHeight) || fog.baseHeight < -10000.0f || fog.baseHeight > 10000.0f)
			fog.baseHeight = 0.0f;
		// The tactical camera sits only a few hundred units up, so the legacy 180-unit start distance
		// faded the whole layer out: the measured amount at the default view was 0.05. Start close and
		// ramp quickly; the height integral, not the start gate, is what keeps near ground clear.
		fog.startDistance = (Real)s_vkFogStart;
		fog.fadeLength = 100.0f;
		fog.maxOpacity = 0.80f;
		fog.noiseScale = 0.020f;
		fog.windSpeed = 2.0f;
		fog.sunScatter = (Real)s_vkFogSunTick * 0.01f;
		// The sky used to be excluded (0) because a constant-colour mix turned it into a grey lid. With
		// transmittance + in-scatter the sky now fades into the same atmosphere as the far terrain, which
		// is what closes the horizon seam; 0.6 leaves the art-authored sky readable.
		fog.skyAffect = 0.6f;
		// 光影升级2.0 A3: contact-shadow strength (0..1) rides the atmosphere block; the post pass
		// marches the sun direction from each pixel and darkens where the depth buffer says a surface
		// sits in the way (the short range the sun shadow map's bias cannot cover).
		fog.contactShadows = (Real)OptionPreferences().getContactShadows() * 0.01f;
		fog.godRays = (Real)OptionPreferences().getGodRays() * 0.01f;
		fog.aoQuality = (Real)OptionPreferences().getAOQuality();
		{
			static Real s_lastContact = -1.0f;
			if (fabsf(s_lastContact - fog.contactShadows) > 1e-4f) {
				s_lastContact = fog.contactShadows;
				fprintf(stderr, "[VKCONTACT] published strength=%.2f\n", (double)fog.contactShadows);
				fflush(stderr);
			}
		}
		fog.time = (TheGameLogic != nullptr ? TheGameLogic->getFrame() : 0) * 0.033f;

		// Every knob above is seeded from OptionPreferences (VolumetricFog*), which keeps the
		// GENERALSX_VK_FOG* QA overrides inside the getters - no second env read here.

		float fr = haze[0], fgc = haze[1], fb = haze[2];
		if (tod == TIME_OF_DAY_NIGHT) {
			fr = haze[0] * 0.25f + s_renderLighting.ambient[0] * 0.75f;
			fgc = haze[1] * 0.25f + s_renderLighting.ambient[1] * 0.75f;
			fb = haze[2] * 0.25f + s_renderLighting.ambient[2] * 0.75f;
		}
		fog.color[0] = fr; fog.color[1] = fgc; fog.color[2] = fb;
		fog.sunColor[0] = s_renderLighting.sunColor[0];
		fog.sunColor[1] = s_renderLighting.sunColor[1];
		fog.sunColor[2] = s_renderLighting.sunColor[2];
		fog.sunDir[0] = s_renderLighting.sunDir[0];
		fog.sunDir[1] = s_renderLighting.sunDir[1];
		fog.sunDir[2] = s_renderLighting.sunDir[2];

		// GeneralsX @feature 26/10/2026 HDR: the post pass mixes this colour with the scene, so it has
		// to live in the same space. The fog owner retunes density/scatter after the HDR merge; only the
		// transfer function changes here.
		if (DX8Wrapper::VK_HDR_Enabled() && W3D_LightColorsAreSrgb()) {
			// GeneralsX @bugfix 27/10/2026 HDR: the in-scatter colour is a BLEND TARGET, not a light.
			// The legacy pass mixes it into a display-space frame, so its linear equivalent is the sRGB
			// decode of the authored colour; RelightColor (which keeps the display luminance) is right
			// for the sun term but leaves the haze about one gamma too bright. Measured on the fixed
			// camera, fog on: a shadowed pad at +41% of the legacy value (+20% with fog off) and the
			// whole frame lifted, i.e. the atmosphere was flattening the shadows it should only tint.
			// GENERALSX_VK_LIN_FOG_SRGB=0 restores the old (relit) transfer for an A/B.
			// Pure linear: both the in-scatter colour and the sun term are authored display colours, so
			// both move to linear here.
			// GeneralsX @bugfix 28/10/2026: luma-preserving, not per-channel (see W3D_LightToLinear).
			// Decoding the veils per channel raised their chroma and put a blue veil over the whole
			// ground/road: with fog on the fixed-camera grass read (83,86,70) against SDR's (94,78,47),
			// and turning the fog off moved blue from 70 to 46 - the atmosphere, not the lighting, was
			// the cold cast. A veil must carry the authored hue and only its intensity may change.
			W3D_LightToLinear(fog.color[0], fog.color[1], fog.color[2]);
			W3D_LightToLinear(fog.sunColor[0], fog.sunColor[1], fog.sunColor[2]);
		}

		// GeneralsX @bugfix 26/10/2026 publish the 3D camera basis for the post shader. The backend used to
		// invert whatever D3DTS_VIEW it could read at post time, but that transform is not the 3D camera by
		// then (it comes back identity), so the fog integrated in view space: camZ/worldZ both read 0 and
		// every sun-shadow lookup projected the wrong point. Rows are right/up/forward with the camera
		// position in .w - exactly the layout fogWorld() consumes.
		if (TheTacticalView != nullptr)
		{
			W3DView *tv = static_cast<W3DView *>(TheTacticalView);
			CameraClass *cam = tv->get3DCamera();
			if (cam != nullptr)
			{
				const Matrix3D &xf = cam->Get_Transform();
				const Vector3 ax = xf.Get_X_Vector();
				const Vector3 ay = xf.Get_Y_Vector();
				const Vector3 az = xf.Get_Z_Vector();
				const Vector3 pos = xf.Get_Translation();
				fog.camWorld[0] = ax.X; fog.camWorld[1] = ax.Y; fog.camWorld[2] = ax.Z; fog.camWorld[3] = pos.X;
				fog.camWorld[4] = ay.X; fog.camWorld[5] = ay.Y; fog.camWorld[6] = ay.Z; fog.camWorld[7] = pos.Y;
				fog.camWorld[8] = az.X; fog.camWorld[9] = az.Y; fog.camWorld[10] = az.Z; fog.camWorld[11] = pos.Z;
				fog.camWorld[12] = 0.0f; fog.camWorld[13] = 0.0f; fog.camWorld[14] = 0.0f; fog.camWorld[15] = 1.0f;
			}
		}

		DX8Wrapper::VK_Set_Atmosphere_Fog(fog);

		// GeneralsX @feature 26/10/2026 volumetric fog: the atmosphere must touch the lighting, not only
		// the post colour. Dim the direct sun by the vertical optical depth and hand the lost energy to
		// the ambient term, so dense air reads as scattered light instead of a grey wash. Render-side
		// only (no GameLogic/CRC involvement), applied before VKDraw_Set_Environment publishes it, and
		// fog.sunColor above keeps the un-attenuated sun so the shafts stay bright.
		{
			const float H = 1.0f / (fog.heightFalloff > 1e-6f ? fog.heightFalloff : 1e-6f);
			const float tauZenith = fog.density * H * 0.5f;	// 0.5 = art-tuned coupling strength
			const float atmT = expf(-tauZenith);
			for (int c = 0; c < 3; ++c) {
				const float sun = s_renderLighting.sunColor[c];
				s_renderLighting.sunColor[c] = sun * atmT;
				s_renderLighting.ambient[c] += sun * (1.0f - atmT) * 0.5f;
			}
			// GeneralsX @bugfix 26/10/2026 HDR: the HDR re-light below reads the sr/ar/fr locals, and
			// the atmosphere above only wrote the state. Re-sync or the calibration multiplies a
			// different colour than the one it publishes (measured: amb gain 1.62 and fill gain 8 at
			// dusk, with the sun/ambient pair no longer matching the state).
			sr = s_renderLighting.sunColor[0]; sg = s_renderLighting.sunColor[1]; sb = s_renderLighting.sunColor[2];
			ar = s_renderLighting.ambient[0];  ag = s_renderLighting.ambient[1];  ab = s_renderLighting.ambient[2];
		}

		static int s_fogLogMode = -1;
		static float s_fogLogDensity = -1.0f;
		static float s_fogLogBase = -100000.0f;
		if (s_vkFogMode > 0 && (s_vkFogMode != s_fogLogMode || fog.density > s_fogLogDensity + 0.00001f ||
			fog.density < s_fogLogDensity - 0.00001f || fabsf(fog.baseHeight - s_fogLogBase) > 16.0f)) {
			s_fogLogMode = s_vkFogMode;
			s_fogLogDensity = fog.density;
			s_fogLogBase = fog.baseHeight;
			fprintf(stderr, "[VKFOG] mode=%d density=%.6f height=%.1f base=%.0f start=%.0f sun=%.2f color=(%.2f,%.2f,%.2f) tod=%d\n",
				fog.mode, fog.density, 1.0f / fog.heightFalloff, fog.baseHeight, fog.startDistance, fog.sunScatter,
				fog.color[0], fog.color[1], fog.color[2], (int)tod);
			fflush(stderr);
		}
	}
	// GeneralsX @feature 26/10/2026 HDR: this snapshot is the single source terrain/road/tree/water/GN/FF
	// all read. In HDR mode it carries linear colour, so every family's sums and blends happen in light
	// space. The atmosphere block above already consumed the display-space values for its own UBO.
	if (DX8Wrapper::VK_HDR_Enabled()) {
		if (W3D_LightColorsAreSrgb()) {
			// GeneralsX @feature 27/10/2026 unified linear rig: every authored light colour is decoded
			// per channel to scene-referred linear, and nothing else. No hue-preserving re-light and no
			// compatibility sum - the ground, objects, FF, water and fog all read THIS snapshot, so a
			// change to sun colour/temperature/intensity reaches the whole frame through the same numbers.
			// GeneralsX @feature 27/10/2026 P1-2: replace the map's display chroma with the 04 T2.6
			// linear rig hue before the single decode. Luminance (TOD/weather intensity) is preserved.
			if (W3D_RigT26()) {
				W3D_ApplyRigHue(sr, sg, sb, W3D_RIG_SUN);
				W3D_ApplyRigHue(ar, ag, ab, W3D_RIG_SKY);
				W3D_ApplyRigHue(fr, fg, fb, W3D_RIG_SKY);
				W3D_ApplyRigHue(s_renderLighting.sunColor[0], s_renderLighting.sunColor[1], s_renderLighting.sunColor[2], W3D_RIG_SUN);
				W3D_ApplyRigHue(s_renderLighting.ambient[0], s_renderLighting.ambient[1], s_renderLighting.ambient[2], W3D_RIG_SKY);
				W3D_ApplyRigHue(s_renderLighting.skyColor[0], s_renderLighting.skyColor[1], s_renderLighting.skyColor[2], W3D_RIG_SKY);
				W3D_ApplyRigHue(s_renderLighting.groundColor[0], s_renderLighting.groundColor[1], s_renderLighting.groundColor[2], W3D_RIG_GROUND);
				W3D_ApplyRigHue(s_renderLighting.fillColor[0], s_renderLighting.fillColor[1], s_renderLighting.fillColor[2], W3D_RIG_SKY);
			}
			// GeneralsX @tweak 28/10/2026 luma-preserving light decode (user calibration). A light colour
			// is an authored hue plus an intensity, not three independent albedo channels: decoding each
			// channel separately raises the chroma of every sum (sky blue/red 1.8 -> 3.5), which is what
			// pushed the ground/road away from the SDR hue while the albedo products cancelled out.
			// Decoding the LUMINANCE and scaling the authored RGB by that gain moves the light into
			// linear space with its authored chroma intact, so an ambient+sun sum keeps the SDR channel
			// ratio. Fog stays a per-channel decode: it is a blend target, not a light.
			auto lin3 = [](float& r, float& g, float& b) { W3D_LightToLinear(r, g, b); };
			lin3(sr, sg, sb);
			lin3(ar, ag, ab);
			lin3(fr, fg, fb);
			lin3(s_renderLighting.sunColor[0], s_renderLighting.sunColor[1], s_renderLighting.sunColor[2]);
			lin3(s_renderLighting.ambient[0], s_renderLighting.ambient[1], s_renderLighting.ambient[2]);
			lin3(s_renderLighting.skyColor[0], s_renderLighting.skyColor[1], s_renderLighting.skyColor[2]);
			lin3(s_renderLighting.groundColor[0], s_renderLighting.groundColor[1], s_renderLighting.groundColor[2]);
			lin3(s_renderLighting.fillColor[0], s_renderLighting.fillColor[1], s_renderLighting.fillColor[2]);
			W3D_LightToLinear(s_renderLighting.fogColor[0], s_renderLighting.fogColor[1], s_renderLighting.fogColor[2]);

			// QA multipliers (default 1.0) so a scripted A/B can still move one leg.
			static float s_envSun = -1.0f, s_envAmb = -1.0f, s_envFill = -1.0f;
			if (s_envSun < 0.0f) {
				const char* es = getenv("GENERALSX_VK_LIN_SUN_GAIN");
				const char* ea = getenv("GENERALSX_VK_LIN_AMB_GAIN");
				const char* ef = getenv("GENERALSX_VK_LIN_FILL_GAIN");
				s_envSun = (es != nullptr && *es != 0) ? (float)atof(es) : 1.0f;
				s_envAmb = (ea != nullptr && *ea != 0) ? (float)atof(ea) : 1.0f;
				s_envFill = (ef != nullptr && *ef != 0) ? (float)atof(ef) : 1.0f;
			}
			const float sunGain = s_envSun, ambGain = s_envAmb, fillGain = s_envFill;

			sr *= sunGain; sg *= sunGain; sb *= sunGain;
			ar *= ambGain; ag *= ambGain; ab *= ambGain;
			fr *= fillGain; fg *= fillGain; fb *= fillGain;
			for (int c = 0; c < 3; ++c) {
				s_renderLighting.sunColor[c] *= sunGain;
				s_renderLighting.ambient[c] *= ambGain;
				s_renderLighting.skyColor[c] *= ambGain;
				s_renderLighting.groundColor[c] *= ambGain;
				s_renderLighting.fillColor[c] *= fillGain;
				s_renderLighting.fogColor[c] *= sunGain;
			}
			DX8Wrapper::VK_Set_Linear_Light_Gains(sunGain, ambGain, fillGain);
		} else {
			// QA leg: no relight colour conversion; the rig still publishes neutral gains.
			DX8Wrapper::VK_Set_Linear_Light_Gains(1.0f, 1.0f, 1.0f);
		}
	}
	// GeneralsX @tweak 28/10/2026 global sun intensity x2 - the shipped look, not a QA knob. Applied to
	// the ONE snapshot every family reads (terrain/water/road/tree read fpEnv, objects and the fixed
	// pipeline read the same state republished by T1.6), and placed AFTER the HDR decode so it is
	// exactly 2x scene-referred linear in HDR and exactly 2x display in SDR - one number, both paths,
	// no map/save edit. The retail sun/ambient ratio was too conservative on vertical facades.
	{
		const float kSunBoost = 2.0f;
		sr *= kSunBoost; sg *= kSunBoost; sb *= kSunBoost;
		for (int c = 0; c < 3; ++c) s_renderLighting.sunColor[c] *= kSunBoost;
	}
	s_renderLighting.cloudShadowStrength =
		(TheGlobalData->m_useCloudMap && TheGlobalData->m_timeOfDay != TIME_OF_DAY_NIGHT)
			? W3DShaderManager::getCloudShadowStrength() : 0.0f;
	s_renderLighting.exposure = DX8Wrapper::VK_HDR_Exposure();

	// GeneralsX @feature 27/10/2026 HDR per-TOD exposure. The volumetric atmosphere's in-scatter lifts
	// the evening frame more than the day frame (measured 0.148 vs legacy 0.124 with fog on, 0.130
	// with fog off), so it needs a small negative offset. This is presentation-side calibration:
	// the light rig and the fog data are untouched.
	// GeneralsX @tweak 27/10/2026 P1-2: the QA slots are 2 = AFTERNOON (the project's "dusk" leg) and
	// 3 = EVENING (the project's "night" leg; NIGHT = 4 is the night slot proper). The old code only
	// applied the offset to NIGHT, so both QA night legs ran with 0 EV - that is why the doc measured
	// "night +25%, mostly fog" with no EV in effect. With the rig decoded once and every family on it:
	//   afternoon 0 EV -> +6.5% against the SDR baseline (gate <= +5%)  => -0.10 EV
	//   evening   0 EV -> +27.0% with fog, +6.3% with fog off (fog is the rest; see 体积雾专项)
	// Day (MORNING) stays 0 EV. Two per-TOD offsets, one global knob each - never a per-family one,
	// and do not use the EV to hide the fog (the fog is added after the tonemap, so EV cannot move it
	// without also crushing the lit surfaces).
	{
		static int s_todEvInit = -1;
		static float s_nightEv = -0.30f;
		// GeneralsX @tweak 27/10/2026: -0.10 -> -0.28 together with ACES pre 0.80. At pre 0.80 the dusk
		// frame read +17.8% over SDR; -0.28 brings it back to ~+3%, night stays ~0%.
		static float s_duskEv = -0.28f;
		if (s_todEvInit < 0) {
			s_todEvInit = 1;
			const char* e = getenv("GENERALSX_VK_TOD_EV_NIGHT");
			if (e != nullptr && *e != 0) s_nightEv = (float)atof(e);
			const char* d = getenv("GENERALSX_VK_TOD_EV_DUSK");
			if (d != nullptr && *d != 0) s_duskEv = (float)atof(d);
			if (s_nightEv < -1.5f) s_nightEv = -1.5f;
			if (s_nightEv > 0.0f) s_nightEv = 0.0f;
			if (s_duskEv < -1.5f) s_duskEv = -1.5f;
			if (s_duskEv > 0.0f) s_duskEv = 0.0f;
		}
		const bool isNightSlot = (tod == TIME_OF_DAY_EVENING || tod == TIME_OF_DAY_NIGHT);
		const bool isDuskSlot = (tod == TIME_OF_DAY_AFTERNOON);
		DX8Wrapper::VK_Set_TOD_Exposure(isNightSlot ? s_nightEv : (isDuskSlot ? s_duskEv : 0.0f));
	}
	++s_renderLighting.revision;

	// GeneralsX @feature 24/10/2026 P34 C4: hand the same snapshot to the Forward+ block's fpEnv[] section,
	// which the object shaders use for the sky/environment reflection. One environment source, like every
	// other term in this file; GENERALSX_SKY_REFLECT=0 turns the additive term off for an A/B.
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
	{
		static Real s_reflectStrength = -1.0f;
		if (s_reflectStrength < 0.0f) {
			const char* ev = getenv("GENERALSX_SKY_REFLECT");
			s_reflectStrength = (ev != nullptr && *ev != 0) ? (Real)atof(ev) : 0.5f;
			if (s_reflectStrength < 0.0f) s_reflectStrength = 0.0f;
			if (s_reflectStrength > 2.0f) s_reflectStrength = 2.0f;
		}
		const float sunDir3[3] = { (float)dir.X, (float)dir.Y, (float)dir.Z };
		const float sunCol3[3] = { sr, sg, sb };
		const float sky3[3] = { s_renderLighting.skyColor[0], s_renderLighting.skyColor[1], s_renderLighting.skyColor[2] };
		const float ground3[3] = { s_renderLighting.groundColor[0], s_renderLighting.groundColor[1], s_renderLighting.groundColor[2] };
		const float fillDir3[3] = { (float)fdir.X, (float)fdir.Y, (float)fdir.Z };
		const float fillCol3[3] = { fr, fg, fb };
		VKDraw_Set_Environment(sunDir3, sunCol3, sky3, ground3, fillDir3, fillCol3, (float)s_reflectStrength);
	}
#endif

	// One line per refresh (TOD/weather changes only) so the three families can be compared.
	static int s_stateLog = -1;
	if (s_stateLog < 0) {
		const char* ev = getenv("GENERALSX_LIGHT_STATE");
		s_stateLog = (ev != nullptr && *ev != 0 && atoi(ev) != 0) ? 1 : 0;
	}
	if (s_stateLog) {
		fprintf(stderr, "[LIGHTSTATE] tod=%d sun=(%.3f,%.3f,%.3f) amb=(%.3f,%.3f,%.3f) fill=(%.3f,%.3f,%.3f) "
			"gains=(%.2f,%.2f,%.2f) hash=%08x rev=%u\n",
			(int)tod, sr, sg, sb, ar, ag, ab, fr, fg, fb,
			DX8Wrapper::VK_Linear_Sun_Gain(), DX8Wrapper::VK_Linear_Amb_Gain(), DX8Wrapper::VK_Linear_Fill_Gain(),
			W3D_LightingStateHash(), s_renderLighting.revision);
		fflush(stderr);
	}
}

W3DDisplay::W3DDisplay()
{
	Int i;

	m_initialized = false;
	m_assetManager = nullptr;
	m_3DScene = nullptr;
	m_2DScene = nullptr;
	m_3DInterfaceScene = nullptr;
	m_averageFPS = TheGlobalData->m_framesPerSecondLimit;
#if defined(RTS_DEBUG)
	m_timerAtCumuFPSStart = 0;
#endif
	for (i=0; i<LightEnvironmentClass::MAX_LIGHTS; i++)
		m_myLight[i] = nullptr;
	m_2DRender = nullptr;
	m_isClippedEnabled = FALSE;
	m_clipRegion.lo.x = 0;
	m_clipRegion.lo.y = 0;
	m_clipRegion.hi.x = 0;
	m_clipRegion.hi.y = 0;

	for (i = 0; i < DisplayStringCount; i++)
		m_displayStrings[i] = nullptr;

	m_batchTexture = nullptr;
	m_batchMode = DRAW_IMAGE_ALPHA;
	m_batchGrayscale = FALSE;
	m_batchNeedsInit = FALSE;

#ifdef PROFILER_ENABLED
	m_profilerFrameCapture = NEW W3DProfilerFrameCapture();
#endif
}

// W3DDisplay::~W3DDisplay ====================================================
/** */
//=============================================================================
W3DDisplay::~W3DDisplay()
{
#ifdef PROFILER_ENABLED
	delete m_profilerFrameCapture;
	m_profilerFrameCapture = nullptr;
#endif

	// get rid of the debug display
	delete m_debugDisplay;
	m_debugDisplay = nullptr;
	m_nativeDebugDisplay = nullptr;

	// delete the display strings
	for (int i = 0; i < DisplayStringCount; i++)
		TheDisplayStringManager->freeDisplayString(m_displayStrings[i]);

	// TheSuperHackers @fix Mauller/Tomsons26 28/04/2025 Free benchmark display string
	if( m_benchmarkDisplayString ) {
		TheDisplayStringManager->freeDisplayString(m_benchmarkDisplayString);
	}

	// delete 2D renderer
	if( m_2DRender )
	{

		m_2DRender->Reset();
		delete m_2DRender;
		m_2DRender = nullptr;

	}

	//
	// delete all our views now since they are W3D views and we need to
	// free them BEFORE we shutdown W3D
	//
	Display::deleteViews();

	REF_PTR_RELEASE( m_3DScene );
	REF_PTR_RELEASE( m_2DScene );
	REF_PTR_RELEASE( m_3DInterfaceScene );
	for (Int j=0; j<LightEnvironmentClass::MAX_LIGHTS; j++)
		REF_PTR_RELEASE( m_myLight[j] );

	PredictiveLODOptimizerClass::Free();

	// shutdown
	Debug_Statistics::Shutdown_Statistics();
	if (!TheGlobalData->m_headless)
		W3DShaderManager::shutdown();
	m_assetManager->Free_Assets();
	delete m_assetManager;
	if (!TheGlobalData->m_headless)
		WW3D::Shutdown();
	WWMath::Shutdown();
	if (!TheGlobalData->m_headless)
		DX8WebBrowser::Shutdown();
	delete TheW3DFileSystem;
	TheW3DFileSystem = nullptr;

}

// TheSuperHackers @tweak valeronm 20/03/2025 No longer filters resolutions by a 4:3 aspect ratio.
inline Bool isResolutionSupported(const ResolutionDescClass &res)
{
	static const Int minBitDepth = 24;

	return res.Width >= DEFAULT_DISPLAY_WIDTH && res.BitDepth >= minBitDepth;
}

// SDL3 display size providers for DX8Wrapper pillarbox (registered at init)
#ifdef SAGE_USE_SDL3
static bool SDL3_GetNativeDisplaySize(int& outW, int& outH, float& outDensity)
{
	extern SDL_Window* TheSDL3Window;
	if (!TheSDL3Window) return false;
	SDL_DisplayID displayId = SDL_GetDisplayForWindow(TheSDL3Window);
	const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(displayId);
	if (!mode || mode->w <= 0 || mode->h <= 0) return false;
	outDensity = mode->pixel_density > 0 ? mode->pixel_density : 1.0f;
	outW = (int)(mode->w * outDensity);
	outH = (int)(mode->h * outDensity);
	return true;
}

static bool SDL3_GetWindowSizeInPixels(int& outW, int& outH, float& outDensity)
{
	extern SDL_Window* TheSDL3Window;
	if (!TheSDL3Window) return false;
	int logW = 0, logH = 0, physW = 0, physH = 0;
	SDL_GetWindowSize(TheSDL3Window, &logW, &logH);
	SDL_GetWindowSizeInPixels(TheSDL3Window, &physW, &physH);
	if (physW <= 0 || physH <= 0) return false;
	outW = physW;
	outH = physH;
	outDensity = (logW > 0) ? (float)physW / (float)logW : 1.0f;
	return true;
}

// GeneralsX @bugfix GitHub Copilot 28/04/2026 Ensure SDL3 fullscreen transition actually lands in native fullscreen and foreground.
static void SDL3_EnsureNativeFullscreen(SDL_Window* window)
{
	if (!window) return;

	for (int attempt = 0; attempt < 3; ++attempt) {
		SDL_PumpEvents();
		if ((SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0) {
			break;
		}
		if (!SDL_SetWindowFullscreen(window, true)) {
			fprintf(stderr, "WARNING: SDL_SetWindowFullscreen(retry) failed: %s\n", SDL_GetError());
			break;
		}
	}

	SDL_RaiseWindow(window);
}

// GeneralsX @bugfix Claude 11/08/2026 Re-center the window after a resolution change. SDL keeps the
// window's top-left anchor across SDL_SetWindowSize(), so growing from the 1024x768 bootstrap size to
// the configured resolution expanded the window down and to the right, pushing it partly off-screen.
// Center on the window's current display, clamped to the usable bounds so it stays clear of the macOS
// menu bar / Linux panels.
//
// width/height MUST be logical points, not physical pixels — SDL_GetDisplayUsableBounds() reports
// logical coordinates, so mixing units here places the window by a factor of the display density.
static void SDL3_CenterWindowOnCurrentDisplay(SDL_Window* window, Int width, Int height)
{
	if (!window) return;

	const SDL_DisplayID displayId = SDL_GetDisplayForWindow(window);
	SDL_Rect usable{};
	if (displayId == 0 || !SDL_GetDisplayUsableBounds(displayId, &usable)) {
		fprintf(stderr, "WARNING: cannot resolve usable bounds for window display: %s\n", SDL_GetError());
		return;
	}

	Int x = usable.x + (usable.w - width) / 2;
	Int y = usable.y + (usable.h - height) / 2;

	// A window larger than the usable area must still anchor to that area's top-left corner, otherwise
	// the title bar ends up off-screen and the user cannot drag the window back into view.
	if (x < usable.x) x = usable.x;
	if (y < usable.y) y = usable.y;

	if (!SDL_SetWindowPosition(window, x, y)) {
		fprintf(stderr, "WARNING: SDL_SetWindowPosition(%d,%d) failed: %s\n", x, y, SDL_GetError());
	}
}

// GeneralsX @bugfix GitHub Copilot 27/04/2026 Apply SDL3 window sizing/fullscreen only after the final render resolution is known.
static void SDL3_ApplyWindowModeForRenderConfig(Bool windowed, Int renderWidth, Int renderHeight)
{
	extern SDL_Window* TheSDL3Window;
	if (!TheSDL3Window) return;

	if (!windowed) {
		if (!SDL_SetWindowFullscreen(TheSDL3Window, false)) {
			fprintf(stderr, "WARNING: SDL_SetWindowFullscreen(false) failed: %s\n", SDL_GetError());
		}

		SDL_DisplayID displayId = SDL_GetDisplayForWindow(TheSDL3Window);
		const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(displayId);
		if (mode) {
			// GeneralsX @bugfix MrMeeseeks 02/07/2026 Pass nullptr to request Desktop Fullscreen (Spaces) instead of Exclusive mode.
			if (!SDL_SetWindowFullscreenMode(TheSDL3Window, nullptr)) {
				fprintf(stderr, "WARNING: SDL_SetWindowFullscreenMode(nullptr) failed: %s\n", SDL_GetError());
			}
		}
		else {
			fprintf(stderr, "WARNING: SDL_GetCurrentDisplayMode failed for fullscreen transition\n");
		}
	}
	else {
		// GeneralsX @bugfix Claude 11/08/2026 Convert the render resolution to logical points before
		// sizing the window. renderWidth/Height are PHYSICAL pixels — buildFilteredResolutions() clamps
		// against SDL3_GetNativeDisplaySize(), which returns mode->w * pixel_density — but
		// SDL_SetWindowSize() takes LOGICAL points. On a 2x Retina display that made the window twice as
		// wide as intended (a 3840px render size produced a 7680px-wide window), which spilled across
		// onto the adjacent monitor and dragged the whole window there, since SDL resolves a window's
		// display by its center point.
		//
		// Derive the ratio from the window itself rather than the display's pixel_density: the drawable
		// is only scaled when the window carries SDL_WINDOW_HIGH_PIXEL_DENSITY (set on macOS only), so
		// this correctly collapses to 1.0 elsewhere and leaves non-HiDPI platforms untouched.
		Int logicalWidth = renderWidth;
		Int logicalHeight = renderHeight;
		{
			int logW = 0, logH = 0, pixW = 0, pixH = 0;
			SDL_GetWindowSize(TheSDL3Window, &logW, &logH);
			SDL_GetWindowSizeInPixels(TheSDL3Window, &pixW, &pixH);
			if (logW > 0 && logH > 0 && pixW > 0 && pixH > 0) {
				const Real scaleX = (Real)pixW / (Real)logW;
				const Real scaleY = (Real)pixH / (Real)logH;
				if (scaleX > 0.0f) logicalWidth = (Int)(renderWidth / scaleX);
				if (scaleY > 0.0f) logicalHeight = (Int)(renderHeight / scaleY);
			}
		}

		if (!SDL_SetWindowSize(TheSDL3Window, logicalWidth, logicalHeight)) {
			fprintf(stderr, "WARNING: SDL_SetWindowSize(%d,%d) failed: %s\n", logicalWidth, logicalHeight, SDL_GetError());
		}
		SDL3_CenterWindowOnCurrentDisplay(TheSDL3Window, logicalWidth, logicalHeight);
	}

	if (!windowed) {
		if (!SDL_SetWindowFullscreen(TheSDL3Window, true)) {
			fprintf(stderr, "WARNING: SDL_SetWindowFullscreen(true) failed: %s\n", SDL_GetError());
		}
		SDL3_EnsureNativeFullscreen(TheSDL3Window);
	}
}
#endif

// Filtered resolution cache — built once, clamps widths to 4:3..16:9 and deduplicates.
struct FilteredRes { Int w, h, bits; };
static std::vector<FilteredRes> s_filteredResolutions;
static bool s_filteredDirty = true;

static void buildFilteredResolutions()
{
	s_filteredResolutions.clear();
	const RenderDeviceDescClass &devDesc = WW3D::Get_Render_Device_Desc(0);
	const DynamicVectorClass<ResolutionDescClass> &resolutions = devDesc.Enumerate_Resolutions();

	int nativeW = 0, nativeH = 0;
	float density = 1.0f;
	DX8Wrapper::GetNativeDisplaySize(nativeW, nativeH, density);

	for (int i = 0; i < resolutions.Count(); i++) {
		if (!isResolutionSupported(resolutions[i])) continue;
		Int w = resolutions[i].Width;
		Int h = resolutions[i].Height;
		Int bits = resolutions[i].BitDepth;
		if (nativeH > 0 && h > nativeH) continue;
		Int minW = h * 4 / 3;
		Int maxW = h * 16 / 9;
		if (w < minW) w = minW;
		if (w > maxW) w = maxW;
		bool duplicate = false;
		for (const auto& e : s_filteredResolutions) {
			if (e.w == w && e.h == h && e.bits == bits) { duplicate = true; break; }
		}
		if (!duplicate) s_filteredResolutions.push_back({w, h, bits});
	}
	s_filteredDirty = false;
}

Int W3DDisplay::getDisplayModeCount()
{
	if (s_filteredDirty) buildFilteredResolutions();
	return (Int)s_filteredResolutions.size();
}

void W3DDisplay::getDisplayModeDescription(Int modeIndex, Int *xres, Int *yres, Int *bitDepth)
{
	if (s_filteredDirty) buildFilteredResolutions();
	if (modeIndex >= 0 && modeIndex < (Int)s_filteredResolutions.size()) {
		*xres = s_filteredResolutions[modeIndex].w;
		*yres = s_filteredResolutions[modeIndex].h;
		*bitDepth = s_filteredResolutions[modeIndex].bits;
	}
}

void W3DDisplay::setGamma(Real gamma, Real bright, Real contrast, Bool calibrate)
{
	if (m_windowed)
		return;	//we don't allow gamma to change in window because it would affect desktop.

	DX8Wrapper::Set_Gamma(gamma,bright,contrast,calibrate, false);
}

/** Set resolution of display */
//=============================================================================
Bool W3DDisplay::setDisplayMode( UnsignedInt xres, UnsignedInt yres, UnsignedInt bitdepth, Bool windowed )
{
	const UnsignedInt oldWidth = getWidth();
	const UnsignedInt oldHeight = getHeight();
	const UnsignedInt oldBitDepth = getBitDepth();
	const Bool oldWindowed = getWindowed();

	if (WW3D_ERROR_OK == WW3D::Set_Device_Resolution(xres,yres,bitdepth,windowed,true))
	{
		#ifdef SAGE_USE_SDL3
		SDL3_ApplyWindowModeForRenderConfig(windowed, xres, yres);
		#endif
		Render2DClass::Set_Screen_Resolution(RectClass(0, 0, xres, yres));
		Display::setDisplayMode(xres, yres, bitdepth, windowed);
		return TRUE;
	}

	//set back to the original mode.
	WW3D::Set_Device_Resolution(oldWidth, oldHeight, oldBitDepth, oldWindowed, true);
	Render2DClass::Set_Screen_Resolution(RectClass(0, 0, oldWidth, oldHeight));
	Display::setDisplayMode(oldWidth, oldHeight, oldBitDepth, oldWindowed);
	return FALSE;	//did not change to a new mode.
}

Bool W3DDisplay::getViewportRect( Int& x, Int& y, Int& width, Int& height ) const
{
	return DX8Wrapper::Pillarbox_Get_Rect(x, y, width, height) ? TRUE : FALSE;
}

/** Set width of display */
//=============================================================================
void W3DDisplay::setWidth( UnsignedInt width )
{

	// extending functionality
	Display::setWidth( width );

	// our 2D renderer will use mapping coords to make (0,0) the upper left
	// of the screen with (width,height) at the lower right
	m_2DRender->Set_Coordinate_Range( RectClass( 0, 0, getWidth(), getHeight() ) );

}

// W3DDisplay::setHeight ======================================================
/** Set height of display */
//=============================================================================
void W3DDisplay::setHeight( UnsignedInt height )
{

	// extending functionality
	Display::setHeight( height );

	// our 2D renderer will use mapping coords to make (0,0) the upper left
	// of the screen with (width,height) at the lower right
	m_2DRender->Set_Coordinate_Range( RectClass( 0, 0, getWidth(), getHeight() ) );

}

void W3DDisplay::onBeginBatch()
{
	m_batchTexture = nullptr;
	m_batchMode = DRAW_IMAGE_ALPHA;
	m_batchGrayscale = FALSE;
	m_batchNeedsInit = TRUE;

	if (m_2DRender)
	{
		m_2DRender->Reset();
	}
}

void W3DDisplay::onEndBatch()
{
	REF_PTR_RELEASE(m_batchTexture);
}

void W3DDisplay::onFlush()
{
	if (m_2DRender && !m_batchNeedsInit)
	{
		m_2DRender->Render();
		m_2DRender->Reset();
		m_batchNeedsInit = TRUE;
	}
}

void W3DDisplay::setup2DRenderState(TextureClass *tex, DrawImageMode mode, Bool grayscale)
{
	if (m_isBatching)
	{
		if (!m_batchNeedsInit && m_batchTexture == tex && m_batchMode == mode && m_batchGrayscale == grayscale)
		{
			return;
		}

		onFlush();

		if (tex != m_batchTexture)
		{
			if (tex)
			{
				tex->Add_Ref();
			}
			if (m_batchTexture)
			{
				m_batchTexture->Release_Ref();
			}

			m_batchTexture = tex;
		}

		m_batchMode = mode;
		m_batchGrayscale = grayscale;
		m_batchNeedsInit = FALSE;
	}
	else if (m_2DRender)
	{
		m_2DRender->Reset();
	}

	if (m_2DRender)
	{
		if (tex)
		{
			m_2DRender->Enable_Texturing(TRUE);
			m_2DRender->Set_Texture(tex);
		}
		else
		{
			m_2DRender->Enable_Texturing(FALSE);
		}

		switch (mode)
		{
			default:
			case DRAW_IMAGE_ALPHA:
				m_2DRender->Enable_Additive(FALSE);
				m_2DRender->Enable_Alpha(TRUE);
				m_2DRender->Enable_Grayscale(grayscale);
				break;
			case DRAW_IMAGE_GRAYSCALE:
				m_2DRender->Enable_Additive(FALSE);
				m_2DRender->Enable_Alpha(TRUE);
				m_2DRender->Enable_Grayscale(TRUE);
				break;
			case DRAW_IMAGE_ADDITIVE:
				m_2DRender->Enable_Additive(TRUE);
				m_2DRender->Enable_Alpha(FALSE);
				m_2DRender->Enable_Grayscale(grayscale);
				break;
			case DRAW_IMAGE_SOLID:
				m_2DRender->Enable_Additive(FALSE);
				m_2DRender->Enable_Alpha(FALSE);
				m_2DRender->Enable_Grayscale(grayscale);
				break;
		}
	}
}

// W3DDisplay::initAssets =====================================================
/** */
//=============================================================================
void W3DDisplay::initAssets()
{

}

// W3DDisplay::init3DScene ====================================================
/** */
//=============================================================================
void W3DDisplay::init3DScene()
{

}

// W3DDisplay::init2DScene ====================================================
/** This is the 2D scene, you can use it to draw on a 2D plane over the
	* 3D background */
//=============================================================================
void W3DDisplay::init2DScene()
{

}

// W3DDisplay::init ===========================================================
/** Initialize or re-initialize the W3D display system.  Here we need to
  * create our window, and get our 3D hardware setup and online */
//=============================================================================
void W3DDisplay::init()
{

	//
	// call our base class init, this method should be able to handle re-entry
	// with its own logic
	//
	Display::init();

	// handle re-entry for ourselves
	if( m_initialized )
	{

		/// @todo W3DDisplay needs RE-init logic!
		return;

	}
	// Override the W3D File system
	TheW3DFileSystem = NEW W3DFileSystem;

	// init the Westwood math library
	WWMath::Init();

	if (!TheGlobalData->m_headless)
	{

		// create our 3D interface scene
		m_3DInterfaceScene = NEW_REF( RTS3DInterfaceScene, () );
		m_3DInterfaceScene->Set_Ambient_Light( Vector3( 1, 1, 1 ) );

		// create our 2D scene
		m_2DScene = NEW_REF( RTS2DScene, () );
		m_2DScene->Set_Ambient_Light( Vector3( 1, 1, 1 ) );

		// create our 3D scene
		m_3DScene =NEW_REF( RTS3DScene, () );
	#if defined(RTS_DEBUG)
		if( TheGlobalData->m_wireframe )
			m_3DScene->Set_Polygon_Mode( SceneClass::LINE );
	#endif
	//============================================================================
		// m_myLight = NEW_REF
	//============================================================================
		Int lindex;
		Int numGlobal = TheGlobalData->m_numGlobalLights;
		// GeneralsX @feature PS3.0 L1 13/10/2026: the sky fill / ground bounce rig writes into
		// global slots [1]/[2] when the map data leaves them black (retail INI only owns slot 0),
		// so make sure those LightClass objects exist even if the data asks for a single sun.
		// Unfilled extra slots keep diffuse=0 and are dropped by LightEnvironment's black-light
		// filter, so registering them is behavior-neutral.
		if (numGlobal < 3) numGlobal = 3;
		if (numGlobal > LightEnvironmentClass::MAX_LIGHTS) numGlobal = LightEnvironmentClass::MAX_LIGHTS;
		for (lindex=0; lindex<numGlobal; lindex++)
		{	m_myLight[lindex] = NEW_REF( LightClass, (LightClass::DIRECTIONAL) );
		}

		setTimeOfDay( TheGlobalData->m_timeOfDay );	//set each light to correct values for given time

		for (lindex=0; lindex<numGlobal; lindex++)
		{	m_3DScene->setGlobalLight( m_myLight[lindex], lindex );
		}

	#ifdef SAMPLE_DYNAMIC_LIGHT
		theDynamicLight = NEW_REF(W3DDynamicLight, ());
		Real red = 1;
		Real green = 1;
		Real blue = 0;
		if(red==0 && blue==0 && green==0) {
			red = green = blue = 1;
		}
		theDynamicLight->Set_Ambient( Vector3( red, green, blue ) );
		theDynamicLight->Set_Diffuse( Vector3( red, green, blue) );
		theDynamicLight->Set_Position(Vector3(0, 0, 4));
		theDynamicLight->Set_Far_Attenuation_Range(1, 8);
		// Note: Don't Add_Render_Object dynamic lights.
		m_3DScene->addDynamicLight( theDynamicLight );
	#endif

	}

	// create a new asset manager
	m_assetManager = NEW W3DAssetManager;
	m_assetManager->Register_Prototype_Loader(&_ParticleEmitterLoader );
	m_assetManager->Register_Prototype_Loader(&_AggregateLoader);
	m_assetManager->Set_WW3D_Load_On_Demand( true );

	if (!TheGlobalData->m_headless)
	{

		if (TheGlobalData->m_incrementalAGPBuf)
		{
			SortingRendererClass::SetMinVertexBufferSize(1);
		}

		// Register SDL3 display size providers for pillarbox before device init
#ifdef SAGE_USE_SDL3
		DX8Wrapper::Set_Display_Size_Provider(SDL3_GetNativeDisplaySize, SDL3_GetWindowSizeInPixels);
#endif

		// GeneralsX @bugfix felipebraz 16/02/2026 Add detailed WW3D init logging
		fprintf(stderr, "DEBUG: About to call WW3D::Init() with ApplicationHWnd=%p\n", ApplicationHWnd);
		WW3DErrorType ww3d_result = WW3D::Init( ApplicationHWnd );

		// Decode error type
		const char* error_names[] = {
			"WW3D_ERROR_OK",
			"WW3D_ERROR_GENERIC",
			"WW3D_ERROR_LOAD_FAILED",
			"WW3D_ERROR_SAVE_FAILED",
			"WW3D_ERROR_WINDOW_NOT_OPEN",
			"WW3D_ERROR_INITIALIZATION_FAILED"
		};
		const char* error_name = (ww3d_result >= 0 && ww3d_result <= 5) ? error_names[ww3d_result] : "UNKNOWN";
		fprintf(stderr, "DEBUG: WW3D::Init() returned: %d (%s)\n", ww3d_result, error_name);

		if (ww3d_result != WW3D_ERROR_OK) {
			fprintf(stderr, "ERROR: WW3D::Init() failed with %s - DirectX8/DXVK initialization error\n", error_name);
			throw ERROR_INVALID_D3D;	//failed to initialize.  User probably doesn't have DX 8.1
		}

		// GeneralsX @debug 25/10/2026 NOSORT: A/B the sorted-mesh path. With sorting off the rotor
		// discs (PROPS01/02) draw through the plain mesh path with their own vertex/index buffers; if
		// they appear here but not with sorting on, the loss is in SortingRendererClass's re-upload.
		if (getenv("GENERALSX_NOSORT")) {
			fprintf(stderr, "[NOSORT] WW3D::Enable_Sorting(false)\n");
			fflush(stderr);
			WW3D::Enable_Sorting(false);
		}

		// GeneralsX @bugfix felipebraz 16/02/2026 Show window after DirectX8/DXVK initialized
		#ifndef _WIN32
		extern SDL_Window* TheSDL3Window;
		if (TheSDL3Window) {
			fprintf(stderr, "DEBUG: Showing SDL3 window after WW3D init...\n");
			SDL_ShowWindow(TheSDL3Window);
		}
		#endif

		WW3D::Set_Prelit_Mode( WW3D::PRELIT_MODE_LIGHTMAP_MULTI_PASS );
		WW3D::Set_Collision_Box_Display_Mask(0x00);	///<set to 0xff to make collision boxes visible
		WW3D::Enable_Static_Sort_Lists(true);
		// GeneralsX @feature PS3.0-P2-3 14/10/2026 T8: the thumbnail chain (ww3d default true) is
		// still disabled at runtime, so load decodes every texture full-size. Re-enabling it can
		// also decode synchronously on first in-game use (dx8texman invalidate path), which needs
		// hitch/VRAM measurement before it can ship as default. Opt-in knob for that A/B:
		// GENERALSX_THUMBNAIL=1 enables the thumbnail fallback storage.
		{
			const char* thumbEnv = getenv("GENERALSX_THUMBNAIL");
			const Bool thumbEnabled = (thumbEnv != nullptr && thumbEnv[0] == '1');
			WW3D::Set_Thumbnail_Enabled(thumbEnabled);
			if (thumbEnabled)
			{
				fprintf(stderr, "[T8] thumbnail storage ENABLED (load-time full decode skipped)\n");
				fflush(stderr);
			}
		}
		WW3D::Set_Screen_UV_Bias( TRUE );  ///< this makes text look good :)

	// GeneralsX @feature PS3.0-P3-25a 14/10/2026 vsync from Options.ini (VSync=0/1, restart to
	// take effect; GENERALSX_SWAPINTERVAL=0..3 still overrides inside the getter).  Injected here,
	// before the device is created below, so Set_Render_Device consumes it without a Reset.
	DX8Wrapper::Set_User_Present_Interval(OptionPreferences().getPresentInterval());

	// GeneralsX @bugfix 28/10/2026 shadow systems were both live until the options page was applied once.
	// VK_Real_Shadow_Enabled() defaults to true, but VK_Retail_Shadow_Off() (which DoShadows() consults)
	// only became true when the menu called the apply hook - so a fresh launch ran the native sun shadow
	// AND the retail projected-decal / stencil managers at the same time. The retail decals are the solid
	// black card-shaped blobs under trees (a stale decal texture multiplied over the terrain) and they
	// re-project every frame, which is what read as "the bush shadow flickers in cut-scenes". The two
	// paths are meant to be complementary, so latch the option once here.
	{
		const char* env = getenv("GENERALSX_SHADOWMAP");
		const Bool nativeShadow = env ? (atoi(env) > 0) : (OptionPreferences().getShadowMapEnabled() != FALSE);
		DX8Wrapper::VK_Set_Real_Shadow_Enabled(nativeShadow != FALSE);
		fprintf(stderr, "[SHADOWOPT] startup native=%d retailOff=%d\n",
			(int)nativeShadow, (int)DX8Wrapper::VK_Retail_Shadow_Off());
		fflush(stderr);
	}

		fprintf(stderr, "[DEBUG-WIN] W3DDisplay::init() - TheGlobalData->m_windowed=%d\n", (int)TheGlobalData->m_windowed);
		setWindowed( TheGlobalData->m_windowed );

		// create a 2D renderer helper
		m_2DRender = NEW Render2DClass;
		DEBUG_ASSERTCRASH( m_2DRender, ("Cannot create Render2DClass") );

		WW3DErrorType renderDeviceError;
		Int attempt = 0;
		do
		{
			switch (attempt)
			{
			case 0:
			{
				// set our default width and height and bit depth
				// GeneralsX @bugfix Qoder 21/10/2026 WINDOWCLAMP: a windowed game may not create a
				// window larger than the desktop work area. Resolution 3840x2160 on a 2560x1440
				// screen produced a window whose bottom/right were off-screen, which read as "only
				// the top-left of the game is visible". Fullscreen keeps the user's choice.
				Int xres = TheGlobalData->m_xResolution;
				Int yres = TheGlobalData->m_yResolution;
				if (TheGlobalData->m_windowed) {
					RECT work = { 0, 0, 0, 0 };
					if (SystemParametersInfo(SPI_GETWORKAREA, 0, &work, 0)) {
						const Int dw = work.right - work.left, dh = work.bottom - work.top;
						if (dw > 0 && dh > 0 && (xres > dw || yres > dh)) {
							const Int oldW = xres, oldH = yres;
							const float sx = (float)dw / (float)xres;
							const float sy = (float)dh / (float)yres;
							const float s = (sx < sy) ? sx : sy;
							xres = (Int)(xres * s);
							yres = (Int)(yres * s);
							if (xres < 800) xres = 800;
							if (yres < 600) yres = 600;
							TheWritableGlobalData->m_xResolution = xres;
							TheWritableGlobalData->m_yResolution = yres;
							fprintf(stderr, "[DISPLAY] windowed resolution clamped %dx%d -> %dx%d (desktop %dx%d)\n",
								(int)oldW, (int)oldH, (int)xres, (int)yres, (int)dw, (int)dh);
							fflush(stderr);
						}
					}
				}
				setWidth( xres );
				setHeight( yres );
				setBitDepth( DEFAULT_DISPLAY_BIT_DEPTH );
				break;
			}
			case 1:
			{
				// Getting the device at the default bit depth (32) didn't work, so try
				// getting a 16 bit display.  (Voodoo 1-3 only supported 16 bit.) jba.
				setBitDepth( MIN_DISPLAY_BIT_DEPTH );
				break;
			}
			case 2:
			{
				// TheSuperHackers @bugfix xezon 11/06/2025 Now tries a safe default resolution
				// if the custom resolution did not succeed. This is unlikely to happen but is possible
				// if the user writes an unsupported resolution in the Option Preferences or if the
				// graphics adapter does not support the minimum display resolution to begin with.
				Int xres = DEFAULT_DISPLAY_WIDTH;
				Int yres = DEFAULT_DISPLAY_HEIGHT;
				Int bitDepth = DEFAULT_DISPLAY_BIT_DEPTH;
				Int displayModeCount = getDisplayModeCount();
				Int displayModeIndex = 0;
				for (; displayModeIndex < displayModeCount; ++displayModeIndex)
				{
					getDisplayModeDescription(displayModeIndex, &xres, &yres, &bitDepth);
					if (xres * yres >= DEFAULT_DISPLAY_WIDTH * DEFAULT_DISPLAY_HEIGHT)
						break; // Is good enough. Use it.
				}
				TheWritableGlobalData->m_xResolution = xres;
				TheWritableGlobalData->m_yResolution = yres;
				setWidth( xres );
				setHeight( yres );
				setBitDepth( bitDepth );
				break;
			}
			}

			// GeneralsX @bugfix 28/10/2026: the retail MSAA row is gone, so the device is created without
			// multisampling. AA on this backend is the scene-only post pass (Advanced "PostAA" row).
			renderDeviceError = WW3D::Set_Render_Device(
				0,
				getWidth(),
				getHeight(),
				getBitDepth(),
				getWindowed(),
				true );

			// TheSuperHackers @info Update the MSAA mode that was set as some GPU's may not support certain levels
			// Texture filtering must also be updated after render device initialization
			if (renderDeviceError == WW3D_ERROR_OK) {
				WW3D::Set_Texture_Filter(TheWritableGlobalData->m_textureFilteringMode);
				TheWritableGlobalData->m_textureFilteringMode = WW3D::Get_Texture_Filter();
				WW3D::Set_Anisotropy_Level(TheWritableGlobalData->m_textureAnisotropyLevel);
				TheWritableGlobalData->m_textureAnisotropyLevel = WW3D::Get_Anisotropy_Level();
			}

			++attempt;
		}
		while (attempt < 3 && renderDeviceError != WW3D_ERROR_OK);

		if (renderDeviceError != WW3D_ERROR_OK)
		{
			WW3D::Shutdown();
			WWMath::Shutdown();
			throw ERROR_INVALID_D3D;	//failed to initialize.  User probably doesn't have DX 8.1
			DEBUG_CRASH( ("Unable to set render device") );
			return;
		}
		// GeneralsX @bugfix Jaredl-Dev 28/08/2026 Import display-based texture depth and normalize it for WW3D.
		// Upstream PR: https://github.com/TheSuperHackers/GeneralsGameCode/pull/3181
		WW3D::Set_Texture_Bitdepth(getBitDepth() == 16 ? 16 : 32);

		#ifdef SAGE_USE_SDL3
		SDL3_ApplyWindowModeForRenderConfig(getWindowed(), getWidth(), getHeight());
		#endif

		//Check if level was never set and default to setting most suitable for system.
		if (TheGameLODManager->getStaticLODLevel() == STATIC_GAME_LOD_UNKNOWN)
		{
			TheGameLODManager->setStaticLODLevel(TheGameLODManager->getRecommendedStaticLODLevel());
		}
		else
		{
			//Static LOD level was applied during GameLOD manager init except for texture reduction
			//which needs to be applied here.
			TheGameClient->setTextureLOD(TheWritableGlobalData->m_textureReductionFactor);
		}

		if (TheGlobalData->m_displayGamma != 1.0f)
			setGamma(TheGlobalData->m_displayGamma,0.0f,1.0f,FALSE);
	}

	initAssets();

	if (!TheGlobalData->m_headless)
	{
		init2DScene();
		init3DScene();
		W3DShaderManager::init();

		// Create and initialize the debug display
		m_nativeDebugDisplay = NEW W3DDebugDisplay();
		m_debugDisplay = m_nativeDebugDisplay;
		if ( m_nativeDebugDisplay )
		{
			m_nativeDebugDisplay->init();
			GameFont *font;

			if (TheGlobalLanguageData && TheGlobalLanguageData->m_nativeDebugDisplay.name.isNotEmpty())
			{
				font=TheFontLibrary->getFont(
					TheGlobalLanguageData->m_nativeDebugDisplay.name,
					TheGlobalLanguageData->m_nativeDebugDisplay.size,
					TheGlobalLanguageData->m_nativeDebugDisplay.bold);
			}
			else
				font=TheFontLibrary->getFont( "FixedSys", 8, FALSE );

			m_nativeDebugDisplay->setFont( font );
			m_nativeDebugDisplay->setFontHeight( 13 );
			m_nativeDebugDisplay->setFontWidth( 9 );
		}

		DX8WebBrowser::Initialize();
	}

	// we're now online
	m_initialized = true;
	if( TheGlobalData->m_displayDebug )
	{
		m_debugDisplayCallback = StatDebugDisplay;
	}
}

// W3DDisplay::reset ===========================================================
/** Reset the W3D display system.  Here we need to
  * remove the objects from the previous map. */
//=============================================================================
void W3DDisplay::reset()
{

	Display::reset();

	// Remove all render objects.

	if (m_3DScene != nullptr)
	{
		SceneIterator *sceneIter = m_3DScene->Create_Iterator();
		sceneIter->First();
		while(!sceneIter->Is_Done()) {
			RenderObjClass * robj = sceneIter->Current_Item();
			robj->Add_Ref();
			m_3DScene->Remove_Render_Object(robj);
			robj->Release_Ref();
			sceneIter->Next();
		}
		m_3DScene->Destroy_Iterator(sceneIter);
	}

	m_isClippedEnabled = FALSE;

	// release any unused assets from W3D
	/// @todo really need that "scene abstraction", having this stuff in the display is icky
	m_assetManager->Release_Unused_Assets();

	if (TheWritableGlobalData)
		TheWritableGlobalData->m_drawSkyBox =0;
}

const UnsignedInt START_CUMU_FRAME = LOGICFRAMES_PER_SECOND / 2;	// skip first half-sec

void W3DDisplay::updateAverageFPS()
{
	constexpr const Int FPS_HISTORY_SIZE = 30;

	static Int64 lastUpdateTime64 = 0;
	static Int historyOffset = 0;
	static Real fpsHistory[FPS_HISTORY_SIZE] = {0};

	const Int64 freq64 = getPerformanceCounterFrequency();
	const Int64 time64 = getPerformanceCounter();

#if defined(RTS_DEBUG)
	if (TheGameLogic->getFrame() == START_CUMU_FRAME)
	{
		m_timerAtCumuFPSStart = time64;
	}
#endif

	const Int64 timeDiff = time64 - lastUpdateTime64;

	// convert elapsed time to seconds
	Real elapsedSeconds = (Real)timeDiff/(Real)freq64;

	// append new sample to fps history.
	if (historyOffset >= FPS_HISTORY_SIZE)
		historyOffset = 0;

	m_currentFPS = 1.0f/elapsedSeconds;
	fpsHistory[historyOffset++] = m_currentFPS;

	// determine average frame rate over our past history.
	const Real sum = std::accumulate(fpsHistory, fpsHistory + FPS_HISTORY_SIZE, 0.0f);
	m_averageFPS = sum / FPS_HISTORY_SIZE;

	lastUpdateTime64 = time64;

	// GeneralsX @feature Qoder 24/10/2026 P5 T5.3: opt-in FPS log for the performance budget.
	// One line per ~60 frames so a 15 s menu run gives a stable average without flooding stderr.
	static int s_fpsLog = -1;
	if (s_fpsLog < 0) {
		const char* ev = getenv("GENERALSX_FPS_LOG");
		s_fpsLog = (ev != nullptr && *ev != 0 && atoi(ev) != 0) ? 1 : 0;
	}
	if (s_fpsLog) {
		static unsigned s_fpsTicks = 0;
		if ((s_fpsTicks++ % 60u) == 0u) {
			const Real avg = (m_averageFPS > 1e-3f) ? m_averageFPS : 0.0f;
			fprintf(stderr, "[FPS] avg=%.1f current=%.1f msec=%.2f\n",
				avg, m_currentFPS, (avg > 0.0f) ? 1000.0f / avg : 0.0f);
			fflush(stderr);
		}
	}
}

#if defined(RTS_DEBUG)	//debug hack to view object under mouse stats
ICoord2D TheMousePos;
#endif

// W3DDisplay::gatherDebugStats ===================================================
/** Compute and display debug stats on screen */
//=============================================================================
void W3DDisplay::gatherDebugStats()
{
	static UnsignedInt s_framesRenderedSinceLastUpdate = 0;
	static Int64 s_lastUpdateTime64 = 0;
	static double s_timeSinceLastUpdateInSecs = 0.0;
	static Int s_drawCallsSinceLastUpdate = 0;
	static Int s_sortedPolysSinceLastUpdate = 0;

	// allocate the display strings if needed
	if( m_displayStrings[0] == nullptr )
	{
		GameFont *font;
		if (TheGlobalLanguageData && TheGlobalLanguageData->m_nativeDebugDisplay.name.isNotEmpty())
		{
			font=TheFontLibrary->getFont(
				TheGlobalLanguageData->m_nativeDebugDisplay.name,
				TheGlobalLanguageData->m_nativeDebugDisplay.size,
				TheGlobalLanguageData->m_nativeDebugDisplay.bold);
		}
		else
			font = TheFontLibrary->getFont( "FixedSys", 8, FALSE );

		for (int i = 0; i < DisplayStringCount; i++)
		{
			if (m_displayStrings[i] == nullptr)
			{
				m_displayStrings[i] = TheDisplayStringManager->newDisplayString();
				DEBUG_ASSERTCRASH( m_displayStrings[i], ("Failed to create DisplayString") );
				m_displayStrings[i]->setFont( font );
			}
		}

	}

	if (m_benchmarkDisplayString == nullptr)
	{
		GameFont *thisFont = TheFontLibrary->getFont( "FixedSys", 8, FALSE );
		m_benchmarkDisplayString = TheDisplayStringManager->newDisplayString();
		DEBUG_ASSERTCRASH( m_benchmarkDisplayString, ("Failed to create DisplayString") );
		m_benchmarkDisplayString->setFont( thisFont );
	}

	++s_framesRenderedSinceLastUpdate;
  s_drawCallsSinceLastUpdate += Debug_Statistics::Get_Draw_Calls();
	s_sortedPolysSinceLastUpdate += Debug_Statistics::Get_Sorting_Polygons();

	Int64 freq64 = getPerformanceCounterFrequency();
	Int64 time64 = getPerformanceCounter();

	s_timeSinceLastUpdateInSecs = ((double)(time64 - s_lastUpdateTime64) / (double)(freq64));

#ifdef EXTENDED_STATS
		static FILE *pListFile = nullptr;
		static Int64 lastFrameTime=0;
		static samples = 0;
		if (pListFile == nullptr) {
			pListFile = fopen("FrameRateLog.txt", "w");
		}
		samples++;
		if (pListFile && lastFrameTime && samples<100) {
			float timeSinceLastFrame = (float)((double)(time64-lastFrameTime) / (double)(freq64));
			fprintf(pListFile, "%d ", (int)(1/timeSinceLastFrame));
		}
		lastFrameTime = time64;
#endif

	// we update stats on a delay
	const Real UPDATE_RATE_SECS = 2.0;
	if( s_timeSinceLastUpdateInSecs >= UPDATE_RATE_SECS || TheGlobalData->m_constantDebugUpdate )
	{
		UnicodeString unibuffer, unibuffer2;
		UnicodeString fpsString;

		// setup texture stats
		Debug_Statistics::Record_Texture_Mode(Debug_Statistics::RECORD_TEXTURE_SIMPLE/*RECORD_TEXTURE_NONE*/);

		// frames per second
		double fps = (Real)s_framesRenderedSinceLastUpdate / s_timeSinceLastUpdateInSecs;
		double drawsPerFrame = Debug_Statistics::Get_Draw_Calls(); //(Real)s_drawCallsSinceLastUpdate / (Real)s_framesRenderedSinceLastUpdate;
		double sortPolysPerFrame = Debug_Statistics::Get_Sorting_Polygons();  //(Real)s_sortedPolysSinceLastUpdate / (Real)s_framesRenderedSinceLastUpdate;
		double skinDrawsPerFrame = Debug_Statistics::Get_DX8_Skin_Renders();

		if (fps<0.1) fps = 0.1;

		double ms = 1000.0f/fps;


#if defined(RTS_DEBUG)
		double cumuTime = ((double)(time64 - m_timerAtCumuFPSStart) / (double)(freq64));
		if (cumuTime < 0.0) cumuTime = 0.0;
		Int numFrames = (Int)TheGameLogic->getFrame() - (Int)START_CUMU_FRAME;
		double cumuFPS = (numFrames > 0 && cumuTime > 0.0) ? (numFrames / cumuTime) : 0.0;
		double skinPolysPerFrame = Debug_Statistics::Get_DX8_Skin_Polygons();

		Int LOD = TheGlobalData->m_terrainLOD;
		//unibuffer.format( L"FPS: %.2f, %.2fms mapLOD=%d [cumu FPS=%.2f] draws: %.2f sort: %.2f", fps, ms, LOD, cumuFPS, drawsPerFrame,sortPolysPerFrame);
		if (TheGlobalData->m_useFpsLimit)
				unibuffer.format( L"%.2f/%d FPS, ", fps, TheFramePacer->getFramesPerSecondLimit());
		else
				unibuffer.format( L"%.2f FPS, ", fps);

		unibuffer2.format( L"%.2fms [cumuFPS=%.2f] draws: %d skins: %d sortP: %d skinP: %d LOD %d", ms, cumuFPS, (Int)drawsPerFrame,(Int)skinDrawsPerFrame,(Int)sortPolysPerFrame, (Int)skinPolysPerFrame, LOD);
		unibuffer.concat(unibuffer2);
#else
		//Int LOD = TheGlobalData->m_terrainLOD;
		//unibuffer.format( L"FPS: %.2f, %.2fms mapLOD=%d draws: %.2f sort %.2f", fps, ms, LOD, drawsPerFrame,sortPolysPerFrame);
		unibuffer.format( L"FPS: %.2f, %.2fms draws: %.2f skins: %.2f sort %.2f", fps, ms, drawsPerFrame,skinDrawsPerFrame,sortPolysPerFrame);
		if (TheGlobalData->m_useFpsLimit)
		{
			unibuffer2.format(L", FPSLock %d",TheGlobalData->m_framesPerSecondLimit);
			unibuffer.concat(unibuffer2);
		}
#endif

		if (TheNetwork != nullptr) {
			fpsString.format( L"FPS: %.2f (Buf: %d)", fps, TheNetwork->getBufferedFramesAvailable() );
		} else {
			fpsString.format( L"FPS: %.2f", fps);
		}
		m_benchmarkDisplayString->setText( fpsString );

		Int polyPerFrame = Debug_Statistics::Get_DX8_Polygons();

#ifdef EXTENDED_STATS
		static float gameOverheadMS = 0.0f;
		static float consoleMS = 0.0f;
		static float threeDOverheadMS = 0.0f;
		static float terrainMS = 0.0f;
		static float objectMS = 0.0f;
		static float overlapMS = 0.0f;
		static int  extendedStats = 0;
		const int SHOW_STATS_TIME=12; // show extended stats for 5 cycles == 10 seconds.
		static enum {disabled, sync, gameOverhead, console, threeDOverhead, terrain, objects, overlap, normal} statMode = disabled;

		if (statMode == sync) {
			extendedStats = SHOW_STATS_TIME;
			statMode = gameOverhead;
		} else if (statMode == gameOverhead) {
			gameOverheadMS = ms;
			statMode = console;
			DX8Wrapper::stats.m_disableTerrain = true;
			DX8Wrapper::stats.m_disableOverhead = true;
			DX8Wrapper::stats.m_disableWater = true;
			DX8Wrapper::stats.m_disableObjects = true;
			DX8Wrapper::stats.m_disableConsole = false;
			DX8Wrapper::stats.m_debugLinesToShow = 1;
		} else if (statMode == console) {
			consoleMS = ms;
			statMode = threeDOverhead;
			DX8Wrapper::stats.m_disableTerrain = true;
			DX8Wrapper::stats.m_disableOverhead = true;
			DX8Wrapper::stats.m_disableWater = true;
			DX8Wrapper::stats.m_disableObjects = true;
			DX8Wrapper::stats.m_disableConsole = true;
			DX8Wrapper::stats.m_debugLinesToShow = 1;
		} else if (statMode == threeDOverhead) {
			threeDOverheadMS = ms;
			statMode = terrain;
			DX8Wrapper::stats.m_disableTerrain = false;
			DX8Wrapper::stats.m_disableOverhead = true;
			DX8Wrapper::stats.m_disableWater = true;
			DX8Wrapper::stats.m_disableObjects = true;
			DX8Wrapper::stats.m_disableConsole = true;
			DX8Wrapper::stats.m_debugLinesToShow = 1;
		} else if (statMode == terrain) {
			terrainMS = ms;
			statMode = objects;
			DX8Wrapper::stats.m_disableOverhead = true;
			DX8Wrapper::stats.m_disableTerrain = true;
			DX8Wrapper::stats.m_disableWater = true;
			DX8Wrapper::stats.m_disableObjects = false;
			DX8Wrapper::stats.m_disableConsole = true;
			DX8Wrapper::stats.m_debugLinesToShow = 1;
		} else if (statMode == objects) {
			objectMS = ms;
			statMode = overlap;
			DX8Wrapper::stats.m_disableOverhead = false;
			DX8Wrapper::stats.m_disableTerrain = false;
			DX8Wrapper::stats.m_disableWater = false;
			DX8Wrapper::stats.m_disableObjects = false;
			DX8Wrapper::stats.m_disableConsole = true;
			DX8Wrapper::stats.m_sleepTime = (int)(terrainMS);
			DX8Wrapper::stats.m_debugLinesToShow = 1;
		} else if (statMode == overlap) {
			overlapMS = ms;
			statMode = normal;
			DX8Wrapper::stats.m_disableOverhead = false;
			DX8Wrapper::stats.m_disableTerrain = false;
			DX8Wrapper::stats.m_disableWater = false;
			DX8Wrapper::stats.m_disableObjects = false;
			DX8Wrapper::stats.m_disableConsole = true;
			DX8Wrapper::stats.m_sleepTime = 0;
			DX8Wrapper::stats.m_debugLinesToShow = 1;
		} else if (statMode == normal) {
			overlapMS = (ms + ((int)terrainMS) - overlapMS );
			statMode = disabled;
			extendedStats = SHOW_STATS_TIME;

			// Done collecting stats. Re-enable stuff
			DX8Wrapper::stats.m_disableConsole = false;
			DX8Wrapper::stats.m_debugLinesToShow = -1;
		} else if (!DX8Wrapper::stats.m_showingStats) {
			// start collecting extended info.
			DX8Wrapper::stats.m_showingStats = true;
			DX8Wrapper::stats.m_disableOverhead = false;
			DX8Wrapper::stats.m_disableTerrain = true;
			DX8Wrapper::stats.m_disableWater = true;
			DX8Wrapper::stats.m_disableObjects = true;
			DX8Wrapper::stats.m_disableConsole = true;
			DX8Wrapper::stats.m_debugLinesToShow = 1;
			statMode = sync;
			gameOverheadMS = 0.0f;
			threeDOverheadMS = 0.0f;
			terrainMS = 0.0f;
			objectMS = 0.0f;
		}
		if (statMode != disabled) {
			unibuffer.format(L"FPS: %.2f, %.2fms - Collecting extended stats.", fps, ms);
		} else if (extendedStats>0) {
			extendedStats--;
			unibuffer.format( L"FPS: %.2f, %.2fms - OH %.2fms, Console %.2fms, 3D OH %.2fms, Terrain %.2fms, Obs %.2fms, CPU %.2fms",
				fps, ms, gameOverheadMS, consoleMS, threeDOverheadMS, terrainMS, objectMS, overlapMS);
			if (extendedStats==SHOW_STATS_TIME-2) {
				char bufferA[ 256 ];
				sprintf( bufferA, "FPS: %.2f, %.2fms - OH %.2fms, Console %.2fms, 3D OH %.2fms, Terrain %.2fms, Obs %.2fms, CPU %.2fms\n",
					fps, ms, gameOverheadMS, consoleMS, threeDOverheadMS, terrainMS, objectMS, overlapMS);
				::OutputDebugString(bufferA);
				if (pListFile) {
					fprintf(pListFile, "\n%s", bufferA);
				}
				sprintf( bufferA, "Polygons: per frame %d, per second %d\n", polyPerFrame,
						(Int)(polyPerFrame*fps));
				::OutputDebugString(bufferA);
				if (pListFile) {
					fprintf(pListFile, "%s", bufferA);
					fflush(pListFile);
				}
			}
		}
 		if (pListFile) {
			fprintf(pListFile, "\nFPS: %.2f, %.2fms\n", fps, ms);
			fflush(pListFile);
		}
		if (pListFile) {
			samples = 0;
			if (statMode != disabled) {
				fprintf(pListFile, "Stat%d-", statMode);
			}
		}

#endif
		// check for debug D3D
		Bool debugD3D=false;
		RegistryClass registry ("Software\\Microsoft\\Direct3d");
		if (registry.Is_Valid ()) {
			if (registry.Get_Int ("LoadDebugRuntime", 0) == 1) {
				debugD3D = true;
			}
		}
		if (debugD3D) {
			unibuffer.concat(L", DEBUG D3D");
		}
#ifdef RTS_DEBUG
		unibuffer.concat(L", DEBUG app");
#endif

		m_displayStrings[FPS]->setText( unibuffer );

		// Actual GameLogic frame number
		unibuffer.format(L"Frame: %d", TheGameLogic->getFrame());
		m_displayStrings[Frame]->setText( unibuffer );

		// polygons this frame
		unibuffer.format( L"Polygons: per frame %d, per second %d", polyPerFrame,
				(Int)(polyPerFrame*fps));
		m_displayStrings[Polygons]->setText( unibuffer );

		// vertices this frame
		unibuffer.format( L"Vertices: %d", Debug_Statistics::Get_DX8_Vertices() );
		m_displayStrings[Vertices]->setText( unibuffer );

		//
		// I'm adjusting the texture memory usage counter by subtracting
		// out the terrain alpha texture (since it's really == terrain texture).
		//
		unibuffer.format( L"Video RAM: %d", Debug_Statistics::Get_Record_Texture_Size() - 1376256 );
		m_displayStrings[VideoRam]->setText( unibuffer );

		s_lastUpdateTime64 = time64;
		s_timeSinceLastUpdateInSecs = 0.0f;
		s_framesRenderedSinceLastUpdate = 0;
		s_drawCallsSinceLastUpdate = 0;
		s_sortedPolysSinceLastUpdate = 0;

		// terrain stats
		unibuffer.format( L"3-Way Blends: %d/%d, Shoreline Blends: %d/%d", TheTerrainRenderObject->getNumExtraBlendTiles(TRUE),
			TheTerrainRenderObject->getNumExtraBlendTiles(FALSE),
			TheTerrainRenderObject->getNumShoreLineTiles(TRUE),
			TheTerrainRenderObject->getNumShoreLineTiles(FALSE));
		m_displayStrings[TerrainStats]->setText( unibuffer );

		// misc debug info
		Coord3D camPos = TheTacticalView->getPosition();
		Real zoom = TheTacticalView->getZoom();
		Real pitch = TheTacticalView->getPitch();
		Real FXPitch = TheTacticalView->getFXPitch();
		Real angle = TheTacticalView->getAngle();
		Real FOV = TheTacticalView->getFieldOfView();
		Real terrainHeight = TheTacticalView->getTerrainHeightAtPivot();
		Real actualHeightAboveGround = TheTacticalView->getCurrentHeightAboveGround();

		unibuffer.format(
			L"Camera zoom: %.3f, pitch: %.2f, FXpitch: %.2f, yaw: %.2f, pos: (%.2f, %.2f, %.2f), FOV: %.2f\n"
			L"Height above ground: %.2f, Terrain height at camera pivot: %.2f",
			zoom,
			RAD_TO_DEGF(pitch),
			RAD_TO_DEGF(FXPitch),
			RAD_TO_DEGF(angle),
			camPos.x, camPos.y, camPos.z,
			RAD_TO_DEGF(FOV),
			actualHeightAboveGround, terrainHeight );

		m_displayStrings[DebugInfo]->setText( unibuffer );

		// display the keyboard modifier and mouse states.
		unibuffer.format( L"States: " );
		if( TheKeyboard->isShift() )
		{
			unibuffer.concat( L"Shift(" );
			if( TheKeyboard->getModifierFlags() & KEY_STATE_LSHIFT )
			{
				unibuffer.concat( L"L" );
			}
			if( TheKeyboard->getModifierFlags() & KEY_STATE_RSHIFT )
			{
				unibuffer.concat( L"R" );
			}
			unibuffer.concat( L") " );
		}
		if( TheKeyboard->isCtrl() )
		{
			unibuffer.concat( L"Ctrl(" );
			if( TheKeyboard->getModifierFlags() & KEY_STATE_LCONTROL )
			{
				unibuffer.concat( L"L" );
			}
			if( TheKeyboard->getModifierFlags() & KEY_STATE_RCONTROL )
			{
				unibuffer.concat( L"R" );
			}
			unibuffer.concat( L") " );
		}
		if( TheKeyboard->isAlt() )
		{
			unibuffer.concat( L"Alt(" );
			if( TheKeyboard->getModifierFlags() & KEY_STATE_LALT )
			{
				unibuffer.concat( L"L" );
			}
			if( TheKeyboard->getModifierFlags() & KEY_STATE_RALT )
			{
				unibuffer.concat( L"R" );
			}
			unibuffer.concat( L") " );
		}

		const MouseIO *mouseStatus = TheMouse->getMouseStatus();

		if( mouseStatus->leftState )
		{
			unibuffer.concat( L"LMB " );
		}
		if( mouseStatus->middleState )
		{
			unibuffer.concat( L"MMB " );
		}
		if( mouseStatus->rightState )
		{
			unibuffer.concat( L"RMB " );
		}

		Object *object = nullptr;
#if defined(RTS_DEBUG)	//debug hack to view object under mouse stats
		Drawable *draw = 	TheTacticalView->pickDrawable(&TheMousePos, FALSE, (PickType)0xffffffff );
#else
		Drawable *draw = TheGameClient->findDrawableByID( TheInGameUI->getMousedOverDrawableID() );
#endif
		if( draw  )
			object = draw->getObject();
		if( object )
		{
			unibuffer2.format( L"Moused over object: %S (%d) ", object->getTemplate()->getName().str(), object->getID() );
			unibuffer.concat( unibuffer2 );
		}
		else
		{
			unibuffer.concat( L"Moused over object: TERRAIN " );
		}

		m_displayStrings[ KEY_MOUSE_STATES ]->setText( unibuffer );

		//display the x and y mouse coordinates
		const MouseIO *mouseIO = TheMouse->getMouseStatus();
		Coord3D worldPos;
		if( TheTacticalView->screenToTerrain(&mouseIO->pos, &worldPos) )
		{
			unibuffer.format( L"Mouse position: screen: (%d, %d), world: (%g, %g, %g)",
				mouseIO->pos.x, mouseIO->pos.y, worldPos.x, worldPos.y, worldPos.z);
		}
		else
		{
			unibuffer.format( L"Mouse position: screen: (%d, %d), world: none",
				mouseIO->pos.x, mouseIO->pos.y);
		}
		m_displayStrings[MousePosition]->setText( unibuffer );

		//display the number of particles in the world and being displayed on screen
		Int totalParticles = TheParticleSystemManager->getParticleCount();
		Int onScreenParticleCount = TheParticleSystemManager->getOnScreenParticleCount();
		unibuffer.format( L"Particles: %d in world, %d being displayed", totalParticles, onScreenParticleCount );
		m_displayStrings[Particles]->setText( unibuffer );

		//display the number of objects in the world
		UnsignedInt objCount = TheGameLogic->getObjectCount();
		UnsignedInt objScreenCount = TheGameClient->getRenderedObjectCount();

		unibuffer.format(L"Objects: %d in world, %d being displayed", objCount, objScreenCount );
		m_displayStrings[Objects]->setText( unibuffer );

		// Network incoming bandwidth stats
		if (TheNetwork != nullptr) {
			unibuffer.format(L"IN: %.2f bytes/sec, %.2f packets/sec",
				TheNetwork->getIncomingBytesPerSecond(), TheNetwork->getIncomingPacketsPerSecond());
			m_displayStrings[NetIncoming]->setText( unibuffer );

			// Network outgoing bandwidth stats
			unibuffer.format(L"OUT: %.2f bytes/sec, %.2f packets/sec",
				TheNetwork->getOutgoingBytesPerSecond(), TheNetwork->getOutgoingPacketsPerSecond());
			m_displayStrings[NetOutgoing]->setText( unibuffer );

			// Network performance stats
			unibuffer.format(L"Run Ahead: %d, Net FPS: %d, Packet arrival cushion: %d",
				TheNetwork->getRunAhead(), TheNetwork->getFrameRate(), TheNetwork->getPacketArrivalCushion());
			m_displayStrings[NetStats]->setText( unibuffer );

			// Client frame rate averages for all players in the game.  This only works right for the packet router.
			unibuffer.clear();
			Int numPlayers = TheNetwork->getNumPlayers();
			for (Int i = 0; i < numPlayers; ++i) {
				UnicodeString tempstr;
				tempstr.format(L"%s: %d ", TheNetwork->getPlayerName(i).str(), TheNetwork->getSlotAverageFPS(i));
				unibuffer.concat(tempstr);
			}
			m_displayStrings[NetFPSAverages]->setText( unibuffer );
		} else {
//			unibuffer.format(L"IN: 0.0 bytes/sec, 0.0 packets/sec");
//			m_displayStrings[NetIncoming]->setText( unibuffer );

			// Network outgoing bandwidth stats
//			unibuffer.format(L"OUT: 0.0 bytes/sec, 0.0 packets/sec");
//			m_displayStrings[NetOutgoing]->setText( unibuffer );
			unibuffer.clear();
//			unibuffer.format(L"Network not present");
			m_displayStrings[NetOutgoing]->setText(unibuffer);
			m_displayStrings[NetIncoming]->setText(unibuffer);
			m_displayStrings[NetStats]->setText(unibuffer);
			m_displayStrings[NetFPSAverages]->setText( unibuffer );
		}

		// selected object info stats
		unibuffer.format( L"Select Info: '%d' drawables selected", TheInGameUI->getSelectCount() );



		//Sorry, guys. I need a special kluge here to get constantdebug results for angry mob.
		//Do no be cross with me.
		//if there is not exactly one drawable selected it will report on the moused-over drawable
		if (TheInGameUI->getSelectCount() == 1)
			draw = TheInGameUI->getFirstSelectedDrawable();


		if( draw )
		{
			Object *obj = draw->getObject();
			AsciiString objectName;

			objectName.set( "No-Name" );
			if( obj && obj->getName().isEmpty() == FALSE )
				objectName = obj->getName();

			unibuffer.format( L"Select Info: '%S'(%S) at (%.3f,%.3f,%.3f)",
												draw->getTemplate()->getName().str(),
												objectName.str(),
												draw->getPosition()->x,
												draw->getPosition()->y,
												draw->getPosition()->z
											);

			const PhysicsBehavior *physics = obj->getPhysics();
			PhysicsTurningType turnType = physics ? physics->getTurning() : TURN_NONE;

			const DrawableLocoInfo *locoInfo = draw->getLocoInfo();
			if( locoInfo )
			{
				unibuffer2.format( L"\nPhysics Info -- Turn: %d, Pitch(accel): %.3f(%.3f), Roll(accel): %.3f(%.3f)",
													 turnType,
													 locoInfo->m_accelerationPitch, locoInfo->m_accelerationPitchRate,
													 locoInfo->m_accelerationRoll, locoInfo->m_accelerationRollRate );
				unibuffer.concat( unibuffer2 );
			}






			// (gth) compute some stats about the rendering cost of this drawable
#if defined(RTS_DEBUG)
			RenderCost rcost;
			for (DrawModule** dm = draw->getDrawModules(); *dm; ++dm)
			{
				(*dm)->getRenderCost(rcost);
			}
			if (rcost.getDrawCallCount() > 0)
			{
				unibuffer2.format( L"\ndraw calls: %d(+%d) sort meshes: %d skins: %d  bones: %d",rcost.getDrawCallCount(),rcost.getShadowDrawCount(),rcost.getSortedMeshCount(),rcost.getSkinMeshCount(),rcost.getBoneCount());
				unibuffer.concat( unibuffer2 );
			}
#endif

			unibuffer.concat( L"\nModelStates: " );
			ModelConditionFlags mcFlags = draw->getModelConditionFlags();
			const int numEntriesPerLine = 4;
			int lineCount = 0;

			for( int i = 0; i < MODELCONDITION_COUNT; i++ )
			{
				if( mcFlags.test( i ) )
				{
					unibuffer2.format( L"%S ", ModelConditionFlags::getBitNames()[ i ] );
					unibuffer.concat( unibuffer2 );
					lineCount++;
					if( lineCount == numEntriesPerLine )
					{
						lineCount = 0;
						unibuffer.concat( L"\n" );
					}
				}
			}

			//Render ALL modelcondition statii

		}
		m_displayStrings[ SelectedInfo ]->setText( unibuffer );

	}

}

// W3DDisplay::drawDebugStats =================================================
/** Draw debug statistics */
//=============================================================================
void W3DDisplay::drawDebugStats()
{
	Int	x = 3;
	Int	y = 30;
	Color textColor = GameMakeColor( 255, 255, 255, 255 );
	Color dropColor = GameMakeColor( 0, 0, 0, 255 );

	int linesOfStrings = DisplayStringCount;
#ifdef EXTENDED_STATS
	if (DX8Wrapper::stats.m_debugLinesToShow > -1)
	{
		linesOfStrings = DX8Wrapper::stats.m_debugLinesToShow;
	}

#endif


	Int w, h;
	for (int i = 0; i < linesOfStrings; i++)
	{
		m_displayStrings[i]->draw( x, y, textColor, dropColor );
		m_displayStrings[i]->getSize(&w, &h);
		y += h;
	}

}

// W3DDisplay::drawFPSStats =================================================
/** Draw the FPS on the screen */
//=============================================================================
void W3DDisplay::drawFPSStats()
{
	Int	x = 3;
	Int	y = 20;
	Color textColor = GameMakeColor( 255, 255, 255, 255 );
	Color dropColor = GameMakeColor( 0, 0, 0, 255 );

	int linesOfStrings = 1;

	for (int i = 0; i < linesOfStrings; i++)
	{
		m_benchmarkDisplayString->draw( x, y, textColor, dropColor );
	}
}


//=============================================================================
void StatDebugDisplay( DebugDisplayInterface *, void *, FILE *fp )
{
	DEBUG_CRASH(("This should never be called directly, but is just a placeholder for drawDebugStats()"));
}

// W3DDisplay::drawCurrentDebugDisplay =================================================
/** Draw current debug display */
//=============================================================================
void W3DDisplay::drawCurrentDebugDisplay()
{
	if (m_debugDisplayCallback == StatDebugDisplay)
	{
		drawDebugStats();
	}
	else
	{
		if ( m_debugDisplay && m_debugDisplayCallback )
		{
			m_debugDisplay->reset();
			m_debugDisplayCallback( m_debugDisplay, m_debugDisplayUserData, nullptr );
		}
	}
}

// W3DDisplay::calculateTerrainLOD =================================================
/** Calculates an adequately speedy terrain Level Of Detail. */
//=============================================================================
void W3DDisplay::calculateTerrainLOD()
{
	const Int NUM_SAMPLES=20;
	const Int NUM_TO_DISCARD=5;

	Int64 freq64 = getPerformanceCounterFrequency();

	char buf[_MAX_PATH];
	float frameTime = 0;
	float maxTimeLimit = TheGlobalData->m_terrainLODTargetTimeMS/1000.0f;
	TerrainLOD goodLOD = TERRAIN_LOD_MIN;
	TerrainLOD curLOD = TERRAIN_LOD_AUTOMATIC;
	Int count = 0;
#ifdef RTS_DEBUG
	// just go to TERRAIN_LOD_NO_WATER, mirror off.
	TheWritableGlobalData->m_terrainLOD = TERRAIN_LOD_NO_WATER;
	m_3DScene->drawTerrainOnly(false);
	TheTerrainRenderObject->adjustTerrainLOD(0);
	return;
#endif
	do {
		Int i;
		float timeForFrame=0;
		frameTime = 0;
		switch(curLOD) {
			default: curLOD = TERRAIN_LOD_DISABLE; break;
			case TERRAIN_LOD_AUTOMATIC: curLOD = TERRAIN_LOD_MAX; break;
			case TERRAIN_LOD_MAX: curLOD = TERRAIN_LOD_NO_WATER; break;
			case TERRAIN_LOD_NO_WATER: curLOD = TERRAIN_LOD_DISABLE; break;
		}
		if (curLOD == TERRAIN_LOD_DISABLE) {
			break;
		}
		TheWritableGlobalData->m_terrainLOD = curLOD;
		m_3DScene->drawTerrainOnly(true);
		TheTerrainRenderObject->adjustTerrainLOD(0);
		for (i=0; i<NUM_SAMPLES; i++) {
			Int64 startTime64 = getPerformanceCounter();
			// start render block
			updateViews();
			if (WW3D::Begin_Render( true, true, Vector3( 0.0f, 0.0f, 0.0f ) ) == WW3D_ERROR_OK)
			{
				// draw all views of the world
				drawViews();
				// render is all done!
				WW3D::End_Render();
			}
			Int64 time64 = getPerformanceCounter();
			timeForFrame = (float)((double)(time64-startTime64) / (double)(freq64));
			sprintf(buf, "%.2fms ", timeForFrame*1000.0f);
			::OutputDebugString(buf);
			if (i>=NUM_TO_DISCARD) {
				frameTime += timeForFrame;
				if (i>NUM_TO_DISCARD+1 &&
					(timeForFrame / ((i+1)-NUM_TO_DISCARD)) > 2*maxTimeLimit) {
					i++;
					break;
				}
			}
		}
		frameTime /= ((i)-NUM_TO_DISCARD);
		count++;
		sprintf(buf, "\n LOD %d, time %.2fms\n", curLOD, frameTime*1000.0f);
		::OutputDebugString(buf);
		if (frameTime<maxTimeLimit && goodLOD<curLOD) {
			goodLOD = curLOD;
		}
		if (frameTime < maxTimeLimit) break;
	} while (count<10);

	TheWritableGlobalData->m_terrainLOD = goodLOD;
	m_3DScene->drawTerrainOnly(false);
	TheTerrainRenderObject->adjustTerrainLOD(0);
#ifdef RTS_DEBUG
	DEBUG_ASSERTCRASH(count<10, ("calculateTerrainLOD") );
#endif

}


Real W3DDisplay::getAverageFPS()
{
	return m_averageFPS;
}

Real W3DDisplay::getCurrentFPS()
{
	return m_currentFPS;
}

Int W3DDisplay::getLastFrameDrawCalls()
{
	return Debug_Statistics::Get_Draw_Calls();
}

// GeneralsX @feature Qoder 13/10/2026 PS3.0: screenshot scheduler for headless runs.
// GENERALSX_SHOTS="60,88" lists seconds after the first scheduled frame at which the game itself
// takes a PNG screenshot (via the fixed GetFrontBufferData capture path in W3DScreenshot.cpp), so
// verification harnesses no longer depend on injecting Ctrl+F12 into a foreground window.
static bool W3D_SSHOT_Due()
{
	static std::vector<Int> s_secs;
	static bool s_init = false;
	static unsigned long s_startMs = 0;
	static size_t s_index = 0;
	if (!s_init)
	{
		s_init = true;
		const char* ev = getenv("GENERALSX_SHOTS");
		if (ev && *ev)
		{
			const char* p = ev;
			while (*p)
			{
				while (*p == ',' || *p == ' ' || *p == ';') ++p;
				if (!*p) break;
				char* end = nullptr;
				long v = strtol(p, &end, 10);
				if (end == p || v < 0) break;
				s_secs.push_back((Int)v);
				p = end;
			}
			s_startMs = timeGetTime();
			fprintf(stderr, "[SSHOT] scheduled %zu shots (s):", s_secs.size());
			for (size_t i = 0; i < s_secs.size(); ++i) fprintf(stderr, " %d", s_secs[i]);
			fprintf(stderr, "\n");
			fflush(stderr);
		}
	}
	if (s_secs.empty() || s_index >= s_secs.size()) return false;
	if ((timeGetTime() - s_startMs) / 1000 < (unsigned long)s_secs[s_index]) return false;
	++s_index;
	return true;
}

// W3DDisplay::update =========================================================
/** Update the client state of the W3D Display */
//=============================================================================
void W3DDisplay::update()
{
	Display::update();

	if (TheGlobalData->m_headless)
		return;

	// TheSuperHackers @bugfix The client updates have moved here from W3DDisplay::draw, so that
	// they also advance when the draw is skipped, for example when the window is minimized.

	Bool freezeTime = TheFramePacer->isTimeFrozen() || TheFramePacer->isGameHalted();

	if (!freezeTime && TheScriptEngine->isTimeFast())
	{
		/// @todo: I'm assuming the first view is our main 3D view.
		W3DView *primaryW3DView=(W3DView *)getFirstView();
		primaryW3DView->updateCameraMovements();  // Update camera motion effects.
		return;
	}

	//update state of all the terrain tracks (fade, remove, etc.)
	if (TheGlobalData->m_loadScreenRender != TRUE)
	{
		if (TheTerrainTracksRenderObjClassSystem)
			TheTerrainTracksRenderObjClassSystem->update();
	}

	// GeneralsX @feature PS3.0-4A agent 16/10/2026 explosion-lighting same-process A/B leg.
	// GENERALSX_PULSE_TEST=1 spawns one explosion-sized orange pulse at the camera look-at
	// point every 90 frames. It calls the transport directly, bypassing both the FXList INI
	// data and the GENERALSX_LIGHTPULSE gate, so render-side response can be screenshotted
	// deterministically (ON vs OFF runs). Default OFF, zero effect on normal play.
	{
		static const Bool s_pulseTest = (getenv("GENERALSX_PULSE_TEST") != nullptr) && (atoi(getenv("GENERALSX_PULSE_TEST")) != 0);
		if (s_pulseTest && m_3DScene != nullptr && TheTacticalView != nullptr)
		{
			static UnsignedInt s_pulseTickCounter = 0;
			if (++s_pulseTickCounter >= 90)
			{
				s_pulseTickCounter = 0;
				Coord3D lookAt = TheTacticalView->getPosition();
				lookAt.z += 6.0f;
				RGBColor testColor;
				testColor.red = 1.0f;
				testColor.green = 0.47f;
				testColor.blue = 0.16f;
				applyLightPulse(m_3DScene, &lookAt, &testColor, 1.0f, 40.0f, 0, 45);
				fprintf(stderr, "[LPULSE-TEST] explosion-light test pulse at (%.0f,%.0f,%.0f)\n",
				        lookAt.x, lookAt.y, lookAt.z);
				fflush(stderr);
			}
		}
	}

	WW3D::Update_Logic_Frame_Time(TheFramePacer->getLogicTimeStepMilliseconds());

	// TheSuperHackers @info This binds the WW3D update to the logic update.
	WW3D::Sync(TheGameLogic->hasUpdated());

	// update all views of the world - recomputes data which will affect drawing
	if (DX8Wrapper::_Get_D3D_Device8() && (DX8Wrapper::_Get_D3D_Device8()->TestCooperativeLevel()) == D3D_OK)
	{
		// Checking if we have the device before updating views because the heightmap crashes otherwise while
		// trying to refresh the visible terrain geometry.
		// TheSuperHackers @todo Remove the renderer dependency from the update.
		updateViews();
	}
}

//=============================================================================
void W3DDisplay::step()
{
	stepViews();
}

//DECLARE_PERF_TIMER(BigAssRenderLoop)

// W3DDisplay::draw ===========================================================
/** Draw the entire W3D Display */
//=============================================================================
//DECLARE_PERF_TIMER(W3DDisplay_draw)
// GeneralsX @performance Qoder 19/10/2026 R-05 wave 3: breakdown of the display draw() call: which part
// of the frame's render traversal costs the CPU. Same env switch as the other phase probes
// (GENERALSX_VKFPS_PHASES). Temporary forensic instrumentation, retire with the other probes.
#include <chrono>
#include <cstdlib>
namespace {
	struct VKFph2
	{
		long long prea, terlod, preb, shroud, prert, begin, sunmap, views, receiver, ingameui, mouse, endr, total, frames;
		bool enabled, checked;
		VKFph2() : prea(0), terlod(0), preb(0), shroud(0), prert(0), begin(0), sunmap(0), views(0), receiver(0), ingameui(0),
			mouse(0), endr(0), total(0), frames(0), enabled(false), checked(false) { }
	};
	VKFph2 g_fph2;

	inline long long fph2NowUs()
	{
		using namespace std::chrono;
		return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
	}

	inline bool fph2On()
	{
		if (!g_fph2.checked)
		{
			g_fph2.checked = true;
			g_fph2.enabled = (getenv("GENERALSX_VKFPS_PHASES") != nullptr);
		}
		return g_fph2.enabled;
	}

	struct VKFph2Scope
	{
		long long* out;
		long long t0;
		VKFph2Scope(long long* o) : out(o), t0(o ? fph2NowUs() : 0) { }
		~VKFph2Scope() { if (out) *out += fph2NowUs() - t0; }
	};

	inline void fph2TickAndDump()
	{
		if (!g_fph2.enabled) return;
		if ((++g_fph2.frames % 300) != 0) return;
		FILE* out = fopen("vkfps_phase2.log", "a");
		if (out)
		{
			fprintf(out, "[VKFPH2] frames=300 avg_us prea=%lld terlod=%lld preb=%lld shroud=%lld prert=%lld begin=%lld sunmap=%lld views=%lld "
				"receiver=%lld ingameui=%lld mouse=%lld endr=%lld total=%lld\n",
				(long long)(g_fph2.prea / 300), (long long)(g_fph2.terlod / 300), (long long)(g_fph2.preb / 300),
				(long long)(g_fph2.shroud / 300), (long long)(g_fph2.prert / 300), (long long)(g_fph2.begin / 300),
				(long long)(g_fph2.sunmap / 300), (long long)(g_fph2.views / 300), (long long)(g_fph2.receiver / 300),
				(long long)(g_fph2.ingameui / 300), (long long)(g_fph2.mouse / 300), (long long)(g_fph2.endr / 300),
				(long long)(g_fph2.total / 300));
			fclose(out);
		}
		g_fph2.prea = g_fph2.terlod = g_fph2.preb = 0;
		g_fph2.shroud = g_fph2.prert = g_fph2.begin = g_fph2.sunmap = g_fph2.views = 0;
		g_fph2.receiver = g_fph2.ingameui = g_fph2.mouse = g_fph2.endr = g_fph2.total = 0;
	}
}

void W3DDisplay::draw()
{
	//USE_PERF_TIMER(W3DDisplay_draw)
	const bool vkP = fph2On();
	fph2TickAndDump();
	VKFph2Scope sTotal(vkP ? &g_fph2.total : nullptr);
	long long tPre = vkP ? fph2NowUs() : 0;

	// GeneralsX @feature Qoder 19/10/2026 BUG-03 forensics: a QA skirmish run presented nothing at all - the
	// window stayed black and the shutdown census recorded ZERO BeginScene/Clear calls on the device, while
	// the logic clearly kept running (unit weapon sounds at 8 minutes in). That narrows the fault to this
	// function returning before WW3D::Begin_Render(), but every early return here reads a different flag and
	// the release build compiles the DEBUG_LOG out of the one that matters. Report which gate is holding the
	// door, once every 300 calls, from the same values the returns test.
	{
		static unsigned s_drawProbe = 0;
		if ((++s_drawProbe % 300u) == 1u) {
			extern HWND ApplicationHWnd;
			fprintf(stderr, "[VKWP1] draw #%u iconic=%d headless=%d breakMovie=%d disableRender=%d "
				"loadScreenRender=%d frame=%d inGame=%d\n",
				s_drawProbe,
				(ApplicationHWnd && ::IsIconic(ApplicationHWnd)) ? 1 : 0,
				TheGlobalData->m_headless ? 1 : 0,
				TheGlobalData->m_breakTheMovie ? 1 : 0,
				TheGlobalData->m_disableRender ? 1 : 0,
				TheGlobalData->m_loadScreenRender ? 1 : 0,
				TheGameLogic ? TheGameLogic->getFrame() : -1,
				(TheGameLogic && TheGameLogic->isInGame()) ? 1 : 0);
			fflush(stderr);
		}
	}

	// GeneralsX @feature xxorza 15/04/2026 Process deferred window resize for pillarbox
	DX8Wrapper::Pillarbox_Process_Resize();

	extern HWND ApplicationHWnd;
	if (ApplicationHWnd && ::IsIconic(ApplicationHWnd)) {
		return;
	}

	if (TheGlobalData->m_headless)
		return;

	// TheSuperHackers @feature bobtista 10/07/2026 Show messages for screenshots finished by the screenshot thread.
	W3D_UpdateScreenshotMessages();

	updateAverageFPS();
	if (TheGlobalData->m_enableDynamicLOD && TheGameLogic->getShowDynamicLOD())
	{
		DynamicGameLODLevel lod=TheGameLODManager->findDynamicLODLevel(m_averageFPS);
		TheGameLODManager->setDynamicLODLevel(lod);
	}
	else
	{
		//if dynamic LOD is turned off, force highest LOD
		TheGameLODManager->setDynamicLODLevel(DYNAMIC_GAME_LOD_VERY_HIGH);
	}

	if (TheGlobalData->m_terrainLOD == TERRAIN_LOD_AUTOMATIC && TheTerrainRenderObject)
	{
		if (vkP) { g_fph2.prea += fph2NowUs() - tPre; tPre = fph2NowUs(); }
		calculateTerrainLOD();
		if (vkP) { g_fph2.terlod += fph2NowUs() - tPre; tPre = fph2NowUs(); }
	}
#ifdef EXTENDED_STATS
AGAIN:
#endif

#ifdef DUMP_PERF_STATS
	if( TheGlobalData->m_dumpPerformanceStatistics )
	{
		TheStatDump.dumpStats( FALSE, TRUE );
		TheWritableGlobalData->m_dumpPerformanceStatistics = FALSE;
	}
  //The <= GAME_REPLAY essentially means, GAME_SINGLE_PLAYER || GAME_LAN || GAME_SKIRMISH || GAME_REPLAY
  else if ( TheGlobalData->m_dumpStatsAtInterval && TheGameLogic->getGameMode() <= GAME_REPLAY )
  {
    Int interval = TheGlobalData->m_statsInterval;
    if ( TheGameLogic->getFrame() > 0 && (TheGameLogic->getFrame() % interval) == 0 )
    {
  	  TheStatDump.dumpStats( TRUE, TRUE );
    	TheInGameUI->message( L"-stats is running, at interval: %d.", TheGlobalData->m_statsInterval );
    }
  }
#endif

	// compute debug statistics for display later
	if ( m_debugDisplayCallback == StatDebugDisplay
#if defined(RTS_DEBUG)
				|| TheGlobalData->m_benchmarkTimer > 0
#endif
			)
	{
		gatherDebugStats();
	}
#ifdef EXTENDED_STATS
	else
	{
		DX8Wrapper::stats.m_showingStats = false;
	}
#endif

#ifdef SAMPLE_DYNAMIC_LIGHT
	Vector3 loc;
	loc = theDynamicLight->Get_Position();
	loc.X += theLightXOffset;
	if(loc.X>128) theLightXOffset = -theLightXOffset;
	if(loc.X<0) theLightXOffset = -theLightXOffset;
	loc.Y += theLightYOffset;
	if(loc.Y>128) theLightYOffset = -theLightYOffset;
	if(loc.Y<0) theLightYOffset = -theLightYOffset;
	theDynamicLight->Set_Position(loc);
#endif


	/// @todo Make more explicit drawing layers(ground, ground UI, objects, object UI, overlay UI)

	///@todo: Ask Vegas why the LOD optimizer hangs particle system.
 	//
  	// Predictive LOD optimizer optimizes the mesh LOD levels to match
  	// the given polygon budget
  	//
	//PredictiveLODOptimizerClass::Optimize_LODs( 5000 );

	Bool freezeTime = TheFramePacer->isTimeFrozen() || TheFramePacer->isGameHalted();

	/// @todo: I'm assuming the first view is our main 3D view.
	W3DView *primaryW3DView=(W3DView *)getFirstView();

	if (!freezeTime && TheScriptEngine->isTimeFast())
	{
		return;
	}

	Debug_Statistics::Begin_Statistics();	//reset all counters (polygons, vertices, etc) before drawing

	if (vkP) { g_fph2.preb += fph2NowUs() - tPre; tPre = fph2NowUs(); }
	if(TheGlobalData->m_loadScreenRender != TRUE)
	{
		VKFph2Scope s(vkP ? &g_fph2.shroud : nullptr);
		//Shroud data is needed to render all other views, so handle this first.
		if (TheTerrainRenderObject)
		{
			//update the shroud surface here since it may be needed by reflections
			if (TheTerrainRenderObject->getMap())	//make sure a valid map is loaded into terrain.
			{
				if (TheTerrainRenderObject->getShroud())
				{
					TheTerrainRenderObject->getShroud()->render(primaryW3DView->get3DCamera());
				}
			}
		}
	}

	static Int now;
	now=timeGetTime();

	if (TheTacticalView->getTimeMultiplier()>1)
	{
		static Int timeMultiplierCounter = 1;
		timeMultiplierCounter--;
		if (timeMultiplierCounter>1)
			return;
		timeMultiplierCounter = TheTacticalView->getTimeMultiplier();
		// limit the framerate, because while fast time is on, the game logic is running as fast as it can.
	}

	Bool viewsUpdated = TRUE;

	do {

		{
		VKFph2Scope s(vkP ? &g_fph2.prert : nullptr);
		if (DX8Wrapper::_Get_D3D_Device8() && (DX8Wrapper::_Get_D3D_Device8()->TestCooperativeLevel()) == D3D_OK)
		{
			// TheSuperHackers @info The views are updated in W3DDisplay::update, except in the repeated passes
			// of this loop, which keep moving the camera while the time is frozen for a camera movement.
			if (!viewsUpdated)
				updateViews();

			TheParticleSystemManager->DRAW();

			if (TheWaterRenderObj && TheGlobalData->m_waterType == 2)
				TheWaterRenderObj->updateRenderTargetTextures(primaryW3DView->get3DCamera());	//do a render into each texture

			//Can't render into textures while rendering to screen so these textures need to be updated
			//before we enter main rendering loop.
			// GeneralsX @bugfix 23/10/2026: the pre-frame bake cannot land in its render target on the
			// pure-Vulkan core (VKDraw_Begin_Frame resets to the scene target), and End_Render(false)
			// does not submit. Running it while the native sun shadow is on only burns a nested frame
			// and can leave the scene target cleared for a frame. The retail path is muted by the
			// options row, so skip the dead bake with it.
			if (TheW3DProjectedShadowManager && !DX8Wrapper::VK_Retail_Shadow_Off())
				TheW3DProjectedShadowManager->updateRenderTargetTextures();

			// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 9.G: the sun shadow depth pass moved
			// below, inside the open render frame (just before drawViews). Its facade draws are recorded
			// into the Vulkan command buffer, and that buffer only exists between Begin_Render and
			// End_Render - running it here, before the scene opens, silently refused every one of them.
			// The pass still runs before anything samples the map, so GN reception sees this frame's
			// depths exactly as it did when the pass lived here.
		}

		viewsUpdated = FALSE;
		// Switch to offscreen RT AFTER pre-render (shadows/water) completes, BEFORE main render.
		DX8Wrapper::Pillarbox_Begin();

		Debug_Statistics::End_Statistics();	//record number of polygons rendered in RenderTargetTextures.
		}

		//Store number of polygons rendered in renderTargetTextures.
		Int numRenderTargetPolygons=Debug_Statistics::Get_DX8_Polygons();
		Int numRenderTargetVertices=Debug_Statistics::Get_DX8_Vertices();

		// start render block
		#if defined(_ALLOW_DEBUG_CHEATS_IN_RELEASE)
    if ( (TheGameLogic->getFrame() % 30 == 1) || ( ! ( !TheGameLogic->isGamePaused() && TheGlobalData->m_TiVOFastMode) ) )
		#else
	    if ( (TheGameLogic->getFrame() % 30 == 1) || ( ! (!TheGameLogic->isGamePaused() && TheGlobalData->m_TiVOFastMode && TheGameLogic->isInReplayGame())) )
    #endif
		{
			//USE_PERF_TIMER(BigAssRenderLoop)
			static Bool couldRender = true;
			Bool vkBeginOK = FALSE;
			// FrameGraph owns the frame prologue: allocate sceneColor/sceneDepth/overlay before
			// VKDraw_Begin_Frame opens the backend frame, then only declare/use them below.
			W3DRenderFrameScope rgFrame;
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
			W3DRenderGraph::SetBarrierHandler([](const char* resource, int layout) {
				VKDraw_Graph_Barrier(resource, layout);
			});
			W3DRenderGraph::SetTargetHandlers(
				[](unsigned* sceneW, unsigned* sceneH, unsigned* colorFormat, unsigned* depthFormat,
					unsigned* overlayW, unsigned* overlayH, unsigned* overlayFormat, int* overlayWanted) {
					return VKDraw_Graph_Target_Desc(sceneW, sceneH, colorFormat, depthFormat,
						overlayW, overlayH, overlayFormat, overlayWanted);
				},
				[](const char* name, unsigned width, unsigned height, unsigned format, bool depth) {
					return VKDraw_Graph_Alloc_Target(name, width, height, format, depth);
				},
				[](const char* name) { VKDraw_Graph_Free_Target(name); },
				[](void* key) { VKDraw_Graph_Free_Target_Key(key); });
			// 6B: P6 bloom chain and the adopted water-reflection image join the same target lifecycle.
			W3DRenderGraph::SetPostTargetHandler(
				[](unsigned* count, unsigned* width, unsigned* height, unsigned* format) {
					return VKDraw_Graph_Post_Target_Desc(count, width, height, format);
				});
			W3DRenderGraph::SetWaterTargetHandler(
				[](void** key, unsigned* width, unsigned* height, unsigned* format) {
					return VKDraw_Graph_Water_Target_Desc(key, width, height, format);
				});
			// Phase 6B: the graph owns the target lifetime. It asks the backend for this frame's exact
			// scene/overlay shape (including render scale and HDR), allocates them, and releases the
			// transient overlay once the P6 resolve stops being wanted.
			// A device loss/recreate invalidates every key the graph remembers; forgetting the records
			// (without touching the dead backend) is what makes the next PrepareTargets reallocate.
			if (!VKDraw_Ready()) W3DRenderGraph::Get().Reset();
			W3DRenderGraph::Get().PrepareTargets();
#endif
			if (TheGlobalData->m_breakTheMovie == FALSE && TheGlobalData->m_disableRender == false)
			{
				VKFph2Scope s(vkP ? &g_fph2.begin : nullptr);
				vkBeginOK = (WW3D::Begin_Render( true, true, Vector3( 0.0f, 0.0f, 0.0f ), TheWaterTransparency->m_minWaterOpacity ) == WW3D_ERROR_OK);
			}
			if (vkBeginOK)
			{
				if(TheGlobalData->m_loadScreenRender == TRUE)
				{
					TheInGameUI->draw();
					if( TheMouse )
						TheMouse->draw();	//keep applying the current cursor style so it remains hidden if needed.
					WW3D::End_Render();
					continue;
				}
				couldRender = true;
				// add the number of verts/polygons drawn before the main scene
				if (numRenderTargetPolygons || numRenderTargetVertices)
					Debug_Statistics::Record_DX8_Polys_And_Vertices(numRenderTargetPolygons,numRenderTargetVertices,ShaderClass::_PresetOpaqueShader);

				// draw all views of the world
				{
					W3DRenderPassScope rgPass("world");
					W3DRenderGraph::Get().Require("sceneColor", W3DRenderGraph::LAYOUT_COLOR_ATTACHMENT, W3DRenderGraph::RES_WRITE);
					W3DRenderGraph::Get().Require("sceneDepth", W3DRenderGraph::LAYOUT_DEPTH_ATTACHMENT, W3DRenderGraph::RES_WRITE);
					VKFph2Scope s(vkP ? &g_fph2.views : nullptr);
					drawViews();
				}

				// GeneralsX @feature Qoder 20/10/2026 native sun shadow (ported from ModernGeneralsZH):
				// drawViews recorded every opaque world-space batch; this replays them from the sun into a
				// depth map and darkens the scene where the depth test says a caster blocks the light.
				// It runs inside the open scene, before the UI, so the shadow only covers the world.
				{
					W3DRenderPassScope rgPass("shadow");
					W3DRenderGraph::Get().Require("sceneDepth", W3DRenderGraph::LAYOUT_SHADER_READ, W3DRenderGraph::RES_READ);
					W3DRenderGraph::Get().Require("shadowMap", W3DRenderGraph::LAYOUT_SHADER_READ, W3DRenderGraph::RES_WRITE);
					VKFph2Scope s(vkP ? &g_fph2.receiver : nullptr);
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
					if (TheW3DShadowManager)
					{
						// GeneralsX @refactor 23/10/2026: the shadow pass now follows the same sun
						// direction the lighting snapshot publishes. The old manager point was a
						// separate source and could disagree with terrain/GN/grass after a TOD edit.
						const W3DRenderLightingState &lighting = W3D_GetRenderLighting();
						float sunDir[3];
						if (lighting.revision > 0u) {
							sunDir[0] = lighting.sunDir[0];
							sunDir[1] = lighting.sunDir[1];
							sunDir[2] = lighting.sunDir[2];
						} else {
							const Vector3 &sunPos = TheW3DShadowManager->getLightPosWorld(0);
							sunDir[0] = sunPos.X;
							sunDir[1] = sunPos.Y;
							sunDir[2] = sunPos.Z;
						}
						VKDraw_Shadow_Apply(sunDir);
					}
#endif
				}

				// GeneralsX @feature 23/10/2026 P6 "底片/镜头": resolve the 3D scene (tonemap/AA/SSAA)
				// into the LDR overlay before the HUD. No-op unless HDR / RenderScale / PostAA is on.
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
				{
					W3DRenderPassScope rgPass("overlay");
					W3DRenderGraph::Get().Require("sceneColor", W3DRenderGraph::LAYOUT_SHADER_READ, W3DRenderGraph::RES_READ);
					W3DRenderGraph::Get().Require("overlay", W3DRenderGraph::LAYOUT_COLOR_ATTACHMENT, W3DRenderGraph::RES_WRITE);
					VKDraw_Begin_UI();
				}
#endif
				// draw the user interface
				{
					W3DRenderPassScope rgPass("ui");
					W3DRenderGraph::Get().Require("overlay", W3DRenderGraph::LAYOUT_COLOR_ATTACHMENT,
						W3DRenderGraph::RES_READ | W3DRenderGraph::RES_WRITE);
					VKFph2Scope s(vkP ? &g_fph2.ingameui : nullptr);
					TheInGameUI->DRAW();
				}

				// end of video example code

				// draw the mouse
				if( TheMouse )
				{
					W3DRenderPassScope rgPass("mouse");
					VKFph2Scope s(vkP ? &g_fph2.mouse : nullptr);
					TheMouse->DRAW();
				}

				if ( m_videoStream && m_videoBuffer )
				{
					// TheSuperHackers @bugfix Mauller 20/07/2025 scale videos based on screen size so they are shown in their original aspect
					drawScaledVideoBuffer( m_videoBuffer, m_videoStream );
				}

				// render letter box before debug display so debug info isn't hidden
				renderLetterBox(now);

				// display cinematicText over the black
				if( m_cinematicText != AsciiString::TheEmptyString && m_cinematicTextFrames != 0)
				{
					DisplayString *displayString = TheDisplayStringManager->newDisplayString();

					// set word wrap if necessary

					Int wordWrapWidth = TheDisplay->getWidth() - 20;
					displayString->setWordWrap( wordWrapWidth );
					displayString->setWordWrapCentered( TRUE );

					UnicodeString text;
					text.translate( m_cinematicText );
					displayString->setText( text );
					Color color = GameMakeColor( 255, 255, 255, 255 );  // white
					Color backColor = GameMakeColor( 0, 0, 0, 0 );      // black
					displayString->setFont( m_cinematicFont );
					Int height = TheDisplay->getHeight() * .9;

					Int width;
					if( displayString->getWidth() > TheDisplay->getWidth() )
						width = 20;
					else
						width = ( TheDisplay->getWidth() - displayString->getWidth() ) / 2;
					displayString->draw( width, height, color, backColor );

					m_cinematicTextFrames--;
				}

				if ( m_debugDisplayCallback )
				{
					// draw the current debug display
					drawCurrentDebugDisplay();
				}

#if defined(RTS_DEBUG)
				if (TheGlobalData->m_benchmarkTimer > 0)
				{
					drawFPSStats();
				}
#endif


#if defined(RTS_DEBUG)
				if (TheGlobalData->m_debugShowGraphicalFramerate)
				{
					drawFramerateBar();
				}
#endif

#ifdef PERF_TIMERS
				TheGraphDraw->render();
				TheGraphDraw->clear();
#endif

#ifdef PROFILER_ENABLED
				if (m_profilerFrameCapture && !TheGlobalData->m_headless)
				{
					m_profilerFrameCapture->Capture(getWidth(), getHeight());
				}
#endif

				// GeneralsX @feature Qoder 13/10/2026 PS3.0: scheduled GENERALSX_SHOTS capture,
				// whole frame drawn and present pending (the historical screenshot spot).
				if (W3D_SSHOT_Due())
				{
					fprintf(stderr, "[SSHOT] firing scheduled screenshot\n");
					fflush(stderr);
					takeScreenShot(SCREENSHOT_PNG, DEFAULT_JPEG_QUALITY);
				}

				// render is all done!
				{
					VKFph2Scope s(vkP ? &g_fph2.endr : nullptr);
					WW3D::End_Render();
				}
			}
			else
			{
				if (couldRender)
				{
					couldRender = false;
					// GeneralsX @feature Qoder 19/10/2026 BUG-03 forensics: DEBUG_LOG is compiled out of a
					// release build, so this branch - the one that says "the frame was refused" - was silent
					// for the whole black-screen run. Say it where the QA log will show it.
					fprintf(stderr, "[VKWP1] draw refused: Begin_Render failed (TestCooperativeLevel) or movie break / render disable latched\n");
					fflush(stderr);
					DEBUG_LOG(("Could not do WW3D::Begin_Render()!  Are we ALT-Tabbed out?"));
				}
			}
		}

		if (TheScriptEngine->isTimeFrozenDebug() || TheScriptEngine->isTimeFrozenScript() || TheGameLogic->isGamePaused())
		{
			freezeTime = false; // We're frozen for debug or for pause, and need to continue out of the loop.
		}

		// GeneralsX @feature 05/10/2026 The scripted camera movement is now driven by real time in
		// W3DView::updateCameraMovements() during the regular view update. The repeated render passes
		// of the legacy spin loop are no longer needed to advance the camera, and they rendered the
		// cinematic scenes outside of the FramePacer rate control, which caused stutter.
		break;

	} while (freezeTime && !TheTacticalView->isCameraMovementFinished());

#ifdef EXTENDED_STATS
	if (DX8Wrapper::stats.m_disableOverhead) {
		goto AGAIN;
	}
#endif
}

#define LETTER_BOX_FADE_TIME	1000.0f		///1000 ms.

/** Render letter-box border at top/bottom of display
*/
void W3DDisplay::renderLetterBox(UnsignedInt currentTime)
{
		if (m_letterBoxEnabled)
		{	if (m_letterBoxFadeLevel != 1.0f)
			{
				m_letterBoxFadeLevel = (currentTime - m_letterBoxFadeStartTime)/LETTER_BOX_FADE_TIME;
				if (m_letterBoxFadeLevel > 1.0f)
					m_letterBoxFadeLevel = 1.0f;
			}

			UnsignedInt lbcolor = (Int)(m_letterBoxFadeLevel * 255.0f) << 24;

#ifdef SLIDE_LETTERBOX
			Int height = (Int)(getHeight() * 0.12f * m_letterBoxFadeLevel);
			TheTacticalView->setOrigin(0, height);
#else
			drawFillRect( 0, 0, m_width, (m_height-(9.0f/16.0f * m_width))*0.5f, lbcolor );
			drawFillRect( 0, m_height-(m_height-(9.0f/16.0f * m_width))*0.5f, m_width, m_height, lbcolor );
#endif
		}
		else
		{
			//letter box is disabled, but may still be fading out
			if (m_letterBoxFadeLevel != 0.0f)
			{
				m_letterBoxFadeLevel = 1.0f - (currentTime - m_letterBoxFadeStartTime)/LETTER_BOX_FADE_TIME;
				if (m_letterBoxFadeLevel < 0.0f)
					m_letterBoxFadeLevel = 0.0f;

				UnsignedInt lbcolor = (Int)(m_letterBoxFadeLevel * 255.0f) << 24;

#ifdef SLIDE_LETTERBOX
				Int height = (Int)(getHeight() * 0.12f * m_letterBoxFadeLevel);
				TheTacticalView->setOrigin(0, height);
#else
				drawFillRect( 0, 0, m_width, (m_height-(9.0f/16.0f * m_width))*0.5f, lbcolor );
				//drawFillRect( 0, m_height-(m_height-(9.0f/16.0f * m_width))*0.5f, m_width, m_height, lbcolor );
#endif
			}
			else
			{
				//box has finished fading out
#ifdef SLIDE_LETTERBOX
				TheTacticalView->setOrigin(0, 0);
#else
				m_letterBoxEnabled = FALSE;
#endif
			}
		}
}

Bool W3DDisplay::isLetterBoxFading()
{
	if (m_letterBoxEnabled && m_letterBoxFadeLevel != 1.0f)
		return TRUE;
	if (!m_letterBoxEnabled && m_letterBoxFadeLevel != 0.0f)
		return TRUE;
	return FALSE;
}

//WST 10/2/2002 added query function.  JSC Integrated 5/20/03
Bool W3DDisplay::isLetterBoxed()
{
	return (m_letterBoxEnabled);
}

// W3DDisplay::createLightPulse ===============================================
/** Create a "light pulse" which is a dynamic light that grows, decays
	* and vanishes over several frames */
//=============================================================================
// GeneralsX @feature PS3.0-4A agent 16/10/2026 shared pulse transport.
// Takes one pooled W3DDynamicLight from the RTS3DScene pool and arms its
// increase/decay frame curve. Split out of createLightPulse so the QA test
// leg below can spawn pulses without going through the FXList data channel.
static void applyLightPulse(RTS3DScene* scene, const Coord3D *pos, const RGBColor *color,
                            Real innerRadius, Real attenuationWidth,
                            UnsignedInt increaseFrameTime, UnsignedInt decayFrameTime)
{
	W3DDynamicLight * theDynamicLight = scene->getADynamicLight();
	if (theDynamicLight == nullptr)
	{
		return;
	}
	// turn it on.
	theDynamicLight->setEnabled(true);

	theDynamicLight->Set_Ambient( Vector3( color->red, color->green, color->blue ) );
	theDynamicLight->Set_Diffuse( Vector3( color->red, color->green, color->blue) );
	// GeneralsX @bugfix 25/10/2026 P35: the pool hands out lights that still carry the previous owner's
	// intensity (a building lamp runs at ~0.4). Pulses relied on the default and therefore inherited that
	// dim value - muzzle/explosion lights looked like they did nothing next to the lamps. A pulse is a
	// flash: full white intensity, its colour comes from Diffuse.
	theDynamicLight->Set_Intensity(1.0f);
	theDynamicLight->Set_Position(Vector3(pos->x, pos->y, pos->z));
	theDynamicLight->Set_Far_Attenuation_Range(innerRadius, innerRadius + attenuationWidth);
	// GeneralsX @feature Qoder 24/10/2026 P4 D7: muzzle/light pulses with decay=0 snapped off in one
	// frame. Give every zero-decay pulse a 4-frame fade; explosions already pass a longer decay.
	if (decayFrameTime == 0) {
		decayFrameTime = 4;
	}
	theDynamicLight->setFrameFade(increaseFrameTime, decayFrameTime);
	theDynamicLight->setDecayRange();
	theDynamicLight->setDecayColor();
	//theDynamicLight->setDonut(donut);
	// (gth) CNC3 enable far attenuation.  C&C3 defaults to disabled.  Must enable to match Generals. MW 8-06-03
	theDynamicLight->Set_Flag(LightClass::FAR_ATTENUATION,true);
}

// GeneralsX @feature Qoder 18/10/2026 PS3.0 4E: the pulse gate is now a live value.
// GENERALSX_LIGHTPULSE still wins outright (cached on the first query, so a QA parity leg can never be
// moved by the menu); without an env the gate seeds from Options.ini LightPulseEffect on the first
// pulse and W3DDisplay::setLightPulseEnabled() re-arms it from the Graphics Options page, so flipping
// the row changes the next explosion instead of the next game start.
static Int s_pulseEnvState = 0;			///< 0 = not looked up yet, 1 = no env, 2 = env OFF, 3 = env ON
static Int s_pulseUserEnabled = -1;		///< -1 = not seeded yet, else 0/1 (env or ini or the menu)

static Bool LightPulse_Enabled()
{
	if (s_pulseEnvState == 0)
	{
		const char* ev = getenv("GENERALSX_LIGHTPULSE");
		if (ev == nullptr || *ev == 0)
			s_pulseEnvState = 1;
		else
			s_pulseEnvState = (atoi(ev) != 0) ? 3 : 2;
	}
	if (s_pulseEnvState == 3)
		return TRUE;
	if (s_pulseEnvState == 2)
		return FALSE;
	if (s_pulseUserEnabled < 0)
		s_pulseUserEnabled = (OptionPreferences().getLightPulseEnabled() != FALSE) ? 1 : 0;
	return s_pulseUserEnabled != 0;
}

void W3DDisplay::setLightPulseEnabled( Bool enabled )
{
	s_pulseUserEnabled = (enabled != FALSE) ? 1 : 0;
	fprintf(stderr, "[LPULSE] user gate re-armed to %d\n", s_pulseUserEnabled);
	fflush(stderr);
}

void W3DDisplay::createLightPulse( const Coord3D *pos, const RGBColor *color,
																	 Real innerRadius, Real attenuationWidth,
																	 UnsignedInt increaseFrameTime,
																	 UnsignedInt decayFrameTime//, Bool donut
																	 )
{
	// GeneralsX @feature PS3.0-4A agent 16/10/2026 dynamic light pulse gate.
	// The retail LightPulse channel (FXList LightPulseFXNugget -> here -> pooled W3DDynamicLight)
	// is the transport for explosion/muzzle lighting in lighting stage four.
	// GENERALSX_LIGHTPULSE=0 kills the whole channel (QA A/B leg); default ON = retail parity.
	// GENERALSX_LIGHTPULSE_LOG=1 prints every accepted pulse for on-hardware evidence.
	// GeneralsX @feature PS3.0 4E 17/10/2026: the env keeps winning; with no env the gate follows
	// the Graphics Options page (Options.ini LightPulseEffect).
	static const Bool s_pulseLog = (getenv("GENERALSX_LIGHTPULSE_LOG") != nullptr) && (atoi(getenv("GENERALSX_LIGHTPULSE_LOG")) != 0);
	if (!LightPulse_Enabled())
	{
		return;
	}

	if (m_3DScene == nullptr)
		return;
	// GeneralsX @bugfix 24/10/2026 P27 D8: retail drops any pulse smaller than two pathfind cells
	// (radius < 2*PATHFIND_CELL_SIZE_F + 1) as "won't make a visible difference". That silently ate every
	// shell-impact LightPulse - the log showed pulses only at the three tank muzzles, never at a hit -
	// so explosions lit the ground with their sprite alone and no dynamic light reached the terrain.
	// The floor is now a QA-tunable number (0 = accept every pulse); the default keeps retail's value so
	// the look only changes when a case is proven to need it.
	static Real s_minPulse = -1.0f;
	if (s_minPulse < 0.0f)
	{
		const char* ev = getenv("GENERALSX_LIGHTPULSE_MIN");
		// Default accepts every pulse. Measured on 00000017.sav: muzzle pulses are radius 27 (accepted by
		// retail's 21 floor) while shell-impact pulses are radius 16 (dropped), so hits lit the ground with
		// the sprite alone. GENERALSX_LIGHTPULSE_MIN=21 restores retail's floor for a comparison leg.
		s_minPulse = (ev != nullptr && *ev != 0) ? (Real)atof(ev) : 0.0f;
		if (s_minPulse < 0.0f) s_minPulse = 0.0f;
	}
	if (innerRadius+attenuationWidth < s_minPulse) {
		if (s_pulseLog)
		{
			fprintf(stderr, "[LPULSE] drop radius=%.1f (min=%.1f) pos=(%.0f,%.0f,%.0f)\n",
			        (double)(innerRadius + attenuationWidth), (double)s_minPulse,
			        pos ? pos->x : 0.0f, pos ? pos->y : 0.0f, pos ? pos->z : 0.0f);
			fflush(stderr);
		}
		return; // it basically won't make any visual difference.  jba.
	}
	if (s_pulseLog)
	{
		fprintf(stderr, "[LPULSE] rgb=(%d,%d,%d) radius=%.0f inc=%u dec=%u pos=(%.0f,%.0f,%.0f)\n",
		        (Int)(color->red * 255.0f), (Int)(color->green * 255.0f), (Int)(color->blue * 255.0f),
		        innerRadius + attenuationWidth, increaseFrameTime, decayFrameTime,
		        pos ? pos->x : 0.0f, pos ? pos->y : 0.0f, pos ? pos->z : 0.0f);
		fflush(stderr);
	}
	applyLightPulse(m_3DScene, pos, color, innerRadius, attenuationWidth, increaseFrameTime, decayFrameTime);
}

void W3DDisplay::toggleLetterBox()
{
	m_letterBoxEnabled = !m_letterBoxEnabled;
	m_letterBoxFadeStartTime = timeGetTime();

	//WST  9/18/2002 This is not a script api to prevent cheat. JSC Integrated 5/20/03
	if( TheTacticalView )
	{
		TheTacticalView->setZoomLimited( !m_letterBoxEnabled );
	}
}

void W3DDisplay::enableLetterBox(Bool enable)
{
	if (enable)
	{
		if (!m_letterBoxEnabled)
		{
			//letterbox mode not previously enabled
			m_letterBoxEnabled = TRUE;
			m_letterBoxFadeStartTime = timeGetTime();

			//WST  9/18/2002 - This is not a script api to prevent cheat.  JSC Integrated 5/20/03
			if( TheTacticalView )
			{
				TheTacticalView->setZoomLimited( 0 );
			}
		}
	}
	else
	{
		if (m_letterBoxEnabled)
		{
			//letterbox mode no previously disabled
			m_letterBoxEnabled = FALSE;
			m_letterBoxFadeStartTime = timeGetTime();

			//WST  9/18/2002. JSC Integrated 5/20/03
			if( TheTacticalView )
			{
				TheTacticalView->setZoomLimited( 1 );
			}
		}
	}
}

// W3DDisplay::setTimeOfDay ===================================================
/** */
//=============================================================================
// GeneralsX @feature Qoder 22/10/2026 FPNIGHT: render-side TOD (after the preview override), shared
// with presentation tables that must follow night even though the logic TOD stays at the map value.
static TimeOfDay s_renderTimeOfDay = TIME_OF_DAY_AFTERNOON;
TimeOfDay W3DDisplay::getRenderTimeOfDay()
{
	return s_renderTimeOfDay;
}

// GeneralsX @tweak Qoder 22/10/2026 night colour temperature (user verdict: keep the warm/cool
// contrast, push the night itself cooler). Applied wherever a raw night table value is read, so a
// second setTimeOfDay can never compound the shift. Presentation-only.
static void NightCoolTint(float& r, float& g, float& b, bool ambient)
{
	const float sr = ambient ? 0.74f : 0.78f;
	const float sg = ambient ? 0.90f : 0.93f;
	const float sb = ambient ? 1.25f : 1.20f;
	r *= sr; g *= sg; b *= sb;
	if (r > 1.0f) r = 1.0f;
	if (g > 1.0f) g = 1.0f;
	if (b > 1.0f) b = 1.0f;
}

void W3DDisplay::setTimeOfDay( TimeOfDay tod )
{
	// GeneralsX @feature PS3.0 4C 16/10/2026 render-side TOD override (光影四阶段/调研/03): same
	// GENERALSX_TOD_OVERRIDE contract as W3DGameClient::setTimeOfDay - clamped here too because the
	// init path (see L~966) calls this function directly, bypassing the GameClient fan-out.
	// Idempotent: the override maps every input to the same slot. Logic TOD state is untouched.
	{
		static int s_todOv = -1;
		if (s_todOv < 0) {
			// GeneralsX @feature PS3.0 4E 17/10/2026: env wins, else the Graphics Options page key
			// Options.ini TimeOfDayPreview (0 = follow the map). Restart to take effect.
			const char* ev = getenv("GENERALSX_TOD_OVERRIDE");
			s_todOv = (ev != nullptr && *ev != 0) ? atoi(ev) : OptionPreferences().getTodPreview();
			if (s_todOv < 0 || s_todOv >= TIME_OF_DAY_COUNT) s_todOv = 0;
		}
		if (s_todOv != 0) {
			tod = (TimeOfDay)s_todOv;
			// The terrain rasterizer (BaseHeightMap::doTheLight), water, tracks, snow and scorch
			// decals all light from the GlobalData-DERIVED m_terrainAmbient/m_terrainDiffuse/
			// m_terrainLightPos arrays, which only the logic-side GlobalData::setTimeOfDay refills -
			// with the UNCLAMPED logic TOD that left the ground in retail afternoon while the object
			// slots below went night (measured 16/10: civilian buildings crushed to black, grass
			// untouched). Mirror the retail refill here for the overridden slot. Presentation-only:
			// grep 16/10 confirmed zero GameLogic consumers and no xfer of these arrays, so Golden
			// Rule 7 (determinism) holds; a later scripted TOD change re-fills from logic tod and the
			// ensuing GameClient->setTimeOfDay fan-out re-applies this override - stays in step.
			for (Int t = 0; t < MAX_GLOBAL_LIGHTS; t++) {
				RGBColor amb = TheGlobalData->m_terrainLighting[tod][t].ambient;
				RGBColor dif = TheGlobalData->m_terrainLighting[tod][t].diffuse;
				if (tod == TIME_OF_DAY_NIGHT) {
					NightCoolTint(amb.red, amb.green, amb.blue, true);
					NightCoolTint(dif.red, dif.green, dif.blue, false);
				}
				TheWritableGlobalData->m_terrainAmbient[t]   = amb;
				TheWritableGlobalData->m_terrainDiffuse[t]   = dif;
				TheWritableGlobalData->m_terrainLightPos[t]  = TheGlobalData->m_terrainLighting[tod][t].lightPos;
			}
		}
	}

	s_renderTimeOfDay = tod;

	// GeneralsX @feature PS3.0 Weather W1 16/10/2026: weather -> lighting coupler.
	// The active weather recipe carries two multipliers (ambientScale / keyScale); they are folded
	// into EVERY lighting consumer below, always derived from the raw INI slot values (or from a
	// value that was already multiplied once this call), so re-entering setTimeOfDay can never
	// compound them. WEATHER_NORMAL reports 1.0/1.0, which short-circuits the whole feature: the
	// arithmetic below is then bit-for-bit the retail/PS3.0 path (Golden Rule: visible difference
	// only when the weather is actually non-normal).
	//   ambientScale crushes the shadow side (scene ambient, dot3/GN ambient feeds, every non-key
	//   slot light) - overcast and blizzard read flat and grey.
	//   keyScale dims the sun (slot0 diffuse + the GN per-pixel key lobe).
	const WeatherRenderState &wx = weatherRenderState();
	const Real wxAmb = wx.m_ambientScale;
	const Real wxKey = wx.m_keyScale;
	const Bool wxActive = (fabsf(wxAmb - 1.0f) > 0.001f || fabsf(wxKey - 1.0f) > 0.001f);
	// GeneralsX @bugfix PS3.0 Weather W5 16/10/2026: this used a 1-in-256 counter, which is the wrong
	// instrument for a call that runs a handful of times per session (retail time-of-day is static, so
	// setTimeOfDay fires at load and on scripted/weather changes only). Every weather leg therefore
	// printed exactly one line - and it was the weather the map declared at load, not the one the QA
	// env or a script switched to, so the leg could not show its own coupling numbers. Log on CHANGE of
	// the (type, ambient, key, fog, wet) tuple instead: one honest line per distinct lighting state,
	// which is what makes "the storm crushed the sun to 0.72/0.60" readable in a capture log.
	static Int s_lastType = -999;
	static Real s_lastAmb = -1.0f, s_lastKey = -1.0f, s_lastFog = -1.0f, s_lastWet = -1.0f;
	static unsigned s_wxLogCount = 0;
	const Int curType = (Int)wx.m_type;
	if (wxActive)
	{
		if (curType != s_lastType
			|| fabsf(wxAmb - s_lastAmb) > 0.001f
			|| fabsf(wxKey - s_lastKey) > 0.001f
			|| fabsf(wx.m_fogScale - s_lastFog) > 0.001f
			|| fabsf(wx.m_wetness - s_lastWet) > 0.001f)
		{
			++s_wxLogCount;
			fprintf(stderr, "[WEATHER_LIGHT] type=%d ambMul=%.2f keyMul=%.2f fog=%.2f wet=%.2f n=%u\n",
				curType, wxAmb, wxKey, wx.m_fogScale, wx.m_wetness, s_wxLogCount);
			fflush(stderr);
		}
		s_lastType = curType;
		s_lastAmb = wxAmb;
		s_lastKey = wxKey;
		s_lastFog = wx.m_fogScale;
		s_lastWet = wx.m_wetness;
	}
	else
	{
		// Neutral (retail parity path): forget the last tuple so the next weather change logs again,
		// and say so once - a leg that switches back to clear weather must be visible in the log too.
		if (s_lastType != -999)
		{
			++s_wxLogCount;
			fprintf(stderr, "[WEATHER_LIGHT] type=%d neutral ambMul=1.00 keyMul=1.00 n=%u\n", curType, s_wxLogCount);
			fflush(stderr);
		}
		s_lastType = -999;
	}

	const GlobalData::TerrainLighting *ol=&TheGlobalData->m_terrainObjectsLighting[tod][0];

	if( m_3DScene )
	{
		Vector3 sceneAmbient( ol->ambient.red, ol->ambient.green, ol->ambient.blue );
		if (tod == TIME_OF_DAY_NIGHT) {
			float r = sceneAmbient.X, g = sceneAmbient.Y, b = sceneAmbient.Z;
			NightCoolTint(r, g, b, true);
			sceneAmbient.Set(r, g, b);
		}
		if (wxActive) {
			sceneAmbient *= wxAmb;
		}
		m_3DScene->Set_Ambient_Light( sceneAmbient );
	}

	for (Int i=0; i<LightEnvironmentClass::MAX_LIGHTS; i++)
	{
		if( m_myLight[i] )
		{
			ol=&TheGlobalData->m_terrainObjectsLighting[tod][i];

			m_myLight[i]->Set_Ambient( Vector3( 0.0f, 0.0f, 0.0f ) );
			Vector3 slotDiffuse( ol->diffuse.red, ol->diffuse.green, ol->diffuse.blue );
			if (tod == TIME_OF_DAY_NIGHT) {
				float r = slotDiffuse.X, g = slotDiffuse.Y, b = slotDiffuse.Z;
				NightCoolTint(r, g, b, false);
				slotDiffuse.Set(r, g, b);
			}
			if (wxActive) {
				// slot0 is the key sun, slots1..3 are fill/bounce/map lamps -> all non-key terms dim
				// with the ambient scale (the same split the L1c crush knob uses).
				slotDiffuse *= (i == 0) ? wxKey : wxAmb;
				if (slotDiffuse.X > 1.0f) slotDiffuse.X = 1.0f;
				if (slotDiffuse.Y > 1.0f) slotDiffuse.Y = 1.0f;
				if (slotDiffuse.Z > 1.0f) slotDiffuse.Z = 1.0f;
			}
			m_myLight[i]->Set_Diffuse( slotDiffuse );
			m_myLight[i]->Set_Specular( Vector3(0,0,0) );
			Matrix3D mtx;
			mtx.Set(Vector3(1,0,0), Vector3(0,1,0), Vector3(ol->lightPos.x, ol->lightPos.y, ol->lightPos.z), Vector3(0,0,0));
			m_myLight[i]->Set_Transform(mtx);
		}
	}
	if(TheTerrainRenderObject) {
		TheTerrainRenderObject->setTimeOfDay(tod);
		TheTacticalView->forceRedraw();
	}

	// GeneralsX @feature T3-0 P1: feed the global sun to the dot3 fallback so battlefield
	// meshes (which carry no per-mesh light environment) can mount normal maps. Sign follows
	// LightEnvironmentClass: its directional direction is -lightZ, and setTimeOfDay puts
	// lightPos into the light's Z column, hence the negation here.
	{
		const GlobalData::TerrainLighting* sl = &TheGlobalData->m_terrainObjectsLighting[tod][0];
		// GeneralsX @feature PS3.0 Weather W1 16/10/2026: same coupler factors as the fixed pipeline
		// above (raw slot0 values * weather), so the dot3 and GN paths dim in lockstep with the sun.
		Vector3 wxFallbackAmbient( sl->ambient.red, sl->ambient.green, sl->ambient.blue );
		Vector3 wxGnKeyColor( sl->diffuse.red, sl->diffuse.green, sl->diffuse.blue );
		if (wxActive) {
			wxFallbackAmbient *= wxAmb;
			wxGnKeyColor *= wxKey;
			if (wxGnKeyColor.X > 1.0f) wxGnKeyColor.X = 1.0f;
			if (wxGnKeyColor.Y > 1.0f) wxGnKeyColor.Y = 1.0f;
			if (wxGnKeyColor.Z > 1.0f) wxGnKeyColor.Z = 1.0f;
		}
		// GeneralsX @feature 27/10/2026 P1-2: same global rig chromaticity as W3D_RefreshRenderLighting.
		// These reach the object path's c9/c39/c40, which is why the building/wall red lived here too.
		if (DX8Wrapper::VK_HDR_Enabled() && W3D_RigT26()) {
			W3D_ApplyRigHue(wxGnKeyColor.X, wxGnKeyColor.Y, wxGnKeyColor.Z, W3D_RIG_SUN);
			W3D_ApplyRigHue(wxFallbackAmbient.X, wxFallbackAmbient.Y, wxFallbackAmbient.Z, W3D_RIG_SKY);
		}
		DX8NormalMap::Set_Global_Fallback_Light(
			Vector3(-sl->lightPos.x, -sl->lightPos.y, -sl->lightPos.z),
			wxFallbackAmbient);
		// GeneralsX @feature PS3.0 L2 14/10/2026: the geometric-normal per-pixel diffuse module
		// corrects the fixed-pipeline sun term, so it needs the same direction plus the key's
		// DIFFUSE color (the fallback light above only carries direction + ambient).
		DX8GeomNormal::Set_Global_Key_Light(
			Vector3(-sl->lightPos.x, -sl->lightPos.y, -sl->lightPos.z),
			wxGnKeyColor);
		// The GN VS rebuilds the Gouraud base, so it also needs the scene object ambient (the same
		// slot0 ambient fed to Set_Ambient_Light above).
		DX8GeomNormal::Set_Global_Ambient( wxFallbackAmbient );
	}

	// GeneralsX @feature PS3.0 L1 13/10/2026: three-point lighting rig (key = the retail sun above).
	// Retail lit the world with that single sun plus a flat ambient, so backfaces lifted with
	// frontfaces and macro shading read flat (Lambert report section 3). Here a cool sky FILL
	// (opposite the sun's azimuth, high) and an optional warm ground BOUNCE (opt-in, default 0)
	// are synthesized from the key and delivered through two paths kept in lockstep:
	//  - fixed pipeline: when the rig is ON, slot1 is FORCE-OWNED by it (GeneralsX @tweak PS3.0 L1
	//    14/10/2026): the map/save TerrainObjectsLighting*2 fill is REPLACED by the synthetic sky
	//    fill every update, so fresh games and save loads render the same treatment instead of
	//    yielding to embedded INI data. Slot2 (bounce) stays opt-in: while
	//    GENERALSX_BOUNCE_INTENSITY=0 the map's third light is left untouched, avoiding a regression
	//    to darker-than-retail scenes. W3DScene then collects the slots into every object's
	//    LightEnvironment; Add_Light's contribution ranking keeps strong map lamps above the fill.
	//  - dot3 normal-map path: it bypasses Lights[4] entirely, so DX8NormalMap receives the same
	//    fill (bounce is fixed-pipeline-only there; point-light scenes dominate anyway).
	// Knobs are process env (restart to change): GENERALSX_FILL_LIGHT=1 enables the rig (see the
	// default-OFF note below); GENERALSX_FILL_INTENSITY = fill as a fraction of the key color
	// (default 0.22); GENERALSX_BOUNCE_INTENSITY = bounce fraction (default 0 = slot stays dark).
	{
		static Real s_fillScale = -1.0f;
		static Real s_bounceScale = -1.0f;
		if (s_fillScale < 0.0f) {
			const char* ev = getenv("GENERALSX_FILL_INTENSITY");
			s_fillScale = (ev != nullptr && *ev != 0) ? (Real)atof(ev) : 0.22f;
			if (s_fillScale < 0.0f) s_fillScale = 0.0f;
			if (s_fillScale > 1.0f) s_fillScale = 1.0f;
		}
		if (s_bounceScale < 0.0f) {
			const char* ev = getenv("GENERALSX_BOUNCE_INTENSITY");
			s_bounceScale = (ev != nullptr && *ev != 0) ? (Real)atof(ev) : 0.0f;
			if (s_bounceScale < 0.0f) s_bounceScale = 0.0f;
			if (s_bounceScale > 0.5f) s_bounceScale = 0.5f;
		}
		const char* evOff = getenv("GENERALSX_FILL_LIGHT");
		// GeneralsX @tweak PS3.0 L1 14/10/2026: synthetic blue fill default flipped to OFF. The user's
		// live A/B rejected the forced cool fill (cold cast on shadow sides, overall too dark) and
		// approved the MAPLIGHT recipe instead: crush the flat ambient and let the MAP's own neutral
		// slot1 fill light the shadow side. The rig is now opt-in: GENERALSX_FILL_LIGHT=1 restores it.
		const bool fillOn = (evOff != nullptr && atoi(evOff) != 0);
		// GeneralsX @feature PS3.0 L1 14/10/2026: fill-gain, now the amplitude of the FORCED synthetic
		// fill (history: the first L1 A/B showed the old "only while black" guard yielding to map
		// slot1 data; an interim gain mode amplified the map's own fill instead. The 14/10 user ruling
		// promoted the rig to default lighting, so gain now simply scales the synthetic lobe before
		// it overwrites slot1). GENERALSX_FILL_GAIN multiplies FILL_INTENSITY's output, clamped to
		// white per channel. Default 4 reproduces the showcase brightness measured on map 00000006
		// (buildings +2..+5 lum, terrain zero bleed); GENERALSX_FILL_GAIN=1 gives the quiet 0.22 fill.
		static Real s_fillGain = -1.0f;
		if (s_fillGain < 0.0f) {
			const char* ev = getenv("GENERALSX_FILL_GAIN");
			s_fillGain = (ev != nullptr && *ev != 0) ? (Real)atof(ev) : 4.0f;
			if (s_fillGain < 1.0f) s_fillGain = 1.0f;
			if (s_fillGain > 4.0f) s_fillGain = 4.0f;
		}

		Vector3 dot3FillDir(0.0f, 0.0f, 0.0f);   // toward the fill; zero = dot3 term disabled
		Vector3 dot3FillColor(0.0f, 0.0f, 0.0f);
		if (fillOn && (s_fillScale > 0.001f || s_bounceScale > 0.001f)) {
			const GlobalData::TerrainLighting* key = &TheGlobalData->m_terrainObjectsLighting[tod][0];
			Vector3 toSun(-key->lightPos.x, -key->lightPos.y, -key->lightPos.z);
			const Real sunLen2 = toSun.X * toSun.X + toSun.Y * toSun.Y + toSun.Z * toSun.Z;
			if (sunLen2 > 1e-6f) {
				toSun *= (Real)(1.0f / sqrtf((float)sunLen2));
				Vector3 kd(key->diffuse.red, key->diffuse.green, key->diffuse.blue);

				// --- cool sky fill: opposite azimuth, stays above the horizon ---
				Vector3 fd(-toSun.X, -toSun.Y, toSun.Z > 0.15f ? (Real)(0.25f + 0.55f * toSun.Z) : 0.85f);
				const Real fdLen2 = fd.X * fd.X + fd.Y * fd.Y + fd.Z * fd.Z;
				if (fdLen2 > 1e-6f) fd *= (Real)(1.0f / sqrtf((float)fdLen2));
				Vector3 fc(kd.X * 0.45f, kd.Y * 0.68f, kd.Z * 1.05f);
				fc *= s_fillScale;
				// GeneralsX @feature PS3.0 Weather W1 16/10/2026: slot1 is force-overwritten here, so the
				// synthetic fill carries the weather ambient factor itself (it is a non-key term).
				if (wxActive) fc *= wxAmb;
				if (fc.X > 1.0f) fc.X = 1.0f;
				if (fc.Y > 1.0f) fc.Y = 1.0f;
				if (fc.Z > 1.0f) fc.Z = 1.0f;

				if (s_fillScale > 0.001f) {
					if (m_myLight[1] != nullptr) {
						// GeneralsX @tweak PS3.0 L1 14/10/2026: FORCE-OWN slot1 (user ruling: the rig IS
						// the default lighting). No "only while black" guard and no map-data branch:
						// every update overwrites slot1 with the synthetic sky fill scaled by gain, so
						// new games and save loads (whose embedded INI previously owned this slot) render
						// identically. The loop above re-reads TheGlobalData from INI every call, so the
						// overwrite is idempotent and a GENERALSX_FILL_LIGHT=0 restart restores retail.
						Vector3 fcEff = fc;
						fcEff.X *= s_fillGain;
						fcEff.Y *= s_fillGain;
						fcEff.Z *= s_fillGain;
						if (fcEff.X > 1.0f) fcEff.X = 1.0f;
						if (fcEff.Y > 1.0f) fcEff.Y = 1.0f;
						if (fcEff.Z > 1.0f) fcEff.Z = 1.0f;

						static unsigned s_fillLogTick = 0;
						if ((s_fillLogTick++ & 511u) == 0u) {
							Vector3 cur; m_myLight[1]->Get_Diffuse(&cur);
							fprintf(stderr, "[L1] slot1 FORCED (map data overwritten): synth=(%.3f,%.3f,%.3f) gain=%.2f -> installed=(%.3f,%.3f,%.3f) was=(%.3f,%.3f,%.3f) n=%u\n",
								fc.X, fc.Y, fc.Z, s_fillGain, fcEff.X, fcEff.Y, fcEff.Z,
								cur.X, cur.Y, cur.Z, s_fillLogTick);
							fflush(stderr);
						}
						m_myLight[1]->Set_Ambient(Vector3(0.0f, 0.0f, 0.0f));
						m_myLight[1]->Set_Diffuse(fcEff);
						m_myLight[1]->Set_Specular(Vector3(0.0f, 0.0f, 0.0f));
						Matrix3D fm;
						fm.Set(Vector3(1,0,0), Vector3(0,1,0), Vector3(-fd.X, -fd.Y, -fd.Z), Vector3(0,0,0));
						m_myLight[1]->Set_Transform(fm);
						dot3FillDir = fd;
						dot3FillColor = fcEff;
					}
				}

				// --- warm ground bounce: same sun azimuth, from below (opt-in) ---
				if (s_bounceScale > 0.001f && m_myLight[2] != nullptr) {
					Vector3 bd(toSun.X, toSun.Y, -0.6f);
					const Real bdLen2 = bd.X * bd.X + bd.Y * bd.Y + bd.Z * bd.Z;
					if (bdLen2 > 1e-6f) bd *= (Real)(1.0f / sqrtf((float)bdLen2));
					Vector3 bc(kd.X, kd.Y * 0.62f, kd.Z * 0.38f);
					bc *= s_bounceScale;
					if (wxActive) bc *= wxAmb;		// GeneralsX @feature PS3.0 Weather W1 16/10/2026: non-key term
					if (bc.X > 1.0f) bc.X = 1.0f;
					if (bc.Y > 1.0f) bc.Y = 1.0f;
					if (bc.Z > 1.0f) bc.Z = 1.0f;
					Vector3 cur; m_myLight[2]->Get_Diffuse(&cur);
					if (cur.Length2() < 0.0025f) {   // slot unused by data -> ours
						m_myLight[2]->Set_Ambient(Vector3(0.0f, 0.0f, 0.0f));
						m_myLight[2]->Set_Diffuse(bc);
						m_myLight[2]->Set_Specular(Vector3(0.0f, 0.0f, 0.0f));
						Matrix3D bm;
						bm.Set(Vector3(1,0,0), Vector3(0,1,0), Vector3(-bd.X, -bd.Y, -bd.Z), Vector3(0,0,0));
						m_myLight[2]->Set_Transform(bm);
					}
				}
			}
		}
		// Single call outside the branches: disabled/off states zero the dot3 fill term.
		DX8NormalMap::Set_Global_Fill_Light(dot3FillDir, dot3FillColor);
		// GeneralsX @feature PS3.0 L2: GN corrects the same fill lobe on plain meshes; keep in lockstep.
		DX8GeomNormal::Set_Global_Fill_Light(dot3FillDir, dot3FillColor);
	}

	// GeneralsX @tweak 24/10/2026: the PS3.0 L1c ambient-crush pass (GENERALSX_AMBIENT_SCALE, default
	// 0.5) was a debug experiment, not a shipping look, and it was the second place the ambient got
	// halved. With it gone the raw retail numbers installed above stay untouched for every consumer:
	// the retail loop's Set_Ambient_Light, the slot0 ambient/weather feeds that the GN and dot3 globals
	// already took earlier in this function, and the map's own slot2/3 lights. Nothing re-scales them.

	// GeneralsX @feature PS3.0 L1k 14/10/2026: key-sun gain. The user approved the MAPLIGHT recipe
	// (crushed scene ambient + the map's own neutral fill; synthetic blue fill retired) and asked
	// to push the sunlit side harder on top of it. GENERALSX_KEY_GAIN (1..3, default 1 = retail
	// parity) amplifies ONLY the key light: slot0 diffuse for the fixed pipeline plus the GN
	// module's global key color, kept in lockstep. The shadow side is untouched (it is fed by the
	// map fill + ambient), so raising this widens the light/dark ratio directly. Terrain keeps its
	// own sun slots (m_terrainLighting) and is deliberately out of scope here. Idempotent like the
	// blocks above: the retail loop rewrites raw INI values into slot0 every call.
	{
		static Real s_keyGain = -1.0f;
		if (s_keyGain < 0.0f) {
			const char* ev = getenv("GENERALSX_KEY_GAIN");
			s_keyGain = (ev != nullptr && *ev != 0) ? (Real)atof(ev) : 1.0f;
			if (s_keyGain < 1.0f) s_keyGain = 1.0f;
			if (s_keyGain > 3.0f) s_keyGain = 3.0f;
		}
		if (s_keyGain > 1.0001f) {
			const GlobalData::TerrainLighting* k0 = &TheGlobalData->m_terrainObjectsLighting[tod][0];
			Vector3 kd(k0->diffuse.red, k0->diffuse.green, k0->diffuse.blue);
			kd *= s_keyGain;
			if (wxActive) kd *= wxKey;	// GeneralsX @feature PS3.0 Weather W1 16/10/2026: raw-derived key term
			if (kd.X > 1.0f) kd.X = 1.0f;
			if (kd.Y > 1.0f) kd.Y = 1.0f;
			if (kd.Z > 1.0f) kd.Z = 1.0f;
			if (DX8Wrapper::VK_HDR_Enabled() && W3D_RigT26())
				W3D_ApplyRigHue(kd.X, kd.Y, kd.Z, W3D_RIG_SUN);
			if (m_myLight[0] != nullptr) {
				m_myLight[0]->Set_Diffuse(kd);
			}
			DX8GeomNormal::Set_Global_Key_Light(
				Vector3(-k0->lightPos.x, -k0->lightPos.y, -k0->lightPos.z), kd);
			static unsigned s_keyLogTick = 0;
			if ((s_keyLogTick++ & 511u) == 0u) {
				Vector3 cur(0,0,0);
				if (m_myLight[0]) m_myLight[0]->Get_Diffuse(&cur);
				fprintf(stderr, "[L1k] keyGain=%.2f raw=(%.3f,%.3f,%.3f) -> sun=(%.3f,%.3f,%.3f) n=%u\n",
					s_keyGain, k0->diffuse.red, k0->diffuse.green, k0->diffuse.blue, cur.X, cur.Y, cur.Z, s_keyLogTick);
				fflush(stderr);
			}
		}
	}

	// GeneralsX @feature PS3.0 Weather W5 16/10/2026: last step of the lighting install. setTimeOfDay
	// is the one place the device writes the lighting state, so pushing the weather's shader-half
	// scalars (GN/ML haze ramp + PBR wetness) from here puts them on the same frame as the fixed
	// pipeline, the terrain haze and the screen layer. WEATHER_NORMAL publishes fogScale 1.0 /
	// wetness 0, which re-uploads exactly the install numbers, so a clear map stays unchanged.
	// applyWeatherToShaders refreshes the one shared lighting snapshot internally, so do not publish
	// a second time here: the old double call bumped revision twice and scheduled duplicate rebakes.
	W3DShaderManager::applyWeatherToShaders();

	// GeneralsX @feature Qoder 24/10/2026 P1 T1.6: the object/FF install above duplicated the weather,
	// ambient-scale and key-gain arithmetic. Publish the finished snapshot back into the scene/global
	// lights so the fixed pipeline, GN objects and terrain all shade from the SAME numbers. The legacy
	// block still runs first (it owns per-slot direction/intensity); this is the final value authority.
	{
		const W3DRenderLightingState &st = W3D_GetRenderLighting();
		if (m_3DScene != nullptr) {
			m_3DScene->Set_Ambient_Light(Vector3(st.ambient[0], st.ambient[1], st.ambient[2]));
		}
		if (m_myLight[0] != nullptr) {
			m_myLight[0]->Set_Diffuse(Vector3(st.sunColor[0], st.sunColor[1], st.sunColor[2]));
		}
		if (m_myLight[1] != nullptr) {
			m_myLight[1]->Set_Diffuse(Vector3(st.fillColor[0], st.fillColor[1], st.fillColor[2]));
		}
	}
}

// GeneralsX @feature Storm996 17/10/2026 PS3.0 haze user switch: hand the player's choice to the
// shader manager. Everything downstream (the GN/dot3 injected ramp, the terrain/road/tree batch
// upload, the weather coupling) reads one recipe, so this single call moves the whole scene.
void W3DDisplay::setDistanceHaze( Bool enabled, Real strength01 )
{
	W3DShaderManager::setDistanceHaze(enabled, strength01);
}

// GeneralsX @feature Qoder 18/10/2026 PS3.0 live lighting knobs: the two remaining shader-side
// values the Graphics Options page now owns without a restart. Both live in W3DShaderManager, which
// is where the bloom composite and the sun shadow map read them (see W3DShaderManager.h).
void W3DDisplay::setBloomIntensity( Real intensity01 )
{
	W3DShaderManager::setBloomIntensity(intensity01);
}

void W3DDisplay::setShadowMapStrength( Real strength01 )
{
	W3DShaderManager::setShadowMapStrength(strength01);
}

// W3DDisplay::drawLine =======================================================
/** draw a line on the display in pixel coordinates with the specified color */
//=============================================================================
void W3DDisplay::drawLine( Int startX, Int startY,
													 Int endX, Int endY,
													 Real lineWidth,
													 UnsignedInt lineColor )
{
	setup2DRenderState(nullptr, DRAW_IMAGE_ALPHA, FALSE);

	m_2DRender->Add_Line( Vector2( startX, startY ), Vector2( endX, endY ),
												lineWidth, lineColor );

	if (!m_isBatching)
	{
		m_2DRender->Render();
	}
}

// W3DDisplay::drawLine =======================================================
/** draw a line on the display in pixel coordinates with the specified color */
//=============================================================================
void W3DDisplay::drawLine( Int startX, Int startY,
													 Int endX, Int endY,
													 Real lineWidth,
													 UnsignedInt lineColor1,UnsignedInt lineColor2 )
{
	setup2DRenderState(nullptr, DRAW_IMAGE_ALPHA, FALSE);

	m_2DRender->Add_Line( Vector2( startX, startY ), Vector2( endX, endY ),
												lineWidth, lineColor1, lineColor2 );

	if (!m_isBatching)
	{
		m_2DRender->Render();
	}

}


// W3DDisplay::drawOpenRect ===================================================
//=============================================================================
void W3DDisplay::drawOpenRect( Int startX, Int startY, Int width, Int height,
															 Real lineWidth, UnsignedInt lineColor )
{

	if (m_isClippedEnabled)
	{
		ICoord2D start, end, returnStart, returnEnd;
		start.x = startX;
		start.y = startY;

		end.x = start.x;
		end.y = start.y + height;
		if(ClipLine2D(&start, &end, &returnStart, &returnEnd, &m_clipRegion ))
			drawLine( returnStart.x, returnStart.y, returnEnd.x, returnEnd.y, lineWidth, lineColor);

		end.x = start.x + width;
		end.y = start.y;
		if(ClipLine2D(&start, &end, &returnStart, &returnEnd, &m_clipRegion ))
			drawLine( returnStart.x, returnStart.y, returnEnd.x, returnEnd.y, lineWidth, lineColor);

		start.x = startX + width;
		start.y = startY;
		end.x = start.x;
		end.y = start.y + height;
		if(ClipLine2D(&start, &end, &returnStart, &returnEnd, &m_clipRegion ))
			drawLine( returnStart.x, returnStart.y, returnEnd.x, returnEnd.y, lineWidth, lineColor);

		start.x = startX;
		start.y = startY + height;
		end.x = start.x + width;
		end.y = start.y;
		if(ClipLine2D(&start, &end, &returnStart, &returnEnd, &m_clipRegion ))
			drawLine( returnStart.x, returnStart.y, returnEnd.x, returnEnd.y, lineWidth, lineColor);
	}
	else
	{
		setup2DRenderState(nullptr, DRAW_IMAGE_ALPHA, FALSE);

		m_2DRender->Add_Outline( RectClass( startX, startY,
																				startX + width, startY + height ),
														 lineWidth, lineColor );

		if (!m_isBatching)
		{
			m_2DRender->Render();
		}
	}

}

// W3DDisplay::drawFillRect ===================================================
//=============================================================================
void W3DDisplay::drawFillRect( Int startX, Int startY, Int width, Int height,
															 UnsignedInt color )
{
	setup2DRenderState(nullptr, DRAW_IMAGE_ALPHA, FALSE);

	m_2DRender->Add_Rect( RectClass( startX, startY,
																	 startX + width, startY + height ),
												0, 0, color );

	if (!m_isBatching)
	{
		m_2DRender->Render();
	}
}

void W3DDisplay::drawRectClock(Int startX, Int startY, Int width, Int height, Int percent, UnsignedInt color)
{
	// sanity
	if(percent < 1 || percent > 100)
		return;

	setup2DRenderState(nullptr, DRAW_IMAGE_ALPHA, FALSE);

// The rectangles are numberd as follows
//(x,y)	|---------|
//			| 4  | 1  |
//			|----+----|
//			| 3  | 2  |
//			|---------| (x + width, y + width)
//
	// we're done, lets just draw one rectangle for it all.
	if(percent == 100)
	{
		m_2DRender->Add_Rect(RectClass( startX, startY,
																		startX + width, startY + height), 0,0, color);
	}
	else if( percent> 75)
	{
		//rectangle #1 & 2
		m_2DRender->Add_Rect(RectClass( startX + width/2, startY,
																		startX + width, startY + height), 0,0, color);
		// rectangle #3
		m_2DRender->Add_Rect(RectClass( startX, startY + height/2,
																		startX + width/2, startY + height), 0,0, color);
		// draw the part of rectangle 4
		Real remain = percent - 75;
		if(remain > 12)
		{
			//draw the full triangle
			m_2DRender->Add_Tri(Vector2(startX, startY),
													Vector2(startX, startY + height/2),
													Vector2(startX + width/2, startY + height/2),
													Vector2(0,0),Vector2(0,0),Vector2(0,0),color);

			// draw the part of triangle
			Real percentDraw = (Real)(remain - 12)/ 13;
			m_2DRender->Add_Tri(Vector2(startX, startY),
													Vector2(startX + width/2, startY + height/2),
													Vector2(startX + (width/2 * percentDraw), startY),
													Vector2(0,0),Vector2(0,0),Vector2(0,0),color);
		}
		else
		{
			// draw the part of triangle
			Real percentDraw = (Real)(remain)/ 12;
			m_2DRender->Add_Tri(Vector2(startX, startY + height/2 - (height/2 * percentDraw)),
													Vector2(startX, startY + height/2),
													Vector2(startX + width/2, startY + height/2),
													Vector2(0,0),Vector2(0,0),Vector2(0,0),color);
		}

	}
	else if( percent > 50)
	{
		//rectangle #1 & 2
		m_2DRender->Add_Rect(RectClass( startX + width/2, startY,
																		startX + width, startY + height), 0,0, color);
		// draw the part of rectangle 3
		Real remain = percent - 50;
		if(remain > 12)
		{
			//draw the full triangle
			m_2DRender->Add_Tri(Vector2(startX + width/2, startY + height/2),
													Vector2(startX, startY + height),
													Vector2(startX + width/2, startY + height),
													Vector2(0,0),Vector2(0,0),Vector2(0,0),color);

			// draw the part of triangle
			Real percentDraw = (Real)(remain - 12)/ 13;
			m_2DRender->Add_Tri(Vector2(startX, startY + height - (height/2 * percentDraw)),
													Vector2(startX, startY + height),
													Vector2(startX + width/2, startY + height/2),
													Vector2(0,0),Vector2(0,0),Vector2(0,0),color);
		}
		else
		{
			// draw the part of triangle
			Real percentDraw = (Real)(remain)/ 12;
			m_2DRender->Add_Tri(Vector2(startX + width/2, startY + height),
													Vector2(startX + width/2, startY + height/2),
													Vector2(startX + width/2 - ( width/2 * percentDraw), startY + height),
													Vector2(0,0),Vector2(0,0),Vector2(0,0),color);
		}
	}
	else if(percent > 25)
	{
		// rectangle #1
		m_2DRender->Add_Rect(RectClass( startX + width/2, startY,
																		startX + width, startY + height/2), 0,0, color);
		// draw the part of rectangle 2
		Real remain = percent - 25;
		if(remain > 12)
		{
			//draw the full triangle
			m_2DRender->Add_Tri(Vector2(startX + width/2, startY + height/2),
													Vector2(startX + width, startY + height),
													Vector2(startX + width, startY + height/2),
													Vector2(0,0),Vector2(0,0),Vector2(0,0),color);

			// draw the part of triangle
			Real percentDraw = (Real)(remain - 12)/ 13;
			m_2DRender->Add_Tri(Vector2(startX + width/2, startY + height/2),
													Vector2(startX + width - (width/2 * percentDraw), startY + height),
													Vector2(startX + width, startY + height),
													Vector2(0,0),Vector2(0,0),Vector2(0,0),color);
		}
		else
		{
			// draw the part of triangle
			Real percentDraw = (Real)(remain)/ 12;
			m_2DRender->Add_Tri(Vector2(startX + width, startY + height/2),
													Vector2(startX + width/2, startY + height/2),
													Vector2(startX + width, startY + height/2 + ( height/2 * percentDraw)),
													Vector2(0,0),Vector2(0,0),Vector2(0,0),color);
		}
	}
	else
	{
				// draw the part of rectangle 1

		if(percent > 12)
		{
			//draw the full triangle
			m_2DRender->Add_Tri(Vector2(startX + width/2, startY),
													Vector2(startX + width/2, startY + height/2),
													Vector2(startX + width, startY),
													Vector2(0,0),Vector2(0,0),Vector2(0,0),color);

			// draw the part of triangle
			Real percentDraw = (Real)(percent - 12)/ 13;
			m_2DRender->Add_Tri(Vector2(startX + width, startY),
													Vector2(startX + width/2, startY + height/2),
													Vector2(startX + width, startY + (height/2 * percentDraw)),
													Vector2(0,0),Vector2(0,0),Vector2(0,0),color);
		}
		else
		{
			// draw the part of triangle
			Real percentDraw = (Real)(percent)/ 12;
			m_2DRender->Add_Tri(Vector2(startX + width/2, startY),
													Vector2(startX + width/2, startY + height/2),
													Vector2(startX + width/2 + (width/2 * percentDraw), startY ),
													Vector2(0,0),Vector2(0,0),Vector2(0,0),color);
		}
	}

	if (!m_isBatching)
	{
		m_2DRender->Render();
	}
}


//--------------------------------------------------------------------------------------------------------------------
// W3DDisplay::drawRemainingRectClock
// Variation added by Kris -- October 2002
// This version will overlay a clock progress from the specified percentage to 100%. Essentially, this function will
// "reveal" an icon as it progresses towards completion.
//--------------------------------------------------------------------------------------------------------------------
void W3DDisplay::drawRemainingRectClock(Int startX, Int startY, Int width, Int height, Int percent, UnsignedInt color)
{
	// sanity
	if( percent < 0 || percent > 99 )
		return;

	setup2DRenderState(nullptr, DRAW_IMAGE_ALPHA, FALSE);

// The rectangles are numbered as follows
//(x,y)	|---------|
//			| 4  | 1  |
//			|----+----|
//			| 3  | 2  |
//			|---------| (x + width, y + width)
//

	Int midX = startX + width/2;
	Int midY = startY + height/2;
	Int endX = startX + width;
	Int endY = startY + height;
	Int halfWidth = width/2;
	Int halfHeight = height/2;

	if( percent == 0 )
	{
		// We just started, so draw the entire remaining rectangle.
		// #1, #2, #3, and #4
		m_2DRender->Add_Rect( RectClass( startX, startY, endX, endY ), 0, 0, color );
	}
	else if( percent < 25 )
	{
		//1-25%
		//-----

		//Rectangle #3 & 4
		m_2DRender->Add_Rect( RectClass( startX, startY, midX, endY ), 0, 0, color );

		//Rectangle #2
		m_2DRender->Add_Rect( RectClass( midX, midY, endX, endY ), 0, 0, color );

		//Handle rectangle #1 than needs partial rendering.
		if( percent < 13 )
		{
			//1-12%
  		//-----

			//Draw the 2nd half of rectangle #1
			m_2DRender->Add_Tri( Vector2( midX, midY ), Vector2( endX, midY ), Vector2( endX, startY ),
													 Vector2( 0, 0 ), Vector2( 0, 0 ), Vector2( 0, 0 ), color );

			//Draw the last part of the 1st portion of rectangle #1
			Real percentDraw = (Real)( 13 - percent ) / 13;
			m_2DRender->Add_Tri( Vector2( midX, midY ), Vector2( endX, startY ), Vector2( endX - halfWidth * percentDraw, startY ),
													 Vector2( 0, 0 ), Vector2( 0, 0 ), Vector2( 0, 0 ), color );
		}
		else
		{
			//13-24%
			//------

			//Draw the last part of the 2nd half of rectangle #1
			Real percentDraw = (Real)( percent - 13 ) / 12;
			m_2DRender->Add_Tri( Vector2( midX, midY ), Vector2( endX, midY ), Vector2( endX, startY + halfHeight * percentDraw ),
													 Vector2( 0, 0 ), Vector2( 0, 0 ), Vector2( 0, 0 ), color );
		}
	}
	else if( percent < 50 )
	{
		//25-49%
		//------

		//rectangle #3 & 4
		m_2DRender->Add_Rect( RectClass( startX, startY, midX, endY ), 0, 0, color );

		//Handle rectangle #2 that needs partial rendering.
		if( percent < 38 )
		{
			//25-37%
  		//-----

			//Draw the 2nd half of rectangle #2
			m_2DRender->Add_Tri( Vector2( midX, midY ), Vector2( midX, endY ), Vector2( endX, endY ),
													 Vector2( 0, 0 ), Vector2( 0, 0 ), Vector2( 0, 0 ), color );

			//Draw the last part of the 1st portion of rectangle #2
			Real percentDraw = (Real)( percent - 25 ) / 13;
			m_2DRender->Add_Tri( Vector2( midX, midY ), Vector2( endX, endY ), Vector2( endX, midY + halfHeight * percentDraw ),
													 Vector2( 0, 0 ), Vector2( 0, 0 ), Vector2( 0, 0 ), color );
		}
		else
		{
			//38-49%
			//------

			//Draw the last part of the 2nd half of rectangle #1
			Real percentDraw = (Real)( percent - 38 ) / 12;
			m_2DRender->Add_Tri( Vector2( midX, midY ), Vector2( midX, endY ), Vector2( endX - halfWidth * percentDraw, endY ),
													 Vector2( 0, 0 ), Vector2( 0, 0 ), Vector2( 0, 0 ), color );
		}
	}
	else if( percent < 75 )
	{
		//50-74%
		//------

		//Rectangle #4
		m_2DRender->Add_Rect( RectClass( startX, startY, midX, midY ), 0, 0, color );

		//Handle rectangle #3 that needs partial rendering.
		if( percent < 63 )
		{
			//50-62%
  		//-----

			//Draw the 2nd half of rectangle #3
			m_2DRender->Add_Tri( Vector2( midX, midY ), Vector2( startX, midY ), Vector2( startX, endY ),
													 Vector2( 0, 0 ), Vector2( 0, 0 ), Vector2( 0, 0 ), color );

			//Draw the last part of the 1st portion of rectangle #3
			Real percentDraw = (Real)( percent - 50 ) / 13;
			m_2DRender->Add_Tri( Vector2( midX, midY ), Vector2( startX, endY ), Vector2( midX - halfWidth * percentDraw, endY ),
													 Vector2( 0, 0 ), Vector2( 0, 0 ), Vector2( 0, 0 ), color );
		}
		else
		{
			//62-74%
			//------

			//Draw the last part of the 2nd half of rectangle #3
			Real percentDraw = (Real)( percent - 62 ) / 12;
			m_2DRender->Add_Tri( Vector2( midX, midY ), Vector2( startX, midY ), Vector2( startX, endY - halfHeight * percentDraw ),
													 Vector2( 0, 0 ), Vector2( 0, 0 ), Vector2( 0, 0 ), color );
		}
	}
	else
	{
		//75-99%
		//------

		//Handle rectangle #4 that needs partial rendering.
		if( percent < 87 )
		{
			//75-87%
  		//-----

			//Draw the 2nd half of rectangle #4
			m_2DRender->Add_Tri( Vector2( midX, midY ), Vector2( midX, startY ), Vector2( startX, startY ),
													 Vector2( 0, 0 ), Vector2( 0, 0 ), Vector2( 0, 0 ), color );

			//Draw the last part of the 1st portion of rectangle #4
			Real percentDraw = (Real)( percent - 75 ) / 13;
			m_2DRender->Add_Tri( Vector2( midX, midY ), Vector2( startX, startY ), Vector2( startX, midY - halfHeight * percentDraw ),
													 Vector2( 0, 0 ), Vector2( 0, 0 ), Vector2( 0, 0 ), color );
		}
		else
		{
			//88-99%
			//------

			//Draw the last part of the 2nd half of rectangle #4
			Real percentDraw = (Real)( percent - 88 ) / 12;
			m_2DRender->Add_Tri( Vector2( midX, midY ), Vector2( midX, startY ), Vector2( startX + halfWidth * percentDraw, startY ),
													 Vector2( 0, 0 ), Vector2( 0, 0 ), Vector2( 0, 0 ), color );
		}
	}

	if (!m_isBatching)
	{
		m_2DRender->Render();
	}
}


// W3DDisplay::drawImage ======================================================
/** Draws an images at the screen coordinates and keeps it within the end
	* screen coords specified */
//=============================================================================
void W3DDisplay::drawImage( const Image *image, Int startX, Int startY,
														Int endX, Int endY, Color color, DrawImageMode mode)
{

	// sanity
	if( image == nullptr )
		return;

	if (m_isClippedEnabled)
	{
		if (endX <= m_clipRegion.lo.x ||
			endY <= m_clipRegion.lo.y ||
			startX >= m_clipRegion.hi.x ||
			startY >= m_clipRegion.hi.y)
		{
			return;	//nothing to render
		}
	}

	// !!
	// Remember to update the GUIEditDisplay::drawImage when you make
	// changes to this, it technically uses W3D code to render itself,
	// but it not derived on the W3DDisplay
	// !!

	const Region2D *uv = image->getUV();

	TextureClass *tex = nullptr;
	if (BitIsSet(image->getStatus(), IMAGE_STATUS_RAW_TEXTURE))
		tex = (TextureClass *)(image->getRawTextureData());
	else
		tex = WW3DAssetManager::Get_Instance()->Get_Texture(image->getFilename().str(), MIP_LEVELS_1);

	Bool grayscale = (mode == DRAW_IMAGE_GRAYSCALE);
	setup2DRenderState(tex, mode, grayscale);

	RectClass screen_rect(startX,startY,endX,endY);
	RectClass uv_rect(uv->lo.x,uv->lo.y,uv->hi.x,uv->hi.y);

	if (m_isClippedEnabled)
	{
		//need to clip this quad to clip rectangle
		if (screen_rect.Left < m_clipRegion.lo.x || screen_rect.Right > m_clipRegion.hi.x || screen_rect.Top < m_clipRegion.lo.y || screen_rect.Bottom > m_clipRegion.hi.y)
		{
			RectClass clipped_rect;
			RectClass clipped_uv_rect;

			if( BitIsSet( image->getStatus(), IMAGE_STATUS_ROTATED_90_CLOCKWISE ) )
			{


				//
				//	Clip the polygons to the specified area
				//

				// GeneralsX @bugfix BenderAI 13/02/2026 Use MAX/MIN macros (cross-platform, defined in BaseTypeCore.h)
				clipped_rect.Left		= MAX (screen_rect.Left, m_clipRegion.lo.x);
				clipped_rect.Right	= MIN (screen_rect.Right, m_clipRegion.hi.x);
				clipped_rect.Top		= MAX (screen_rect.Top, m_clipRegion.lo.y);
				clipped_rect.Bottom	= MIN (screen_rect.Bottom, m_clipRegion.hi.y);

				//
				//	Clip the texture to the specified area
				//

				float percent				= ((clipped_rect.Left - screen_rect.Left) / screen_rect.Width ());
				clipped_uv_rect.Top		= uv_rect.Top + (uv_rect.Height () * percent);

				percent						= ((clipped_rect.Right - screen_rect.Left) / screen_rect.Width ());
				clipped_uv_rect.Bottom	= uv_rect.Top + (uv_rect.Height () * percent);

				percent						= ((clipped_rect.Top - screen_rect.Top) / screen_rect.Height ());
				clipped_uv_rect.Right	= uv_rect.Right - (uv_rect.Width () * percent);

				percent						= ((clipped_rect.Bottom - screen_rect.Top) / screen_rect.Height ());
				clipped_uv_rect.Left		= uv_rect.Right - (uv_rect.Width () * percent);
			}
			else

			{

				//
				//	Clip the polygons to the specified area
				//

				// GeneralsX @bugfix BenderAI 13/02/2026 Use MAX/MIN macros (cross-platform, defined in BaseTypeCore.h)
				clipped_rect.Left		= MAX (screen_rect.Left, m_clipRegion.lo.x);
				clipped_rect.Right	= MIN (screen_rect.Right, m_clipRegion.hi.x);
				clipped_rect.Top		= MAX (screen_rect.Top, m_clipRegion.lo.y);
				clipped_rect.Bottom	= MIN (screen_rect.Bottom, m_clipRegion.hi.y);

				//
				//	Clip the texture to the specified area
				//

				float percent				= ((clipped_rect.Left - screen_rect.Left) / screen_rect.Width ());
				clipped_uv_rect.Left		= uv_rect.Left + (uv_rect.Width () * percent);

				percent						= ((clipped_rect.Right - screen_rect.Left) / screen_rect.Width ());
				clipped_uv_rect.Right	= uv_rect.Left + (uv_rect.Width () * percent);

				percent						= ((clipped_rect.Top - screen_rect.Top) / screen_rect.Height ());
				clipped_uv_rect.Top		= uv_rect.Top + (uv_rect.Height () * percent);

				percent						= ((clipped_rect.Bottom - screen_rect.Top) / screen_rect.Height ());
				clipped_uv_rect.Bottom	= uv_rect.Top + (uv_rect.Height () * percent);
			}

			//
			//	Use the clipped rectangles to render
			//
			screen_rect = clipped_rect;
			uv_rect		= clipped_uv_rect;
		}
	}

	// if rotated 90 degrees clockwise we have to adjust the uv coords
	if( BitIsSet( image->getStatus(), IMAGE_STATUS_ROTATED_90_CLOCKWISE ) )
	{

		m_2DRender->Add_Tri( Vector2( screen_rect.Left, screen_rect.Top ),
												 Vector2( screen_rect.Left, screen_rect.Bottom ),
												 Vector2( screen_rect.Right, screen_rect.Top ),
												 Vector2( uv_rect.Right, uv_rect.Top),
												 Vector2( uv_rect.Left, uv_rect.Top),
												 Vector2( uv_rect.Right, uv_rect.Bottom ),
												 color );

		m_2DRender->Add_Tri( Vector2( screen_rect.Right, screen_rect.Bottom ),
												 Vector2( screen_rect.Right, screen_rect.Top ),
												 Vector2( screen_rect.Left, screen_rect.Bottom ),
												 Vector2( uv_rect.Left, uv_rect.Bottom ),
												 Vector2( uv_rect.Right, uv_rect.Bottom ),
												 Vector2( uv_rect.Left, uv_rect.Top ),
												 color );

	}
	else
	{

		// just draw as normal
		m_2DRender->Add_Quad( screen_rect, uv_rect, color );

	}

	if (!m_isBatching)
	{
		m_2DRender->Render();
		m_2DRender->Enable_Grayscale(false);
		if (mode == DRAW_IMAGE_ADDITIVE || mode == DRAW_IMAGE_SOLID)
		{
			m_2DRender->Enable_Alpha(true);
		}
	}

	if (tex != nullptr && !BitIsSet(image->getStatus(), IMAGE_STATUS_RAW_TEXTURE))
	{
		tex->Release_Ref();
	}

}

//============================================================================
// W3DDisplay::createVideoBuffer
//============================================================================

VideoBuffer*	W3DDisplay::createVideoBuffer()
{
	VideoBuffer::Type format = VideoBuffer::TYPE_UNKNOWN;

	/// @todo query video player for supported formats - we assume bink formats here

	// first try to use the native format

	WW3DFormat displayFormat = DX8Wrapper::getBackBufferFormat();

	if ( DX8Wrapper::Get_Current_Caps()->Support_Texture_Format( displayFormat ))
	{
		format = W3DVideoBuffer::W3DFormatToType( displayFormat );
	}

	if ( format == VideoBuffer::TYPE_UNKNOWN )
	{
		if ( DX8Wrapper::Get_Current_Caps()->Support_Texture_Format( WW3D_FORMAT_X8R8G8B8 ))
		{
			format = VideoBuffer::TYPE_X8R8G8B8;
		}
		else if ( DX8Wrapper::Get_Current_Caps()->Support_Texture_Format( WW3D_FORMAT_R8G8B8 ))
		{
			format = VideoBuffer::TYPE_R8G8B8;
		}
		else if ( DX8Wrapper::Get_Current_Caps()->Support_Texture_Format( WW3D_FORMAT_R5G6B5 ))
		{
			format = VideoBuffer::TYPE_R5G6B5;
		}
		else if ( DX8Wrapper::Get_Current_Caps()->Support_Texture_Format( WW3D_FORMAT_X1R5G5B5 ))
		{
			format = VideoBuffer::TYPE_X1R5G5B5;
		}
		else
		{
			// card does not support any of the formats we need
			return nullptr;
		}
	}
	// on low mem machines, render every video in 16bit
	if (TheGameLODManager && (!TheGameLODManager->didMemPass() || W3DShaderManager::getChipset() == DC_GEFORCE2))
		format = VideoBuffer::TYPE_R5G6B5;

	W3DVideoBuffer *buffer = NEW W3DVideoBuffer( format );

	return buffer;
}

//============================================================================
// W3DDisplay::drawScaledVideoBuffer
//============================================================================

void W3DDisplay::drawScaledVideoBuffer( VideoBuffer *buffer, VideoStreamInterface *stream )
{
	// TheSuperHackers @bugfix Mauller 20/07/2025 scale videos based on screen size so they are shown in their original aspect
	Real videoAspect = (Real)stream->width() / (Real)stream->height();
	Real displayAspect = (Real)getWidth() / (Real)getHeight();
	Bool wideAspect = displayAspect >= videoAspect;

	Int startX = 0;
	Int endX = 0;
	Int startY = 0;
	Int endY = 0;

	if (wideAspect)
	{
		// TheSuperHackers @info if we are in a wide aspect, we scale the videos width and fill the height
		Real heightScale = (Real)getHeight() / (Real)stream->height();
		startX = (getWidth() / 2.0f) - (stream->width() * heightScale / 2.0f);
		endX = (getWidth() / 2.0f) + (stream->width() * heightScale / 2.0f);

		endY = getHeight();
	}
	else
	{
		// TheSuperHackers @info if we are in a narrow aspect, we scale the videos height and fill the width
		Real widthScale = (Real)getWidth() / (Real)stream->width();
		startY = (getHeight() / 2.0f) - (stream->height() * widthScale / 2.0f);
		endY = (getHeight() / 2.0f) + (stream->height() * widthScale / 2.0f);

		endX = getWidth();
	}

	drawVideoBuffer( buffer, startX, startY, endX, endY );
}

//============================================================================
// W3DDisplay::drawVideoBuffer
//============================================================================

void W3DDisplay::drawVideoBuffer( VideoBuffer *buffer, Int startX, Int startY, Int endX, Int endY )
{
	W3DVideoBuffer *vbuffer = (W3DVideoBuffer*) buffer;

	setup2DRenderState(vbuffer->texture(), DRAW_IMAGE_ALPHA, FALSE);

	m_2DRender->Add_Quad( RectClass( startX, startY, endX, endY ),
												vbuffer->Rect( 0, 0, 1, 1) );
	
	if (!m_isBatching)
	{
		m_2DRender->Render();
	}

}

// W3DDisplay::setClipRegion ============================================
/** Set the clipping region for images.
  @todo: Make this work for all primitives, not just drawImage. */
//=============================================================================
void W3DDisplay::setClipRegion( IRegion2D *region )
{
		// assign new region
		m_clipRegion = *region;
		m_isClippedEnabled = TRUE;

}

//=============================================================================
/* we don't really need to override this call, since we will soon be called to
	update every shroud cell explicitly...
*/
void W3DDisplay::clearShroud()
{
	// nothing
}

//=============================================================================
void W3DDisplay::setBorderShroudLevel(UnsignedByte level)
{
	if (TheTerrainRenderObject && TheTerrainRenderObject->getShroud())
	{
		TheTerrainRenderObject->getShroud()->setBorderShroudLevel((W3DShroudLevel)level);
	}
}

//=============================================================================
void W3DDisplay::setShroudLevel( Int x, Int y, CellShroudStatus setting )
{
	if (TheTerrainRenderObject && TheTerrainRenderObject->getShroud())
	{
		#ifdef INTENSE_DEBUG
		TheTerrainRenderObject->getShroud()->setShroudFilter(false);
		#endif
		if( setting == CELLSHROUD_SHROUDED )
			TheTerrainRenderObject->getShroud()->setShroudLevel(x, y, (W3DShroudLevel)TheGlobalData->m_shroudAlpha );
		else if( setting == CELLSHROUD_FOGGED )
			TheTerrainRenderObject->getShroud()->setShroudLevel(x, y, (W3DShroudLevel)TheGlobalData->m_fogAlpha );///< @todo placeholder to get feedback on logic work while graphic side being decided
		else
			TheTerrainRenderObject->getShroud()->setShroudLevel(x, y, (W3DShroudLevel)TheGlobalData->m_clearAlpha );
		//Logic is saying shroud.  We can add alpha levels here in client if needed.
		// W3DShroud is a 0-255 alpha byte.  Logic shroud is a double reference count.

		TheTerrainRenderObject->notifyShroudChanged();

	}
}


/** Start/Stop capturing an AVI movie*/
void W3DDisplay::toggleMovieCapture()
{
	WW3D::Toggle_Movie_Capture("Movie",30);
}

void W3DDisplay::takeScreenShot(ScreenshotFormat format, Int jpegQuality)
{
	W3D_TakeCompressedScreenshot(format, jpegQuality);
}


#if defined(RTS_DEBUG)

static FILE *AssetDumpFile=nullptr;

void dumpMeshAssets(MeshClass *mesh)
{
	if (mesh)
	{
		TextureClass *texture;
		//MaterialInfoClass	*material = mesh->Get_Material_Info();
		MeshModelClass *model=mesh->Get_Model();
		for (int stage=0;stage<MeshMatDescClass::MAX_TEX_STAGES;++stage)
		{
			for (int pass=0;pass<model->Get_Pass_Count();++pass)
			{
				if (model->Has_Texture_Array(pass,stage))
				{
					for (int i=0;i<model->Get_Polygon_Count();++i)
					{
						if ((texture=model->Peek_Texture(i,pass,stage)) != nullptr)
						{
							fprintf(AssetDumpFile,"\t%s\n",texture->Get_Texture_Name().str());
						}
					}
				}
				else
				{
					if ((texture=model->Peek_Single_Texture(pass,stage)) != nullptr)
					{
						fprintf(AssetDumpFile,"\t%s\n",texture->Get_Texture_Name().str());
					}
				}
			}
		}
	}
}

void dumpHLODAssets(HLodClass *hlod)
{
	if (hlod)
	{
		//model composed of multiple meshes.
		for (Int i=0; i<hlod->Get_Num_Sub_Objects(); i++)
		{
			RenderObjClass *subObj=hlod->Get_Sub_Object(i);
			if (subObj->Class_ID() == RenderObjClass::CLASSID_HLOD)
				dumpHLODAssets((HLodClass *)subObj);
			else
			if (subObj->Class_ID() == RenderObjClass::CLASSID_MESH)
				dumpMeshAssets((MeshClass *)subObj);
		}
	}
}

//-------------------------------------------------------------------------------------------------
/**  dump all used models/textures to a file.*/
//-------------------------------------------------------------------------------------------------
void W3DDisplay::dumpModelAssets(const char *path)
{
	if (m_3DScene)
	{
		AssetDumpFile=fopen(path,"w");
		if (AssetDumpFile)
		{
			fprintf(AssetDumpFile,"Models and Textures used on %s:\n\n",TheGlobalData->m_mapName.str());
			SceneIterator *sceneIter = m_3DScene->Create_Iterator();
			sceneIter->First();
			while(!sceneIter->Is_Done())
			{
				RenderObjClass * robj = sceneIter->Current_Item();
				if (robj->Class_ID() == RenderObjClass::CLASSID_HLOD)
				{	fprintf(AssetDumpFile,"%s.W3D:\n",robj->Get_Name());
					dumpHLODAssets((HLodClass *)robj);
				}
				else
				if (robj->Class_ID() == RenderObjClass::CLASSID_MESH)
				{	fprintf(AssetDumpFile,"%s.W3D:\n",robj->Get_Name());
					dumpMeshAssets((MeshClass *)robj);
				}
				sceneIter->Next();
			}
			m_3DScene->Destroy_Iterator(sceneIter);
			fclose(AssetDumpFile);
		}
	}
}
#endif	//only include above code in debug and internal
//-------------------------------------------------------------------------------------------------
/** Preload using the W3D asset manager the model referenced by the string parameter */
//-------------------------------------------------------------------------------------------------
void W3DDisplay::preloadModelAssets( AsciiString model )
{

	if( m_assetManager )
	{
		AsciiString nameWithExtension;

		nameWithExtension.format( "%s.w3d", model.str() );
		m_assetManager->Load_3D_Assets( nameWithExtension.str() );

	}

}

//-------------------------------------------------------------------------------------------------
/** Preload using the W3D asset manager the texture referenced by the string parameter */
//-------------------------------------------------------------------------------------------------
void W3DDisplay::preloadTextureAssets( AsciiString texture )
{

	if( m_assetManager )
	{
		TextureClass *theTexture = m_assetManager->Get_Texture( texture.str() );
		theTexture->Release_Ref();//release reference
	}

}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
void W3DDisplay::doSmartAssetPurgeAndPreload(const char* usageFileName)
{
	if (!m_assetManager || !usageFileName || !*usageFileName)
		return;

	DynamicVectorClass<StringClass> names(8000);

	// use TheFileSystem here so we can bigify these files
	File* f = TheFileSystem->openFile(usageFileName, File::READ | File::TEXT);
	if (f)
	{
		for (;;)
		{
			AsciiString tmp;
			if (f->scanString(tmp) == FALSE)
				break;

			// allow for comments in the file. Note that this doesn't allow for comments
			// with spaces! doh. oh well. better than nothing.
			if (tmp.str()[0] == ';')
				continue;

			names.Add(StringClass(tmp.str()));
		}
		f->close();
	}

	// just free everything if there's no exclusion list file (send in an empty list)
	m_assetManager->Free_Assets_With_Exclusion_List(names);
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
#if defined(RTS_DEBUG)
void W3DDisplay::dumpAssetUsage(const char* mapname)
{
	if (!m_assetManager || !mapname || !*mapname)
		return;

	DynamicVectorClass<StringClass> names(8000);
	m_assetManager->Create_Asset_List(names);

	const char* leafname = strrchr(mapname, '\\');
	if (leafname)
		++leafname;					// point to first character after the last backslash
	else
		leafname = mapname;		// point to the start of the filename

	char buf[256];
	int idx = 1;
	while (true)
	{
		sprintf(buf, "AssetUsage_%s_%04d.txt",leafname,idx);
		if (_access(buf, 0) != 0)
			break;	// it exists, we're good
		++idx;
	}

	FILE *fp = fopen(buf, "w");
	if (fp)
	{
		for (int i=0; i<names.Count(); i++)
		{
			const char* n = names[i];
			fprintf(fp, "%s\n", n);
		}
		fclose(fp);
	}
}
#endif

//-------------------------------------------------------------------------------------------------
static void drawFramerateBar()
{
	static DWORD prevTime = timeGetTime();
	DWORD now = timeGetTime();
	Real percTime = (1000.0f / (now - prevTime) ) / (1000.0f / TheGlobalData->m_framesPerSecondLimit);

	if (percTime > 1.0f)
		percTime = 1.0f;
	else if (percTime < 0.0f)
		percTime = 0.0f;
	Int width = REAL_TO_INT(percTime * TheDisplay->getWidth());
	UnsignedInt colorToUse = GameMakeColor( REAL_TO_UNSIGNEDBYTE((1.0f - percTime) * 255),
																					REAL_TO_UNSIGNEDBYTE(percTime * 255),
																					0,
																					0x7F);

	TheDisplay->drawFillRect(1, 1, width, 15, colorToUse);
	prevTime = now;
}
