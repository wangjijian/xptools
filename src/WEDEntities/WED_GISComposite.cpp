/*
 * Copyright (c) 2007, Laminar Research.
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

#include "WED_GISComposite.h"

TRIVIAL_COPY(WED_GISComposite, WED_Entity)

WED_GISComposite::WED_GISComposite(WED_Archive * a, int i) : WED_Entity(a,i)
{
}

WED_GISComposite::~WED_GISComposite()
{
}

GISClass_t		WED_GISComposite::GetGISClass		(void				 ) const
{
	return gis_Composite;
}

const char *	WED_GISComposite::GetGISSubtype	(void				 ) const
{
	return GetClass();
}

bool			WED_GISComposite::HasUV			(void				 ) const
{
	return GetClass();
}


void			WED_GISComposite::GetBounds		(	   Bbox2&  bounds) const
{
	if (CacheBuild())	RebuildCache();
	bounds = mCacheBounds;
}

bool			WED_GISComposite::IntersectsBox	(const Bbox2&  bounds) const
{
	Bbox2	me;
	GetBounds(me);
	if (!bounds.overlap(me)) return false;

	int n = GetNumEntities();
	for (int i = 0; i < n; ++i)
		if (GetNthEntity(i)->IntersectsBox(bounds)) return true;
	return false;
}

bool			WED_GISComposite::WithinBox		(const Bbox2&  bounds) const
{
	Bbox2	me;
	GetBounds(me);
	if (bounds.contains(me)) return true;

	int n = GetNumEntities();
	for (int i = 0; i < n; ++i)
		if (!GetNthEntity(i)->WithinBox(bounds)) return false;
	return (n > 0);
}

bool			WED_GISComposite::PtWithin		(const Point2& p	 ) const
{
	Bbox2	me;
	GetBounds(me);
	if (!me.contains(p)) return false;

	int n = GetNumEntities();
	for (int i = 0; i < n; ++i)
		if (GetNthEntity(i)->PtWithin(p)) return true;
	return false;
}

bool			WED_GISComposite::PtOnFrame		(const Point2& p, double d) const
{
	Bbox2	me;
	GetBounds(me);
	me.p1 -= Vector2(d,d);
	me.p2 += Vector2(d,d);
	if (!me.contains(p)) return false;

	int n = GetNumEntities();
	for (int i = 0; i < n; ++i)
		if (GetNthEntity(i)->PtOnFrame(p, d)) return true;
	return false;
}

void			WED_GISComposite::Rescale(const Bbox2& old_bounds,const Bbox2& new_bounds)
{
	int n = GetNumEntities();
	for (int i = 0; i < n; ++i)
		GetNthEntity(i)->Rescale(old_bounds,new_bounds);
}

void			WED_GISComposite::Rotate(const Point2& ctr, double angle)
{
	int n = GetNumEntities();
	for (int i = 0; i < n; ++i)
		GetNthEntity(i)->Rotate(ctr, angle);
}

int				WED_GISComposite::GetNumEntities(void ) const
{
	if (CacheBuild())	RebuildCache();
	return mEntities.size();
}

IGISEntity *	WED_GISComposite::GetNthEntity  (int n) const
{
	if (CacheBuild())	RebuildCache();
	return mEntities[n];
}


void	WED_GISComposite::RebuildCache(void) const
{
	mCacheBounds = Bbox2();
	mEntities.clear();
	int n = CountChildren();
	mEntities.reserve(n);
	for (int i = 0; i <  n; ++i)
	{
		IGISEntity * ent = dynamic_cast<IGISEntity *>(GetNthChild(i));
		if (ent)
		{
			Bbox2 child;
			ent->GetBounds(child);
			mCacheBounds += child;
			mEntities.push_back(ent);
		}
	}
}