
/*
 * Copyright (c) 2004, Laminar Research.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include "MapDefs.h"
#include "MeshAlgs.h"
#include "ParamDefs.h"
#include "CompGeomDefs2.h"
#include "CompGeomDefs3.h"
#include "CompGeomUtils.h"
#include "PolyRasterUtils.h"
#include <CGAL/Triangulation_conformer_2.h>
#include "AssertUtils.h"
#include "PlatformUtils.h"
#include "PerfUtils.h"
#include "MapAlgs.h"
#include "DEMAlgs.h"
#include "DEMTables.h"
#include "GISUtils.h"
#include "GreedyMesh.h"
#if APL && !defined(__MACH__)
#define __DEBUGGING__
#include "XUtils.h"
#endif
#define PROFILE_PERFORMANCE 1


#if PHONE
#define LOW_RES_WATER_INTERVAL 50
#else
#define LOW_RES_WATER_INTERVAL 40
#endif

#define NO_BORDER_SHARING 0

#define DEBUG_DROPPED_PTS 0

#if DEBUG_DROPPED_PTS
#include "GISTool_Globals.h"
#endif

#if PROFILE_PERFORMANCE
#define TIMER(x)	StElapsedTime	__PerfTimer##x(#x);
#else
#define TIMER(x)
#endif

MeshPrefs_t gMeshPrefs = {
#if PHONE
				25000,	// 30000,
				15,		// 12.0,
#else
				78000,	// 65000,
				6.25,	// 7.5,
#endif
				1,
				1,
#if PHONE
				6000.0,	// 3500.0,	// 3500.0,
				50000.0
#else
				3500.0,
				50000.0
#endif
};

/* Conststraint markers - here's the deal: the way we set the water body
 * triangles to water (precisely) is we remember the pairs of vertices that
 * make up their constrained edges.  These vertices are directed and form
 * a CCB, so we know that the left side of this pair is a triangle that is
 * wet.  This lets us seed the water-finding process. */

typedef pair<CDT::Vertex_handle, CDT::Vertex_handle>	ConstraintMarker_t;
// won't compile if these are const, even though they really are
typedef pair< Halfedge_handle,  Halfedge_handle>	LandusePair_t;			// "Left" and "right" side
//typedef pair<const Halfedge_handle, const Halfedge_handle>	LandusePair_t;			// "Left" and "right" side
typedef pair<ConstraintMarker_t, LandusePair_t>			LanduseConstraint_t;

struct FaceHandleVectorHack {
	vector<CDT::Face_handle>	v;
};

static	void	SlightClampToDEM(Point_2& ioPoint, const DEMGeo& ioDEM);

inline bool	PersistentFindEdge(CDT& ioMesh, CDT::Vertex_handle a, CDT::Vertex_handle b, CDT::Face_handle& h, int& vnum)
{
	if (ioMesh.is_edge(a, b, h, vnum))
	{
		DebugAssert(ioMesh.is_constrained(CDT::Edge(h, vnum)));
		return true;
	}

	Vector_2	along(a->point(), b->point());

	CDT::Vertex_handle v = a;
	do {
		CDT::Vertex_circulator	circ, stop;
		CDT::Vertex_handle best = CDT::Vertex_handle();
		circ = stop = ioMesh.incident_vertices(v);
		do {
			if(!ioMesh.is_infinite(circ))				// Skip infinite vertices, for which the point location is undefined and will piss off CGAL!!
			{
				if (v->point() == circ->point())
				{
					best = circ;
					break;
				} else {
					Vector_2	step(v->point(), circ->point());
					if (along * step > 0.0 && (along.x() * step.y() == along.y() * step.x()))
					{
						best = circ;
						break;
					}
				}
			}
			++circ;
		} while (circ != stop);

		if (best == CDT::Vertex_handle()) 
			return false;
		if (!ioMesh.is_edge(v, best, h, vnum)) 
			return false;
		DebugAssert(ioMesh.is_constrained(CDT::Edge(h, vnum)));

		v = best;
	} while (v != b);

	return true;
}

inline bool IsEdgeVertex(CDT& inMesh, CDT::Vertex_handle v)
{
	CDT::Vertex_circulator circ, stop;
	circ = stop = inMesh.incident_vertices(v);
	do {
		if (inMesh.is_infinite(circ)) return true;
		circ++;
	} while (circ != stop);
	return false;
}

inline bool is_border(const CDT& inMesh, CDT::Face_handle f)
{
	for (int n = 0; n < 3; ++n)
	{
		if (f->neighbor(n)->has_vertex(inMesh.infinite_vertex()))
			return true;
	}
	return false;
}

inline void FindNextEast(CDT& ioMesh, CDT::Face_handle& ioFace, int& index, bool is_bot_edge)
{
	CDT::Vertex_handle sv = ioFace->vertex(index);
	CDT::Point p = sv->point();
	CDT::Vertex_circulator stop, now;
	stop = now = ioMesh.incident_vertices(sv);

	//printf("Starting with: %lf, %lf\n", CGAL::to_double(sv->point().x()), CGAL::to_double(sv->point().y()));

	CDT::Geom_traits::Compare_y_2 cy;
	CDT::Geom_traits::Compare_x_2 cx;
	do {
		//printf("Checking: %lf, %lf\n", CGAL::to_double(now->point().x()), CGAL::to_double(now->point().y()));
		if (now != ioMesh.infinite_vertex())
		if (cy(now->point(), p) == CGAL::EQUAL)
		if (cx(now->point(), p) == CGAL::LARGER)
		{
			CDT::Face_handle	a_face;
			CDT::Vertex_circulator next = now;
			if(is_bot_edge)	++next;
			else			--next;
			Assert(ioMesh.is_face(sv, now, next, a_face));
			Assert(!ioMesh.is_infinite(a_face));
			ioFace = a_face;
			index = ioFace->index(now);
			return;
		}
		++now;
	} while (stop != now);
	AssertPrintf("Next mesh point not found.");
}

inline void FindNextNorth(CDT& ioMesh, CDT::Face_handle& ioFace, int& index, bool is_right_edge)
{
	CDT::Vertex_handle sv = ioFace->vertex(index);
	CDT::Point p = sv->point();
	CDT::Vertex_circulator stop, now;
	stop = now = ioMesh.incident_vertices(sv);

	//printf("Starting with: %lf, %lf\n", CGAL::to_double(sv->point().x()), CGAL::to_double(sv->point().y()));

	CDT::Geom_traits::Compare_y_2 cy;
	CDT::Geom_traits::Compare_x_2 cx;
	do {
		//printf("Checking: %lf, %lf\n", CGAL::to_double(now->point().x()), CGAL::to_double(now->point().y()));
		if (now != ioMesh.infinite_vertex())
		if (cx(now->point(), p) == CGAL::EQUAL)
		if (cy(now->point(), p) == CGAL::LARGER)
		{
			CDT::Face_handle	a_face;
			CDT::Vertex_circulator next = now;
			if(is_right_edge)	++next;
			else				--next;
			Assert(ioMesh.is_face(sv, now, next, a_face));
			Assert(!ioMesh.is_infinite(a_face));
			ioFace = a_face;
			index = ioFace->index(now);
			return;
		}
		++now;
	} while (stop != now);
	Assert(!"Next pt not found.");
}

// This builds the set of all continuous triangles that have the same variation of a terrain (a contiguous blob if you will)
void	FindAllCovariant(CDT& inMesh, CDT::Face_handle f, set<CDT::Face_handle>& all, Bbox_2& bounds)
{
	bounds = Point_2(f->vertex(0)->point().x(),f->vertex(0)->point().y()).bbox();
	bounds = bounds + Point_2(f->vertex(1)->point().x(),f->vertex(1)->point().y()).bbox();
	bounds = bounds + Point_2(f->vertex(2)->point().x(),f->vertex(2)->point().y()).bbox();

	all.clear();
	set<CDT::Face_handle>	working;
	working.insert(f);

	while (!working.empty())
	{
		CDT::Face_handle w = *working.begin();
		working.erase(working.begin());
		all.insert(w);

		for (int n = 0; n < 3; ++n)
		{
			bounds = bounds + Point_2(w->vertex(0)->point().x(),w->vertex(0)->point().y()).bbox();
			CDT::Face_handle t = w->neighbor(n);

			if (!inMesh.is_infinite(t))
			if (t->info().terrain != terrain_Water)
			if (AreVariants(t->info().terrain, w->info().terrain))
			if (all.count(t) == 0)
				working.insert(t);
		}
	}
}

#pragma mark -
/************************************************************************************************************************
 * BORDER MATCHING
 ************************************************************************************************************************

	BORDER MATCHING - THEORY

	We cannot do proper blending and transitions across DSF borders because we write one DSF at a time - we have no way
	to go back and edit a previous DSF when we get to the next one and find a transition should have leaked across files.
	So instead we use a master slave system...the west and south files always dominate the north and east.

	The right and top borders of a DSF are MASTER borders and the left and bottom are SLAVES.

	When we write a DSF we write out the border info for the master borders into text files - this includes both vertex
	position along the border and texturing.

	When we write a new DSF we find our old master borders via text file and use it to conform our work.

	VERTEX MATCHING

	We write out all vertices on our master border.  For the slave border we add the MINIMUM number of points to the slave
	border - basically just mandatory water-body edges.  We then do a nearest-fit match from the master and add any non-
	matched master vertices to the slave.  This gives exact matchups except for mandatrory features which should be close.
	If the water bodies are not totally discontinuous this works.  X-plane can also resolve very slight vertex discrepancies.

	TRANSITION AND LANDUSE MATCHING

 	Each master edge vertex will contain some level of blending for each border that originates there as well as a set of
 	base transitions from each incident triangle.  (Each base layer can be thought of as being represented at 100%.)  When
 	sorted by priority this forms a total set of 'stuff' intruding from this vertex.

	To blend the border, we build overlays on the slave triangles incident to these borders that have the master's mix levels
	on the incident vertices and 0 levels on the interior.

	REBASING

	There is one problem with the above scheme - if the border from above is LOWER priority than the terrain it will cover,
	the border will not work.  Fundamentally we can't force a border to go left to right against priority!

	So we use a trick called "rebasing".  Basically given a slave tri with a high prio ("HIGH") and a master vertex with a
	low prio terrain ("LOW") we set the base of the slave tri to "LOW" and add a border of type "HIGH" to the slave with 0%
	blend on the edges and 100% in the interior.  We then also find all tris not touching the border incident to the 100% vertex
	and blend from 100% back to 0%.


 */


// This is one vertex from our master
struct	mesh_match_vertex_t {
	Point_2					loc;			// Location in master
	double					height;			// Height in master
	hash_map<int, float>	blending;		// List of borders and blends in master
	CDT::Vertex_handle		buddy;			// Vertex on slave that is matched to it
};

// This is one edge from our master
struct	mesh_match_edge_t {
	int						base;			// For debugging
	set<int>				borders;		// For debugging
	CDT::Face_handle		buddy;			// Tri in our mesh that corresponds
};

struct	mesh_match_t {
	vector<mesh_match_vertex_t>	vertices;
	vector<mesh_match_edge_t>	edges;
};

inline bool MATCH(const char * big, const char * msmall)
{
	return strncmp(big, msmall, strlen(msmall)) == 0;
}

static mesh_match_t gMatchBorders[4];


static void border_find_edge_tris(CDT& ioMesh, mesh_match_t& ioBorder)
{
//	printf("Finding edge tris for %d edgse.\n",ioBorder.edges.size());
	DebugAssert(ioBorder.vertices.size() == (ioBorder.edges.size()+1));
	for (int n = 0; n < ioBorder.edges.size(); ++n)
	{
#if DEV
		CDT::Point	p1 = ioBorder.vertices[n  ].buddy->point();
		CDT::Point	p2 = ioBorder.vertices[n+1].buddy->point();
#endif
		if (!(ioMesh.is_face(ioBorder.vertices[n].buddy, ioBorder.vertices[n+1].buddy, ioMesh.infinite_vertex(), ioBorder.edges[n].buddy)))
//		if (!(ioMesh.is_face(ioBorder.vertices[n+1].buddy, ioBorder.vertices[n].buddy, ioMesh.infinite_vertex(), ioBorder.edges[n].buddy)))
		{
/*
			CDT::Vertex_circulator	circ, stop;
			printf("    Vert 1 vert = %lf,%lf (0x%08X)\n", ioBorder.vertices[n].buddy->point().x(), ioBorder.vertices[n].buddy->point().y(), &*ioBorder.vertices[n].buddy);
			circ = stop = ioMesh.incident_vertices(ioBorder.vertices[n].buddy);
			do {
				printf("    Buddy 1 vert = %lf,%lf (0x%08X)\n", circ->point().x(), circ->point().y(), &*circ);
				++circ;
			} while (circ != stop);
			circ = stop = ioMesh.incident_vertices(ioBorder.vertices[n+ 1].buddy);
			printf("    Vert 2 vert = %lf,%lf (0x%08X)\n", ioBorder.vertices[n+ 1].buddy->point().x(), ioBorder.vertices[n+ 1].buddy->point().y(), &*ioBorder.vertices[n+ 1].buddy);
			do {
				printf("    Buddy 2 vert = %lf,%lf (0x%08X)\n", circ->point().x(), circ->point().y(), &*circ);
				++circ;
			} while (circ != stop);
			AssertPrintf("Border match failure: %lf,%lf to %lf,%lf\n",
				ioBorder.vertices[n  ].buddy->point().x(),
				ioBorder.vertices[n  ].buddy->point().y(),
				ioBorder.vertices[n+1].buddy->point().x(),
				ioBorder.vertices[n+1].buddy->point().y());
*/
		// BEN SEZ: this used to be an error but - there are cases where the SLAVE file has a lake ENDING at the edge...there is no way the MASTER
		// could have induced these pts, so we're screwed.  For now - we'll just blunder on.
			ioBorder.edges[n].buddy = CDT::Face_handle();
		} else {
			int idx = ioBorder.edges[n].buddy->index(ioMesh.infinite_vertex());
			ioBorder.edges[n].buddy = ioBorder.edges[n].buddy->neighbor(idx);
		}
	}
}

inline void AddZeroMixIfNeeded(CDT::Face_handle f, int layer)
{
	if (f->info().terrain == terrain_Water) return;
	DebugAssert(layer != -1);
	f->info().terrain_border.insert(layer);
	for (int i = 0; i < 3; ++i)
	{
		CDT::Vertex_handle vv = f->vertex(i);
		if (vv->info().border_blend.count(layer) == 0)
			vv->info().border_blend[layer] = 0.0;
	}
}

inline void ZapBorders(CDT::Vertex_handle v)
{
	for (hash_map<int, float>::iterator i = v->info().border_blend.begin(); i != v->info().border_blend.end(); ++i)
		i->second = 0.0;
}

static bool	load_match_file(const char * path, mesh_match_t& outLeft, mesh_match_t& outBottom, mesh_match_t& outRight, mesh_match_t& outTop)
{
	outTop.vertices.clear();
	outTop.edges.clear();
	outRight.vertices.clear();
	outRight.edges.clear();
	outBottom.vertices.clear();
	outBottom.edges.clear();
	outLeft.vertices.clear();
	outLeft.edges.clear();

	FILE * fi = fopen(path, "r");
	if (fi == NULL) return false;
	char buf[80];
	bool go = true;
	int count;
	float mix;
	char ter[80];
	double x, y;
	int token;

	for(int b = 0; b < 4; ++b)
	{
		go = true;
		mesh_match_t *	dest;
		switch(b) {
		case 0: dest = &outLeft;	break;
		case 1: dest = &outBottom;	break;
		case 2: dest = &outRight;	break;
		case 3: dest = &outTop;		break;
		}

		while (go)
		{
			if (fgets(buf, sizeof(buf), fi) == NULL) goto bail;
			if (MATCH(buf, "VT"))
			{
				dest->vertices.push_back(mesh_match_vertex_t());
				sscanf(buf, "VT %lf, %lf, %lf", &x, &y, &dest->vertices.back().height);
				dest->vertices.back().loc = Point_2(x,y);
				dest->vertices.back().buddy = NULL;
				//fprintf(stderr, "%lf, %lf  %lf, %lf\n",
				//		CGAL::to_double(dest->vertices.back().loc.x()),
				//		CGAL::to_double(dest->vertices.back().loc.y()), x, y);
			}
			if (MATCH(buf, "VC"))
			{
				go = false;
				dest->vertices.push_back(mesh_match_vertex_t());
				sscanf(buf, "VT %lf, %lf, %lf", &x, &y, &dest->vertices.back().height);
				dest->vertices.back().loc = Point_2(x,y);
				dest->vertices.back().buddy = NULL;
				//fprintf(stderr, "%lf, %lf  %lf, %lf\n",
				//		CGAL::to_double(dest->vertices.back().loc.x()),
				//		CGAL::to_double(dest->vertices.back().loc.y()), x, y);
			}
			if (fgets(buf, sizeof(buf), fi) == NULL) goto bail;
			sscanf(buf, "VBC %d", &count);
			while (count--)
			{
				if (fgets(buf, sizeof(buf), fi) == NULL) goto bail;
				sscanf(buf, "VB %f %s", &mix, ter);
				dest->vertices.back().blending[token=LookupToken(ter)] = mix;
				DebugAssert(token != -1);
			}
			if (go)
			{
				if (fgets(buf, sizeof(buf), fi) == NULL) goto bail;
				sscanf(buf, "TERRAIN %s", ter);
				dest->edges.push_back(mesh_match_edge_t());
				dest->edges.back().base = token=LookupToken(ter);
				DebugAssert(token != -1);
				if (fgets(buf, sizeof(buf), fi) == NULL) goto bail;
				sscanf(buf, "BORDER_C %d", &count);
				while (count--)
				{
					if (fgets(buf, sizeof(buf), fi) == NULL) goto bail;
					sscanf(buf, "BORDER_T %s", ter);
					dest->edges.back().borders.insert( token=LookupToken(ter));
					DebugAssert(token != -1);
				}
			}
		}
	}

	return true;

bail:
	outTop.vertices.clear();
	outTop.edges.clear();
	outRight.vertices.clear();
	outRight.edges.clear();
	outBottom.vertices.clear();
	outBottom.edges.clear();
	outLeft.vertices.clear();
	outLeft.edges.clear();
	fclose(fi);
	return false;
}

// Given a point on the left edge of the top border or top edge of the right border, this fetches all border
// points in order of distance from that origin.
void	fetch_border(CDT& ioMesh, const Point_2& origin, map<double, CDT::Vertex_handle>& outPts, int side_num)
{
	CDT::Vertex_handle sv = ioMesh.infinite_vertex();
	CDT::Vertex_circulator stop, now;
	stop = now = ioMesh.incident_vertices(sv);

	CDT::Point	pt(origin.x(), origin.y());

	outPts.clear();

	CDT::Geom_traits::Compare_y_2 cy;
	CDT::Geom_traits::Compare_x_2 cx;
	do {
		double dist;
		if ((side_num == 0 || side_num == 2) && cx(now->point(), pt) == CGAL::EQUAL)
		{
			dist = CGAL::to_double(now->point().y() - origin.y());
			DebugAssert(outPts.count(dist)==0);
			outPts[dist] = now;
		}
		if ((side_num == 1 || side_num == 3) && cy(now->point(), pt) == CGAL::EQUAL)
		{
			dist = CGAL::to_double(now->point().x() - origin.x());
			DebugAssert(outPts.count(dist)==0);
			outPts[dist] = now;
		}

		++now;
	} while (stop != now);
}

// Border matching:
// We are going to go through a master edge from an old render and our slave render and try to correlate
// vertices.  This is a 3-part algorithm:
// 1. Find all of the slave edge points.
// 2. Match existing slave points with master points.
// 3. Induce any extra slave points as needed.

void	match_border(CDT& ioMesh, mesh_match_t& ioBorder, int side_num)
{
	map<double, CDT::Vertex_handle>	slaves;					// Slave map, from relative border offset to the handle.  Allows for fast slave location.
	Point_2	origin = ioBorder.vertices.front().loc;			// Origin of our tile.

	// Step 1.  Fetch the entire border from the mesh.
	fetch_border(ioMesh, origin, slaves, side_num);

	// Step 2. Until we have exhausted all of the slaves, we are going to try to find the neaerest master-slave pair and link them.

	while (!slaves.empty())
	{
		multimap<double, pair<double, mesh_match_vertex_t *> >	nearest;	// This is a slave/master pair - slave is IDed by its offset.

		// Go through each non-assigned vertex.
		for (vector<mesh_match_vertex_t>::iterator pts = ioBorder.vertices.begin(); pts != ioBorder.vertices.end(); ++pts)
		if (pts->buddy == NULL)
		{
			// Find the nearest slave for it by decreasing distance.
			for (map<double, CDT::Vertex_handle>::iterator sl = slaves.begin(); sl != slaves.end(); ++sl)
			{
				double myDist = (side_num == 0 || side_num == 2) ? (CGAL::to_double(pts->loc.y() - sl->second->point().y())) : (CGAL::to_double(pts->loc.x() - sl->second->point().x()));
				if (myDist < 0.0) myDist = -myDist;
				nearest.insert(multimap<double, pair<double, mesh_match_vertex_t *> >::value_type(myDist, pair<double, mesh_match_vertex_t *>(sl->first, &*pts)));
			}
		}

		// If we have not found a nearest pair, it means that we have assigned all masters to slaves and still have slaves left over!  This happens when we
		// cannot conform the border due to more water in the slave than master (or at least different water).  The most common case is the US-Canada border,
		// where the US is the master, Canada is the slave; because the US is not hydro-reconstructed it does not force the Canada border to water match.  We just
		// accept a discontinuity on the 49th parallel for now. :-(
		if (nearest.empty())
			break;

		// File off the match, and nuke the slave.
		pair<double, mesh_match_vertex_t *> best_match = nearest.begin()->second;
		DebugAssert(slaves.count(best_match.first) > 0);
		best_match.second->buddy = slaves[best_match.first];
		//printf("Matched: %lf,%lf to %lf,%lf\n",			CGAL::to_double(best_match.second->buddy->point().x()),
		//	   CGAL::to_double(best_match.second->buddy->point().y()),
		//	   CGAL::to_double(best_match.second->loc.x()),			CGAL::to_double(best_match.second->loc.y()));
		slaves.erase(best_match.first);

//		gMeshPoints.push_back(pair<Point2,Point3>(cgal2ben(best_match.second->buddy->point()	),Point3(1,1,0)));
//		gMeshPoints.push_back(pair<Point2,Point3>(cgal2ben(best_match.second->loc				),Point3(0,1,0)));
		
	}

	// Step 3.  Go through all unmatched masters and insert them diriectly into the mesh.
	CDT::Face_handle	nearf = NULL;
	for (vector<mesh_match_vertex_t>::iterator pts = ioBorder.vertices.begin(); pts != ioBorder.vertices.end(); ++pts)
	if (pts->buddy == NULL)
	{	
		//printf("Found no buddy for: %lf,%lf\n", CGAL::to_double(pts->loc.x()), CGAL::to_double(pts->loc.y()));
		pts->buddy = ioMesh.safe_insert(CDT::Point(CGAL::to_double(pts->loc.x()), CGAL::to_double(pts->loc.y())), nearf);
		nearf = pts->buddy->face();
		pts->buddy->info().height = pts->height;
//		gMeshPoints.push_back(pair<Point2,Point3>(cgal2ben(pts->loc),Point3(1,0,0)));
		
	}

	// At this point all masters have a slave, and some slaves may be connected to a master.
}

// RebaseTriangle -

inline bool has_no_xon(int tex1, int tex2)
{
	int ind1 = gNaturalTerrainIndex[tex1];
	int	ind2 = gNaturalTerrainIndex[tex2];

	NaturalTerrainInfo_t& rec1(gNaturalTerrainTable[ind1]);
	NaturalTerrainInfo_t& rec2(gNaturalTerrainTable[ind2]);

	return rec1.xon_dist == 0.0 || rec2.xon_dist == 0.0;
}

static void RebaseTriangle(CDT& ioMesh, CDT::Face_handle tri, int new_base, CDT::Vertex_handle v1, CDT::Vertex_handle v2, set<CDT::Vertex_handle>& ioModVertices)
{
	int old_base = tri->info().terrain;

	if (old_base == terrain_Water || new_base == terrain_Water)
		return;
	if(has_no_xon(old_base,new_base))
		return;

	DebugAssert(new_base != terrain_Water);
	DebugAssert(tri->info().terrain != terrain_Water);
	tri->info().terrain = new_base;
	if (new_base != terrain_Water)
	{
		DebugAssert(old_base != -1);
		tri->info().terrain_border.insert(old_base);

		for (int i = 0; i < 3; ++i)
		{
			CDT::Vertex_handle v = tri->vertex(i);
			if (v == v1 || v == v2)
				v->info().border_blend[old_base] = max(v->info().border_blend[old_base], 0.0f);
			else {
				v->info().border_blend[old_base] = 1.0;
				ioModVertices.insert(v);
			}
		}
	}
}

// Safe-smear border: when we have a vertex involved in a border from a master file
// then we need to make sure all incident triangles can transition out1
void SafeSmearBorder(CDT& mesh, CDT::Vertex_handle vert, int layer)
{
	if (vert->info().border_blend[layer] > 0.0)
	{
		CDT::Face_circulator iter, stop;
		iter = stop = mesh.incident_faces(vert);
		do {
			if (!mesh.is_infinite(iter))
			if (iter->info().terrain != layer)
			if (iter->info().terrain != terrain_Water)
			{
				DebugAssert(layer != -1);
				iter->info().terrain_border.insert(layer);
				for (int n = 0; n < 3; ++n)
				{
					CDT::Vertex_handle v = iter->vertex(n);
					v->info().border_blend[layer] = max(0.0f, v->info().border_blend[layer]);
				}
			}
			++iter;
		} while (iter != stop);
	}
}

#pragma mark -
/************************************************************************************************************************
 * TRANSITIONS
 ************************************************************************************************************************/

inline int MAJORITY_RULES(int a, int b, int c, int d)
{
	int la = 1, lb = 1, lc = 1, ld = 1;
	if (a == b) ++la, ++lb;
	if (a == c) ++la, ++lc;
	if (a == d) ++la, ++ld;
	if (b == c) ++lb, ++lc;
	if (b == d) ++lb, ++ld;
	if (c == d) ++lc, ++ld;

	if (la >= lb && la >= lc && la >= ld) return a;
	if (lb >= la && lb >= lc && lb >= ld) return b;
	if (lc >= la && lc >= lb && lc >= ld) return c;
	if (ld >= la && ld >= lb && ld >= lc) return d;
	return a;
}

inline float SAFE_AVERAGE(float a, float b, float c)
{
	int i = 0;
	float t = 0.0;
	if (a != DEM_NO_DATA) t += a, ++i;
	if (b != DEM_NO_DATA) t += b, ++i;
	if (c != DEM_NO_DATA) t += c, ++i;
	if (i == 0) return DEM_NO_DATA;
	return t / i;
}

inline float SAFE_MAX(float a, float b, float c)
{
	return max(a, max(b, c));
}

inline double GetXonDist(int layer1, int layer2, double y_normal)
{
	int ind1 = gNaturalTerrainIndex[layer1];
	int	ind2 = gNaturalTerrainIndex[layer2];

	NaturalTerrainInfo_t& rec1(gNaturalTerrainTable[ind1]);
	NaturalTerrainInfo_t& rec2(gNaturalTerrainTable[ind2]);

#if DEV
	const char * t1 = FetchTokenString(rec1.name);
	const char * t2 = FetchTokenString(rec2.name);
#endif

	double dist_1 = rec1.xon_dist;
	double dist_2 = rec2.xon_dist;

	double base_dist = min(dist_1, dist_2);

return base_dist * y_normal;

	if (!rec1.xon_hack ||
		!rec2.xon_hack ||
		rec1.terrain == terrain_Airport ||
		rec2.terrain == terrain_Airport
		) return base_dist * y_normal;

#if 0
	bool diff_lat = 	rec1.lat_min != rec2.lat_min ||
						rec1.lat_max != rec2.lat_max;

	bool diff_temp = 	rec1.temp_min > rec2.temp_max ||
				     	rec2.temp_min > rec1.temp_max;

	bool diff_rain = 	rec1.rain_min > rec2.rain_max ||
				     	rec2.rain_min > rec1.rain_max;

	bool diff_temp_rng =rec1.temp_rng_min > rec2.temp_rng_max ||
				     	rec2.temp_rng_min > rec1.temp_rng_max;

	if (rec1.temp_min == rec1.temp_max)	diff_temp = false;
	if (rec2.temp_min == rec2.temp_max)	diff_temp = false;
	if (rec1.rain_min == rec1.rain_max)	diff_rain = false;
	if (rec2.rain_min == rec2.rain_max)	diff_rain = false;
	if (rec1.temp_rng_min == rec1.temp_rng_max)	diff_temp_rng = false;
	if (rec2.temp_rng_min == rec2.temp_rng_max)	diff_temp_rng = false;

	if (diff_lat || diff_temp || diff_rain || diff_temp_rng)
	{
//		printf("%s, %s (%lf,%lf) 30x norm=%lf\n", t1, t2, dist_1, dist_2, y_normal);
		base_dist *= 30.0;
	}
#endif

	if (rec1.proj_angle != proj_Down ||
		rec2.proj_angle != proj_Down)
	return 1.0;

	return 500.0 * y_normal * y_normal * y_normal;

//	return max(base_dist, 50.0) * y_normal * y_normal;
}



inline double	DistPtToTri(CDT::Vertex_handle v, CDT::Face_handle f)
{
	// Find the closest a triangle comes to a point.  Inputs are in lat/lon, otuput is in meters!
	Point_2	vp(v->point().x(), v->point().y());
	Vector_2	tp1(f->vertex(0)->point().x(), f->vertex(0)->point().y());
	Vector_2	tp2(f->vertex(1)->point().x(), f->vertex(1)->point().y());
	Vector_2	tp3(f->vertex(2)->point().x(), f->vertex(2)->point().y());
	Vector_2	vpv(v->point().x(), v->point().y());
	tp1 = tp1 - vpv;
	tp2 = tp2 - vpv;
	tp3 = tp3 - vpv;
	double	DEG_TO_NM_LON = DEG_TO_NM_LAT * cos(CGAL::to_double(vp.y()) * DEG_TO_RAD);
	tp1 = tp1 * (DEG_TO_NM_LON * NM_TO_MTR);
	//tp1.y *= (DEG_TO_NM_LAT * NM_TO_MTR);
	tp2 = tp2 * (DEG_TO_NM_LON * NM_TO_MTR);
	//tp2.y *= (DEG_TO_NM_LAT * NM_TO_MTR);
	tp3 = tp3 * (DEG_TO_NM_LON * NM_TO_MTR);
	//tp3.y *= (DEG_TO_NM_LAT * NM_TO_MTR);

//	Segment_2	s1(tp1, tp2);
//	Segment_2	s2(tp2, tp3);
//	Segment_2	s3(tp3, tp1);
//	Point_2	origin(0.0, 0.0);

//	double d1 = s1.squared_distance(origin);
//	double d2 = s2.squared_distance(origin);
//	double d3 = s3.squared_distance(origin);
	double d4 = CGAL::to_double(tp1.squared_length());
	double d5 = CGAL::to_double(tp2.squared_length());
	double d6 = CGAL::to_double(tp3.squared_length());

//	double	nearest = min(min(d1, d2), min(min(d3, d4), min(d5, d6)));
	double	nearest = min(min(d4, d5), d6);
	return sqrt(nearest);
}







#pragma mark -
/***************************************************************************
 * ALGORITHMS TO FIND VALUABLE POINTS IN A DEM *****************************
 ***************************************************************************
 *
 * These routines take a fully populated DEM and copy points of interest into
 * an empty DEM to build up a small number of points we can use to triangulate.
 * 'orig' is always the main DEM and 'deriv' the sparse one.  The goal is to
 * get about 20,000-30,000 points that provide good coverage and capture the
 * terrain morphology.
 */


/*
 * CopyWetPoints
 *
 * This routine copies the points that are inside water bodies and copies
 * them, modifying their altitude to be at sea level.  It also copies
 * to another DEM (if desired) points that are near the edges of the water bodies.
 * this can be a useful reference.  We return the number of wet points.
 *
 */
int CopyWetPoints(
				const DEMGeo& 			orig, 		// The original DEM
				DEMGeo& 				deriv, 		// All water points, flattened, are added to this DEM
				DEMGeo * 				corners, 	// Near-vertices are added to this DEM
				const Pmwx& 			map)		// The map we get the water bodies from
{
	// BEN NOTE ON CLAMPING: I think we do NOT care if an edge is microscopically outside the DEM
	// in this case...xy_nearest could care less...and the polygon rasterizer doesn't care much
	// either.  We do not generate any coastline edges here.

//	FILE * fi = fopen("dump.txt", "a");
	PolyRasterizer	rasterizer;
	SetupWaterRasterizer(map, orig, rasterizer);
	fprintf(stderr, "!");

	int wet = 0;

	for (Pmwx::Edge_const_iterator i = map.edges_begin(); i != map.edges_end(); ++i)
	{
		bool	iWet = i->face()->data().IsWater() && !i->face()->is_unbounded();
		bool	oWet = i->twin()->face()->data().IsWater() && !i->twin()->face()->is_unbounded();

		if (iWet != oWet)
		{
			if (corners)
			{
				int xp, yp;
				float e;
				e = orig.xy_nearest(CGAL::to_double(i->source()->point().x()),CGAL::to_double(i->source()->point().y()), xp, yp);
				if (e != DEM_NO_DATA)
					(*corners)(xp,yp) = e;

				e = orig.xy_nearest(CGAL::to_double(i->target()->point().x()),CGAL::to_double(i->target()->point().y()), xp, yp);
				if (e != DEM_NO_DATA)
					(*corners)(xp,yp) = e;
			}
		}
	}


	int y = 0;
	rasterizer.StartScanline(y);
	while (!rasterizer.DoneScan())
	{
		int x1, x2;
		while (rasterizer.GetRange(x1, x2))
		{
			for (int x = x1; x < x2; ++x)
			{
			// BENTODO
//				int e = orig.get_lowest(x,y,5);
				int e = orig.get(x,y);
				if (e != DEM_NO_DATA)
				{
					deriv(x,y) = e;
					++wet;
//					gMeshPoints.push_back(Point_2(orig.x_to_lon(x), orig.y_to_lat(y)));
				}
			}
		}
		++y;
		if (y >= orig.mHeight) break;
		rasterizer.AdvanceScanline(y);
	}

	return wet;
}

/*
 * BuildSparseWaterMesh
 *
 * Given a DEM that contains all wet points and a DEM that contains approximations (snapped to the DEM) of
 * all coastline points, this routine adds to a final DEM a sparse subset of points from the wet DEM
 * that are every X points and at least Y points from any coastlines.  This builds a sparse mesh for
 * water bodies to keep fogging working properly.  It also erases the rest of the water body.
 *
 */
void	BuildSparseWaterMesh(
//					const DEMGeo& inWet, 		// A mesh that contains all water points, dropped to water level
//					const DEMGeo& inEdges, 		// The vertices of the water bodies
					DEMGeo& deriv, 				// A few water points are added to this DEM
					int skip, 					// The skip interval - add a water point once every N DEM poionts
					int search)					// Search range for coast vertices - search this far for a nearby  coast point.
{
	int x, y, dx, dy;
	float h;
	for (y = 0; y < deriv.mHeight; y++)
	for (x = 0; x < deriv.mWidth; x++)
	{
		h = deriv.get(x,y);
		if (h != DEM_NO_DATA)
		{
			if ((x % skip) != 0  || (y % skip) != 0)
				deriv(x,y) = DEM_NO_DATA;
		}
	}
}



/*
 * AddEdgePoints
 *
 * This function adds the edges to the DEMs, at the interval specified.
 *
 */
void AddEdgePoints(
			const DEMGeo& 		orig, 			// The original DEM
			DEMGeo& 			deriv, 			// Edge points are added to this
			int 				interval,		// The interval - add an edge point once every N points.
			int					divisions,		// Number of divisions - 1 means 1 big, "2" means 4 parts, etc.
			bool				has_border[4])	// Useful in making sure our borders match up.
{
	int	div_skip_x = (deriv.mWidth-1) / divisions;
	int	div_skip_y = (deriv.mHeight-1) / divisions;
	int x, y, dx, dy;
	bool has_left = has_border[0];
	bool has_bottom = has_border[1];
	bool has_right = has_border[2];
	bool has_top = has_border[3];

	for (y = (has_bottom ? div_skip_y : 0); y < (deriv.mHeight - (has_top ? div_skip_y : 0)) ; y += div_skip_y)
	for (x = (has_left ? div_skip_x : 0); x < (deriv.mWidth - (has_right ? div_skip_x : 0)); x += div_skip_x)
	{
		for (dy = 0; dy < deriv.mHeight; dy += interval)
		for (dx = 0; dx < deriv.mWidth; dx += interval)
		{
			deriv(x,dx) = orig(x,dx);
			deriv(dx,y) = orig(dx,y);
		}
		if (orig(x,y) == DEM_NO_DATA)
			AssertPrintf("ERROR: mesh point %d,%d lacks data for cutting and edging!\n", x, y);
		deriv(x,y) = orig(x,y);

	}
//	deriv(0				,0				) = orig(0			  , 0			  );
//	deriv(0				,deriv.mHeight-1) = orig(0			  , orig.mHeight-1);
//	deriv(deriv.mWidth-1,deriv.mHeight-1) = orig(orig.mWidth-1, orig.mHeight-1);
//	deriv(deriv.mWidth-1,0				) = orig(orig.mWidth-1, 0			  );
}


static Halfedge_handle ExtendLanduseEdge(Halfedge_handle start)
{
	Vertex_handle target;
	Pmwx::Halfedge_around_vertex_circulator	circ, stop;
	Halfedge_handle next;

	start->data().mMark = true;
	start->twin()->data().mMark = true;
	Vector_2 dir_v(start->source()->point(), start->target()->point());
	dir_v = normalize(dir_v);

	while (1)
	{
		target = start->target();
		circ = stop = target->incident_halfedges();
		next = start;
		do {
			if (start != circ)
			{
				if (circ->face()->data().mTerrainType != circ->twin()->face()->data().mTerrainType ||
					circ->data().mParams.count(he_MustBurn) ||
					circ->twin()->data().mParams.count(he_MustBurn))
				{
					Vector_2 d(circ->target()->point(), circ->source()->point());
					d = normalize(d);
					if (dir_v * d > 0.999847695156 &&
						!circ->data().mMark &&
						circ->face()->data().mTerrainType == start->twin()->face()->data().mTerrainType &&
						circ->twin()->face()->data().mTerrainType == start->face()->data().mTerrainType &&
						circ->data().mParams.count(he_MustBurn) == 0 &&
						circ->twin()->data().mParams.count(he_MustBurn) == 0)
					{
						//DebugAssert(next == NULL);
						next = circ->twin();
					} else
						return start;
				}
			}
			++circ;
		} while (circ != stop);

		if (next == start)
			return start;
		else {
			start = next;
			next->data().mMark = true;
			next->twin()->data().mMark = true;
		}
	}
}

void CollectPointsAlongLine(const Point_2& p1, const Point_2& p2, vector<Point_2>& outPts, DEMGeo& ioDem)
{
	outPts.push_back(p1);

#if 0
	int x1 = floor(ioDem.lon_to_x(CGAL::to_double(p1.x())));
	int y1 = floor(ioDem.lat_to_y(CGAL::to_double(p1.y())));
	int x2 = ceil(ioDem.lon_to_x(CGAL::to_double(p2.x())));
	int y2 = ceil(ioDem.lat_to_y(CGAL::to_double(p2.y())));
	if (x1 > x2) swap(x1, x2);
	if (y1 > y2) swap(y1, y2);

	double	DEG_TO_MTR_LON = DEG_TO_MTR_LAT * cos(DEG_TO_RAD * (CGAL::to_double(p1.y() + p2.y())) * 0.5);

	Segment_2	seg(p1, p2);
	//Vector_2		dir(p1, p2);
	//dir.normalize();
	multimap<double, Point_2>	added_pts;

	if ((x2-x1)>2 && (y2-y1)>2)
	for (int y = y1; y <= y2; ++y)
	for (int x = x1; x <= x2; ++x)
	{
		float e = ioDem.get(x,y);
		if (e != DEM_NO_DATA)
		{
			Point_2 test = Point_2(ioDem.x_to_lon(x), ioDem.y_to_lat(y));
			Point_2 proj = seg.supporting_line().projection(test);
			if (seg.collinear_has_on(proj) && proj != p1 && proj != p2)
			{
				double alen = CGAL::to_double(Vector_2(p1, proj).squared_length());
				double llen = CGAL::to_double(Vector_2(proj, test).squared_length());

				if ((llen*16.0) < alen)
				{
//					gMeshPoints.push_back(test);
					added_pts.insert(multimap<double, Point_2>::value_type(alen, proj));

					if (LonLatDistMetersWithScale(CGAL::to_double(test.x()), CGAL::to_double(test.y()), CGAL::to_double(proj.x()), CGAL::to_double(proj.y()), DEG_TO_MTR_LON, DEG_TO_MTR_LAT) < 10.0)
					if (x != 0 && x != (ioDem.mWidth-1) && y != 0 && y != (ioDem.mHeight-1))
						ioDem(x,y) = DEM_NO_DATA;
				}
			}
		}
	}

	for (multimap<double, Point_2>::iterator i = added_pts.begin(); i != added_pts.end(); ++i)
	if (LonLatDistMetersWithScale(CGAL::to_double(outPts.back().x()), CGAL::to_double(outPts.back().y()), CGAL::to_double(i->second.x()), CGAL::to_double(i->second.y()), DEG_TO_MTR_LON, DEG_TO_MTR_LAT) > 90.0)
	if (LonLatDistMetersWithScale(CGAL::to_double(p2.x()), CGAL::to_double(p2.y()), CGAL::to_double(i->second.x()), CGAL::to_double(i->second.y()), DEG_TO_MTR_LON, DEG_TO_MTR_LAT) > 90.0)
		outPts.push_back(i->second);
#endif
	outPts.push_back(p2);

}

/*
 * AddWaterMeshPoints
 *
 * Given a water map, this point adds the vertices (based on the rough height of the master DEM) into
 * the triangualtion and clears any nearby DEM points in the slave DEM.  If it is hires, constraints
 * are added to enforce coastlines.
 *
 */
void	AddWaterMeshPoints(
				Pmwx& 								inMap, 		// Vec Map of waterbodies
				const DEMGeo& 						master, 	// Master DEM with elevations
				DEMGeo& 							slave, 		// This DEM has mesh points erased where vertices are added
				CDT& 								outMesh, 	// Vertices and constraints added to this mesh
				vector<LanduseConstraint_t>&		outCons,	// The constraints we add for water are added here for later use
				bool 								hires)		// True if we are hires and need constraints.
{
	/*******************************************************************************************
	 * FIND POLYGON GROUPS THAT CONTAIN LAND USE DIFFERENCES
	 *******************************************************************************************/

	// We are going to go through the whole map and find every halfedge that represents a real land use
	// change.

		CDT::Face_handle	locale = NULL;	// For cache coherency
		CDT::Vertex_handle	v1, v2;
		float				e1, e2;

		Pmwx::Halfedge_iterator he;

	for (he = inMap.halfedges_begin(); he != inMap.halfedges_end(); ++he)
		he->data().mMark = false;

	for (he = inMap.halfedges_begin(); he != inMap.halfedges_end(); ++he)
	if (!he->twin()->data().mMark)
	if (!he->data().mMark)
	{
		Pmwx::Face_const_handle	f1 = he->face();
		Pmwx::Face_const_handle	f2 = he->twin()->face();
		//fprintf(stderr,"\\");
		if (!f1->is_unbounded() && !f2->is_unbounded() &&
			(f1->data().mTerrainType != f2->data().mTerrainType ||
			he->data().mParams.count(he_MustBurn) ||
			he->twin()->data().mParams.count(he_MustBurn)))
		{
			Halfedge_handle extended1 = ExtendLanduseEdge(he);
			Halfedge_handle extended2 = ExtendLanduseEdge(he->twin());
			//Point_2	p1(extended2->target()->point().x(), extended2->target()->point().y());
			//Point_2	p2( extended1->target()->point().x(), extended1->target()->point().y());
			// Dr IT HURTS!
			//SlightClampToDEM(p1, master);
			//SlightClampToDEM(p2, master);

			vector<Point_2>	pts;
			CollectPointsAlongLine(extended2->target()->point(), extended1->target()->point(), pts, slave);
			int c=0;

			for (int n = 1; n < pts.size(); ++n)
			{
				e1 = DEM_NO_DATA;
				e2 = DEM_NO_DATA;
//				if (f1->mTerrainType == terrain_Water || f2->mTerrainType == terrain_Water)
//				{
//					e1 = water.xy_nearest(pts[n-1].x, pts[n-1].y);
//					e2 = water.xy_nearest(pts[n].x, pts[n].y);
//					if (e1 == DEM_NO_DATA)
//						e1 = water.search_nearest(pts[n-1].x, pts[n-1].y);
//					if (e2 == DEM_NO_DATA)
//						e2 = water.search_nearest(pts[n].x,pts[n].y);
//					BEN SEZ: this is not really an error!  Remember - water data is only points contained inside a water body; very narrrow ones will contain NO
//					DEM points and this will fail.
//					if (e1 == DEM_NO_DATA || e2 == DEM_NO_DATA)
//						printf("WARNING: FOUND NO FLAT WATER DATA NEARBY.  LOC=%lf,%lf->%lf,%lf\n",pts[n-1].x,pts[n-1].y,pts[n].x,pts[n].y);
//				}
				if (e1 == DEM_NO_DATA)					e1 = master.value_linear(CGAL::to_double(pts[n-1].x()), CGAL::to_double(pts[n-1].y()));
				if (e2 == DEM_NO_DATA)					e2 = master.value_linear(CGAL::to_double(pts[n].x()), CGAL::to_double(pts[n].y()));
				if (e1 == DEM_NO_DATA)					e1 = master.xy_nearest(CGAL::to_double(pts[n-1].x()), CGAL::to_double(pts[n-1].y()));
				if (e2 == DEM_NO_DATA)					e2 = master.xy_nearest(CGAL::to_double(pts[n].x()), CGAL::to_double(pts[n].y()));
//				slave.zap_linear(pts[n-1].x, pts[n-1].y);
//				slave.zap_linear(pts[n].x, pts[n].y);

				if (e1 == DEM_NO_DATA || e2 == DEM_NO_DATA)
					AssertPrintf("ERROR: missing elevation data for constraint.\n");
				v1 = outMesh.insert(pts[n-1]);
				v1->info().height = e1;
				locale = v1->face();
				v2 = outMesh.insert(pts[n], locale);
				v2->info().height = e2;
				locale = v2->face();

				outMesh.insert_constraint(v1, v2);
				outCons.push_back(LanduseConstraint_t(ConstraintMarker_t(v1,v2),LandusePair_t(he, he->twin())));
			}
		}
	}
	printf("\n");
}


void	SetWaterBodiesToWet(CDT& ioMesh, vector<LanduseConstraint_t>& inCoastlines, const DEMGeo& allPts)
{
	set<CDT::Face_handle>		wet_faces;
	set<CDT::Face_handle>		visited;

	fprintf(stderr, "SetWaterToWet ");
	// Quick pass - set everyone to natural.   This is needed because if there are no polys,
	// then the outside of those polys won't make natural terrain.

	for (CDT::Finite_faces_iterator ffi = ioMesh.finite_faces_begin(); ffi != ioMesh.finite_faces_end(); ++ffi)
	{
		ffi->info().terrain = terrain_Natural;
		ffi->info().feature = NO_VALUE;
	}

	// Next mark every point on a tri that's just inside as hot unless it's also an edge point.
	// Also mark these tris as wet.
	for (vector<LanduseConstraint_t>::iterator c = inCoastlines.begin(); c != inCoastlines.end(); ++c)
	{
		CDT::Face_handle	face_h;
		int					vnum;
		// Dig up the face that includes our edge.  is_edge gives us the right-hand side triangle, but we want
		// the left since this is a counter clockwise boundary, so go backward on the constraint.

		if (!PersistentFindEdge(ioMesh, c->first.second, c->first.first, face_h, vnum))
		{
			AssertPrintf("ASSERTION FAILURE: constraint not an edge.\n");
		} else {
			face_h->info().terrain = c->second.first->face()->data().mTerrainType;
			face_h->info().feature = c->second.first->face()->data().mTerrainType;
			// BEN SEZ: we will get conflicts on origin faces!  imagine water tries separated by a bridge - WED thinks they're
			// al the same but they're not.
//			DebugAssert(face_h->info().orig_face == NULL || face_h->info().orig_face == c->second.first->face());
			if (face_h->info().orig_face == Face_handle())
				face_h->info().orig_face = c->second.first->face();
			wet_faces.insert(face_h);
		}

		if (!PersistentFindEdge(ioMesh, c->first.first, c->first.second, face_h, vnum))
		{
			AssertPrintf("ASSERTION FAILURE: constraint not an edge.\n");
		} else {
			face_h->info().terrain = c->second.second->face()->data().mTerrainType;
			face_h->info().feature = c->second.second->face()->data().mTerrainType;
//			DebugAssert(face_h->info().orig_face == NULL || face_h->info().orig_face == c->second.second->face());
			if (face_h->info().orig_face == Face_handle())
				face_h->info().orig_face = c->second.second->face();

			wet_faces.insert(face_h);
		}
	}


	while (!wet_faces.empty())
	{
		CDT::Face_handle f = *wet_faces.begin();
		wet_faces.erase(f);
		visited.insert(f);

		int tg = f->info().terrain;
		const Face_handle of = f->info().orig_face;
		f->info().flag = 0;
		CDT::Face_handle	fn;
		for (int vi = 0; vi < 3; ++ vi)
		if (!ioMesh.is_constrained(CDT::Edge(f,vi)))
		{
			fn = f->neighbor(vi);
			if (!ioMesh.is_infinite(fn))
			if (visited.find(fn) == visited.end())
			{
				if (fn->info().terrain != terrain_Natural && fn->info().terrain != tg) {
					printf("Error: conflicting terrain assignment between %s and %s, near %lf, %lf\n",
							FetchTokenString(fn->info().terrain), FetchTokenString(tg),
							CGAL::to_double(f->vertex(vi)->point().x()), CGAL::to_double(f->vertex(vi)->point().y()));
				} else {
				fn->info().terrain = tg;
				fn->info().feature = tg;
				}
				if (fn->info().orig_face == Face_handle()) fn->info().orig_face = of;
				wet_faces.insert(fn);
			}
		}
	}

	for (CDT::Finite_faces_iterator ffi = ioMesh.finite_faces_begin(); ffi != ioMesh.finite_faces_end(); ++ffi)
	if (ffi->info().terrain == terrain_Water)
	for (int vi = 0; vi < 3; ++vi)
	{
		int xw, yw;
		float e = allPts.xy_nearest(CGAL::to_double(ffi->vertex(vi)->point().x()),CGAL::to_double(ffi->vertex(vi)->point().y()), xw, yw);

		//e = allPts.get_lowest_heuristic(xw, yw, 5);
		if (e != DEM_NO_DATA)
			ffi->vertex(vi)->info().height = e;
	}
	fprintf(stderr, "\n");

}



/*
 * AddBulkPointsToMesh
 *
 * Given a DEM with points that need to be in the mesh, this routine blasts them in.
 *
 */
void	AddBulkPointsToMesh(
				DEMGeo& 				ioDEM, 		// DEM with data only where we want mesh points, points are nuked
				CDT& 					outMesh,
				ProgressFunc			inFunc)	// mesh to receive points.
{
	if (inFunc) inFunc(1, 3, "Building Triangle Mesh", 0.0);

	int x, y, step, total = 0;
	CDT::Face_handle	local;
	for (step = 1024; step > 0; step /= 4)
	{
		for (y = 0; y < ioDEM.mHeight; y+=step)
		for (x = 0; x < ioDEM.mWidth; x+=step)
		{
			if (inFunc && x == 0 && (y % 20 == 0)) inFunc(1, 3, "Building Triangle Mesh", (double) y / (double) ioDEM.mHeight);
			float h = ioDEM(x,y);
			if (h != DEM_NO_DATA)
			{
//				gMeshPoints.push_back(pair<Point2,Point3>(Point2(ioDEM.x_to_lon(x),ioDEM.y_to_lat(y)),Point3(1,0,0)));
				CDT::Point	p(ioDEM.x_to_lon(CGAL::to_double(x)),ioDEM.y_to_lat(CGAL::to_double(y)));
				CDT::Locate_type tp;
				int vnum;
				local = outMesh.locate(p, tp, vnum, local);
//				if (tp != CDT::EDGE)
				{
					//printf("Bulk Point %lf, %lf\n",  ioDEM.x_to_lon(x),  ioDEM.y_to_lat(y));
					CDT::Vertex_handle vv = outMesh.safe_insert(p, local);
					vv->info().height = h;
					local = vv->face();
					++total;
				}
				ioDEM(x,y) = DEM_NO_DATA;
			}
		}
//		if (step == 1) break;
	}
	printf("Inserted %d points.\n", total);
	if (inFunc) inFunc(1, 3, "Building Triangle Mesh", 1.0);
}

/*
 * CalculateMeshNormals
 *
 * This routine calcs the normals per vertex.
 *
 */
void CalculateMeshNormals(CDT& ioMesh)
{
// BEN SAYS:
// PRESERVED here is Andrew McGreggor's port to CGAL - but see below.

	for (CDT::Finite_vertices_iterator i = ioMesh.finite_vertices_begin(); i != ioMesh.finite_vertices_end(); ++i)
	{
		Vector3	total(0.0, 0.0, 0.0);
//		FastKernel::Vector_3	total(0.0, 0.0, 0.0);
		CDT::Vertex_circulator last = ioMesh.incident_vertices(i);
		CDT::Vertex_circulator nowi = last, stop = last;
		Point3	selfP(CGAL::to_double(i->point().x()), CGAL::to_double(i->point().y()), i->info().height);

//		FastKernel::Point_3	selfP(CGAL::to_double(i->point().x()), CGAL::to_double(i->point().y()), i->info().height);
		do {
			last = nowi;
			++nowi;
			if(!ioMesh.is_infinite(last) && !ioMesh.is_infinite(nowi))
			{

                Point3  lastP(CGAL::to_double(last->point().x()), CGAL::to_double(last->point().y()), last->info().height);
                Point3  nowiP(CGAL::to_double(nowi->point().x()), CGAL::to_double(nowi->point().y()), nowi->info().height);
                Vector3 v1(selfP, lastP);
                Vector3 v2(selfP, nowiP);
                v1.dx *= (DEG_TO_MTR_LAT * cos(selfP.y * DEG_TO_RAD));
                v2.dx *= (DEG_TO_MTR_LAT * cos(selfP.y * DEG_TO_RAD));
                v1.dy *= (DEG_TO_MTR_LAT);
                v2.dy *= (DEG_TO_MTR_LAT);
                DebugAssert(v1.dx != 0.0 || v1.dy != 0.0 || v1.dz != 0.0);
                DebugAssert(v2.dx != 0.0 || v2.dy != 0.0 || v2.dz != 0.0);
                v1.normalize();
                v2.normalize();
                Vector3 normal(v1.cross(v2));
                DebugAssert(normal.dx != 0.0 || normal.dy != 0.0 || normal.dz != 0.0);
                DebugAssert(normal.dz > 0.0);
                normal.normalize();
 /*
				FastKernel::Point_3	lastP(last->point().x(), last->point().y(), last->info().height);
				FastKernel::Point_3	nowiP(nowi->point().x(), nowi->point().y(), nowi->info().height);
				BEN SAYS:	note that application of degrees->meters is WRONG here - scaling must be done to the VECTOR, not just ONE point.
				FastKernel::Vector_3	v1(selfP, FastKernel::Point_3(lastP.x()*(DEG_TO_MTR_LAT * cos(CGAL::to_double(selfP.y()) * DEG_TO_RAD)),
													   lastP.y()*(DEG_TO_MTR_LAT),
													   last->info().height));
				FastKernel::Vector_3	v2(selfP, FastKernel::Point_3(nowiP.x()*(DEG_TO_MTR_LAT * cos(CGAL::to_double(selfP.y()) * DEG_TO_RAD)),
													   nowiP.y()*(DEG_TO_MTR_LAT),
													   nowi->info().height));
				DebugAssert(v1.x() != 0.0 || v1.y() != 0.0 || v1.z() != 0.0);
				DebugAssert(v2.x() != 0.0 || v2.y() != 0.0 || v2.z() != 0.0);
				v1 = normalize(v1);
				v2 = normalize(v2);
				FastKernel::Vector_3	normal = CGAL::cross_product(v1,v2);
				DebugAssert(normal.x() != 0.0 || normal.y() != 0.0 || normal.z() != 0.0);
				//DebugAssert(normal.z() > 0.0);
				normal = normalize(normal);
				// not sure we have a guaranteed CCW circulation, so this will fix us (normals are ALWAYS upward).
				if (normal.z() < 0.0)
					normal = -normal;
				//fprintf(stderr, "v1 = (%lf, %lf, %lf), v2 = (%lf, %lf, %lf), normal = (%lf, %lf, %lf)\n",
				//		CGAL::to_double(v1.x()), CGAL::to_double(v1.y()), CGAL::to_double(v1.z()),
				//		CGAL::to_double(v2.x()), CGAL::to_double(v2.y()), CGAL::to_double(v2.z()),
				//		CGAL::to_double(normal.x()), CGAL::to_double(normal.y()), CGAL::to_double(normal.z()));
*/
				CDT::Face_handle	a_face;
				if (ioMesh.is_face(i, last, nowi, a_face))
				{
                    a_face->info().normal[0] = normal.dx;
                    a_face->info().normal[1] = normal.dy;
                    a_face->info().normal[2] = normal.dz;
 /*
					a_face->info().normal[0] = CGAL::to_double(normal.x());
					a_face->info().normal[1] = CGAL::to_double(normal.y());
					a_face->info().normal[2] = CGAL::to_double(normal.z());
*/
				}
				total = total + normal;
			}
		} while (nowi != stop);
        DebugAssert(total.dx != 0.0 || total.dy != 0.0 || total.dz != 0.0);
        DebugAssert(total.dz > 0.0);
        total.normalize();
        i->info().normal[0] = total.dx;
        i->info().normal[1] = total.dy;
        i->info().normal[2] = total.dz;

/*
		DebugAssert(total.x() != 0.0 || total.y() != 0.0 || total.z() != 0.0);
		DebugAssert(total.z() > 0.0);
		total = normalize(total);
		i->info().normal[0] = CGAL::to_double(total.x());
		i->info().normal[1] = CGAL::to_double(total.y());
		i->info().normal[2] = CGAL::to_double(total.z());
*/
	}
}

/*******************************************************************************************
 *******************************************************************************************
 ** GENERATION OF A MESH MASTER ROUTINE ****************************************************
 *******************************************************************************************
 *******************************************************************************************/







void	TriangulateMesh(Pmwx& inMap, CDT& outMesh, DEMGeoMap& inDEMs, const char * mesh_folder, ProgressFunc prog)
{
	TIMER(Total)
	outMesh.clear();

	int		x, y;
	DEMGeo&	orig(inDEMs[dem_Elevation]);

	Assert(orig.get(0			 ,0				) != DEM_NO_DATA);
	Assert(orig.get(orig.mWidth-1,orig.mHeight-1) != DEM_NO_DATA);
	Assert(orig.get(0			 ,orig.mHeight-1) != DEM_NO_DATA);
	Assert(orig.get(orig.mWidth-1,orig.mHeight-1) != DEM_NO_DATA);

	DEMGeo	deriv(orig.mWidth, orig.mHeight);					// A mash-up of points we will add to the final mesh.
	deriv.copy_geo_from(orig);
	deriv = DEM_NO_DATA;

	/*********************************************************************************************************************
	 * PRECALCULATION OF MASKS
	 *********************************************************************************************************************/

//	DEMGeo	outline(deriv);										// DEM points that are near the corners of coastlines.
//	DEMGeo	water(deriv);										// Flattened DEM points inside water.
//	DEMGeo	land(orig);											// DEM points that are not in water.

	double	land_ratio;

	if (prog) prog(0, 3, "Calculating Mesh Points", 0.3);

	{
		// This step builds the water, land, and outline raster layers
		// from the map water GT-polygons.

		TIMER(build_wet_map)
		int wet_pts = CopyWetPoints(orig, deriv, NULL, inMap);
//		for (y = 0; y < deriv.mHeight; ++y)
//		for (x = 0; x < deriv.mWidth; ++x)
//		if (water.get(x,y) != DEM_NO_DATA)
//			land(x,y) = DEM_NO_DATA;

		int total_pts = deriv.mWidth * deriv.mHeight;
		int dry_pts = total_pts - wet_pts;
		land_ratio =  (double) dry_pts /  (double) total_pts;
		printf("Land ratio: %lf: %d/%d\n", land_ratio, dry_pts, total_pts);
	}

/*
	{
		DEMGeo	temp(water);
		for (y = 0; y < temp.mHeight; ++y)
		for (x = 0; x < temp.mWidth; ++x)
		{
			float me = water.get(x,y);
			if (me != DEM_NO_DATA)
			if (water.get(x-1,y-1) > me &&
				water.get(x  ,y-1) > me &&
				water.get(x+1,y  ) > me &&
				water.get(x-1,y  ) > me &&
				water.get(x+1,y+1) > me &&
				water.get(x  ,y+1) > me &&
				water.get(x+1,y+1) > me)
			{
				temp(x,y) = DEM_NO_DATA;
			}
		}

		for (y = 0; y < temp.mHeight; ++y)
		for (x = 0; x < temp.mWidth; ++x)
		{
			water(x,y) = temp.get_lowest_heuristic(x,y,5);
		}

//		if (SpreadDEMValuesIterate(water))
//			SpreadDEMValuesIterate(water);
	}*/

	if (prog) prog(0, 3, "Calculating Mesh Points", 0.4);

	{
		// This step builds a derived "sparse" water mesh from the thick water raster layer.

		TIMER(sparsify_wet_map)
		BuildSparseWaterMesh(deriv, LOW_RES_WATER_INTERVAL, LOW_RES_WATER_INTERVAL/2);
	}


	int	ct = 0;
	for (y = 0; y < deriv.mHeight; ++y)
	for (x = 0; x < deriv.mWidth; ++x)
	{
		float h = deriv(x,y);
		if (h != DEM_NO_DATA)
			++ct;
	}

	char	buf[100];
	sprintf(buf,"%d triangles", ct);

	vector<LanduseConstraint_t>	coastlines_markers;

	{
		// This adds edge points to the DEM if we need to (e.g. no slaving) or loads slaves.

		TIMER(edges);

		char	fname_lef[512];
		char	fname_bot[512];
		char	fname_rgt[512];
		char	fname_top[512];

		string border_loc = mesh_folder;
#if APL && !defined(__MACH__)
		string	appP;
		AppPath(appP);
		string::size_type b = appP.rfind(':');
		appP.erase(b+1);
		border_loc = appP + border_loc;
#endif

		sprintf(fname_lef,"%s%s%+03d%+04d%s%+03d%+04d.border.txt", border_loc.c_str(), DIR_STR,latlon_bucket(deriv.mSouth),latlon_bucket(deriv.mWest - 1), DIR_STR, (int) (deriv.mSouth), (int) (deriv.mWest - 1));
		sprintf(fname_bot ,"%s%s%+03d%+04d%s%+03d%+04d.border.txt", border_loc.c_str(), DIR_STR,latlon_bucket(deriv.mSouth - 1),latlon_bucket(deriv.mWest), DIR_STR, (int) (deriv.mSouth - 1), (int) (deriv.mWest));
		sprintf(fname_rgt,"%s%s%+03d%+04d%s%+03d%+04d.border.txt", border_loc.c_str(), DIR_STR,latlon_bucket(deriv.mSouth),latlon_bucket(deriv.mWest + 1), DIR_STR, (int) (deriv.mSouth), (int) (deriv.mWest + 1));
		sprintf(fname_top ,"%s%s%+03d%+04d%s%+03d%+04d.border.txt", border_loc.c_str(), DIR_STR,latlon_bucket(deriv.mSouth + 1),latlon_bucket(deriv.mWest), DIR_STR, (int) (deriv.mSouth + 1), (int) (deriv.mWest));

		mesh_match_t junk1, junk2, junk3;
		bool	has_borders[4];
		has_borders[0] = gMeshPrefs.border_match ? load_match_file(fname_lef, junk1, junk2, gMatchBorders[0], junk3) : false;
		has_borders[1] = gMeshPrefs.border_match ? load_match_file(fname_bot, junk1, junk2, junk3, gMatchBorders[1]) : false;
		has_borders[2] = gMeshPrefs.border_match ? load_match_file(fname_rgt, gMatchBorders[2], junk1, junk2, junk3) : false;
		has_borders[3] = gMeshPrefs.border_match ? load_match_file(fname_top, junk1, gMatchBorders[3], junk2, junk3) : false;

		AddEdgePoints(orig, deriv, 20, 1, has_borders);
	}

	/*********************************************************************************************************************
	 * TRIANGULATION
	 *********************************************************************************************************************/

	if (prog) prog(0, 3, "Calculating Mesh Points", 0.6);

	// Order matters - it is better to do coastlines first and the DEM second.
	// A regular type-writer style insert means that all points are inserted
	// outside the hull, which is real slow - the hull has a huge number of
	// vertices.


	// outMesh not touched before here

	CDT::Vertex_handle	corner;
	outMesh.insert(CDT::Point( orig.mWest,  orig.mSouth))->info().height = orig.get(0			,0			   );
	outMesh.insert(CDT::Point( orig.mWest,  orig.mNorth))->info().height = orig.get(0			,orig.mHeight-1);
	outMesh.insert(CDT::Point( orig.mEast,  orig.mSouth))->info().height = orig.get(orig.mWidth-1,0			   );
	outMesh.insert(CDT::Point( orig.mEast,  orig.mNorth))->info().height = orig.get(orig.mWidth-1,orig.mHeight-1);

	// This warrants some explaining.  Basically...the greedy mesh build is not allowed to put vertices in the slaved edges.
	// But if it can't, it will furiously add them one row over in an attempt to minimize how goofy it would look if we REALLY
	// had no vertices there.  So...

	// We temporarily build the whole slaved edge.

	vector<CDT::Vertex_handle> temporary;
	int n,b,tc=0;
	for(b=0;b<4;++b)
	if (!gMatchBorders[b].vertices.empty())  {// Because size-1 for empty is max-unsigned-int - gross.
		tc += gMatchBorders[b].vertices.size();
	for (n = 1; n < gMatchBorders[b].vertices.size()-1; ++n)
	{
			//printf("temp: %lf, %lf\n",  gMatchBorders[b].vertices[n].loc.x, gMatchBorders[b].vertices[n].loc.y);
			temporary.push_back(outMesh.insert(CDT::Point(gMatchBorders[b].vertices[n].loc.x(),
														  gMatchBorders[b].vertices[n].loc.y())));
		temporary.back()->info().height = gMatchBorders[b].vertices[n].height;
	}
	}
	printf("temporary contains %d points\n", tc);
	// Clear out the slaved edges in the data so that we don't add them as part of our process.
	// If we burned some of the sparse water mesh into deriv at the edges (because a lake is on our tile edge)
	// this clears it out if we are a slave.
	{
		if (!gMatchBorders[2].vertices.empty())
		for (y = 0; y < deriv.mHeight; ++y)
			deriv(deriv.mWidth-1, y) = DEM_NO_DATA;
		if (!gMatchBorders[3].vertices.empty())
		for (x = 0; x < deriv.mWidth; ++x)
			deriv(x, deriv.mHeight-1) = DEM_NO_DATA;
		if (!gMatchBorders[0].vertices.empty())
		for (y = 0; y < deriv.mHeight; ++y)
			deriv(0, y) = DEM_NO_DATA;
		if (!gMatchBorders[1].vertices.empty())
		for (x = 0; x < deriv.mWidth; ++x)
			deriv(x, 0) = DEM_NO_DATA;
	}

	// Add any bulk points - mostly edges and water.

	if (prog) prog(0, 3, "Calculating Mesh Points", 1.0);
	{
		TIMER(Triangulate_Elevation)
		AddBulkPointsToMesh(deriv, outMesh, prog);
	}
	
	// Falsify that deriv edges were burned - this is the "flag" to greedy-insert to not go around adding those vertices.
	{
		if (!gMatchBorders[2].vertices.empty())
		for (y = 0; y < deriv.mHeight; ++y)
			deriv(deriv.mWidth-1, y) = orig(deriv.mWidth-1, y);
		if (!gMatchBorders[3].vertices.empty())
		for (x = 0; x < deriv.mWidth; ++x)
			deriv(x, deriv.mHeight-1) = orig(x, deriv.mHeight-1);
		if (!gMatchBorders[0].vertices.empty())
		for (y = 0; y < deriv.mHeight; ++y)
			deriv(0, y) =  orig(0,y);
		if (!gMatchBorders[1].vertices.empty())
		for (x = 0; x < deriv.mWidth; ++x)
			deriv(x, 0) = orig(x,0);
	}
	
	// Now greedy mesh build - first add pts to cover land.
	{
		TIMER(Greedy_Mesh)
		GreedyMeshBuild(outMesh, orig, deriv, gMeshPrefs.max_error, 0.0, (land_ratio * 0.8 + 0.2) * gMeshPrefs.max_points, prog);
	}

	// Another greedy mesh designed to limit triangle size.
	{
		TIMER(Greedy_Mesh_LimitSize)
		GreedyMeshBuild(outMesh, orig, deriv, 0.0, gMeshPrefs.max_tri_size_m * MTR_TO_NM * NM_TO_DEG_LAT, gMeshPrefs.max_points, prog);
	}

	// Now go nuke the temporary edge - we don't need it now.

	for (n = 0; n < temporary.size(); ++n)
	{
		DebugAssert(!outMesh.are_there_incident_constraints(temporary[n]));
//		gMeshPoints.push_back(pair<Point2,Point3>(cgal2ben(temporary[n]->point()),Point3(1,1,1)));
		if (!outMesh.are_there_incident_constraints(temporary[n]))
			outMesh.remove(temporary[n]);
	}

	temporary.clear();


	// Now that our mesh is mostly done, add the coastline vertices with constraints) - they count on the mountain pts being in so we
	// can do anti-slivering on the fly.
	{

		TIMER(Triangulate_Coastlines)

		AddWaterMeshPoints(inMap, orig, deriv, outMesh, coastlines_markers, true);
	}

	// Finally, add the REAL slaved edge, canibalizing any coastline points as we go.
	{
		// This HAS to go after water.  Why?  Well, it can absorb existing points but the other routines dumbly add.
		// So we MUST build all forced points (water) first.
		for(b=0;b<4;++b)
		if (!gMatchBorders[b].vertices.empty())
			match_border(outMesh, gMatchBorders[b], b);
	}

	if (prog) prog(0, 3, "Calculating Mesh Points", 0.8);


	int n_vert = outMesh.number_of_vertices();					// Ben says: typically the end() iterator for the triangulation is _not_ stable across inserts.
	CGAL::make_conforming_Delaunay_2(outMesh);					// Because the finite iterator is a filtered wrapper around the triangulation, it too is not stable
																// across inserts.  To get around this, simply note how many vertices we inserted.  Note that we are assuming
	CDT::Vertex_iterator v1,v2,v;								// vertices to be inserted into the END of the iteration list!
	v1 = outMesh.vertices_begin();
	v2 = outMesh.vertices_end();	
	DebugAssert(outMesh.number_of_vertices() >= n_vert);
	std::advance(v1,n_vert);
	
	for(v=v1;v!=v2;++v)
	{
		v->info().height = orig.value_linear(CGAL::to_double(v->point().x()),CGAL::to_double(v->point().y()));
		#if DEV
		if(!gMatchBorders[0].vertices.empty())
			DebugAssert(v->point().x() != orig.mWest);
		if(!gMatchBorders[1].vertices.empty())
			DebugAssert(v->point().y() != orig.mSouth);
		if(!gMatchBorders[2].vertices.empty())
			DebugAssert(v->point().x() != orig.mEast);
		if(!gMatchBorders[3].vertices.empty())
			DebugAssert(v->point().y() != orig.mNorth);
		#endif	
	}

	/*********************************************************************************************************************
	 * LAND USE CALC (A LITTLE BIT)
	 *********************************************************************************************************************/


	if (prog) prog(2, 3, "Calculating Wet Areas", 0.2);
	{
		SetWaterBodiesToWet(outMesh, coastlines_markers, orig);
	}

	/*********************************************************************************************************************
	 * CLEANUP - CALC MESH NORMALS
	 *********************************************************************************************************************/

	if (prog) prog(2, 3, "Calculating Wet Areas", 0.5);
	CalculateMeshNormals(outMesh);

	if (prog) prog(2, 3, "Calculating Wet Areas", 1.0);

//	orig.swap(water);
}


#pragma mark -
/*******************************************************************************************
 *******************************************************************************************
 ** MESH LANDUSE ASSIGNMENT ****************************************************************
 *******************************************************************************************
 *******************************************************************************************/

/*
	NOTE ON TERRAIN TYPES:
		The vector map contains a terrain type like none or airport or water.
		From this we then get natural, airport, or water in the mesh.  We then substitute
		on all but water through the spreadsheet.
*/

void	AssignLandusesToMesh(	DEMGeoMap& inDEMs,
								CDT& ioMesh,
								const char * mesh_folder,
								ProgressFunc	inProg)
{


		CDT::Finite_faces_iterator tri;
		CDT::Finite_vertices_iterator vert;

		int	rock_enum = LookupToken("rock_gray.ter");

	if (inProg) inProg(0, 1, "Assigning Landuses", 0.0);

	DEMGeo&	inClimate(inDEMs[dem_Climate]);
	DEMGeo&	inElevation(inDEMs[dem_Elevation]);
	DEMGeo&	inSlope(inDEMs[dem_Slope]);
	DEMGeo&	inSlopeHeading(inDEMs[dem_SlopeHeading]);
	DEMGeo&	inRelElev(inDEMs[dem_RelativeElevation]);
	DEMGeo&	inRelElevRange(inDEMs[dem_ElevationRange]);
	DEMGeo&	inTemp(inDEMs[dem_Temperature]);
	DEMGeo&	inTempRng(inDEMs[dem_TemperatureRange]);
	DEMGeo&	inRain(inDEMs[dem_Rainfall]);
	DEMGeo& inUrbanDensity(inDEMs[dem_UrbanDensity]);
	DEMGeo& inUrbanRadial(inDEMs[dem_UrbanRadial]);
	DEMGeo& inUrbanTransport(inDEMs[dem_UrbanTransport]);
	DEMGeo& usquare(inDEMs[dem_UrbanSquare]);

	DEMGeo	landuse(inDEMs[dem_LandUse]);

// BEN SEZ: do NOT overwrite interrupted and other such areas with nearest landuse - that causes problems.
	for (int y = 0; y < landuse.mHeight;++y)
	for (int x = 0; x < landuse.mWidth; ++x)
	{
		float e = landuse(x,y);
		if (e == NO_VALUE ||
//			e == lu_usgs_INTERRUPTED_AREAS ||
//			e == lu_usgs_URBAN_SQUARE ||
//			e == lu_usgs_URBAN_IRREGULAR ||
			e == lu_usgs_INLAND_WATER ||
			e == lu_usgs_SEA_WATER)
//			e == lu_usgs_DEM_NO_DATA)
			landuse(x,y) = DEM_NO_DATA;
	}
	landuse.fill_nearest();

	/***********************************************************************************************
	 * ASSIGN BASIC LAND USES TO MESH
	 ***********************************************************************************************/

	if (inProg) inProg(0, 1, "Assigning Landuses", 0.1);
	for (tri = ioMesh.finite_faces_begin(); tri != ioMesh.finite_faces_end(); ++tri)
	{
		// First assign a basic land use type.
		{
			tri->info().flag = 0;
			// Hires - take from DEM if we don't have one.
			if (tri->info().terrain != terrain_Water)
			{
				double x0 = CGAL::to_double(tri->vertex(0)->point().x());
				double y0 = CGAL::to_double(tri->vertex(0)->point().y());
				double x1 = CGAL::to_double(tri->vertex(1)->point().x());
				double y1 = CGAL::to_double(tri->vertex(1)->point().y());
				double x2 = CGAL::to_double(tri->vertex(2)->point().x());
				double y2 = CGAL::to_double(tri->vertex(2)->point().y());
				double	center_x = (x0 + x1 + x2) / 3.0;
				double	center_y = (y0 + y1 + y2) / 3.0;

				float lu  = landuse.search_nearest(center_x, center_y);
				float lu1 = landuse.search_nearest(x0,y0);
				float lu2 = landuse.search_nearest(x1,y1);
				float lu3 = landuse.search_nearest(x2,y2);

				float cl  = inClimate.search_nearest(center_x, center_y);
				float cl1 = inClimate.search_nearest(x0,y0);
				float cl2 = inClimate.search_nearest(x1,y1);
				float cl3 = inClimate.search_nearest(x2,y2);

				// Ben sez: tiny island in the middle of nowhere - do NOT expect LU.  That's okay - Sergio doesn't need it.
//				if (lu == DEM_NO_DATA)
//					fprintf(stderr, "NO data anywhere near %f, %f\n", center_x, center_y);
				lu = MAJORITY_RULES(lu,lu1,lu2, lu3);
				cl = MAJORITY_RULES(cl, cl1, cl2, cl3);

				float	el1 = inElevation.value_linear(x0,y0);
				float	el2 = inElevation.value_linear(x1,y1);
				float	el3 = inElevation.value_linear(x2,y2);
				float	el = SAFE_AVERAGE(el1, el2, el3);

				float	sl1 = inSlope.value_linear(x0,y0);
				float	sl2 = inSlope.value_linear(x1,y1);
				float	sl3 = inSlope.value_linear(x2,y2);
				float	sl = SAFE_MAX	 (sl1, sl2, sl3);	// Could be safe max.
				if (sl<0.0) sl=0.0;

				float	tm1 = inTemp.value_linear(x0,y0);
				float	tm2 = inTemp.value_linear(x1,y1);
				float	tm3 = inTemp.value_linear(x2,y2);
				float	tm = SAFE_AVERAGE(tm1, tm2, tm3);	// Could be safe max.

				float	tmr1 = inTempRng.value_linear(x0,y0);
				float	tmr2 = inTempRng.value_linear(x1,y1);
				float	tmr3 = inTempRng.value_linear(x2,y2);
				float	tmr = SAFE_AVERAGE(tmr1, tmr2, tmr3);	// Could be safe max.

				float	rn1 = inRain.value_linear(x0,y0);
				float	rn2 = inRain.value_linear(x1,y1);
				float	rn3 = inRain.value_linear(x2,y2);
				float	rn = SAFE_AVERAGE(rn1, rn2, rn3);	// Could be safe max.

//				float	sh1 = inSlopeHeading.value_linear(x0,y0);
//				float	sh2 = inSlopeHeading.value_linear(x1,y1);
///				float	sh3 = inSlopeHeading.value_linear(x2,y2);
//				float	sh = SAFE_AVERAGE(sh1, sh2, sh3);	// Could be safe max.

				float	re1 = inRelElev.value_linear(x0,y0);
				float	re2 = inRelElev.value_linear(x1,y1);
				float	re3 = inRelElev.value_linear(x2,y2);
				float	re = SAFE_AVERAGE(re1, re2, re3);	// Could be safe max.

				float	er1 = inRelElevRange.value_linear(x0,y0);
				float	er2 = inRelElevRange.value_linear(x1,y1);
				float	er3 = inRelElevRange.value_linear(x2,y2);
				float	er = SAFE_AVERAGE(er1, er2, er3);	// Could be safe max.

				int		near_water =(tri->neighbor(0)->info().terrain == terrain_Water && !ioMesh.is_infinite(tri->neighbor(0))) ||
									(tri->neighbor(1)->info().terrain == terrain_Water && !ioMesh.is_infinite(tri->neighbor(1))) ||
									(tri->neighbor(2)->info().terrain == terrain_Water && !ioMesh.is_infinite(tri->neighbor(2)));

				float	uden1 = inUrbanDensity.value_linear(x0,y0);
				float	uden2 = inUrbanDensity.value_linear(x1,y1);
				float	uden3 = inUrbanDensity.value_linear(x2,y2);
				float	uden = SAFE_AVERAGE(uden1, uden2, uden3);	// Could be safe max.

				float	urad1 = inUrbanRadial.value_linear(x0,y0);
				float	urad2 = inUrbanRadial.value_linear(x1,y1);
				float	urad3 = inUrbanRadial.value_linear(x2,y2);
				float	urad = SAFE_AVERAGE(urad1, urad2, urad3);	// Could be safe max.

				float	utrn1 = inUrbanTransport.value_linear(x0,y0);
				float	utrn2 = inUrbanTransport.value_linear(x1,y1);
				float	utrn3 = inUrbanTransport.value_linear(x2,y2);
				float	utrn = SAFE_AVERAGE(utrn1, utrn2, utrn3);	// Could be safe max.

				float usq  = usquare.search_nearest(center_x, center_y);
				float usq1 = usquare.search_nearest(x0,y0);
				float usq2 = usquare.search_nearest(x1,y1);
				float usq3 = usquare.search_nearest(x2,y2);
				usq = MAJORITY_RULES(usq, usq1, usq2, usq3);

//				float	el1 = tri->vertex(0)->info().height;
//				float	el2 = tri->vertex(1)->info().height;
//				float	el3 = tri->vertex(2)->info().height;
//				float	el_tri = (el1 + el2 + el3) / 3.0;

				float	sl_tri = 1.0 - tri->info().normal[2];
				float	flat_len = sqrt(tri->info().normal[1] * tri->info().normal[1] + tri->info().normal[0] * tri->info().normal[0]);
				float	sh_tri = tri->info().normal[1];
				if (flat_len != 0.0)
				{
					sh_tri /= flat_len;
					sh_tri = max(-1.0f, min(sh_tri, 1.0f));
				}

				float	patches = (gMeshPrefs.rep_switch_m == 0.0) ? 100.0 : (60.0 * NM_TO_MTR / gMeshPrefs.rep_switch_m);
				int x_variant = fabs(center_x /*+ RandRange(-0.03, 0.03)*/) * patches; // 25.0;
				int y_variant = fabs(center_y /*+ RandRange(-0.03, 0.03)*/) * patches; // 25.0;
				int variant_blob = ((x_variant + y_variant * 2) % 4) + 1;
				int variant_head = (tri->info().normal[0] > 0.0) ? 6 : 8;

				if (sh_tri < -0.7)	variant_head = 7;
				if (sh_tri >  0.7)	variant_head = 5;

				//fprintf(stderr, " %d", tri->info().feature);
				int terrain = FindNaturalTerrain(tri->info().feature, lu, cl, el, sl, sl_tri, tm, tmr, rn, near_water, sh_tri, re, er, uden, urad, utrn, usq, fabs((float) center_y), variant_blob, variant_head);
				if (terrain == -1)
					AssertPrintf("Cannot find terrain for: %s, %s, %f, %f\n", FetchTokenString(lu), FetchTokenString(cl), el, sl);

				tri->info().debug_slope_dem = sl;
				tri->info().debug_slope_tri = sl_tri;
				tri->info().debug_temp = tm;
				tri->info().debug_temp_range = tmr;
				tri->info().debug_rain = rn;
				tri->info().debug_heading = sh_tri;

				if (terrain == gNaturalTerrainTable.back().name)
				{
					AssertPrintf("Hit %s rule. lu=%s, msl=%f, slope=%f, trislope=%f, temp=%f, temprange=%f, rain=%f, water=%d, heading=%f, lat=%f\n",
						FetchTokenString(gNaturalTerrainTable.back().name),
						FetchTokenString(lu), el, acos(1-sl)*RAD_TO_DEG, acos(1-sl_tri)*RAD_TO_DEG, tm, tmr, rn, near_water, sh_tri, center_y);
				}
				//fprintf(stderr, "->%d", terrain);

				tri->info().terrain = terrain;

			}

		}
	}

	/***********************************************************************************************
	 * TRY TO CONSOLIDATE BLOBS
	 ***********************************************************************************************/
	// If a blob's total area is less than the blobbing distance, it's not really needed!  Simplify
	// it.

	int tri_merged = 0;
	set<CDT::Face_handle>	all_variants;

	for (CDT::Finite_faces_iterator f = ioMesh.finite_faces_begin(); f != ioMesh.finite_faces_end(); ++f)
	if (f->info().terrain != terrain_Water)
	if (HasVariant(f->info().terrain))
		all_variants.insert(f);

	float max_rat = gMeshPrefs.rep_switch_m * MTR_TO_NM * NM_TO_DEG_LAT;

	while (!all_variants.empty())
	{
		CDT::Face_handle		w = *all_variants.begin();
		int						base = SpecificVariant(w->info().terrain,0);
		set<CDT::Face_handle>	tri_set;
		Bbox_2					bounds;
		FindAllCovariant(ioMesh, w, tri_set, bounds);

		bool devary = (bounds.ymax() - bounds.ymin() < max_rat) && (bounds.xmax() - bounds.xmin()) < max_rat;

		for (set<CDT::Face_handle>::iterator kill = tri_set.begin(); kill != tri_set.end(); ++kill)
		{
			if (devary)
			{
				(*kill)->info().terrain = base;
				++tri_merged;
			}
			all_variants.erase(*kill);
		}
	}

	/***********************************************************************************************
	 * DEAL WITH INTRUSION FROM OUR MASTER SIDE
	 ***********************************************************************************************/

	// BEN SEZ - IS THIS COMMENT TRUE?
	// ??? This must be POST optmize - we can go OUT OF ORDER on the borders because must have left-master/right-slave.
	// ??? So the optmizer will NUKE this stuff. :-(

	// First build a correlation between our border info and some real tris in the mesh.
	int b;
	for(b=0;b<4;++b)
	if (!gMatchBorders[b].vertices.empty())
		border_find_edge_tris(ioMesh, gMatchBorders[b]);
	int lowest;
	int n;
#if !NO_BORDER_SHARING

	set<CDT::Vertex_handle>	vertices;
	// Now we have to "rebase" our edges.  Basically it is possible that we are getting intruded from the left
	// by a lower priority texture.  If we just use borders, that low prio tex will end up UNDER our base, and we'll
	// never see it.  So we need to take the tex on our right side and reduce it.
	for(b=0;b < 4; ++b)
	{
		for (n = 0; n < gMatchBorders[b].edges.size(); ++n)
		if (gMatchBorders[b].edges[n].buddy != CDT::Face_handle())
		{
			lowest = gMatchBorders[b].edges[n].buddy->info().terrain;
			if (LowerPriorityNaturalTerrain(gMatchBorders[b].edges[n].base, lowest))
				lowest = gMatchBorders[b].edges[n].base;
			for (set<int>::iterator bl = gMatchBorders[b].edges[n].borders.begin(); bl != gMatchBorders[b].edges[n].borders.end(); ++bl)
			{
				if (LowerPriorityNaturalTerrain(*bl, lowest))
					lowest = *bl;
			}

			if (lowest != gMatchBorders[b].edges[n].buddy->info().terrain)
				RebaseTriangle(ioMesh, gMatchBorders[b].edges[n].buddy, lowest, gMatchBorders[b].vertices[n].buddy, gMatchBorders[b].vertices[n+1].buddy, vertices);
		}

		for (n = 0; n < gMatchBorders[b].vertices.size(); ++n)
		{
			CDT::Face_circulator circ, stop;
			circ = stop = ioMesh.incident_faces(gMatchBorders[b].vertices[n].buddy);
			do {
				if (!ioMesh.is_infinite(circ))
				if (!is_border(ioMesh, circ))
				{
					lowest = circ->info().terrain;
					for (hash_map<int, float>::iterator bl = gMatchBorders[b].vertices[n].blending.begin(); bl != gMatchBorders[b].vertices[n].blending.end(); ++bl)
					if (bl->second > 0.0)
					if (LowerPriorityNaturalTerrain(bl->first, lowest))
						lowest = bl->first;

					if (lowest != circ->info().terrain)
						RebaseTriangle(ioMesh, circ, lowest, gMatchBorders[b].vertices[n].buddy, CDT::Vertex_handle(), vertices);
				}
				++circ;
			} while (circ != stop);
		}
	}

	// These vertices were given partial borders by rebasing - go in and make sure that all incident triangles match them.
	for (set<CDT::Vertex_handle>::iterator rebased_vert = vertices.begin(); rebased_vert != vertices.end(); ++rebased_vert)
	{
		CDT::Face_circulator circ, stop;
		circ = stop = ioMesh.incident_faces(*rebased_vert);
		do {
			if (!ioMesh.is_infinite(circ))
			for (hash_map<int, float>::iterator bl = (*rebased_vert)->info().border_blend.begin(); bl != (*rebased_vert)->info().border_blend.end(); ++bl)
			if (bl->second > 0.0)
				AddZeroMixIfNeeded(circ, bl->first);
			++circ;
		} while (circ != stop);
	}

#endif

	/***********************************************************************************************
	 * CALCULATE BORDERS
	 ***********************************************************************************************/

	if (inProg) inProg(0, 1, "Assigning Landuses", 0.5);

	/* 	Here's the idea:
		We are going to go through each triangle, which now has a land use, and figure ouet which
		ones have borders.  A triangle that has a border will get:
		(1) the land use of the border triangle in its set of "border landuses", so it
		 	can easily be identified in that mesh, and
		(2) for each of its vertices, a hash map entry with the alpha level for the border at that
			point, so we can figure out how  the border fades.

		To do this we say: for each triangle, we do a "spreading" type algorithm, e.g. we collect
		non-visited neighbors that meet our criteria in a set and go outward.  We only take neighbors
		that have a lower natural land use and haven't been visited.  We calc our distance to the
		corners to get the blend, and if we're not all faded out, keep going.
	*/

	int		visited = 0;	// flag value - by using a rolling flag, we don't have to reset
							// this all of the time.
	int		tri_total = 0, tri_border = 0, tri_check = 0, tri_opt = 0;
	for (tri = ioMesh.finite_faces_begin(); tri != ioMesh.finite_faces_end(); ++tri)
	if (tri->info().terrain != terrain_Water)
	{
		++visited;
		set<CDT::Face_handle>	to_visit;
		to_visit.insert(tri);
		bool					spread;
		int						layer = tri->info().terrain;
		tri->info().flag = visited;

		while (!to_visit.empty())
		{
			CDT::Face_handle	border = *to_visit.begin();
			to_visit.erase(border);
			spread = false;
			if (&*border != &*tri)
			{
				// Calculation phase - figure out alphas of
				// the corners.
				CDT::Vertex_handle v1 = border->vertex(0);
				CDT::Vertex_handle v2 = border->vertex(1);
				CDT::Vertex_handle v3 = border->vertex(2);
				double	dist1 = DistPtToTri(v1, tri);
				double	dist2 = DistPtToTri(v2, tri);
				double	dist3 = DistPtToTri(v3, tri);
				double	dist_max = GetXonDist(layer, border->info().terrain, border->info().normal[2]);

				if (dist_max > 0.0)
				{
					dist1 = max(0.0, min((dist_max-dist1)/dist_max,1.0));
					dist2 = max(0.0, min((dist_max-dist2)/dist_max,1.0));
					dist3 = max(0.0, min((dist_max-dist3)/dist_max,1.0));

					++tri_check;
					if (dist1 > 0.0 || dist2 > 0.0 || dist3 > 0.0)
					{
						double	odist1 = v1->info().border_blend[layer];
						double	odist2 = v2->info().border_blend[layer];
						double	odist3 = v3->info().border_blend[layer];

						// Border propagation - we only want to set the levels of this border if we are are adjacent to ourselves..this way we don't set the far-side distance
						// unless there will be another border tri to continue with.

						bool has_0 = false, has_1 = false, has_2 = false;
						if (border->neighbor(0)->info().terrain_border.count(layer) || border->neighbor(0)->info().terrain == layer) { has_1 = true; has_2 = true; }
						if (border->neighbor(1)->info().terrain_border.count(layer) || border->neighbor(1)->info().terrain == layer) { has_2 = true; has_0 = true; }
						if (border->neighbor(2)->info().terrain_border.count(layer) || border->neighbor(2)->info().terrain == layer) { has_0 = true; has_1 = true; }

						// BUT...if we're at the edge of the file, go across anyway, what the hell...
						// Ben sez: no- try to limit cross-border madness or we get projection mismatches.
//						if (!has_0 && IsEdgeVertex(ioMesh, v1))	has_0 = true;
//						if (!has_1 && IsEdgeVertex(ioMesh, v2))	has_1 = true;
//						if (!has_2 && IsEdgeVertex(ioMesh, v3))	has_2 = true;

						if (!has_0) dist1 = 0.0;
						if (!has_1) dist2 = 0.0;
						if (!has_2) dist3 = 0.0;

						// If we're not faded out totally, record an increase.  ONLY keep
						// searching if we are increasing one of the vertices.  Otherwise
						// someone else has been over this territory who is already closer
						// and we're just wasting our time.
						if (dist1 > odist1) { spread = true; v1->info().border_blend[layer] = dist1; }
						if (dist2 > odist2) { spread = true; v2->info().border_blend[layer] = dist2; }
						if (dist3 > odist3) { spread = true; v3->info().border_blend[layer] = dist3; }

						// HACK - does always extending the borders fix a bug?
						DebugAssert(layer != -1);
						border->info().terrain_border.insert(layer);
						spread = true;
					}
				}
			} else
				spread = true;

			border->info().flag = visited;

			// Spreading case: check our neighbors to make sure we haven't seen them and it makes
			// sense to check them.
			if (spread)
			{
				CDT::Face_handle b1 = border->neighbor(0);
				CDT::Face_handle b2 = border->neighbor(1);
				CDT::Face_handle b3 = border->neighbor(2);

				if (b1->info().flag != visited && !ioMesh.is_infinite(b1) && b1->info().terrain != terrain_Water && LowerPriorityNaturalTerrain(b1->info().terrain, layer))	to_visit.insert(b1);
				if (b2->info().flag != visited && !ioMesh.is_infinite(b2) && b2->info().terrain != terrain_Water && LowerPriorityNaturalTerrain(b2->info().terrain, layer))	to_visit.insert(b2);
				if (b3->info().flag != visited && !ioMesh.is_infinite(b3) && b3->info().terrain != terrain_Water && LowerPriorityNaturalTerrain(b3->info().terrain, layer))	to_visit.insert(b3);
			}
		}
	}

	/***********************************************************************************************
	 * DEAL WITH INTRUSION FROM OUR MASTER SIDE
	 ***********************************************************************************************/
#if !NO_BORDER_SHARING
	// First - force border blend of zero at the slaved edge, no matter how ridiculous.  We can't possibly propagate
	// this border into a previously rendered file, so a hard stop is better than a cutoff.
	for(b=0;b<4;++b)
	for (n = 0; n < gMatchBorders[b].vertices.size(); ++n)
	for (hash_map<int, float>::iterator blev = gMatchBorders[b].vertices[n].buddy->info().border_blend.begin(); blev != gMatchBorders[b].vertices[n].buddy->info().border_blend.end(); ++blev)
		blev->second = 0.0;

	// Now we are going to go in and add borders on our slave edges from junk coming in on the left.  We have ALREADY
	// "rebased" the terrain.  This means that the border on the slave side is guaranteed to be lower priority than the border
	// on the master, so that we can make this border-extension safely.  For the base and borders on the master we just add
	// a border on the slave - the edge blend levels are the master's blend and the interior poiont gets a blend of 0 or whatever
	// was already there.

	for(b=0;b<4;++b)
	for (n = 0; n < gMatchBorders[b].edges.size(); ++n)
	if (gMatchBorders[b].edges[n].buddy != CDT::Face_handle())
	if (gMatchBorders[b].edges[n].buddy->info().terrain != terrain_Water)
	{
		// Handle the base terrain
		if (gMatchBorders[b].edges[n].buddy->info().terrain != gMatchBorders[b].edges[n].base)
		{
			AddZeroMixIfNeeded(gMatchBorders[b].edges[n].buddy, gMatchBorders[b].edges[n].base);
			gMatchBorders[b].vertices[n].buddy->info().border_blend[gMatchBorders[b].edges[n].base] = 1.0;
			SafeSmearBorder(ioMesh, gMatchBorders[b].vertices[n].buddy, gMatchBorders[b].edges[n].base);
			gMatchBorders[b].vertices[n+1].buddy->info().border_blend[gMatchBorders[b].edges[n].base] = 1.0;
			SafeSmearBorder(ioMesh, gMatchBorders[b].vertices[n+1].buddy, gMatchBorders[b].edges[n].base);
		}

		// Handle any overlay layers...
		for (set<int>::iterator bl = gMatchBorders[b].edges[n].borders.begin(); bl != gMatchBorders[b].edges[n].borders.end(); ++bl)
		{
			if (gMatchBorders[b].edges[n].buddy->info().terrain != *bl)
			{
				AddZeroMixIfNeeded(gMatchBorders[b].edges[n].buddy, *bl);
				gMatchBorders[b].vertices[n].buddy->info().border_blend[*bl] = gMatchBorders[b].vertices[n].blending[*bl];
				SafeSmearBorder(ioMesh, gMatchBorders[b].vertices[n].buddy, *bl);
				gMatchBorders[b].vertices[n+1].buddy->info().border_blend[*bl] = gMatchBorders[b].vertices[n+1].blending[*bl];
				SafeSmearBorder(ioMesh, gMatchBorders[b].vertices[n+1].buddy, *bl);
			}
		}
	}

#endif

	/***********************************************************************************************
	 * OPTIMIZE BORDERS!
	 ***********************************************************************************************/
	if (inProg) inProg(0, 1, "Assigning Landuses", 0.75);

	if (gMeshPrefs.optimize_borders)
	{
		for (tri = ioMesh.finite_faces_begin(); tri != ioMesh.finite_faces_end(); ++tri)
		if (tri->info().terrain != terrain_Water)
		{
			bool need_optimize = false;
			for (set<int>::iterator blayer = tri->info().terrain_border.begin();
				blayer != tri->info().terrain_border.end(); ++blayer)
			{
				if (tri->vertex(0)->info().border_blend[*blayer] == 1.0 &&
					tri->vertex(1)->info().border_blend[*blayer] == 1.0 &&
					tri->vertex(2)->info().border_blend[*blayer] == 1.0)
				{
					if (LowerPriorityNaturalTerrain(tri->info().terrain, *blayer))
					{
						tri->info().terrain = *blayer;
						need_optimize = true;
					}
				}
			}
			if (need_optimize)
			{
				set<int>	nuke;
				for (set<int>::iterator blayer = tri->info().terrain_border.begin();
					blayer != tri->info().terrain_border.end(); ++blayer)
				{
					if (!LowerPriorityNaturalTerrain(tri->info().terrain, *blayer))
						nuke.insert(*blayer);
				}
				for (set<int>::iterator nlayer = nuke.begin(); nlayer != nuke.end(); ++nlayer)
				{
					tri->info().terrain_border.erase(*nlayer);
					// DO NOT eliminate these - maybe our neighbor is using them!!
//					tri->vertex(0)->info().border_blend.erase(*nlayer);
//					tri->vertex(1)->info().border_blend.erase(*nlayer);
//					tri->vertex(2)->info().border_blend.erase(*nlayer);
					++tri_opt;
				}
			}
		}
	}

	{
		for (tri = ioMesh.finite_faces_begin(); tri != ioMesh.finite_faces_end(); ++tri)
		if (tri->info().terrain != terrain_Water)
		{
			tri_total++;
			tri_border += (tri->info().terrain_border.size());
		} else if (!tri->info().terrain_border.empty())
			AssertPrintf("BORDER ON WATER LAND USE!  Terrain = %s", FetchTokenString(tri->info().terrain));
		printf("Total: %d - border: %d - check: %d - opt: %d, devary=%d\n", tri_total, tri_border, tri_check, tri_opt,tri_merged);
	}



	/***********************************************************************************************
	 * WRITE OUT MESH
	 ***********************************************************************************************/

	// We need to write out an edge file for our next guy in line.

	if (gMeshPrefs.border_match)
	{
		double	west = inElevation.mWest;
		double	east = inElevation.mEast;
		double	south = inElevation.mSouth;
		double	north = inElevation.mNorth;
		char	fname[512];

		string border_loc = mesh_folder;
#if APL && !defined(__MACH__)
		string	appP;
		AppPath(appP);
		string::size_type b = appP.rfind(':');
		appP.erase(b+1);
		border_loc = appP + border_loc;
#endif


		sprintf(fname, "%s%s%+03d%+04d%s%+03d%+04d.border.txt", border_loc.c_str(), DIR_STR, latlon_bucket (south), latlon_bucket (west), DIR_STR, (int) south, (int) west);

		FILE * border = fopen(fname, "w");
		if (border == NULL) AssertPrintf("Unable to open file %s for writing.", fname);

		CDT::Point cur,stop;
		for(int b = 0; b < 4; ++b)
		{
			switch(b) {
			case 0:	cur = CDT::Point(west,south);	stop = CDT::Point(west,north);	break;
			case 1:	cur = CDT::Point(west,south);	stop = CDT::Point(east,south);	break;
			case 2:	cur = CDT::Point(east,south);	stop = CDT::Point(east,north);	break;
			case 3:	cur = CDT::Point(west,north);	stop = CDT::Point(east,north);	break;
			}

			CDT::Face_handle	f;
			int					i;
			CDT::Locate_type	lt;
			f = ioMesh.locate(cur, lt, i);
			Assert(lt == CDT::VERTEX);

			CDT::Face_circulator circ, circstop;

			do {
				fprintf(border, "VT %.12lf, %.12lf, %lf\n",
					CGAL::to_double(f->vertex(i)->point().x()),
					CGAL::to_double(f->vertex(i)->point().y()),
					CGAL::to_double(f->vertex(i)->info().height));

				hash_map<int, float>	borders;
				for (hash_map<int, float>::iterator hfi = f->vertex(i)->info().border_blend.begin(); hfi != f->vertex(i)->info().border_blend.end(); ++hfi)
				if (hfi->second > 0.0)
					borders[hfi->first] = max(borders[hfi->first], hfi->second);
				circ = circstop = ioMesh.incident_faces(f->vertex(i));
				do {
					if (!ioMesh.is_infinite(circ))
					{
						borders[circ->info().terrain] = 1.0;
					}
					++circ;
				} while (circ != circstop);

				fprintf(border, "VBC %d\n", borders.size());
				for (hash_map<int, float>::iterator hfi = borders.begin(); hfi != borders.end(); ++hfi)
					fprintf(border, "VB %f %s\n", hfi->second, FetchTokenString(hfi->first));

				if(b == 1 || b == 3)				FindNextEast(ioMesh, f, i, b==1);
				else								FindNextNorth(ioMesh, f, i, b==2);
				DebugAssert(!ioMesh.is_infinite(f));

				fprintf(border, "TERRAIN %s\n", FetchTokenString(f->info().terrain));
				fprintf(border, "BORDER_C %d\n", f->info().terrain_border.size());
				for (set<int>::iterator si = f->info().terrain_border.begin(); si != f->info().terrain_border.end(); ++si)
					fprintf(border, "BORDER_T %s\n", FetchTokenString(*si));

			} while (f->vertex(i)->point() != stop);

			fprintf(border, "VC %.12lf, %.12lf, %lf\n",
					CGAL::to_double(f->vertex(i)->point().x()),
					CGAL::to_double(f->vertex(i)->point().y()),
					CGAL::to_double(f->vertex(i)->info().height));
			fprintf(border, "VBC %d\n", f->vertex(i)->info().border_blend.size());
			for (hash_map<int, float>::iterator hfi = f->vertex(i)->info().border_blend.begin(); hfi != f->vertex(i)->info().border_blend.end(); ++hfi)
				fprintf(border, "VB %f %s\n", hfi->second, FetchTokenString(hfi->first));
		}

		fprintf(border, "END\n");
		fclose(border);

	}

	if (inProg) inProg(0, 1, "Assigning Landuses", 1.0);

}


#pragma mark -
/*******************************************************************************************
 *	UTILITY ROUTINES
 *******************************************************************************************/
void SetupWaterRasterizer(const Pmwx& map, const DEMGeo& orig, PolyRasterizer& rasterizer)
{
	fprintf(stderr,"SetupWaterRasterizer");
	for (Pmwx::Edge_const_iterator i = map.edges_begin(); i != map.edges_end(); ++i)
	{
		bool	iWet = i->face()->data().IsWater() && !i->face()->is_unbounded();
		bool	oWet = i->twin()->face()->data().IsWater() && !i->twin()->face()->is_unbounded();

		if (iWet != oWet)
		{
			//fprintf(stderr,"|");
			double x1 = orig.lon_to_x(CGAL::to_double(i->source()->point().x()));
			double y1 = orig.lat_to_y(CGAL::to_double(i->source()->point().y()));
			double x2 = orig.lon_to_x(CGAL::to_double(i->target()->point().x()));
			double y2 = orig.lat_to_y(CGAL::to_double(i->target()->point().y()));

//				gMeshLines.push_back(i->source()->point());
//				gMeshLines.push_back(i->target()->point());

//				fprintf(fi,"%lf,%lf    %lf,%lf   %s\n", x1,y1,x2,y2, ((y1 == 0.0 || y2 == 0.0) && y1 != y2) ? "****" : "");

			if (y1 != y2)
			{
				if (y1 < y2)
					rasterizer.masters.push_back(PolyRasterSeg_t(x1,y1,x2,y2));
				else
					rasterizer.masters.push_back(PolyRasterSeg_t(x2,y2,x1,y1));
			}
		}
	}
//	fclose(fi);
	fprintf(stderr,".");

	rasterizer.SortMasters();
	fprintf(stderr,".");
}

void	Calc2ndDerivative(DEMGeo& deriv)
{
	int x, y;
	float 	h, ha, hr, hb, hl;
	for (y = 0; y < (deriv.mHeight-1); ++y)
	for (x = 0; x < (deriv.mWidth-1); ++x)
	{
		h  = deriv(x,y);
		ha = deriv(x,y+1);
		hr = deriv(x+1,y);

		if (h == DEM_NO_DATA || ha == DEM_NO_DATA || hr == DEM_NO_DATA)
			deriv(x,y) = DEM_NO_DATA;
		else
			deriv(x,y) = (ha - h) + (hr - h);
	}

	for (y = (deriv.mHeight-2); y >= 1; --y)
	for (x = (deriv.mWidth-2);  x >= 1; --x)
	{
		h  = deriv(x,y);
		hb = deriv(x,y-1);
		hl = deriv(x-1,y);

		if (h == DEM_NO_DATA || hb == DEM_NO_DATA || hl == DEM_NO_DATA)
			deriv(x,y) = DEM_NO_DATA;
		else
			deriv(x,y) = (h - hl) + (h - hb);
	}

	for (x = 0; x < deriv.mWidth; ++x)
	{
		deriv(x, 0) = DEM_NO_DATA;
		deriv(x, deriv.mHeight-1) = DEM_NO_DATA;
	}
	for (x = y; y < deriv.mHeight; ++y)
	{
		deriv(0, y) = DEM_NO_DATA;
		deriv(deriv.mWidth-1, y) = DEM_NO_DATA;
	}
}

double	HeightWithinTri(CDT& inMesh, CDT::Face_handle f, CDT::Point in)
{
	Assert(!inMesh.is_infinite(f));

	double	DEG_TO_NM_LON = DEG_TO_NM_LAT * cos(CGAL::to_double(in.y()) * DEG_TO_RAD);

	Point_3	p1((f->vertex(0)->point().x() * (DEG_TO_NM_LON * NM_TO_MTR)),
			   (f->vertex(0)->point().y() * (DEG_TO_NM_LAT * NM_TO_MTR)),
			   (f->vertex(0)->info().height));

	Point_3	p2((f->vertex(1)->point().x() * (DEG_TO_NM_LON * NM_TO_MTR)),
			   (f->vertex(1)->point().y() * (DEG_TO_NM_LAT * NM_TO_MTR)),
			   (f->vertex(1)->info().height));

	Point_3	p3((f->vertex(2)->point().x() * (DEG_TO_NM_LON * NM_TO_MTR)),
			   (f->vertex(2)->point().y() * (DEG_TO_NM_LAT * NM_TO_MTR)),
			   (f->vertex(2)->info().height));

	Vector_3	s1(p2, p3);
	Vector_3	s2(p2, p1);
	Vector_3	n = cross_product(s1, s2);
	//Plane_3	plane = Plane_3(p1, n);
	//plane.n = n;
	//plane.ndotp = n * Vector_3(p1);
	double r = CGAL::to_double(p1.z() - ((n.x() * (in.x() * (DEG_TO_NM_LON * NM_TO_MTR) - p1.x()) + (n.y() * (in.y() * (DEG_TO_NM_LAT * NM_TO_MTR) - p1.y()))) / n.z()));

	//Plane_3 plane = Plane_3(p2, p1, p3);
	//double r = CGAL::to_double(plane.to_3d(Point_2(in.x() * (DEG_TO_NM_LON * NM_TO_MTR), in.y() * (DEG_TO_NM_LAT * NM_TO_MTR))).z());

	//printf("%lf, %lf, %lf. ", CGAL::to_double(in.x()), CGAL::to_double(in.y()), r);
	return r;

}


double	MeshHeightAtPoint(CDT& inMesh, double inLon, double inLat, int hint_id)
{
	if (inMesh.number_of_faces() < 1) return DEM_NO_DATA;
	CDT::Face_handle	f = NULL;
	int	n;
	CDT::Locate_type lt;
	f = inMesh.locate_cache(CDT::Point(inLon, inLat), lt, n, hint_id);
	if (lt == CDT::VERTEX)
	{
		return f->vertex(n)->info().height;
	}
	if (lt == CDT::EDGE && inMesh.is_infinite(f))
	{
		f = f->neighbor(n);
	}

	if (!inMesh.is_infinite(f))
	{
		return HeightWithinTri(inMesh, f, CDT::Point(inLon, inLat));
	} else {
		printf("Requested point was off mesh: %lf, %lf\n", inLon, inLat);
		return DEM_NO_DATA;
	}
}




#define 	CLAMP_EPSILON	0.001

static void SlightClampToDEM(Point_2& ioPoint, const DEMGeo& ioDEM)
{
#if 0
	double	wicked_north = ioDEM.mNorth + CLAMP_EPSILON;
	double	wicked_south = ioDEM.mSouth - CLAMP_EPSILON;
	double	wicked_east = ioDEM.mEast + CLAMP_EPSILON;
	double	wicked_west = ioDEM.mWest - CLAMP_EPSILON;
	if (ioPoint.y() > wicked_north ||
		ioPoint.y() < wicked_south ||
		ioPoint.x() > wicked_east ||
		ioPoint.x() < wicked_west)
	{
		AssertPrintf("WARNING: Point is way outside DEM.  Will probably cause a leak.\n");
	} else {
		if (ioPoint.x() > ioDEM.mEast)	ioPoint.x() = ioDEM.mEast;
		if (ioPoint.x() < ioDEM.mWest)	ioPoint.x() = ioDEM.mWest;
		if (ioPoint.y() > ioDEM.mNorth)	ioPoint.y() = ioDEM.mNorth;
		if (ioPoint.y() < ioDEM.mSouth)	ioPoint.y() = ioDEM.mSouth;
	}
#endif
}

#undef CLAMP_EPSILON

int	CalcMeshError(CDT& mesh, DEMGeo& elev, map<float, int>& err, ProgressFunc inFunc)
{
	if (inFunc) inFunc(0, 1, "Calculating Error", 0.0);
	int hint = CDT::gen_cache_key();
	err.clear();
	int ctr = 0;
	for (int y = 0; y < elev.mHeight; ++y)
	{
		if (inFunc && (y % 20) == 0) inFunc(0, 1, "Calculating Error", (float) y / (float) elev.mHeight);

		for (int x = 0; x < elev.mWidth ; ++x)
		{
			float ideal = elev.get(x,y);
			if (ideal != DEM_NO_DATA)
			{
				float real = MeshHeightAtPoint(mesh, elev.x_to_lon(x), elev.y_to_lat(y), hint);
				if (real != DEM_NO_DATA)
				{
					float derr = real - ideal;
					err[derr]++;
					++ctr;
				}
			}
		}
	}
	if (inFunc) inFunc(0, 1, "Calculating Error", 1.0);
	return ctr;
}

int	CalcMeshTextures(CDT& inMesh, map<int, int>& out_lus)
{
	out_lus.clear();
	int total = 0;
	for(CDT::Face_iterator  f = inMesh.finite_faces_begin(); f != inMesh.finite_faces_end(); ++f)
	{
		out_lus[f->info().terrain]++;
		for(set<int>::iterator b =  f->info().terrain_border.begin();
							   b != f->info().terrain_border.end(); ++b)
			out_lus[*b]++;
		total += (1 + f->info().terrain_border.size());
	}
	return total;
}

static bool RayInTri(CDT::Face_handle tri, CDT::Vertex_handle v, const CDT::Point& goal)
{
	CDT::Orientation_2 pred;

	CDT::Vertex_handle	v_cw =  tri->vertex(CDT::cw (tri->index(v)));
	CDT::Vertex_handle	v_ccw = tri->vertex(CDT::ccw(tri->index(v)));

	if (pred(v->point(),  v_cw->point(), goal) == CGAL::LEFT_TURN ) return false;
	if (pred(v->point(), v_ccw->point(), goal) == CGAL::RIGHT_TURN) return false;
																	 return true;
}

bool common_vertex(CDT::Face_handle t1, CDT::Face_handle t2, int& index)
{
	if (t2->has_vertex(t1->vertex(0))) { index = 0; return true; }
	if (t2->has_vertex(t1->vertex(1))) { index = 1; return true; }
	if (t2->has_vertex(t1->vertex(2))) { index = 2; return true; }
	return false;
}



CDT_MarchOverTerrain_t::CDT_MarchOverTerrain_t()
{
	locate_face = NULL;
}

void MarchHeightStart(CDT& inMesh, const CDT::Point& loc, CDT_MarchOverTerrain_t& info)
{
	CDT::Locate_type	locate_type;
	int					locate_index;

	info.locate_face = inMesh.locate(loc, locate_type, locate_index, info.locate_face);

	// Special case: under some conditions we'll get the infinite-face edge.  This actually depends
	// on what our seed locate was.  Either way it is unacceptable - passing in an infinite face
	// generally makes the locate algorithm a little bonkers.  Reverse it here.
	if (inMesh.is_infinite(info.locate_face) && (locate_type == CDT::EDGE || locate_type == CDT::VERTEX))
	{
		info.locate_face = info.locate_face->neighbor(info.locate_face->index(inMesh.infinite_vertex()));
	}
	info.locate_pt = loc;
	info.locate_height = HeightWithinTri(inMesh, info.locate_face, loc);
}

void  MarchHeightGo(CDT& inMesh, const CDT::Point& goal, CDT_MarchOverTerrain_t& march_info, vector<Point3>& intermediates)
{
	static int level = 0;
	Assert(level < 2);

	// Makse sure our input makes some sense!
	DebugAssert(!inMesh.is_infinite(march_info.locate_face));
	DebugAssert(inMesh.triangle(march_info.locate_face).bounded_side(march_info.locate_pt) != CGAL::ON_UNBOUNDED_SIDE);

	intermediates.clear();

	CDT::Line_face_circulator circ(inMesh.line_walk(march_info.locate_pt, goal, march_info.locate_face));
	CDT::Line_face_circulator stop(circ);

	// Ben says: CGAL allows this, believe it or not - see special "null-type" comparator.
	// The REAL handle comparator is zapped out of MSC for templating reasons.
	if (circ == NULL)
	{
		CDT::Locate_type	goal_type;
		int					goal_index;
		CDT::Face_handle	goal_face;
		CDT::Point			rev_goal = march_info.locate_pt;
		goal_face = inMesh.locate(goal, goal_type, goal_index, march_info.locate_face);
		if (inMesh.is_infinite(goal_face) && goal_type == CDT::EDGE)
			goal_face = goal_face->neighbor(goal_index);

		double				goal_height = HeightWithinTri(inMesh, goal_face, goal);

		march_info.locate_pt = goal;
		march_info.locate_face = goal_face;
		march_info.locate_height = goal_height;

		++level;
		MarchHeightGo(inMesh, rev_goal, march_info, intermediates);
		--level;

		march_info.locate_pt = goal;
		march_info.locate_face = goal_face;
		march_info.locate_height = goal_height;

		int s = intermediates.size() / 2;
		for (int n = 0; n < s; ++n)
		{
			swap(intermediates[n], intermediates[intermediates.size() - n - 1]);
		}
		DebugAssert(!inMesh.is_infinite(march_info.locate_face));
		DebugAssert(inMesh.triangle(march_info.locate_face).bounded_side(march_info.locate_pt) != CGAL::ON_UNBOUNDED_SIDE);
		return;
	}

	intermediates.push_back(Point3(CGAL::to_double(march_info.locate_pt.x()), CGAL::to_double(march_info.locate_pt.y()), march_info.locate_height));

	CDT::Segment	ray(march_info.locate_pt, goal);
	int				cross_side;

	CDT::Geom_traits::Orientation_2 pred;

	while (1)
	{
		CDT::Point	last_pt;
		double		last_ht;

		CDT::Face_handle now = circ;
		++circ;
		CDT::Face_handle next = circ;

		if (!inMesh.is_infinite(now) && inMesh.triangle(now).bounded_side(goal) != CGAL::ON_UNBOUNDED_SIDE)
		{
			march_info.locate_pt = last_pt = goal;
			march_info.locate_height = last_ht = HeightWithinTri(inMesh, now, goal);
			march_info.locate_face = now;
			intermediates.push_back(Point3(CGAL::to_double(last_pt.x()), CGAL::to_double(last_pt.y()), last_ht));
			DebugAssert(!inMesh.is_infinite(march_info.locate_face));
			DebugAssert(inMesh.triangle(march_info.locate_face).bounded_side(march_info.locate_pt) != CGAL::ON_UNBOUNDED_SIDE);
			break;
		}

		if (now->has_neighbor(next))
		{
			cross_side = now->index(next);
			CDT::Segment crossed_seg = inMesh.segment(CDT::Edge(now, cross_side));

			CGAL::Orientation o1 = pred(ray.source(), ray.target(), crossed_seg.source());
			CGAL::Orientation o2 = pred(ray.source(), ray.target(), crossed_seg.target());

			// We can't both be any one value - that means the common side is on both tris -
			// one tri shouldn't be in the iteration!
			DebugAssert(o1 != o2);

			if (o1 == CGAL::COLLINEAR)
			{
				last_pt = now->vertex(CDT::ccw(cross_side))->point();
				last_ht = now->vertex(CDT::ccw(cross_side))->info().height ;
				intermediates.push_back(Point3(CGAL::to_double(last_pt.x()), CGAL::to_double(last_pt.y()), last_ht));
			} else if (o2 == CGAL::COLLINEAR)
			{
				last_pt = now->vertex(CDT::cw(cross_side))->point();
				last_ht = now->vertex(CDT::cw(cross_side))->info().height ;
				intermediates.push_back(Point3(CGAL::to_double(last_pt.x()), CGAL::to_double(last_pt.y()), last_ht));

			} else {
				CGAL::Object o = CGAL::intersection(ray, crossed_seg);
				if (CGAL::assign(last_pt, o))
				{
					Bbox_2 lim_ray = CDT::Segment(march_info.locate_pt,goal).bbox();
					Bbox_2 lim_seg = crossed_seg.bbox();
					Point_2	result(last_pt.x(),last_pt.y());
					if(!CGAL::do_overlap(lim_ray, result.bbox()))
					{
						#if DEBUG_DROPPED_PTS
							printf("WARNING: failed intersection: %.10lf, %.10lf\n",last_pt.x(),last_pt.y());
							gMeshPoints.push_back(pair<Point_2,Point_3>(last_pt,Point_3(1,1,0)));
							gMeshLines.push_back(pair<Point_2,Point_3>(march_info,Point_3(0,1,0)));
							gMeshLines.push_back(pair<Point_2,Point_3>(goal,Point_3(0,1,0)));
							gMeshLines.push_back(pair<Point_2,Point_3>(crossed_seg.source(),Point_3(0,0,1)));
							gMeshLines.push_back(pair<Point_2,Point_3>(crossed_seg.target(),Point_3(0,0,1)));
						#endif
					} else {

						last_ht = HeightWithinTri(inMesh, now, last_pt) ;
						intermediates.push_back(Point3(CGAL::to_double(last_pt.x()), CGAL::to_double(last_pt.y()), last_ht));
					}

				} else {
#if DEV
					printf("Ray: %lf,%lf->%lf,%lf\nSide: %lf,%lf->%lf,%lf\n",
						CGAL::to_double(ray.source().x()), CGAL::to_double(ray.source().y()),
						CGAL::to_double(ray.target().x()), CGAL::to_double(ray.target().y()),
						CGAL::to_double(crossed_seg.source().x()), CGAL::to_double(crossed_seg.source().y()),
						CGAL::to_double(crossed_seg.target().x()), CGAL::to_double(crossed_seg.target().y()));
#endif
					AssertPrintf("Intersection failed.");
				}
			}
		}
		else if (common_vertex(now, next, cross_side))
		{
			last_pt = now->vertex(cross_side)->point();
			last_ht = now->vertex(cross_side)->info().height ;
			printf("On Vertex: %lf, %lf\n", CGAL::to_double(last_pt.x()), CGAL::to_double(last_pt.y()));
			intermediates.push_back(Point3(CGAL::to_double(last_pt.x()), CGAL::to_double(last_pt.y()), last_ht));
		} else
			AssertPrintf("Cannot determine relationship between triangles!");

		// If we hit our goal dead-on, great!
		if (last_pt == goal)
		{
			march_info.locate_pt = last_pt;
			march_info.locate_height = last_ht;
			march_info.locate_face = next;
			DebugAssert(!inMesh.is_infinite(march_info.locate_face));
			DebugAssert(inMesh.triangle(march_info.locate_face).bounded_side(march_info.locate_pt) != CGAL::ON_UNBOUNDED_SIDE);
			break;
		}

/*
		// VERY STRANGE: given a simple horizontal line case, collinear_has_on is returning CRAP results.
		if (!ray.collinear_has_on(last_pt))
		{
			intermediates.pop_back();
			march_info.locate_pt = last_pt = goal;
			march_info.locate_height = last_ht = HeightWithinTri(now, goal.x(), goal.y());
			march_info.locate_face = now;
			intermediates.push_back(Point_3(last_pt.x(), last_pt.y(), last_ht));
			DebugAssert(!inMesh.is_infinite(march_info.locate_face));
			DebugAssert(inMesh.triangle(march_info.locate_face).bounded_side(march_info.locate_pt) != CGAL::ON_UNBOUNDED_SIDE);
			break;
		}
*/
		DebugAssert(circ != stop);
	}
}