/* 
 * Copyright (c) 2009, Laminar Research.
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

#include "MapRaster.h"

inline void push_vertical(double x, double y1, double y2, vector<X_monotone_curve_2>& c, int key)
{
	c.push_back(X_monotone_curve_2(Segment_2(Point_2(x,y1),Point_2(x,y2)), key));
}

inline void push_horizontal(double y, double x1, double x2, vector<X_monotone_curve_2>& c, int key)
{
	c.push_back(X_monotone_curve_2(Segment_2(Point_2(x1,y),Point_2(x2,y)),key));
}

void	MapFromDEM(
				const DEMGeo&	in_dem,
				int				x1,
				int				y1,
				int				x2,
				int				y2,
				float			null_post,
				Pmwx&			out_map)
{
	out_map.clear();
	int x, y;
	
	vector<X_monotone_curve_2>		curves;
	
	if(in_dem.mPost)
	{
		/* Vertical dividers */		
		for(y = y1; y < y2; ++y)
		{
			double y_bot = in_dem.y_to_lat_double(y-0.5);
			double y_top = in_dem.y_to_lat_double(y+0.5);
			
			if(in_dem.get(x1,y) != null_post)
				push_vertical(in_dem.x_to_lon_double(x1-0.5), y_bot, y_top, curves, null_post);

			for(x = x1+1; x < x2; ++x)
			if(in_dem.get(x-1,y) != in_dem.get(x,y))
				push_vertical(in_dem.x_to_lon_double(x-0.5), y_bot, y_top, curves, in_dem.get(x-1,y));

			if(in_dem.get(x2-1,y) != null_post)
				push_vertical(in_dem.x_to_lon_double(x2-0.5), y_bot, y_top, curves, in_dem.get(x2-1,y));
			
		}

		/* Horizontal dividers */
		for(x = x1; x < x2; ++x)
		{
			double x_lft = in_dem.x_to_lon_double(x-0.5);
			double x_rgt = in_dem.x_to_lon_double(x+0.5);
			
			if(in_dem.get(x,y1) != null_post)
				push_horizontal(in_dem.y_to_lat_double(y1-0.5), x_rgt, x_lft, curves, null_post);

			for(y = y1+1; y < y2; ++y)
			if(in_dem.get(x,y-1) != in_dem.get(x,y))
				push_horizontal(in_dem.y_to_lat_double(y-0.5), x_rgt, x_lft, curves, in_dem.get(x,y-1));

			if(in_dem.get(x,y2-1) != null_post)
				push_horizontal(in_dem.y_to_lat_double(y2-0.5), x_rgt, x_lft, curves, in_dem.get(x,y2-1));			
		}
	} 
	else
	{
		/* Vertical dividers */		
		for(y = y1; y < y2; ++y)
		{
			double y_bot = in_dem.y_to_lat_double(y  );
			double y_top = in_dem.y_to_lat_double(y+1);
			
			if(in_dem.get(x1,y) != null_post)
				push_vertical(in_dem.x_to_lon_double(x1), y_bot, y_top, curves, null_post);

			for(x = x1+1; x < x2; ++x)
			if(in_dem.get(x-1,y) != in_dem.get(x,y))
				push_vertical(in_dem.x_to_lon_double(x), y_bot, y_top, curves, in_dem.get(x-1,y));
				
			if(in_dem.get(x2-1,y) != null_post)
				push_vertical(in_dem.x_to_lon_double(x2), y_bot, y_top, curves, null_post);
				
				
		}

		/* Horizontal dividers */
		for(x = x1; x < x2; ++x)
		{
			double x_lft = in_dem.x_to_lon_double(x  );
			double x_rgt = in_dem.x_to_lon_double(x+1);
			
			if(in_dem.get(x,y1) != null_post)
				push_horizontal(in_dem.y_to_lat_double(y1), x_rgt, x_lft, curves, null_post);

			for(y = y1+1; y < y2; ++y)
			if(in_dem.get(x,y-1) != in_dem.get(x,y))
				push_horizontal(in_dem.y_to_lat_double(y), x_rgt, x_lft, curves, in_dem.get(x,y-1));

			if(in_dem.get(x,y2-1) != null_post)
				push_horizontal(in_dem.y_to_lat_double(y2), x_rgt, x_lft, curves, null_post);
		}	
	}
	
	CGAL::insert_non_intersecting_curves(out_map, curves.begin(), curves.end());
	
	for(Pmwx::Edge_iterator e = out_map.edges_begin(); e != out_map.edges_end(); ++e)
	{
		Pmwx::Halfedge_handle ee = he_get_same_direction(e);
		if(!ee->face()->is_unbounded())
		if(*(ee->curve().data().begin()) != null_post)
			ee->face()->data().mTerrainType = *(ee->curve().data().begin());
	}
	
//	CGAL::insert_curves(out_map, curves.begin(), curves.end());

//	for(Pmwx::Face_iterator f = out_map.faces_begin(); f != out_map.faces_end(); ++f)
//	if(!f->is_unbounded())
//	{
//	
///	}
}
