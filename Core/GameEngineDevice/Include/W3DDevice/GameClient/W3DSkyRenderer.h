// GeneralsX @refactor 23/10/2026: the skybox is its own scene renderer. It used to be owned and
// drawn by WaterRenderObjClass even though nothing about it is water: the five `new_skybox` faces
// feed the cubemap path, and the mesh itself is no longer submitted. Keeping it here means a map
// without a water object can still have a sky, and water no longer has to exist for the sky pass.
#pragma once

#include "WWLib/always.h"
#include "WWLib/wwstring.h"

class RenderInfoClass;
class RenderObjClass;

class W3DSkyRenderer
{
public:
	W3DSkyRenderer();
	~W3DSkyRenderer();

	/// Load `new_skybox` once. Safe to call repeatedly; a failed load leaves isReady() false and the
	/// render pass simply skips the sky instead of bringing the retired five-face mesh path back.
	void init();
	void reset();

	/// The scene-pass entry point. Called by W3DView after the world, inside the `sky` graph pass.
	void render(RenderInfoClass &rinfo);

	/// Map-specific sky swaps still go through the W3DAssetManager prototype, exactly as before.
	void replaceTexture(const AsciiString& oldTexName, const AsciiString& newTexName);

	Bool isReady() const { return m_skyBox != nullptr; }

private:
	RenderObjClass *m_skyBox;
};

extern W3DSkyRenderer *TheSkyRenderer;
