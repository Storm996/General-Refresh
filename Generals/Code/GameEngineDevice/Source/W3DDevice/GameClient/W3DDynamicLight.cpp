/*
**	Command & Conquer Generals(tm)
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

// W3DDynamicLight.cpp
// Class to handle dynamic lights.
// Author: John Ahlquist, April 2001
#include <stdlib.h>

#include "W3DDevice/GameClient/W3DDynamicLight.h"

W3DDynamicLight::W3DDynamicLight():
LightClass(LightClass::POINT)
{

	m_priorEnable = false;
	m_enabled = true;

}

W3DDynamicLight::~W3DDynamicLight()
{
}

void W3DDynamicLight::On_Frame_Update()
{
	if (!m_enabled) {
		return;
	}
	Real factor = 1.0f;
	if (m_curIncreaseFrameCount>0 && m_increaseFrameCount>0) {
		// increasing
		m_curIncreaseFrameCount--;
		Real x = (m_increaseFrameCount-m_curIncreaseFrameCount)/(Real)m_increaseFrameCount;
		// GeneralsX @tweak PS3.0 4G 17/10/2026 smoothstep rise (backported from GeneralsMD):
		// the linear ramp read as an instant full-brightness pop at short increase times.
		factor = x*x*(3.0f-2.0f*x);

	}	else if (m_decayFrameCount==0) {
		factor = 1.0;  // never decays,
	}	else {
		m_curDecayFrameCount--;
		if (m_curDecayFrameCount == 0) {
			m_enabled = false;
			return;
		}
		// GeneralsX @tweak PS3.0 4G 17/10/2026 smoothstep fall: linear decay ended in a visible
		// snap-to-black; the eased curve fades the tail out progressively.
		Real x = m_curDecayFrameCount/(Real)m_decayFrameCount;
		factor = x*x*(3.0f-2.0f*x);
	}
	if (m_decayRange) {
		this->FarAttenEnd = factor*m_targetRange;
		if (FarAttenEnd < FarAttenStart) {
			FarAttenEnd = FarAttenStart;
		}
	}
	if (m_decayColor) {
		this->Ambient = m_targetAmbient*factor;
		this->Diffuse = m_targetDiffuse*factor;
	}
}

void W3DDynamicLight::setFrameFade(UnsignedInt frameIncreaseTime, UnsignedInt decayFrameTime)
{
	m_decayFrameCount = decayFrameTime;
	m_curDecayFrameCount = decayFrameTime;
	m_curIncreaseFrameCount = frameIncreaseTime;
	m_increaseFrameCount = frameIncreaseTime;
	m_targetAmbient = Ambient;
	m_targetDiffuse = Diffuse;
	m_targetRange = FarAttenEnd;
}
