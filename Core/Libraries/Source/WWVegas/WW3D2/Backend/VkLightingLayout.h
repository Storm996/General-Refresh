// GeneralsX @feature Qoder 24/10/2026 P0: single CPU/GPU layout contract for the Forward+ light block.
//
// The CPU and every GN/B1 fragment shader must agree exactly; VkShadersPgm*.h is baked from shaders
// that use the matching GLSL layout, and check_lighting_layout.py verifies the SPIR-V offsets. The
// block is a read-only SSBO at binding 4 (a UBO cannot hold it on NVIDIA).
//
// Layout v3 (std430), 98,784 B:
//   offset 0     : vec4 fpHeader       (tilesX, tilesY, tileSize, lightCount)
//   offset 16    : vec4 fpHeader2      (zSlices, depthNear, depthFar, viewportH)
//   offset 32    : 8 x vec4 fpEnv      (sunDir/sunColor/skyColor/ground/fill/fog; read by C4)
//   offset 160   : vec4 fpLights[512]  (256 x (posRange, colorIntensity))
//   offset 8352  : uint fpTiles[20480] (1024 x 20 uints)
//   offset 90272 : vec4 fpLightExtra[512] (256 x (dir.xyz, cosOuter) / (cosInner, cookie, kind, exp))
//   offset 98464 : vec4 fpLightShadow[20] (4 maps x (4 VP rows + light xyz/gateRange))
//   size 98784
//
// v2 (GeneralsX @feature 24/10/2026 P4 D2) appends the per-light spot/cookie record *after* the
// clusters instead of widening the light array, so the v1 offsets (header 160, lights 160, clusters
// 8352) did not move and v1 shaders keep compiling against the same layout. The record is:
//   extra[li*2+0] = (dirX, dirY, dirZ, cosOuter)   dir points *away* from the fixture
//   extra[li*2+1] = (cosInner, cookieId, kind, spotExponent)   kind 0 = point, 1 = spot
// A zeroed record is a point light, which is also what the per-object eight-light fallback is.
//
// The shaders skip the reserved 128 bytes with `layout(offset = 160)` on lights[512], so P1 can fill
// the reserved fields without touching the light/cluster offsets or rebaking the arrays again.
//
// v3 (GeneralsX @feature 24/10/2026 P34 E3/E4) appends the light-shadow records after fpLightExtra, so
// every v2 offset is untouched. The eight reserved header vec4s are now declared by the shaders as
// `fpEnv[8]` and carry sun direction/colour, sky/ground ambient and the environment-reflection strength
// (C4); the CPU already had those fields in VKLightingHeader, they were simply never written.
#pragma once

#include <cstddef>
#include <cstdint>

namespace VkLighting {

constexpr uint32_t kMaxLights       = 256;
constexpr uint32_t kMaxClusters     = 1024;
constexpr uint32_t kMaxPerCluster   = 16;
constexpr uint32_t kClusterUints    = 20;   // count + 16 indices + 3 pad
constexpr uint32_t kHeaderVec4      = 10;   // 2 used by P0 + 8 reserved for P1/P2
constexpr uint32_t kLightVec4       = 2;    // posRange, colorIntensity
constexpr uint32_t kAuxVec4         = 2;    // direction/cosOuter, cosInner/cookie/kind/exponent

constexpr uint32_t kHeaderBytes     = kHeaderVec4 * 16u;                       // 160
constexpr uint32_t kLightsOffset    = kHeaderBytes;                            // 160
constexpr uint32_t kLightsBytes     = kMaxLights * kLightVec4 * 16u;           // 8192
constexpr uint32_t kClustersOffset  = kLightsOffset + kLightsBytes;            // 8352
constexpr uint32_t kClustersBytes   = kMaxClusters * kClusterUints * 4u;       // 81920
constexpr uint32_t kAuxOffset       = kClustersOffset + kClustersBytes;        // 90272
constexpr uint32_t kAuxBytes        = kMaxLights * kAuxVec4 * 16u;             // 8192
// GeneralsX @feature 24/10/2026 P34 E3: four independent light-shadow maps. Each slot owns five vec4:
// four row-vector light-VP rows and (light xyz, w = range when the slot's map is live, 0 when empty).
// The receivers index this by light-list slot, so slot s in fpLights is shadowed by map s.
constexpr uint32_t kShadowMaps       = 4;
constexpr uint32_t kShadowVec4PerMap = 5;
constexpr uint32_t kShadowVec4       = kShadowMaps * kShadowVec4PerMap;         // 20
constexpr uint32_t kShadowOffset     = kAuxOffset + kAuxBytes;                  // 98464
constexpr uint32_t kShadowBytes      = kShadowVec4 * 16u;                       // 320
constexpr uint32_t kTotalBytes       = kShadowOffset + kShadowBytes;            // 98784

// One frame slot. Kept larger than kTotalBytes so future header growth does not move the next slot.
constexpr uint32_t kSlotBytes       = 131072;

// kind values carried in fpLightExtra[li*2+1].z
constexpr uint32_t kKindPoint       = 0;
constexpr uint32_t kKindSpot        = 1;

// cookieId values carried in fpLightExtra[li*2+1].y
//   0          : no cookie (1.0)
//   1          : procedural 3x4 mullion grid (kept for compatibility with v2 publishes)
//   2          : procedural soft radial
//   3..10      : baked atlas tile (cookieId - 3) from VkLightCookieAtlas.h, binding 9
constexpr uint32_t kCookieNone      = 0;
constexpr uint32_t kCookieWindow    = 1;   // 3x4 mullion grid, for a lit window
constexpr uint32_t kCookieSoft      = 2;   // soft radial, approximates an IES batwing
constexpr uint32_t kCookieAtlasFirst = 3;
constexpr uint32_t kCookieAtlasCount = 8;  // 4 columns x 2 rows, 128 px tiles in a 512x256 atlas

} // namespace VkLighting

struct VKLightingHeader
{
    float fpHeader[4];          // tilesX, tilesY, tileSize, lightCount
    float fpHeader2[4];         // zSlices, depthNear, depthFar, viewportH
    float sunDirIntensity[4];   // reserved for P1/P2
    float sunColor[4];
    float skyColor[4];
    float groundColor[4];
    float fillDirIntensity[4];
    float fillColor[4];
    float fogColorStrength[4];
    float fogRamp[4];
};

struct VKLightingBuffer
{
    VKLightingHeader header;
    float            lights[VkLighting::kMaxLights * 8];                      // 512 x vec4
    uint32_t         tiles[VkLighting::kMaxClusters * VkLighting::kClusterUints];
    float            aux[VkLighting::kMaxLights * 8];                         // v2 spot/cookie
    float            shadow[VkLighting::kShadowVec4 * 4];                      // v3 light-shadow maps
};

static_assert(sizeof(VKLightingHeader) == VkLighting::kHeaderBytes, "VKLightingHeader must be 160 B");
static_assert(offsetof(VKLightingBuffer, lights) == VkLighting::kLightsOffset, "lights offset drift");
static_assert(offsetof(VKLightingBuffer, tiles) == VkLighting::kClustersOffset, "cluster offset drift");
static_assert(offsetof(VKLightingBuffer, aux) == VkLighting::kAuxOffset, "aux offset drift");
static_assert(offsetof(VKLightingBuffer, shadow) == VkLighting::kShadowOffset, "shadow offset drift");
static_assert(sizeof(VKLightingBuffer) == VkLighting::kTotalBytes, "VKLightingBuffer size drift");
static_assert(VkLighting::kTotalBytes <= VkLighting::kSlotBytes, "slot too small");
