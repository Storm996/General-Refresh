// GeneralsX @refactor 23/10/2026: shared render-side atmosphere snapshot.
//
// The renderer still has per-family uploads, but they now read one source for the values that used
// to drift: sun direction/colour, ambient, the terrain/GN/tree haze ramp, and exposure. This is the
// first half of the FrameConstants contract; MaterialDesc (features/census) is the next half.
//
// Plain floats on purpose: WW3D2-adjacent code can include this without pulling GlobalData into the
// backend. Filled by W3D_RefreshRenderLighting() in W3DDisplay.cpp when the render TOD changes.
#pragma once

struct W3DRenderLightingState
{
	float sunDir[3];		///< world-space direction TOWARD the sun, normalized
	float sunColor[3];		///< RGB of the key light (terrain/object lighting table)
	float ambient[3];		///< ambient RGB of the same slot
	float fillDir[3];		///< world-space direction TOWARD the map fill, normalized (P1)
	float fillColor[3];		///< RGB of the map fill, weather-scaled (P1)
	float skyColor[3];		///< P3 hemisphere ambient, upper hemisphere
	float groundColor[3];	///< P3 hemisphere ambient, lower hemisphere
	float fogColor[4];		///< rgb = haze colour, w = blend strength (0 disables)
	float fogRamp[4];		///< x = eye-depth start, y = 1/ramp length, zw unused
	float cloudOffset[2];	///< scrolling cloud UV offset, shared by terrain/road/sky consumers
	float cloudShadowStrength;	///< 0 disables the native receiver's cloud term
	float exposure;			///< P6 HDR exposure (presentation-side)
	unsigned revision;		///< bumped whenever the snapshot is refilled; baked families rebake on change
};

const W3DRenderLightingState& W3D_GetRenderLighting();
void W3D_RefreshRenderLighting();
unsigned W3D_LightingStateHash();
void W3D_SetCloudOffset(float x, float y);
