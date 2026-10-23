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

// FILE: W3DSnow.h /////////////////////////////////////////////////////////

#include "W3DDevice/GameClient/W3DSnow.h"
#include "W3DDevice/GameClient/HeightMap.h"
#include "GameClient/View.h"
#include "WW3D2/dx8wrapper.h"
#include "WW3D2/rinfo.h"
#include "WW3D2/camera.h"
#include "WW3D2/assetmgr.h"

// GeneralsX @feature PS3.0-P3-26 14/10/2026 V2: snow receives scene light + per-flake sway phase.
#include "Common/GlobalData.h"
// GeneralsX @feature PS3.0 Weather W1/W3 16/10/2026: the render-side weather scalars (tint, flash
// envelope) that tint the whole particle batch, plus the recipe profile accessors.
#include "GameClient/WeatherFX.h"

// GeneralsX @feature PS3.0-P3-26: deterministic per-flake sway phase in [0,2pi). The retail phase is
// the emitter column index, so every flake in a column sways as one sheet (visible banding). A cheap
// integer hash decorrelates neighbours while staying frame-stable for the same x,y. salt picks the
// X or Y channel phase so they do not move in lockstep either.
static Real Snow_Sway_Phase(Int x, Int y, Int salt)
{
	UnsignedInt h = (UnsignedInt)(x + salt) * 73856093u ^ (UnsignedInt)(y - salt) * 19349663u;
	h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
	return (Real)(h & 0xffffu) * (6.2831853f / 65536.0f);
}

static bool Snow_Sway_On()
{
	static int cached = -1;
	if (cached < 0) {
		cached = 1;
		const char* ev = getenv("GENERALSX_SNOW_SWAY");
		if (ev != nullptr && *ev != 0) cached = (strcmp(ev, "0") != 0) ? 1 : 0;
	}
	return cached == 1;
}

// GeneralsX @feature PS3.0-P3-26: quad vertex tint from the same time-of-day pair the tracks use
// (terrain ambient + half key). Retail leaves it 0xffffffff so snow ignores the scene light entirely.
// GENERALSX_SNOW_LIT=0 restores 0xffffffff bit-for-bit.
// GeneralsX @feature PS3.0 Weather W1/W3 16/10/2026: the same tint is now shared by every weather
// primitive and gains two weather terms:
//   - the recipe's own particle tint (cold blue for rain, bright white for a blizzard), and
//   - the lightning envelope, so the falling sheet brightens for the two frames a bolt burns.
// The per-channel floor is the recipe's MinSceneShade (0.45 = the old literal, so the retail snow
// look is unchanged). Particles are drawn PRELIT_DIFFUSE (the vertex color IS the final color), so
// this is a whole-batch tint decided once per frame rather than per-flake lighting - per-particle
// lighting is not expressible in the fixed pipeline here.
static void Weather_Shade_RGB(Real shade[3])
{
	shade[0] = shade[1] = shade[2] = 1.0f;

	static bool litOn = [] {
		const char* ev = getenv("GENERALSX_SNOW_LIT");
		return ev == nullptr || strcmp(ev, "0") != 0;
	}();
	if (!litOn || !TheGlobalData || TheWeatherSetting == nullptr) return;

	shade[0] = TheGlobalData->m_terrainAmbient[0].red + TheGlobalData->m_terrainDiffuse[0].red * 0.5f;
	shade[1] = TheGlobalData->m_terrainAmbient[0].green + TheGlobalData->m_terrainDiffuse[0].green * 0.5f;
	shade[2] = TheGlobalData->m_terrainAmbient[0].blue + TheGlobalData->m_terrainDiffuse[0].blue * 0.5f;

	// GeneralsX @feature PS3.0 Weather W1: recipe tint, then the floor/clamp.
	shade[0] *= TheWeatherSetting->m_particleTint.red;
	shade[1] *= TheWeatherSetting->m_particleTint.green;
	shade[2] *= TheWeatherSetting->m_particleTint.blue;

	const Real flash = weatherRenderState().m_flash * 0.85f;
	for (Int i = 0; i < 3; i++) {
		shade[i] += flash;
		if (shade[i] < TheWeatherSetting->m_minShade) shade[i] = TheWeatherSetting->m_minShade;
		if (shade[i] > 1.0f) shade[i] = 1.0f;
	}
}

static UnsignedInt Weather_Tint_RGB()
{
	Real shade[3];
	Weather_Shade_RGB(shade);
	const UnsignedInt r = (UnsignedInt)(shade[0] * 255.0f);
	const UnsignedInt g = (UnsignedInt)(shade[1] * 255.0f);
	const UnsignedInt b = (UnsignedInt)(shade[2] * 255.0f);
	return (r << 16) | (g << 8) | b;
}

static UnsignedInt Snow_Lit_Color()
{
	static bool litOn = [] {
		const char* ev = getenv("GENERALSX_SNOW_LIT");
		return ev == nullptr || strcmp(ev, "0") != 0;
	}();
	if (!litOn || !TheGlobalData) return 0xffffffff;
	return 0xff000000u | Weather_Tint_RGB();
}

// GeneralsX @feature PS3.0 Weather W3 16/10/2026: rain streak color = the shared batch tint plus this
// drop's own alpha (opacity ceiling x distance falloff). Only the quad streak path can do that: the
// point-sprite path has no per-particle alpha at all (FVF is XYZ only), which is why distant snow
// relies on box culling and the screen layer instead.
static UnsignedInt Rain_Streak_Color(Real alphaScale)
{
	Real a = alphaScale;
	if (a < 0.0f) a = 0.0f;
	if (a > 1.0f) a = 1.0f;
	const UnsignedInt alpha = (UnsignedInt)(a * 255.0f);
	return (alpha << 24) | Weather_Tint_RGB();
}

// GeneralsX @feature PS3.0 Weather W3 16/10/2026: half-width, in texture units, of the band the streak
// ribbon samples out of the rain sprite. EXRainDrop's opaque bar sits in columns 14..17 of 32 (u
// 0.4375..0.5625); 0.07 covers it with the sprite's own soft shoulders and nothing else.
static const Real RAIN_STREAK_U_BAND = 0.07f;

// GeneralsX @feature PS3.0 Weather W3 16/10/2026: which primitive the active recipe wants, resolved
// the same way in render() and in ReAcquireResources() - the two must agree or the pass would draw
// with a buffer that was never allocated for it.
static bool Weather_Use_Point_Sprites()
{
	// GeneralsX @bugfix Qoder 21/10/2026: the pure-Vulkan fixed-function point path sizes gl_PointSize
	// from the D3D point registers and the distance curve, which on this backend renders every snow
	// particle as a screen-filling white square (normal snow and blizzard both use PARTICLE_SNOW).
	// Rain already uses the world-space quad/streak path and looks correct. Force snow onto the retail
	// quad fallback, whose size is the authored SnowQuadSize in world units; the D3D9 build keeps its
	// original point-sprite path.
#if defined(SAGE_USE_VULKAN_CORE)
	return false;
#else
	return TheWeatherSetting != nullptr
		&& TheWeatherSetting->m_particleKind != WeatherSetting::PARTICLE_RAIN
		&& TheWeatherSetting->m_usePointSprites
		&& DX8Wrapper::Get_Current_Caps()->Support_PointSprites();
#endif
}



#define D3DFVF_POINTVERTEX (D3DFVF_XYZ)
#define SNOW_BUFFER_SIZE 4096	//size of vertex buffer holding particles.
#define SNOW_BATCH_SIZE	2048	//we render at most this many particles per drawprimitive call.  This number * 6 must be less than 65536 to fit into index buffer.

struct POINTVERTEX
{
    Vector3 v;	//center of particle.
};

W3DSnowManager::W3DSnowManager()
{
	m_indexBuffer=nullptr;
	m_snowTexture=nullptr;
	m_VertexBufferD3D=nullptr;
}

W3DSnowManager::~W3DSnowManager()
{
	ReleaseResources();
}

void W3DSnowManager::init()
{
	SnowManager::init();
	ReAcquireResources();
}

/** Releases all W3D/D3D assets before a reset.. */
void W3DSnowManager::ReleaseResources()
{
	REF_PTR_RELEASE(m_snowTexture);

	if (m_VertexBufferD3D)
		m_VertexBufferD3D->Release();

	m_VertexBufferD3D=nullptr;

	REF_PTR_RELEASE(m_indexBuffer);
}

/** (Re)allocates all W3D/D3D assets after a reset.. */
Bool W3DSnowManager::ReAcquireResources()
{
	ReleaseResources();

	if (!TheWeatherSetting->wantsParticles())
		return TRUE;	//no need for resources if no weather is drawing.

	// GeneralsX @feature PS3.0 Weather W3 16/10/2026: rain always uses the quad streak path, so it
	// needs the index buffer even on cards with point sprites. Uses the same predicate as the render
	// pass so the buffer that gets allocated is always the one that gets drawn with.
	if (Weather_Use_Point_Sprites())
	{
		IDirect3DDevice9* m_pDev=DX8Wrapper::_Get_D3D_Device8();

		DEBUG_ASSERTCRASH(m_pDev, ("Trying to ReAcquireResources on W3DSnowManager without device"));

		if (m_VertexBufferD3D == nullptr)
		{
			// Create vertex buffer

			if (FAILED(m_pDev->CreateVertexBuffer
			(
				SNOW_BUFFER_SIZE*sizeof(POINTVERTEX),
				D3DUSAGE_WRITEONLY|D3DUSAGE_DYNAMIC|D3DUSAGE_POINTS,
				D3DFVF_POINTVERTEX,
				D3DPOOL_DEFAULT,
				&m_VertexBufferD3D,
				nullptr
			)))
				return FALSE;
		}
	}
	else
	{
		m_indexBuffer=NEW_REF(DX8IndexBufferClass,(SNOW_BATCH_SIZE *6));	//allocate 2 triangles per flake, each with 3 indices.

		// Fill up the IB with static vertex indices that will be used for all smudges.
		{
			DX8IndexBufferClass::WriteLockClass lockIdxBuffer(m_indexBuffer);
			UnsignedShort *ib=lockIdxBuffer.Get_Index_Array();
			//quad of 4 triangles:
			//	0-----3
			//  |\   /|
			//  |  X  |
			//	|/   \|
			//  1-----2
			Int vbCount=0;
			for (Int i=0; i<SNOW_BATCH_SIZE; i++)
			{
				//Top
				ib[0]=vbCount+3;
				ib[1]=vbCount;
				ib[2]=vbCount+2;
				//Bottom
				ib[3]=vbCount+2;
				ib[4]=vbCount;
				ib[5]=vbCount+1;

				vbCount += 4;
				ib+=6;
			}
		}
	}

	m_snowTexture = WW3DAssetManager::Get_Instance()->Get_Texture(TheWeatherSetting->m_snowTexture.str());

	m_dwBase = SNOW_BUFFER_SIZE;
	m_dwDiscard = SNOW_BUFFER_SIZE;
	m_dwFlush = SNOW_BATCH_SIZE;

	return TRUE;
}

void W3DSnowManager::updateIniSettings()
{
	//Call base class
	SnowManager::updateIniSettings();

	if (m_snowTexture && stricmp(m_snowTexture->Get_Texture_Name(),TheWeatherSetting->m_snowTexture.str()) != 0)
	{
		REF_PTR_RELEASE(m_snowTexture);
		m_snowTexture = WW3DAssetManager::Get_Instance()->Get_Texture(TheWeatherSetting->m_snowTexture.str());
	}
}

void W3DSnowManager::reset()
{
	SnowManager::reset();
}

void W3DSnowManager::update()
{
	// GeneralsX @bugfix Storm996 16/10/2026 PS3.0 Weather W1: the device class shadows the base
	// update(), so the weather mirror (logic value -> active recipe) and the render-side scalars
	// (WeatherFX) must be driven from HERE - GameClient only ever calls this override.
	SnowManager::update();

	// TheSuperHackers @tweak The snow render update is now decoupled from the logic step.
	m_time += WW3D::Get_Logic_Frame_Time_Seconds();

	//find current time offset, adjusting for overflow
	m_time=fmod(m_time,m_fullTimePeriod);
}

#define MAXIMUM_CAMERA_DISTANCE 100000	//maximum distance of camera position from world origin.
#define ISPOW2(x)  (x && (x & (x-1)) == 0)	//is a number a power of 2?
#define MODPOW2(x,y) ((x) & (y-1))		//mod '%' operator for powers of 2.

// Helper function to stuff a FLOAT into a DWORD argument
inline DWORD FtoDW( FLOAT f ) { return *((DWORD*)&f); }

/*Recursively subdivide the large snow box enclosing the camera until we reach some predefined leaf size.  This
method is used so that very few off-screen particles end up getting rendered.  Culling them individually would
be too expensive since we're dealing with 1000's for this effect.*/
void W3DSnowManager::renderSubBox(RenderInfoClass &rinfo, Int originX, Int originY, Int cubeDimX, Int cubeDimY )
{
	//check if this box is too large and needs subdivision
	Int boxDimX=cubeDimX - originX;
	Int boxDimY=cubeDimY - originY;
	Int halfX=REAL_TO_INT_CEIL(boxDimX*0.5f);
	Int halfY=REAL_TO_INT_CEIL(boxDimY*0.5f);

	CameraClass &camera=rinfo.Camera;
	MinMaxAABoxClass mmbox;

	if (boxDimX > m_leafDim)
	{
		//subdivide the box
		if (boxDimY > m_leafDim)
		{
			//subdivide in both directions
			//Upper left
			mmbox.MinCorner.Set(originX*m_emitterSpacing-m_cullOverscan, (originY + halfY)*m_emitterSpacing-m_cullOverscan, m_snowCeiling-m_boxDimensions);
			mmbox.MaxCorner.Set((originX + halfX)*m_emitterSpacing+m_cullOverscan, cubeDimY*m_emitterSpacing+m_cullOverscan, m_snowCeiling);
			if (CollisionMath::Overlap_Test(camera.Get_Frustum(),mmbox) != CollisionMath::OUTSIDE)
				renderSubBox(rinfo, originX, originY + halfY, originX + halfX, cubeDimY);
			//Upper right
			mmbox.MinCorner.Set((originX + halfX)*m_emitterSpacing-m_cullOverscan, (originY + halfY)*m_emitterSpacing-m_cullOverscan, m_snowCeiling-m_boxDimensions);
			mmbox.MaxCorner.Set(cubeDimX*m_emitterSpacing+m_cullOverscan, cubeDimY*m_emitterSpacing+m_cullOverscan, m_snowCeiling);
			if (CollisionMath::Overlap_Test(camera.Get_Frustum(),mmbox) != CollisionMath::OUTSIDE)
				renderSubBox(rinfo, originX + halfX, originY + halfY,cubeDimX, cubeDimY);
			//Lower left
			mmbox.MinCorner.Set(originX*m_emitterSpacing-m_cullOverscan, originY*m_emitterSpacing-m_cullOverscan, m_snowCeiling-m_boxDimensions);
			mmbox.MaxCorner.Set((originX + halfX)*m_emitterSpacing+m_cullOverscan, (originY + halfY)*m_emitterSpacing+m_cullOverscan, m_snowCeiling);
			if (CollisionMath::Overlap_Test(camera.Get_Frustum(),mmbox) != CollisionMath::OUTSIDE)
				renderSubBox(rinfo, originX,originY,originX + halfX, originY + halfY);
			//Lower right
			mmbox.MinCorner.Set((originX + halfX)*m_emitterSpacing-m_cullOverscan, originY*m_emitterSpacing-m_cullOverscan, m_snowCeiling-m_boxDimensions);
			mmbox.MaxCorner.Set(cubeDimX*m_emitterSpacing+m_cullOverscan, (originY + halfY)*m_emitterSpacing+m_cullOverscan, m_snowCeiling);
			if (CollisionMath::Overlap_Test(camera.Get_Frustum(),mmbox) != CollisionMath::OUTSIDE)
				renderSubBox(rinfo, originX + halfX, originY, cubeDimX, originY + halfY);
			return;
		}
		else
		{
			//only subdivide in x direction.
			//Left
			mmbox.MinCorner.Set(originX*m_emitterSpacing-m_cullOverscan, originY*m_emitterSpacing-m_cullOverscan, m_snowCeiling-m_boxDimensions);
			mmbox.MaxCorner.Set((originX + halfX)*m_emitterSpacing+m_cullOverscan, cubeDimY*m_emitterSpacing+m_cullOverscan, m_snowCeiling);
			if (CollisionMath::Overlap_Test(camera.Get_Frustum(),mmbox) != CollisionMath::OUTSIDE)
				renderSubBox(rinfo, originX, originY, originX + halfX, cubeDimY);
			//Right
			mmbox.MinCorner.Set((originX + halfX)*m_emitterSpacing-m_cullOverscan, originY*m_emitterSpacing-m_cullOverscan, m_snowCeiling-m_boxDimensions);
			mmbox.MaxCorner.Set(cubeDimX*m_emitterSpacing+m_cullOverscan, cubeDimY*m_emitterSpacing+m_cullOverscan, m_snowCeiling);
			if (CollisionMath::Overlap_Test(camera.Get_Frustum(),mmbox) != CollisionMath::OUTSIDE)
				renderSubBox(rinfo, originX + halfX, originY, cubeDimX, cubeDimY);
			return;
		}
	}
	else
	if (boxDimY > m_leafDim)
	{
		//only subdivide in y direction
		//Top
		mmbox.MinCorner.Set(originX*m_emitterSpacing-m_cullOverscan, (originY+halfY)*m_emitterSpacing-m_cullOverscan, m_snowCeiling-m_boxDimensions);
		mmbox.MaxCorner.Set(cubeDimX*m_emitterSpacing+m_cullOverscan, cubeDimY*m_emitterSpacing+m_cullOverscan, m_snowCeiling);
		if (CollisionMath::Overlap_Test(camera.Get_Frustum(),mmbox) != CollisionMath::OUTSIDE)
			renderSubBox(rinfo, originX, originY+halfY,cubeDimX, cubeDimY);
		//Bottom
		mmbox.MinCorner.Set(originX*m_emitterSpacing-m_cullOverscan, originY*m_emitterSpacing-m_cullOverscan, m_snowCeiling-m_boxDimensions);
		mmbox.MaxCorner.Set(cubeDimX*m_emitterSpacing+m_cullOverscan, (originY + halfY)*m_emitterSpacing+m_cullOverscan, m_snowCeiling);
		if (CollisionMath::Overlap_Test(camera.Get_Frustum(),mmbox) != CollisionMath::OUTSIDE)
			renderSubBox(rinfo, originX, originY, cubeDimX, originY + halfY);
		return;
	}

	//Box too small to subdivide so render it.

	//Find total number of particles that need rendering.
	Int totalPart=(cubeDimY-originY)*(cubeDimX-originX);

	if (!totalPart)
		return;	//nothing to render.

	Int y=originY;	//loop counter.
	Int cubeOriginXRemainder = originX;	//loop counter - adjusted when not all particles fit into render buffer.
	Vector3 snowCenter;

	// GeneralsX @feature PS3.0 Weather W3 16/10/2026: wind carry for the sprite path. Both values are
	// 0 in the retail recipe, so every position below stays bit-identical; a blizzard recipe (WindX=16)
	// uses them to rake the sheet across the box as it falls. Read once per leaf box, applied per flake.
	const Real windX = TheWeatherSetting != nullptr ? TheWeatherSetting->m_windX : 0.0f;
	const Real windY = TheWeatherSetting != nullptr ? TheWeatherSetting->m_windY : 0.0f;
	const Real boxDim = m_boxDimensions > 0.0001f ? m_boxDimensions : 1.0f;

	m_totalRendered += totalPart;

	while (totalPart)
	{
		Int batchSize=totalPart;

		if (batchSize > m_dwFlush)
			batchSize = m_dwFlush;

		if((m_dwBase + batchSize) > m_dwDiscard)
			m_dwBase = 0;

		POINTVERTEX* verts;

		if(m_VertexBufferD3D->Lock(m_dwBase * sizeof(POINTVERTEX), batchSize * sizeof(POINTVERTEX),
			(void**) &verts, m_dwBase ? D3DLOCK_NOOVERWRITE : D3DLOCK_DISCARD) != D3D_OK )
			return;	//couldn't lock buffer.

		Int numberInBatch=0;

		for (;y<cubeDimY; y++)
		{
			for (Int x=cubeOriginXRemainder; x<cubeDimX; x++)
			{
				if (numberInBatch >= batchSize)
				{	cubeOriginXRemainder = x;
					goto flush_particles;
				}

				//Get initial height from noise table.  We add a large value to make sure it's positive.  Then
				//modulate by table dimensions to find a value.
				Int noiseOffset=MODPOW2(x+MAXIMUM_CAMERA_DISTANCE,SNOW_NOISE_X)+MODPOW2(y+MAXIMUM_CAMERA_DISTANCE,SNOW_NOISE_Y)*SNOW_NOISE_X;
				if (noiseOffset > (SNOW_NOISE_X * SNOW_NOISE_Y))
					noiseOffset = 0;	//this should never happen but check to prevent buffer over/under flow.

				//find current height
				Real h0=m_snowCeiling-fmod(m_heightTraveled+m_startingHeights[noiseOffset],m_boxDimensions);

				//find world-space position of snow flake
				snowCenter.Set(x*m_emitterSpacing,y*m_emitterSpacing,h0);

				// GeneralsX @feature PS3.0 Weather W3 16/10/2026: the wind carries the flake sideways in
				// proportion to how far it has fallen. Skipped entirely at zero wind (the retail recipe).
				if (windX != 0.0f || windY != 0.0f)
				{
					const Real fallFrac = (m_snowCeiling - h0) / boxDim;
					snowCenter.X += windX * fallFrac;
					snowCenter.Y += windY * fallFrac;
				}

				//Adjust position so snow flakes don't fall straight down.
				// GeneralsX @feature PS3.0-P3-26: per-flake phase (GENERALSX_SNOW_SWAY=0 restores the
				// retail per-column phase bit-for-bit).
				const Real phaseX = Snow_Sway_On() ? Snow_Sway_Phase(x, y, 0) : (Real)x;
				const Real phaseY = Snow_Sway_On() ? Snow_Sway_Phase(x, y, 17) : (Real)y;
				snowCenter.X += m_amplitude * WWMath::Fast_Sin( h0 * m_frequencyScaleX + phaseX);
				snowCenter.Y += m_amplitude * WWMath::Fast_Sin( h0 * m_frequencyScaleY + phaseY);

				*(Vector3 *)verts=snowCenter;
				verts++;

				numberInBatch++;
			}
			//getting here means we did not overflow the render buffer, so reset x origin to normal.
			cubeOriginXRemainder = originX;	//reset to normal amount
		}

flush_particles:
		// GeneralsX @feature Qoder 18/10/2026 step 9.I: hand the batch to the Vulkan facade while the
		// lock still names the memory it wrote. The facade copies the vertices into its scratch ring
		// (VK_Draw_User_Primitive_State), so nothing here depends on a mirrored D3D9 vertex buffer: the
		// bare SetStreamSource only exists for the refusal path below.
		bool vkSubmitted = false;
		if (numberInBatch)
		{
			vkSubmitted = DX8Wrapper::Draw_User_Primitive(D3DPT_POINTLIST, (unsigned)numberInBatch,
				verts - numberInBatch, sizeof(POINTVERTEX));
		}
		m_VertexBufferD3D->Unlock();
		//Render any particles that may be queued up.
		if (numberInBatch)
		{
			{
				static unsigned s_snowProbe = 0;
				if (s_snowProbe < 8) {
					fprintf(stderr, "[SNOWPROBE] points vk=%d n=%d\n", (int)vkSubmitted, (int)numberInBatch);
					fflush(stderr);
					++s_snowProbe;
				}
			}
			Debug_Statistics::Record_DX8_Polys_And_Vertices(numberInBatch*2,numberInBatch*4,ShaderClass::_PresetOpaqueShader);
			if (!vkSubmitted)
			{
				DX8Wrapper::_Get_D3D_Device8()->SetStreamSource( 0, m_VertexBufferD3D, 0, sizeof(POINTVERTEX) );
				DX8Wrapper::_Get_D3D_Device8()->DrawPrimitive( D3DPT_POINTLIST, m_dwBase, numberInBatch);
			}
			totalPart -= numberInBatch;
			m_dwBase += numberInBatch;
		}
	}
}

void W3DSnowManager::render(RenderInfoClass &rinfo)
{
	// GeneralsX @feature PS3.0 Weather W1/W3 16/10/2026: the active recipe now decides both whether
	// anything draws and which primitive it wants. wantsParticles() already carries the
	// GENERALSX_WEATHER=0 kill leg, so the retail A/B leg short-circuits here.
	if (!TheWeatherSetting->wantsParticles() || !m_isVisible)
		return;

	const Bool isRain = (TheWeatherSetting->m_particleKind == WeatherSetting::PARTICLE_RAIN);
	const Bool usePointSprites = isRain ? FALSE : Weather_Use_Point_Sprites();

	//make sure the noise table is powers of 2 in dimensions.
	WWASSERT(ISPOW2(SNOW_NOISE_X) && ISPOW2(SNOW_NOISE_Y));

	//CameraClass &camera=rinfo.Camera;

	const Coord3D &cPos=TheTacticalView->get3DCameraPosition();
	Vector3 camPos(cPos.x,cPos.y,cPos.z);

	//Number of emitters from cube center to edge of visible extent.
	Int mumEmittersInHalf=(Int)floor(m_boxDimensions / m_emitterSpacing * 0.5f);

	//Find origin of visible cube surrounding camera.
	Int cubeCenterX=(Int)floor(camPos.X/m_emitterSpacing);
	Int cubeCenterY=(Int)floor(camPos.Y/m_emitterSpacing);

	//Find extents of visible cube surrounding camera.
	Int cubeOriginX=cubeCenterX - mumEmittersInHalf;	//top/left extents.
	Int cubeOriginY=cubeCenterY - mumEmittersInHalf;
	Int cubeDimX=cubeCenterX + mumEmittersInHalf;		//bottom/right extents.
	Int cubeDimY=cubeCenterY + mumEmittersInHalf;

 	const FrustumClass & frustum = rinfo.Camera.Get_Frustum();
	AABoxClass bbox;

	//Get a bounding box around our visible universe.  Bounded by terrain and the sky
	//so much tighter fitting volume than what's actually visible.  This will cull
	//particles falling under the ground.

 	TheTerrainRenderObject->getMaximumVisibleBox(frustum, &bbox, TRUE);

	//Particles move outside the visible box as a result of local sine movement
	//so adjust bounding box to include them.
	bbox.Extent.X += m_amplitude+m_quadSize;
	bbox.Extent.Y += m_amplitude+m_quadSize;

	// GeneralsX @feature PS3.0 Weather W3 16/10/2026: rain drifts sideways by up to the recipe's wind
	// over one fall cycle, so the clip box has to leave room for it or the slanted sheet gets cut.
	if (isRain)
	{
		bbox.Extent.X += fabsf(TheWeatherSetting->m_windX);
		bbox.Extent.Y += fabsf(TheWeatherSetting->m_windY);
	}

	//Clip our visible snow rendering box
	if ((cubeOriginX * m_emitterSpacing ) < (bbox.Center.X - bbox.Extent.X))
		cubeOriginX = (Int)floor ((bbox.Center.X - bbox.Extent.X)/m_emitterSpacing);

	if ((cubeOriginY * m_emitterSpacing ) < (bbox.Center.Y - bbox.Extent.Y))
		cubeOriginY = (Int)floor ((bbox.Center.Y - bbox.Extent.Y)/m_emitterSpacing);

	if ((cubeDimX * m_emitterSpacing ) > (bbox.Center.X + bbox.Extent.X))
		cubeDimX = (Int)floor ((bbox.Center.X + bbox.Extent.X)/m_emitterSpacing);

	if ((cubeDimY * m_emitterSpacing ) > (bbox.Center.Y + bbox.Extent.Y))
		cubeDimY = (Int)floor ((bbox.Center.Y + bbox.Extent.Y)/m_emitterSpacing);

	if ((cubeDimY - cubeOriginY) < 0 || (cubeDimX-cubeOriginX) < 0)
		return;	//entire snow box is culled by either x or y screen boundary.

	//Find total number of particles that need rendering.
	Int totalPart=(cubeDimY-cubeOriginY)*(cubeDimX-cubeOriginX);

	if (totalPart <= 0)
		return;	//nothing to render.

	//Height at the top of the cube with camera at center.
	m_snowCeiling = camPos.Z + m_boxDimensions/2.0f;

	//Offset to allow cube extents to move with camera.
	Real cameraOffset = fmod (camPos.Z,m_boxDimensions);
	m_heightTraveled=m_time*m_velocity+cameraOffset;	//height that snow flake traveled this frame.

	Matrix4x4 identity(true);
	DX8Wrapper::Set_Transform(D3DTS_WORLD,identity);

	DX8Wrapper::Set_Shader(ShaderClass::_PresetAlphaShader);

	VertexMaterialClass *vmat=VertexMaterialClass::Get_Preset(VertexMaterialClass::PRELIT_DIFFUSE);
	DX8Wrapper::Set_Material(vmat);
	REF_PTR_RELEASE(vmat);

	//make sure we have all the resources we need
	if (usePointSprites && !m_VertexBufferD3D)
		ReAcquireResources();

	if (!usePointSprites && !m_indexBuffer)
		ReAcquireResources();

	DX8Wrapper::Set_Texture(0,m_snowTexture);

	// GeneralsX @feature PS3.0 Weather W3 16/10/2026: streak pass for the rain family.
	if (isRain)
	{
		renderRainStreaks(rinfo, cubeOriginX, cubeOriginY, cubeDimX, cubeDimY);
		return;
	}

	if (!usePointSprites)
	{
		renderAsQuads(rinfo,cubeOriginX,cubeOriginY,cubeDimX,cubeDimY);
		return;
	}

	Vector3 snowCenter;

	DX8Wrapper::Apply_Render_State_Changes();

    // Set the render states for using point sprites
	DX8Wrapper::Set_DX8_Render_State( D3DRS_POINTSPRITEENABLE, TRUE );
    DX8Wrapper::Set_DX8_Render_State( D3DRS_POINTSCALEENABLE,  TRUE );
    DX8Wrapper::Set_DX8_Render_State( D3DRS_POINTSIZE,     FtoDW(m_pointSize) );
    DX8Wrapper::Set_DX8_Render_State( D3DRS_POINTSIZE_MIN, FtoDW(m_minPointSize) );
    DX8Wrapper::Set_DX8_Render_State( D3DRS_POINTSIZE_MAX, FtoDW(m_maxPointSize) );
    DX8Wrapper::Set_DX8_Render_State( D3DRS_POINTSCALE_A,  FtoDW(0.00f) );
    DX8Wrapper::Set_DX8_Render_State( D3DRS_POINTSCALE_B,  FtoDW(0.00f) );
    DX8Wrapper::Set_DX8_Render_State( D3DRS_POINTSCALE_C,  FtoDW(1.00f) );

	// GeneralsX @feature Qoder 18/10/2026 step 9.I: the stream source moved into flush_particles' refusal
	// path. A batch the facade consumes never binds the D3D9 vertex buffer at all.
    DX8Wrapper::Set_Vertex_FVF( D3DFVF_POINTVERTEX );
	m_dwBase = SNOW_BUFFER_SIZE;	//start with a new vertex buffer each frame.

	m_leafDim = 45;	//cull boxes that are 20x20 emitters in size. Making them much smaller will result in too many draw calls.
	m_totalRendered = 0;	//keep track of how many particles were rendered.

	//Particle centers can deviate from center by by amplitude of sine offset.  They also have radius m_quadSize.
	//Enlarge culling bounds to compensate.
	m_cullOverscan = m_amplitude+m_quadSize;
	renderSubBox(rinfo,cubeOriginX,cubeOriginY,cubeDimX,cubeDimY);

	// Reset render states
    DX8Wrapper::Set_DX8_Render_State( D3DRS_POINTSPRITEENABLE, FALSE );
    DX8Wrapper::Set_DX8_Render_State( D3DRS_POINTSCALEENABLE,  FALSE );

}

/**For hardware that doesn't support point sprites*/
void W3DSnowManager::renderAsQuads(RenderInfoClass &rinfo, Int cubeOriginX, Int cubeOriginY, Int cubeDimX, Int cubeDimY)
{

	Matrix4x4 proj;
	Matrix3D view;
	Vector3 snowCenter;
	Vector3 snowCenterVS;

	CameraClass &camera=rinfo.Camera;

	camera.Get_View_Matrix(&view);
	camera.Get_Projection_Matrix(&proj);

	Vector3 vertex_offsets[4] = {
		Vector3(-0.5f, 0.5f, 0.0f),
		Vector3(-0.5f, -0.5f, 0.0f),
		Vector3(0.5f, -0.5f, 0.0f),
		Vector3(0.5f, 0.5f, 0.0f)
	};

	Vector2 quad_uvs[4] = {
		Vector2(0.0f, 0.0f),
		Vector2(0.0f, 1.0f),
		Vector2(1.0f, 1.0f),
		Vector2(1.0f, 0.0f)
	};


	//pre-multiple the offsets by particle size
	for (Int i=0; i<4; i++)
	{
		vertex_offsets[i] *= m_quadSize;
	}

	Matrix4x4 identity(true);
	DX8Wrapper::Set_Transform(D3DTS_VIEW,identity);

	DX8Wrapper::Set_Index_Buffer(m_indexBuffer,0);

	Int y=cubeOriginY;	//loop counter.
	Int cubeOriginXRemainder = cubeOriginX;	//loop counter - adjusted when not all particles fit into render buffer.

	// GeneralsX @feature PS3.0 Weather W3 16/10/2026: same wind carry as the sprite path (zero wind =
	// bit-identical to retail), read once per batch.
	const Real quadWindX = TheWeatherSetting != nullptr ? TheWeatherSetting->m_windX : 0.0f;
	const Real quadWindY = TheWeatherSetting != nullptr ? TheWeatherSetting->m_windY : 0.0f;
	const Real quadBoxDim = m_boxDimensions > 0.0001f ? m_boxDimensions : 1.0f;

	//Find total number of particles that need rendering.
	Int totalPart=(cubeDimY-cubeOriginY)*(cubeDimX-cubeOriginX);

	m_totalRendered += totalPart;

	while (totalPart)
	{
		Int batchSize=totalPart;

		if (batchSize > SNOW_BATCH_SIZE)
			batchSize = SNOW_BATCH_SIZE;

		Int numberInBatch=0;

		DynamicVBAccessClass vb_access(BUFFER_TYPE_DYNAMIC_DX8,dynamic_fvf_type,batchSize*4);	//allocate 4 verts per flake
		{
			DynamicVBAccessClass::WriteLockClass lock(&vb_access);
			VertexFormatXYZNDUV2* verts=lock.Get_Formatted_Vertex_Array();

			for (;y<cubeDimY; y++)
			{
				for (Int x=cubeOriginXRemainder; x<cubeDimX; x++)
				{
					if (numberInBatch >= batchSize)
					{	cubeOriginXRemainder = x;
						goto flush_particles;
					}

					//Get initial height from noise table.  We add a large value to make sure it's positive.  Then
					//modulate by table dimensions to find a value.
					Int noiseOffset=MODPOW2(x+MAXIMUM_CAMERA_DISTANCE,SNOW_NOISE_X)+MODPOW2(y+MAXIMUM_CAMERA_DISTANCE,SNOW_NOISE_Y)*SNOW_NOISE_X;
					if (noiseOffset > (SNOW_NOISE_X * SNOW_NOISE_Y))
						noiseOffset = 0;	//this should never happen but check to prevent buffer over/under flow.

					//find current height
					Real h0=m_snowCeiling-fmod(m_heightTraveled+m_startingHeights[noiseOffset],m_boxDimensions);

					//find world-space position of snow flake
					snowCenter.Set(x*m_emitterSpacing,y*m_emitterSpacing,h0);

					// GeneralsX @feature PS3.0 Weather W3 16/10/2026: wind carry before the view
					// transform, so the drift is in world units like the sprite path.
					if (quadWindX != 0.0f || quadWindY != 0.0f)
					{
						const Real quadFallFrac = (m_snowCeiling - h0) / quadBoxDim;
						snowCenter.X += quadWindX * quadFallFrac;
						snowCenter.Y += quadWindY * quadFallFrac;
					}

					//Get view-space position
					Matrix3D::Transform_Vector(view,snowCenter,&snowCenterVS);

					//Adjust position so snow flakes don't fall straight down.
					// GeneralsX @feature PS3.0-P3-26: per-flake phase (GENERALSX_SNOW_SWAY=0 restores
					// the retail per-column phase bit-for-bit).
					const Real quadPhaseX = Snow_Sway_On() ? Snow_Sway_Phase(x, y, 0) : (Real)x;
					const Real quadPhaseY = Snow_Sway_On() ? Snow_Sway_Phase(x, y, 17) : (Real)y;
					snowCenterVS.X += m_amplitude * WWMath::Fast_Sin( h0 * m_frequencyScaleX + quadPhaseX);
					snowCenterVS.Y += m_amplitude * WWMath::Fast_Sin( h0 * m_frequencyScaleY + quadPhaseY);

					// GeneralsX @feature PS3.0-P3-26: scene-light tint instead of the constant white.
					const UnsignedInt litColor = Snow_Lit_Color();

					for (Int i=0; i<4; i++)
					{
						*(Vector3 *)verts=snowCenterVS + vertex_offsets[i];
						verts->nx=0;	//keep AGP write-combining active
						verts->ny=0;
						verts->nz=0;
						verts->diffuse=litColor;	//GeneralsX @feature PS3.0-P3-26: opaque alpha, scene-lit rgb
						verts->u1=quad_uvs[i].X;
						verts->v1=quad_uvs[i].Y;
						verts->u2=0;	//keep AGP write-combining active
						verts->v2=0;
						verts++;
					}

					numberInBatch++;
				}
				//getting here means we did not overflow the render buffer, so reset x origin to normal.
				cubeOriginXRemainder = cubeOriginX;	//reset to normal amount
			}
flush_particles:
			numberInBatch;	//need something at goto destination - stupid c compiler.
		}

		//Render any particles that may be queued up.
		if (numberInBatch)
		{
			DX8Wrapper::Set_Vertex_Buffer(vb_access);
			DX8Wrapper::Draw_Triangles(	0,numberInBatch*2, 0, numberInBatch*4);
			totalPart -= numberInBatch;
		}
	}
}

//=============================================================================
// W3DSnowManager::renderRainStreaks
// GeneralsX @feature Storm996 16/10/2026 PS3.0 Weather W3: the rain family's world layer.
//
// Same camera-following emitter grid as snow (same noise table, same fall cycle, same box clip),
// but every drop is a thin quad stretched along its own motion vector instead of a square sprite.
// Why a quad and not a line primitive: hardware line width is a legacy D3D feature - every modern
// driver, DXVK included, draws 1-pixel lines - and a streak needs an authorable thickness, a
// per-drop alpha for the distance falloff, and a slant that tracks the wind. All three are vertex
// attributes on a quad, and none of them are expressible with D3DPT_LINELIST.
//
// The quads reuse the retail index buffer (same four-vertex two-triangle layout as renderAsQuads),
// and the vertices go in VIEW space with D3DTS_VIEW set to identity, exactly like the retail quad
// fallback. Drop POSITIONS are built in world space first so the wind drift and the sway stay in
// world units at any zoom; only the streak's long axis is then rotated into view space, where it
// keeps its length (the view matrix is orthonormal) and the slant comes out of the projection.
//=============================================================================
void W3DSnowManager::renderRainStreaks(RenderInfoClass &rinfo, Int originX, Int originY, Int cubeDimX, Int cubeDimY)
{
	const WeatherSetting *recipe = TheWeatherSetting;
	if (recipe == nullptr)
		return;
	if (m_indexBuffer == nullptr)
		return;	//device not acquired yet - the quad streak pass has no buffers to draw with.

	Matrix3D view;
	CameraClass &camera = rinfo.Camera;
	camera.Get_View_Matrix(&view);

	const Real boxDim = m_boxDimensions > 0.0001f ? m_boxDimensions : 1.0f;
	const Real halfLength = 0.5f * recipe->m_streakLength;
	const Real halfWidth = 0.5f * recipe->m_streakThickness;
	const Real opacity = recipe->m_opacity > 0.0f ? recipe->m_opacity : 1.0f;

	// GeneralsX @bugfix PS3.0 Weather W3 16/10/2026: the fade is measured from the centre of the emitter
	// box this frame actually draws, in the ground plane. It used to measure from the eye, which sits a
	// couple of hundred units above and behind that centre, so at the normal RTS zoom every drop was
	// already past FalloffStart and the whole sheet came out at one flat, dimmed alpha instead of a
	// gradient. Ground-plane distance from the box centre is what the author numbers read as ("rain thins
	// out past 460 units from where I am looking") and it does not change when the player zooms.
	Real fadeCX = 0.5f * (Real)(originX + cubeDimX) * m_emitterSpacing;
	Real fadeCY = 0.5f * (Real)(originY + cubeDimY) * m_emitterSpacing;

	// Distance fade, measured in squared world units so the per-drop test costs no sqrt.
	// FalloffEnd <= FalloffStart means "the map did not author a fade", and the batch stays solid.
	const Real start2 = recipe->m_falloffStart * recipe->m_falloffStart;
	const Real end2 = recipe->m_falloffEnd * recipe->m_falloffEnd;
	const Real span2 = end2 > start2 + 1.0f ? end2 - start2 : 0.0f;

	// World-space motion of one drop over a full fall cycle: straight down, sheared by the wind.
	Vector3 motion(recipe->m_windX, recipe->m_windY, -boxDim);
	if (motion.Length2() > 1e-8f)
	{
		motion.Normalize();
	}
	else
	{
		motion.Set(0.0f, 0.0f, -1.0f);
	}

	// Loop-invariant: the streak axes depend only on the view orientation, which is fixed for the frame.
	Vector3 longVS;
	Matrix3D::Rotate_Vector(view, motion, &longVS);
	longVS *= halfLength;
	Vector3 sideVS(-longVS.Y, longVS.X, 0.0f);	// the long axis rotated 90 degrees in the screen plane
	const Real sideLen2 = sideVS.Length2();
	if (sideLen2 > 1e-8f)
	{
		sideVS *= (halfWidth / sqrtf((float)sideLen2));
	}
	else
	{
		sideVS.Set(halfWidth, 0.0f, 0.0f);	//streak aimed straight at the eye: fall back to a horizontal ribbon.
	}

	const Vector3 corner_offsets[4] = {
		-longVS + sideVS,
		-longVS - sideVS,
		 longVS - sideVS,
		 longVS + sideVS
	};

	// GeneralsX @bugfix PS3.0 Weather W3 16/10/2026: the ZH rain sprite (EXRainDrop) is a 32x32 sheet
	// whose visible bar is only columns 14..17 wide and runs the full height, so the texture's U axis is
	// its THIN axis and V is its length. The first draft mapped them the other way round (u along the
	// streak), which sampled the bar across the length and left a hard-edged slab the full authored
	// thickness wide. Now u spans the width and is banded onto the bar so the sprite's soft shoulders
	// land exactly on the ribbon's edges, and v runs the length with v=0 at the upper end - the sprite
	// fades in at its top row, which is what makes the streak taper behind the drop.
	const Real uLo = 0.5f - RAIN_STREAK_U_BAND;
	const Real uHi = 0.5f + RAIN_STREAK_U_BAND;
	const Vector2 streak_uvs[4] = {
		Vector2(uHi, 0.0f),
		Vector2(uLo, 0.0f),
		Vector2(uLo, 1.0f),
		Vector2(uHi, 1.0f)
	};

	Matrix4x4 identity(true);
	DX8Wrapper::Set_Transform(D3DTS_VIEW, identity);

	DX8Wrapper::Set_Index_Buffer(m_indexBuffer, 0);

	Int y = originY;									//loop counter.
	Int cubeOriginXRemainder = originX;	//loop counter - adjusted when not all drops fit into the batch.

	Int totalPart = (cubeDimY - originY) * (cubeDimX - originX);
	if (totalPart <= 0)
		return;

	m_totalRendered += totalPart;

	while (totalPart)
	{
		Int batchSize = totalPart;
		if (batchSize > SNOW_BATCH_SIZE)
			batchSize = SNOW_BATCH_SIZE;

		Int numberInBatch = 0;
		Int cellsVisited = 0;	//grid cells consumed, faded-out ones included - see the loop tail.

		DynamicVBAccessClass vb_access(BUFFER_TYPE_DYNAMIC_DX8, dynamic_fvf_type, batchSize * 4);
		{
			DynamicVBAccessClass::WriteLockClass lock(&vb_access);
			VertexFormatXYZNDUV2 *verts = lock.Get_Formatted_Vertex_Array();

			for (; y < cubeDimY; y++)
			{
				for (Int x = cubeOriginXRemainder; x < cubeDimX; x++)
				{
					if (numberInBatch >= batchSize)
					{
						cubeOriginXRemainder = x;
						goto flush_streaks;
					}
					cellsVisited++;
					Int noiseOffset = MODPOW2(x + MAXIMUM_CAMERA_DISTANCE, SNOW_NOISE_X)
						+ MODPOW2(y + MAXIMUM_CAMERA_DISTANCE, SNOW_NOISE_Y) * SNOW_NOISE_X;
					if (noiseOffset > (SNOW_NOISE_X * SNOW_NOISE_Y))
						noiseOffset = 0;

					//height of this drop inside the falling column, then how far it has travelled.
					const Real h0 = m_snowCeiling - fmod(m_heightTraveled + m_startingHeights[noiseOffset], boxDim);
					const Real fallFrac = (m_snowCeiling - h0) / boxDim;	//0 at the top of the box, 1 at the bottom.

					//Wind carries the drop sideways as it descends; the sway adds the small ripple the
					//recipe already uses for snow (a rain recipe keeps its amplitude near zero).
					const Real phaseX = Snow_Sway_Phase(x, y, 0);
					const Real phaseY = Snow_Sway_Phase(x, y, 17);
					Vector3 dropWS(
						x * m_emitterSpacing + recipe->m_windX * fallFrac + m_amplitude * WWMath::Fast_Sin(h0 * m_frequencyScaleX + phaseX),
						y * m_emitterSpacing + recipe->m_windY * fallFrac + m_amplitude * WWMath::Fast_Sin(h0 * m_frequencyScaleY + phaseY),
						h0);

					//Fade the distant drops (and drop them entirely) instead of letting the sheet turn
					//into a wall of aliased pixels at the far edge of the box.
					Real fade = 1.0f;
					if (span2 > 0.0f)
					{
						const Real dx = dropWS.X - fadeCX;
						const Real dy = dropWS.Y - fadeCY;
						fade = (end2 - (dx * dx + dy * dy)) / span2;
						if (fade < 0.0f)
						{
							continue;	//fully faded - no vertices to write.
						}
						if (fade > 1.0f)
						{
							fade = 1.0f;
						}
					}

					const UnsignedInt streakColor = Rain_Streak_Color(opacity * fade);
					Vector3 dropVS;
					Matrix3D::Transform_Vector(view, dropWS, &dropVS);

					for (Int i = 0; i < 4; i++)
					{
						*(Vector3 *)verts = dropVS + corner_offsets[i];
						verts->nx = 0;	//keep AGP write-combining active
						verts->ny = 0;
						verts->nz = 0;
						verts->diffuse = streakColor;	//scene-lit rgb + this drop's own alpha
						verts->u1 = streak_uvs[i].X;
						verts->v1 = streak_uvs[i].Y;
						verts->u2 = 0;	//keep AGP write-combining active
						verts->v2 = 0;
						verts++;
					}

					numberInBatch++;
				}
				cubeOriginXRemainder = originX;	//reset to normal amount
			}

flush_streaks:
			numberInBatch;	//need something at goto destination - same construct the retail quad pass uses.
		}

		if (numberInBatch)
		{
			DX8Wrapper::Set_Vertex_Buffer(vb_access);
			DX8Wrapper::Draw_Triangles(0, numberInBatch * 2, 0, numberInBatch * 4);
		}

		// GeneralsX @bugfix PS3.0 Weather W3 16/10/2026: the retail loop this was copied from decrements
		// its counter by the number of particles it WROTE, which is safe there because nothing in that
		// pass is ever skipped. Here a fully-faded drop writes nothing, so a batch that culled more than
		// it drew would leave the counter above zero with the grid already walked to the end - the next
		// iteration visits nothing, draws nothing and spins forever (reachable at maximum zoom-out, where
		// the clip box grows past FalloffEnd and the whole sheet culls). Count cells consumed instead,
		// and break on a batch that consumed nothing so a hang is structurally impossible.
		if (cellsVisited <= 0)
			break;
		totalPart -= cellsVisited;
	}
}
