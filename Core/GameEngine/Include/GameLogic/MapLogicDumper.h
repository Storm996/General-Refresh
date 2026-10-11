// MapLogicDumper.h
// GeneralsX @feature Qoder 11/10/2026 Dump a map's editable logic layer (scripts,
// triggers, sides/teams/build-lists) from the loaded runtime singletons into plain
// text sidecars that both the game (loader) and an AI assistant can read/edit.
//
// See 地图升级/新格式设计_GXL.md for the format specification.
//
// This is a read-only migration/inspection tool. It is only invoked when the
// -dumplogic command-line flag is present, so it never affects normal gameplay,
// determinism, or replays.

#pragma once

class AsciiString;

class MapLogicDumper
{
public:
	// Dump the currently-loaded map logic to text sidecars.
	//   mapPath : the loaded map path (used to derive the base name and, when
	//             outDir is empty, the destination directory next to the .map).
	//   outDir  : explicit destination directory, or empty to write beside the map.
	// Emits <base>.scripts.ini, <base>.triggers.ini, <base>.sides.ini.
	static void dumpAll(const AsciiString& mapPath, const AsciiString& outDir);
};
