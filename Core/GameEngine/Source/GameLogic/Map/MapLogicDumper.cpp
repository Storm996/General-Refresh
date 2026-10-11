// FILE: MapLogicDumper.cpp ////////////////////////////////////////////////////
// GeneralsX @feature Qoder 11/10/2026 See MapLogicDumper.h.
//
// Walks the loaded runtime logic singletons (TheSidesList -> ScriptList/Script/
// OrCondition/Condition/ScriptAction/Parameter, PolygonTrigger, build lists) and
// writes them out as the editable text sidecar format defined in
// 地图升级/新格式设计_GXL.md.
//
// Read-only. Only ever runs under the -dumplogic flag, so gameplay, determinism
// and replays are unaffected.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include <stdio.h>

#include "Common/AsciiString.h"
#include "Common/WellKnownKeys.h"
#include "GameLogic/MapLogicDumper.h"
#include "GameLogic/Scripts.h"
#include "GameLogic/ScriptEngine.h"
#include "GameLogic/SidesList.h"
#include "GameLogic/PolygonTrigger.h"

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
static const char *yn( Bool b ) { return b ? "Yes" : "No"; }

// Derive the directory (with trailing separator) and the base file name
// (without the .map extension) from a full map path.
static void splitMapPath( const AsciiString& full, AsciiString& dir, AsciiString& base )
{
	dir.clear();
	base = full;

	Int lastSep = -1;
	const char* s = full.str();
	for ( Int i = 0; s && s[i] != 0; ++i )
	{
		if ( s[i] == '\\' || s[i] == '/' )
			lastSep = i;
	}

	if ( lastSep >= 0 )
	{
		// include the separator in dir
		char buf[ _MAX_PATH ];
		Int n = lastSep + 1;
		if ( n >= (Int)sizeof( buf ) ) n = (Int)sizeof( buf ) - 1;
		memcpy( buf, s, n );
		buf[ n ] = 0;
		dir = buf;
		base.set( s + lastSep + 1 );
	}

	// strip a trailing ".map" (case-insensitive)
	if ( base.endsWithNoCase( ".map" ) )
		base.truncateBy( 4 );

	if ( base.isEmpty() )
		base.set( "maplogic" );
}

// Resolve a script action's stable internal name from its type index.
static AsciiString actionInternalName( ScriptAction* a )
{
	if ( !a || !TheScriptEngine )
		return AsciiString( "<unknown_action>" );
	const ActionTemplate* t = TheScriptEngine->getActionTemplate( a->getActionType() );
	if ( t && !t->m_internalName.isEmpty() )
		return t->m_internalName;
	return AsciiString( "<unknown_action>" );
}

static AsciiString conditionInternalName( Condition* c )
{
	if ( !c || !TheScriptEngine )
		return AsciiString( "<unknown_condition>" );
	const ConditionTemplate* t = TheScriptEngine->getConditionTemplate( c->getConditionType() );
	if ( t && !t->m_internalName.isEmpty() )
		return t->m_internalName;
	return AsciiString( "<unknown_condition>" );
}

// Emit a single parameter value as a text token, based on its declared type.
static void writeParamToken( FILE* f, Parameter* p )
{
	if ( !p )
	{
		fputs( "<null>", f );
		return;
	}

	switch ( p->getParameterType() )
	{
		case Parameter::COORD3D:
		{
			Coord3D c;
			p->getCoord3D( &c );
			fprintf( f, "(%.4f,%.4f,%.4f)", c.x, c.y, c.z );
			break;
		}
		case Parameter::REAL:
		case Parameter::ANGLE:
		case Parameter::PERCENT:
			fprintf( f, "%g", (double)p->getReal() );
			break;
		case Parameter::BOOLEAN:
			fputs( p->getInt() ? "Yes" : "No", f );
			break;
		case Parameter::INT:
			fprintf( f, "%d", p->getInt() );
			break;
		default:
		{
			// String-valued and enum-as-int parameters. Strings are authoritative
			// when present; otherwise fall back to the integer payload.
			const AsciiString& str = p->getString();
			if ( !str.isEmpty() )
				fprintf( f, "\"%s\"", str.str() );
			else
				fprintf( f, "%d", p->getInt() );
			break;
		}
	}
}

// ---------------------------------------------------------------------------
// Script action / condition chains
// ---------------------------------------------------------------------------
static void writeActionList( FILE* f, ScriptAction* head, const char* label, Int indent )
{
	if ( !head )
		return;

	for ( Int i = 0; i < indent; ++i ) fputc( '\t', f );
	fprintf( f, "%s\n", label );

	for ( ScriptAction* a = head; a; a = a->getNext() )
	{
		AsciiString name = actionInternalName( a );
		for ( Int i = 0; i < indent + 1; ++i ) fputc( '\t', f );
		fprintf( f, "Action = \"%s\"\n", name.str() );

		Int n = a->getNumParameters();
		for ( Int p = 0; p < n; ++p )
		{
			Parameter* par = a->getParameter( p );
			if ( !par ) continue;
			for ( Int i = 0; i < indent + 2; ++i ) fputc( '\t', f );
			fprintf( f, "Param%d = ", p );
			writeParamToken( f, par );
			fputc( '\n', f );
		}
	}

	for ( Int i = 0; i < indent; ++i ) fputc( '\t', f );
	fprintf( f, "End\n" );
}

static void writeConditions( FILE* f, Script* script, Int indent )
{
	for ( Int i = 0; i < indent; ++i ) fputc( '\t', f );
	fprintf( f, "# IF (any OR clause true -> THEN, else -> ELSE)\n" );
	for ( Int i = 0; i < indent; ++i ) fputc( '\t', f );
	fprintf( f, "Conditions\n" );

	Bool any = false;
	for ( OrCondition* o = script->getOrCondition(); o; o = o->getNextOrCondition() )
	{
		any = true;
		for ( Int i = 0; i < indent + 1; ++i ) fputc( '\t', f );
		fprintf( f, "OrCondition\n" );

		for ( Condition* c = o->getFirstAndCondition(); c; c = c->getNext() )
		{
			AsciiString name = conditionInternalName( c );
			for ( Int i = 0; i < indent + 2; ++i ) fputc( '\t', f );
			fprintf( f, "Condition = \"%s\"\n", name.str() );

			Int n = c->getNumParameters();
			for ( Int p = 0; p < n; ++p )
			{
				Parameter* par = c->getParameter( p );
				if ( !par ) continue;
				for ( Int i = 0; i < indent + 3; ++i ) fputc( '\t', f );
				fprintf( f, "Param%d = ", p );
				writeParamToken( f, par );
				fputc( '\n', f );
			}
		}

		for ( Int i = 0; i < indent + 1; ++i ) fputc( '\t', f );
		fprintf( f, "End\n" );
	}

	if ( !any )
	{
		for ( Int i = 0; i < indent + 1; ++i ) fputc( '\t', f );
		fprintf( f, "# (no conditions - script evaluates unconditionally)\n" );
	}

	for ( Int i = 0; i < indent; ++i ) fputc( '\t', f );
	fprintf( f, "End\n" );
}

// ---------------------------------------------------------------------------
// One script
// ---------------------------------------------------------------------------
static void writeScript( FILE* f, Script* script, Int indent )
{
	if ( !script )
		return;

	for ( Int i = 0; i < indent; ++i ) fputc( '\t', f );
	fprintf( f, "Script \"%s\"\n", script->getName().str() );

	const AsciiString comment = script->getComment();
	if ( !comment.isEmpty() )
	{
		for ( Int i = 0; i < indent + 1; ++i ) fputc( '\t', f );
		fprintf( f, "Comment = \"%s\"\n", comment.str() );
	}

	for ( Int i = 0; i < indent + 1; ++i ) fputc( '\t', f );
	fprintf( f, "Active = %s\n", yn( script->isActive() ) );
	for ( Int i = 0; i < indent + 1; ++i ) fputc( '\t', f );
	fprintf( f, "OneShot = %s\n", yn( script->isOneShot() ) );
	for ( Int i = 0; i < indent + 1; ++i ) fputc( '\t', f );
	fprintf( f, "Subroutine = %s\n", yn( script->isSubroutine() ) );
	for ( Int i = 0; i < indent + 1; ++i ) fputc( '\t', f );
	fprintf( f, "DelayEvaluateSeconds = %d\n", script->getDelayEvalSeconds() );

	// Difficulty: which skill levels this script participates in.
	for ( Int i = 0; i < indent + 1; ++i ) fputc( '\t', f );
{
	fputs( "Difficulty =", f );
	if ( script->isEasy() )   fputs( " Easy", f );
	if ( script->isNormal() ) fputs( " Normal", f );
	if ( script->isHard() )   fputs( " Hard", f );
	fputc( '\n', f );
}

	writeConditions( f, script, indent + 1 );
	writeActionList( f, script->getAction(), "Actions", indent + 1 );
	writeActionList( f, script->getFalseAction(), "ActionsFalse", indent + 1 );

	for ( Int i = 0; i < indent; ++i ) fputc( '\t', f );
	fprintf( f, "End\n\n" );
}

// ---------------------------------------------------------------------------
// One ScriptList (belongs to a side)
// ---------------------------------------------------------------------------
static void writeScriptList( FILE* f, ScriptList* list, const AsciiString& sideName )
{
	if ( !list )
		return;

	fprintf( f, "ScriptList Side = \"%s\"\n", sideName.str() );

	for ( ScriptGroup* g = list->getScriptGroup(); g; g = g->getNext() )
	{
		fprintf( f, "\tScriptGroup \"%s\"\n", g->getName().str() );
		fprintf( f, "\t\tActive = %s\n", yn( g->isActive() ) );
		fprintf( f, "\t\tSubroutine = %s\n", yn( g->isSubroutine() ) );

		for ( Script* s = g->getScript(); s; s = s->getNext() )
			writeScript( f, s, 2 );

		fprintf( f, "\tEnd\n\n" );
	}

	// Loose scripts (not part of any group).
	for ( Script* s = list->getScript(); s; s = s->getNext() )
		writeScript( f, s, 1 );

	fprintf( f, "End\n\n" );
}

// ---------------------------------------------------------------------------
// Public entry
// ---------------------------------------------------------------------------
void MapLogicDumper::dumpAll( const AsciiString& mapPath, const AsciiString& outDir )
{
	AsciiString dir, base;
	splitMapPath( mapPath, dir, base );

	AsciiString dest;
	if ( !outDir.isEmpty() )
		dest = outDir;
	else
		dest = dir;

	// ensure a trailing separator on dest
	if ( !dest.isEmpty() && dest.str()[ dest.getLength() - 1 ] != '\\' && dest.str()[ dest.getLength() - 1 ] != '/' )
		dest.concat( "\\" );

	AsciiString scriptsPath, triggersPath, sidesPath;
	scriptsPath.format( "%s%s.scripts.ini", dest.str(), base.str() );
	triggersPath.format( "%s%s.triggers.ini", dest.str(), base.str() );
	sidesPath.format( "%s%s.sides.ini", dest.str(), base.str() );

	// ---- scripts ----
	FILE* fs = fopen( scriptsPath.str(), "w" );
	if ( fs )
	{
		fputs( "# GeneralsX map logic - SCRIPTS (AI-editable)\n", fs );
		fputs( "# Generated by MapLogicDumper. Difficulty, enemy counts and attack\n", fs );
		fputs( "# timers live here. See 地图升级/新格式设计_GXL.md.\n\n", fs );

		if ( TheSidesList )
		{
			Int numSides = TheSidesList->getNumSides();
			for ( Int i = 0; i < numSides; ++i )
			{
				SidesInfo* si = TheSidesList->getSideInfo( i );
				if ( !si ) continue;
				AsciiString sideName;
				Dict* d = si->getDict();
				if ( d ) sideName = d->getAsciiString( TheKey_playerName );
				if ( sideName.isEmpty() ) sideName.format( "Side%d", i );
				writeScriptList( fs, si->getScriptList(), sideName );
			}
		}
		fclose( fs );
		printf( "[dumplogic] wrote scripts -> %s\n", scriptsPath.str() );
	}
	else
	{
		fprintf( stderr, "[dumplogic] FAILED to open %s (does the directory exist?)\n", scriptsPath.str() );
	}

	// ---- triggers ----
	FILE* ft = fopen( triggersPath.str(), "w" );
	if ( ft )
	{
		fputs( "# GeneralsX map logic - TRIGGER AREAS (AI-editable)\n\n", ft );
		for ( PolygonTrigger* trig = PolygonTrigger::getFirstPolygonTrigger();
			trig; trig = trig->getNext() )
		{
			fprintf( ft, "Trigger \"%s\"\n", trig->getTriggerName().str() );
			fprintf( ft, "\tId = %d\n", trig->getID() );
			if ( !trig->getLayerName().isEmpty() )
				fprintf( ft, "\tLayer = \"%s\"\n", trig->getLayerName().str() );
			fprintf( ft, "\tWater = %s\n", yn( trig->isWaterArea() ) );
			fprintf( ft, "\tRiver = %s\n", yn( trig->isRiver() ) );
			if ( trig->isRiver() )
				fprintf( ft, "\tRiverStart = %d\n", trig->getRiverStart() );
			fprintf( ft, "\tPoints\n" );
			Int np = trig->getNumPoints();
			for ( Int i = 0; i < np; ++i )
			{
				const ICoord3D* p = trig->getPoint( i );
				if ( p )
					fprintf( ft, "\t\t(%d,%d,%d)\n", p->x, p->y, p->z );
			}
			fprintf( ft, "\tEnd\n" );
			fprintf( ft, "End\n\n" );
		}
		fclose( ft );
		printf( "[dumplogic] wrote triggers -> %s\n", triggersPath.str() );
	}
	else
	{
		fprintf( stderr, "[dumplogic] FAILED to open %s\n", triggersPath.str() );
	}

	// ---- sides / teams / build lists ----
	FILE* fd = fopen( sidesPath.str(), "w" );
	if ( fd )
	{
		fputs( "# GeneralsX map logic - SIDES / TEAMS / BUILD LISTS (AI-editable)\n", fd );
		fputs( "# Note: full per-side attribute dictionaries are dumped in a later phase.\n\n", fd );

		if ( TheSidesList )
		{
			Int numSides = TheSidesList->getNumSides();
			for ( Int i = 0; i < numSides; ++i )
			{
				SidesInfo* si = TheSidesList->getSideInfo( i );
				if ( !si ) continue;
				AsciiString sideName;
				Dict* d = si->getDict();
				if ( d ) sideName = d->getAsciiString( TheKey_playerName );
				if ( sideName.isEmpty() ) sideName.format( "Side%d", i );

				fprintf( fd, "Side \"%s\"\n", sideName.str() );
				BuildListInfo* bl = si->getBuildList();
				if ( bl )
				{
					fprintf( fd, "\tBuildList\n" );
					for ( ; bl; bl = bl->getNext() )
					{
						fprintf( fd, "\t\tEntry\n" );
						const Coord3D* loc = bl->getLocation();
						fprintf( fd, "\t\t\tBuildingName = \"%s\"\n", bl->getBuildingName().str() );
						fprintf( fd, "\t\t\tTemplateName = \"%s\"\n", bl->getTemplateName().str() );
						if ( loc )
							fprintf( fd, "\t\t\tLocation = (%.4f,%.4f,%.4f)\n", loc->x, loc->y, loc->z );
						fprintf( fd, "\t\t\tAngle = %g\n", (double)bl->getAngle() );
						fprintf( fd, "\t\t\tInitiallyBuilt = %s\n", yn( bl->isInitiallyBuilt() ) );
						fprintf( fd, "\t\t\tNumRebuilds = %d\n", bl->getNumRebuilds() );
						fprintf( fd, "\t\t\tHealth = %d\n", bl->getHealth() );
						if ( !bl->getScript().isEmpty() )
							fprintf( fd, "\t\t\tScript = \"%s\"\n", bl->getScript().str() );
						fprintf( fd, "\t\tEnd\n" );
					}
					fprintf( fd, "\tEnd\n" );
				}
				fprintf( fd, "End\n\n" );
			}

			Int numTeams = TheSidesList->getNumTeams();
			for ( Int i = 0; i < numTeams; ++i )
			{
				TeamsInfo* ti = TheSidesList->getTeamInfo( i );
				if ( !ti ) continue;
				Dict* td = ti->getDict();
				if ( !td ) continue;
				fprintf( fd, "Team \"%s\"\n", td->getAsciiString( TheKey_teamName ).str() );
				fprintf( fd, "\tOwner = \"%s\"\n", td->getAsciiString( TheKey_teamOwner ).str() );
				fprintf( fd, "End\n\n" );
			}
		}
		fclose( fd );
		printf( "[dumplogic] wrote sides -> %s\n", sidesPath.str() );
	}
	else
	{
		fprintf( stderr, "[dumplogic] FAILED to open %s\n", sidesPath.str() );
	}

	fflush( stdout );
	fflush( stderr );
}
