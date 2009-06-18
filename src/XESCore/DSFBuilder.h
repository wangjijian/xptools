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
#ifndef DSFBUILDER_H
#define DSFBUILDER_H

#include "MeshDefs.h"
#include "MapDefs.h"
#include "DEMDefs.h"
#include "ProgressUtils.h"

struct	DSFBuildPrefs_t {
	int	export_roads;
};

extern DSFBuildPrefs_t	gDSFBuildPrefs;

#if !DEV
	#error Ben needs to put this somewhere sane.
#endif
struct tex_proj_info;
void ProjectTex(double lon, double lat, double& s, double& t, const tex_proj_info * info);


void	BuildDSF(
			const char *	inFileName1,
			const char *	inFileName2,
			const DEMGeo&	inLanduse,
//			const DEMGeo&	inVege,
			CDT&			inHiresMesh,
//			CDT&			inLowresMesh,
			Pmwx&			inVectorMap,
			ProgressFunc	inProgress);

#endif /* DSFBUILDER_H */