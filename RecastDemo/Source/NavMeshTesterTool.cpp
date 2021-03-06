//
// Copyright (c) 2009-2010 Mikko Mononen memon@inside.org
//
// This software is provided 'as-is', without any express or implied
// warranty.  In no event will the authors be held liable for any damages
// arising from the use of this software.
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
// 1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software. If you use this software
//    in a product, an acknowledgment in the product documentation would be
//    appreciated but is not required.
// 2. Altered source versions must be plainly marked as such, and must not be
//    misrepresented as being the original software.
// 3. This notice may not be removed or altered from any source distribution.
//

#define _USE_MATH_DEFINES
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "SDL.h"
#include "SDL_opengl.h"
#include "imgui.h"
#include "NavMeshTesterTool.h"
#include "Sample.h"
#include "Recast.h"
#include "RecastDebugDraw.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "DetourDebugDraw.h"
#include "DetourCommon.h"

#ifdef WIN32
#	define snprintf _snprintf
#endif

// Uncomment this to dump all the requests in stdout.
#define DUMP_REQS

// Returns a random number [0..1)
static float frand()
{
//	return ((float)(rand() & 0xffff)/(float)0xffff);
	return (float)rand()/(float)RAND_MAX;
}

inline bool inRange(const float* v1, const float* v2, const float r, const float h)
{
	const float dx = v2[0] - v1[0];
	const float dy = v2[1] - v1[1];
	const float dz = v2[2] - v1[2];
	return (dx*dx + dz*dz) < r*r && fabsf(dy) < h;
}


static int fixupCorridor(dtPolyRef* path, const int npath, const int maxPath,
						 const dtPolyRef* visited, const int nvisited)
{
	int furthestPath = -1;
	int furthestVisited = -1;
	
	// Find furthest common polygon.
	for (int i = npath-1; i >= 0; --i)
	{
		bool found = false;
		for (int j = nvisited-1; j >= 0; --j)
		{
			if (path[i] == visited[j])
			{
				furthestPath = i;
				furthestVisited = j;
				found = true;
			}
		}
		if (found)
			break;
	}

	// If no intersection found just return current path. 
	if (furthestPath == -1 || furthestVisited == -1)
		return npath;
	
	// Concatenate paths.	

	// Adjust beginning of the buffer to include the visited.
	const int req = nvisited - furthestVisited;
	const int orig = rcMin(furthestPath+1, npath);
	int size = rcMax(0, npath-orig);
	if (req+size > maxPath)
		size = maxPath-req;
	if (size)
		memmove(path+req, path+orig, size*sizeof(dtPolyRef));
	
	// Store visited
	for (int i = 0; i < req; ++i)
		path[i] = visited[(nvisited-1)-i];				
	
	return req+size;
}

// This function checks if the path has a small U-turn, that is,
// a polygon further in the path is adjacent to the first polygon
// in the path. If that happens, a shortcut is taken.
// This can happen if the target (T) location is at tile boundary,
// and we're (S) approaching it parallel to the tile edge.
// The choice at the vertex can be arbitrary, 
//  +---+---+
//  |:::|:::|
//  +-S-+-T-+
//  |:::|   | <-- the step can end up in here, resulting U-turn path.
//  +---+---+
static int fixupShortcuts(dtPolyRef* path, int npath, dtNavMeshQuery* navQuery)
{
	if (npath < 3)
		return npath;

	// Get connected polygons
	static const int maxNeis = 16;
	dtPolyRef neis[maxNeis];
	int nneis = 0;

	const dtMeshTile* tile = 0;
	const dtPoly* poly = 0;
	if (dtStatusFailed(navQuery->getAttachedNavMesh()->getTileAndPolyByRef(path[0], &tile, &poly)))
		return npath;
	
	for (unsigned int k = poly->firstLink; k != DT_NULL_LINK; k = tile->links[k].next)
	{
		const dtLink* link = &tile->links[k];
		if (link->ref != 0)
		{
			if (nneis < maxNeis)
				neis[nneis++] = link->ref;
		}
	}

	// If any of the neighbour polygons is within the next few polygons
	// in the path, short cut to that polygon directly.
	static const int maxLookAhead = 6;
	int cut = 0;
	for (int i = dtMin(maxLookAhead, npath) - 1; i > 1 && cut == 0; i--) {
		for (int j = 0; j < nneis; j++)
		{
			if (path[i] == neis[j]) {
				cut = i;
				break;
			}
		}
	}
	if (cut > 1)
	{
		int offset = cut-1;
		npath -= offset;
		for (int i = 1; i < npath; i++)
			path[i] = path[i+offset];
	}

	return npath;
}

static bool getSteerTarget(dtNavMeshQuery* navQuery, const float* startPos, const float* endPos,
						   const float minTargetDist,
						   const dtPolyRef* path, const int pathSize,
						   float* steerPos, unsigned char& steerPosFlag, dtPolyRef& steerPosRef,
						   float* outPoints = 0, int* outPointCount = 0)							 
{
	// Find steer target.
	static const int MAX_STEER_POINTS = 3;
	float steerPath[MAX_STEER_POINTS*3];
	unsigned char steerPathFlags[MAX_STEER_POINTS];
	dtPolyRef steerPathPolys[MAX_STEER_POINTS];
	int nsteerPath = 0;
	navQuery->findStraightPath(startPos, endPos, path, pathSize,
							   steerPath, steerPathFlags, steerPathPolys, &nsteerPath, MAX_STEER_POINTS);
	if (!nsteerPath)
		return false;
		
	if (outPoints && outPointCount)
	{
		*outPointCount = nsteerPath;
		for (int i = 0; i < nsteerPath; ++i)
			dtVcopy(&outPoints[i*3], &steerPath[i*3]);
	}

	
	// Find vertex far enough to steer to.
	int ns = 0;
	while (ns < nsteerPath)
	{
		// Stop at Off-Mesh link or when point is further than slop away.
		if ((steerPathFlags[ns] & DT_STRAIGHTPATH_OFFMESH_CONNECTION) ||
			!inRange(&steerPath[ns*3], startPos, minTargetDist, 1000.0f))
			break;
		ns++;
	}
	// Failed to find good point to steer to.
	if (ns >= nsteerPath)
		return false;
	
	dtVcopy(steerPos, &steerPath[ns*3]);
	steerPos[1] = startPos[1];
	steerPosFlag = steerPathFlags[ns];
	steerPosRef = steerPathPolys[ns];
	
	return true;
}


NavMeshTesterTool::NavMeshTesterTool() :
	m_sample(0),
	m_navMesh(0),
	m_navQuery(0),
	m_pathFindStatus(DT_FAILURE),
    m_toolMode(TOOLMODE_PATHFIND_STRAIGHT),
	m_straightPathOptions(0),
	m_startRef(0),
	m_endRef(0),
	m_npolys(0),
	m_nstraightPath(0),
	m_nrandPoints(0),
	m_randPointsInCircle(false),
	m_sposSet(false),
    m_eposSet(false)
{
	m_filter.setIncludeFlags(SAMPLE_POLYFLAGS_ALL ^ SAMPLE_POLYFLAGS_DISABLED);
	m_filter.setExcludeFlags(0);

	m_polyPickExt[0] = 2;
	m_polyPickExt[1] = 4;
	m_polyPickExt[2] = 2;
	
	m_randomRadius = 5.0f;
}

NavMeshTesterTool::~NavMeshTesterTool()
{
}

void NavMeshTesterTool::init(Sample* sample)
{
	m_sample = sample;
	m_navMesh = sample->getNavMesh();
	m_navQuery = sample->getNavMeshQuery();
	recalc();

	if (m_navQuery)
	{
		// Change costs.
		m_filter.setAreaCost(SAMPLE_POLYAREA_GROUND, 1.0f);
		m_filter.setAreaCost(SAMPLE_POLYAREA_WATER, 10.0f);
		m_filter.setAreaCost(SAMPLE_POLYAREA_ROAD, 1.0f);
		m_filter.setAreaCost(SAMPLE_POLYAREA_DOOR, 1.0f);
		m_filter.setAreaCost(SAMPLE_POLYAREA_GRASS, 2.0f);
		m_filter.setAreaCost(SAMPLE_POLYAREA_JUMP, 1.5f);
	}
	
	m_randomRadius = sample->getAgentRadius() * 30.0f;
}

void NavMeshTesterTool::handleMenu()
{
	if (imguiCheck("Pathfind Straight", m_toolMode == TOOLMODE_PATHFIND_STRAIGHT))
	{
		m_toolMode = TOOLMODE_PATHFIND_STRAIGHT;
		recalc();
	}
	if (m_toolMode == TOOLMODE_PATHFIND_STRAIGHT)
	{
		imguiIndent();
		imguiLabel("Vertices at crossings");
		if (imguiCheck("None", m_straightPathOptions == 0))
		{
			m_straightPathOptions = 0;
			recalc();
		}
		if (imguiCheck("Area", m_straightPathOptions == DT_STRAIGHTPATH_AREA_CROSSINGS))
		{
			m_straightPathOptions = DT_STRAIGHTPATH_AREA_CROSSINGS;
			recalc();
		}
		if (imguiCheck("All", m_straightPathOptions == DT_STRAIGHTPATH_ALL_CROSSINGS))
		{
			m_straightPathOptions = DT_STRAIGHTPATH_ALL_CROSSINGS;
			recalc();
		}

		imguiUnindent();
	}

	imguiSeparator();
	
	if (imguiButton("Set Random Start"))
	{
		dtStatus status = m_navQuery->findRandomPoint(&m_filter, frand, &m_startRef, m_spos);
		if (dtStatusSucceed(status))
		{
			m_sposSet = true;
			recalc();
		}
	}
	if (imguiButton("Set Random End", m_sposSet))
	{
		if (m_sposSet)
		{
			dtStatus status = m_navQuery->findRandomPointAroundCircle(m_startRef, m_spos, m_randomRadius, &m_filter, frand, &m_endRef, m_epos);
			if (dtStatusSucceed(status))
			{
				m_eposSet = true;
				recalc();
			}
		}
	}

	imguiSeparator();

	if (imguiButton("Make Random Points"))
	{
		m_randPointsInCircle = false;
		m_nrandPoints = 0;
		for (int i = 0; i < MAX_RAND_POINTS; i++)
		{
			float pt[3];
			dtPolyRef ref;
			dtStatus status = m_navQuery->findRandomPoint(&m_filter, frand, &ref, pt);
			if (dtStatusSucceed(status))
			{
				dtVcopy(&m_randPoints[m_nrandPoints*3], pt);
				m_nrandPoints++;
			}
		}
	}
	if (imguiButton("Make Random Points Around", m_sposSet))
	{
		if (m_sposSet)
		{
			m_nrandPoints = 0;
			m_randPointsInCircle = true;
			for (int i = 0; i < MAX_RAND_POINTS; i++)
			{
				float pt[3];
				dtPolyRef ref;
				dtStatus status = m_navQuery->findRandomPointAroundCircle(m_startRef, m_spos, m_randomRadius, &m_filter, frand, &ref, pt);
				if (dtStatusSucceed(status))
				{
					dtVcopy(&m_randPoints[m_nrandPoints*3], pt);
					m_nrandPoints++;
				}
			}
		}
	}

	
	imguiSeparator();

	imguiLabel("Include Flags");

	imguiIndent();
	if (imguiCheck("Walk", (m_filter.getIncludeFlags() & SAMPLE_POLYFLAGS_WALK) != 0))
	{
		m_filter.setIncludeFlags(m_filter.getIncludeFlags() ^ SAMPLE_POLYFLAGS_WALK);
		recalc();
	}
	if (imguiCheck("Swim", (m_filter.getIncludeFlags() & SAMPLE_POLYFLAGS_SWIM) != 0))
	{
		m_filter.setIncludeFlags(m_filter.getIncludeFlags() ^ SAMPLE_POLYFLAGS_SWIM);
		recalc();
	}
	if (imguiCheck("Door", (m_filter.getIncludeFlags() & SAMPLE_POLYFLAGS_DOOR) != 0))
	{
		m_filter.setIncludeFlags(m_filter.getIncludeFlags() ^ SAMPLE_POLYFLAGS_DOOR);
		recalc();
	}
	if (imguiCheck("Jump", (m_filter.getIncludeFlags() & SAMPLE_POLYFLAGS_JUMP) != 0))
	{
		m_filter.setIncludeFlags(m_filter.getIncludeFlags() ^ SAMPLE_POLYFLAGS_JUMP);
		recalc();
	}
	imguiUnindent();

	imguiSeparator();
	imguiLabel("Exclude Flags");
	
	imguiIndent();
	if (imguiCheck("Walk", (m_filter.getExcludeFlags() & SAMPLE_POLYFLAGS_WALK) != 0))
	{
		m_filter.setExcludeFlags(m_filter.getExcludeFlags() ^ SAMPLE_POLYFLAGS_WALK);
		recalc();
	}
	if (imguiCheck("Swim", (m_filter.getExcludeFlags() & SAMPLE_POLYFLAGS_SWIM) != 0))
	{
		m_filter.setExcludeFlags(m_filter.getExcludeFlags() ^ SAMPLE_POLYFLAGS_SWIM);
		recalc();
	}
	if (imguiCheck("Door", (m_filter.getExcludeFlags() & SAMPLE_POLYFLAGS_DOOR) != 0))
	{
		m_filter.setExcludeFlags(m_filter.getExcludeFlags() ^ SAMPLE_POLYFLAGS_DOOR);
		recalc();
	}
	if (imguiCheck("Jump", (m_filter.getExcludeFlags() & SAMPLE_POLYFLAGS_JUMP) != 0))
	{
		m_filter.setExcludeFlags(m_filter.getExcludeFlags() ^ SAMPLE_POLYFLAGS_JUMP);
		recalc();
	}
	imguiUnindent();

	imguiSeparator();	
}

void NavMeshTesterTool::handleClick(const float* /*s*/, const float* p, bool shift)
{
	if (shift)
	{
		m_sposSet = true;
		dtVcopy(m_spos, p);
	}
	else
	{
		m_eposSet = true;
		dtVcopy(m_epos, p);
	}
	recalc();
}

void NavMeshTesterTool::handleStep()
{
}

void NavMeshTesterTool::handleToggle()
{

}

void NavMeshTesterTool::handleUpdate(const float /*dt*/)
{
}

void NavMeshTesterTool::reset()
{
	m_startRef = 0;
	m_endRef = 0;
	m_npolys = 0;
    m_nstraightPath = 0;
}


void NavMeshTesterTool::recalc()
{
	if (!m_navMesh)
		return;
	
	if (m_sposSet)
		m_navQuery->findNearestPoly(m_spos, m_polyPickExt, &m_filter, &m_startRef, 0);
	else
		m_startRef = 0;
	
	if (m_eposSet)
		m_navQuery->findNearestPoly(m_epos, m_polyPickExt, &m_filter, &m_endRef, 0);
	else
		m_endRef = 0;
	
	m_pathFindStatus = DT_FAILURE;
	
    if (m_toolMode == TOOLMODE_PATHFIND_STRAIGHT)
	{
		if (m_sposSet && m_eposSet && m_startRef && m_endRef)
		{
#ifdef DUMP_REQS
			printf("ps  %f %f %f  %f %f %f  0x%x 0x%x\n",
				   m_spos[0],m_spos[1],m_spos[2], m_epos[0],m_epos[1],m_epos[2],
				   m_filter.getIncludeFlags(), m_filter.getExcludeFlags()); 
#endif
			m_navQuery->findPath(m_startRef, m_endRef, m_spos, m_epos, &m_filter, m_polys, &m_npolys, MAX_POLYS);
			m_nstraightPath = 0;
			if (m_npolys)
			{
				// In case of partial path, make sure the end point is clamped to the last polygon.
				float epos[3];
				dtVcopy(epos, m_epos);
				if (m_polys[m_npolys-1] != m_endRef)
					m_navQuery->closestPointOnPoly(m_polys[m_npolys-1], m_epos, epos, 0);
				
				m_navQuery->findStraightPath(m_spos, epos, m_polys, m_npolys,
											 m_straightPath, m_straightPathFlags,
											 m_straightPathPolys, &m_nstraightPath, MAX_POLYS, m_straightPathOptions);
			}
		}
		else
		{
			m_npolys = 0;
			m_nstraightPath = 0;
		}
	}
}

static void getPolyCenter(dtNavMesh* navMesh, dtPolyRef ref, float* center)
{
	center[0] = 0;
	center[1] = 0;
	center[2] = 0;
	
	const dtMeshTile* tile = 0;
	const dtPoly* poly = 0;
	dtStatus status = navMesh->getTileAndPolyByRef(ref, &tile, &poly);
	if (dtStatusFailed(status))
		return;
		
	for (int i = 0; i < (int)poly->vertCount; ++i)
	{
		const float* v = &tile->verts[poly->verts[i]*3];
		center[0] += v[0];
		center[1] += v[1];
		center[2] += v[2];
	}
	const float s = 1.0f / poly->vertCount;
	center[0] *= s;
	center[1] *= s;
	center[2] *= s;
}



void NavMeshTesterTool::handleRender()
{
	DebugDrawGL dd;
	
	static const unsigned int startCol = duRGBA(128,25,0,192);
	static const unsigned int endCol = duRGBA(51,102,0,129);
	static const unsigned int pathCol = duRGBA(0,0,0,64);
	
	const float agentRadius = m_sample->getAgentRadius();
	const float agentHeight = m_sample->getAgentHeight();
	const float agentClimb = m_sample->getAgentClimb();
	
	dd.depthMask(false);
	if (m_sposSet)
		drawAgent(m_spos, agentRadius, agentHeight, agentClimb, startCol);
	if (m_eposSet)
		drawAgent(m_epos, agentRadius, agentHeight, agentClimb, endCol);
	dd.depthMask(true);
	
	if (!m_navMesh)
	{
		return;
	}

    if (m_toolMode == TOOLMODE_PATHFIND_STRAIGHT)
	{
		duDebugDrawNavMeshPoly(&dd, *m_navMesh, m_startRef, startCol);
		duDebugDrawNavMeshPoly(&dd, *m_navMesh, m_endRef, endCol);
		
		if (m_npolys)
		{
			for (int i = 0; i < m_npolys; ++i)
			{
				if (m_polys[i] == m_startRef || m_polys[i] == m_endRef)
					continue;
				duDebugDrawNavMeshPoly(&dd, *m_navMesh, m_polys[i], pathCol);
			}
		}
		
		if (m_nstraightPath)
		{
			dd.depthMask(false);
			const unsigned int spathCol = duRGBA(64,16,0,220);
			const unsigned int offMeshCol = duRGBA(128,96,0,220);
			dd.begin(DU_DRAW_LINES, 2.0f);
			for (int i = 0; i < m_nstraightPath-1; ++i)
			{
				unsigned int col = 0;
				if (m_straightPathFlags[i] & DT_STRAIGHTPATH_OFFMESH_CONNECTION)
					col = offMeshCol;
				else
					col = spathCol;
				
				dd.vertex(m_straightPath[i*3], m_straightPath[i*3+1]+0.4f, m_straightPath[i*3+2], col);
				dd.vertex(m_straightPath[(i+1)*3], m_straightPath[(i+1)*3+1]+0.4f, m_straightPath[(i+1)*3+2], col);
			}
			dd.end();
			dd.begin(DU_DRAW_POINTS, 6.0f);
			for (int i = 0; i < m_nstraightPath; ++i)
			{
				unsigned int col = 0;
				if (m_straightPathFlags[i] & DT_STRAIGHTPATH_START)
					col = startCol;
				else if (m_straightPathFlags[i] & DT_STRAIGHTPATH_START)
					col = endCol;
				else if (m_straightPathFlags[i] & DT_STRAIGHTPATH_OFFMESH_CONNECTION)
					col = offMeshCol;
				else
					col = spathCol;
				dd.vertex(m_straightPath[i*3], m_straightPath[i*3+1]+0.4f, m_straightPath[i*3+2], col);
			}
			dd.end();
			dd.depthMask(true);
		}
	}

	if (m_nrandPoints > 0)
	{
		dd.begin(DU_DRAW_POINTS, 6.0f);
		for (int i = 0; i < m_nrandPoints; i++)
		{
			const float* p = &m_randPoints[i*3];
			dd.vertex(p[0],p[1]+0.1f,p[2], duRGBA(220,32,16,192));
		} 
		dd.end();
		
		if (m_randPointsInCircle && m_sposSet)
		{
			duDebugDrawCircle(&dd, m_spos[0], m_spos[1]+agentHeight/2, m_spos[2], m_randomRadius, duRGBA(64,16,0,220), 2.0f);
		}
	}
}

void NavMeshTesterTool::handleRenderOverlay(double* proj, double* model, int* view)
{
	GLdouble x, y, z;
	
	// Draw start and end point labels
	if (m_sposSet && gluProject((GLdouble)m_spos[0], (GLdouble)m_spos[1], (GLdouble)m_spos[2],
								model, proj, view, &x, &y, &z))
	{
		imguiDrawText((int)x, (int)(y-25), IMGUI_ALIGN_CENTER, "Start", imguiRGBA(0,0,0,220));
	}
	if (m_eposSet && gluProject((GLdouble)m_epos[0], (GLdouble)m_epos[1], (GLdouble)m_epos[2],
								model, proj, view, &x, &y, &z))
	{
		imguiDrawText((int)x, (int)(y-25), IMGUI_ALIGN_CENTER, "End", imguiRGBA(0,0,0,220));
	}
	
	// Tool help
	const int h = view[3];
	imguiDrawText(280, h-40, IMGUI_ALIGN_LEFT, "LMB+SHIFT: Set start location  LMB: Set end location", imguiRGBA(255,255,255,192));	
}

void NavMeshTesterTool::drawAgent(const float* pos, float r, float h, float c, const unsigned int col)
{
	DebugDrawGL dd;
	
	dd.depthMask(false);
	
	// Agent dimensions.	
	duDebugDrawCylinderWire(&dd, pos[0]-r, pos[1]+0.02f, pos[2]-r, pos[0]+r, pos[1]+h, pos[2]+r, col, 2.0f);

	duDebugDrawCircle(&dd, pos[0],pos[1]+c,pos[2],r,duRGBA(0,0,0,64),1.0f);

	unsigned int colb = duRGBA(0,0,0,196);
	dd.begin(DU_DRAW_LINES);
	dd.vertex(pos[0], pos[1]-c, pos[2], colb);
	dd.vertex(pos[0], pos[1]+c, pos[2], colb);
	dd.vertex(pos[0]-r/2, pos[1]+0.02f, pos[2], colb);
	dd.vertex(pos[0]+r/2, pos[1]+0.02f, pos[2], colb);
	dd.vertex(pos[0], pos[1]+0.02f, pos[2]-r/2, colb);
	dd.vertex(pos[0], pos[1]+0.02f, pos[2]+r/2, colb);
	dd.end();
	
	dd.depthMask(true);
}
