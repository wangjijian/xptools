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

// Stuff we need to init
#include "XWidgetApp.h"
#include "XESInit.h"
#include "WED_ProcessingCmds.h"
#include "WED_FileCommands.h"
#include "WED_EditCommands.h"
#include "WED_SpecialCommands.h"
#include "WED_MapView.h"
#include "WED_PrefsDialog.h"
#include "XPLMProcessing.h"
#include "WED_Assert.h"
#include "DEMTables.h"
#include "ObjTables.h"
#include <errno.h>
#include <CGAL/assertions.h>
#include <sys/stat.h>
#include "fcntl.h"
#include "XPWidgets.h"
#include "XPWidgetDialogs.h"
#include "GISTool_Utils.h"
#include "GISTooL_ObsCmds.h"
#include "GISTool_DemCmds.h"
#include "GISTool_CoreCmds.h"
#include "GISTool_DumpCmds.h"
#include "GISTool_MiscCmds.h"
#include "GISTool_ProcessingCmds.h"
#include "GISTool_VectorCmds.h"
#include "GISTool_Globals.h"
#include "WED_Notify.h"
#include "WED_Msgs.h"


#if APL && defined(__MWERKS__)
#include "SIOUX.h"
#endif


// This stuff is only needed for the hack open .elv code.
#include "AptElev.h"
#include "WED_Globals.h"
#include "ParamDefs.h"


// This is stuff only needed to hack around with TIGER files.
#if 0
//#include <Profiler.h>
#include "TigerRead.h"
#include "TigerImport.h"
#include "TigerProcess.h"
#endif


CGAL::Failure_function	gFailure = NULL;
void	cgal_failure(const char* a, const char* b, const char* c, int d, const char* e)
{
	if (gFailure)
		(*gFailure)(a, b, c, d, e);
	throw a;
}


void	XGrindDragStart(int x, int y)
{
}

void	XGrindDragOver(int x, int y)
{
}

void	XGrindDragLeave(void)
{
}

void	XGrindFiles(const vector<string>& fileList, int x, int y)
{
	for (vector<string>::const_iterator i = fileList.begin();
		i != fileList.end(); ++i)
	{
		if (i->find(".elv") != string::npos)		
		{
			BuildDifferentialDegree(i->c_str(), gDem[dem_Elevation].mWest, gDem[dem_Elevation].mSouth, 1201, 1201, gDem[dem_Elevation], false);
			BuildDifferentialDegree(i->c_str(), gDem[dem_Elevation].mWest, gDem[dem_Elevation].mSouth, 241, 241, gDem[dem_UrbanDensity], true);
			continue;
		}

		WED_FileOpen(*i);
	}
}

#if 0
void	import_tiger_repository(const string& rt)
{
	string root(rt);
	int	fnum = -1;
	if ((root.length() > 3) &&
		(root.substr(root.length()-4)==".RT1" ||
		 root.substr(root.length()-4)==".rt1"))
	{
		root = root.substr(0, root.length()-4);
		fnum = atoi(root.c_str() + root.length() - 5);
		root.erase(root.rfind('/'));
	}
	if ((root.length() > 3) &&
		(root.substr(root.length()-4)==".zip" ||
		 root.substr(root.length()-4)==".ZIP"))
	{
		fnum = atoi(root.c_str() + root.length() - 9);
	}
	
	if (fnum == -1)
	{
		printf("Could not identify file %s as a TIGER file.\n", root.c_str());
	} else {

		MFFileSet * fs = FileSet_Open(root.c_str());
		if (fs)
		{	
			printf("Reading %s/TGR%05d.RT1\n", root.c_str(), fnum);
			TIGER_LoadRT1(fs, fnum);
			printf("Reading %s/TGR%05d.RT2\n", root.c_str(), fnum);
			TIGER_LoadRT2(fs, fnum);
			printf("Reading %s/TGR%05d.RTP\n", root.c_str(), fnum);
			TIGER_LoadRTP(fs, fnum);
			printf("Reading %s/TGR%05d.RTI\n", root.c_str(), fnum);
			TIGER_LoadRTI(fs, fnum);
			printf("Reading %s/TGR%05d.RT7\n", root.c_str(), fnum);
			TIGER_LoadRT7(fs, fnum);
			printf("Reading %s/TGR%05d.RT8\n", root.c_str(), fnum);
			TIGER_LoadRT8(fs, fnum);
							
			FileSet_Close(fs);
		} else 
			printf("Could not open %s as a file set.\n", root.c_str());
	}
}
#endif




static int DoSelfTest(const vector<const char *>& args)		{	/*SelfTestAll();*/ 	return 0; 	}
static int DoVerbose(const vector<const char *>& args)		{	gVerbose = 1;	return 0;	}
static int DoQuiet(const vector<const char *>& args)		{	gVerbose = 0;	return 0;	}
static int DoTiming(const vector<const char *>& args)		{	gTiming = 1;	return 0;	}
static int DoNoTiming(const vector<const char *>& args)		{	gTiming = 0;	return 0;	}
static int DoProgress(const vector<const char *>& args)		{	/*gProgress = ConsoleProgressFunc;	*/return 0;	}
static int DoNoProgress(const vector<const char *>& args)	{	/*gProgress = NULL;					*/return 0;	}

static	GISTool_RegCmd_t		sUtilCmds[] = {
//{ "-help",			0, 1, DoHelp, "Prints help info for a command.", "" },
{ "-verbose",		0, 0, DoVerbose, "Enables loggging messages.", "" },
{ "-quiet",			0, 0, DoQuiet, "Disables logging messages.", "" },
{ "-timing",		0, 0, DoTiming, "Enables performance timing.", "" },
{ "-notiming",		0, 0, DoNoTiming, "Disables performance timing.", "" },
{ "-progress",		0, 0, DoProgress, "Shows progress bars", "" },
{ "-noprogress",	0, 0, DoNoProgress, "Disables progress bars", "" },
{ "-selftest",		0, 0, DoSelfTest, "Self test internal algorithms.", "" },
#if USE_CHUD
{ "-chud_start",	1, 1, DoChudStart, "Start profiling", "" },
{ "-chud_stop",		0, 0, DoChudStop, "stop profiling", "" },
#endif
{ 0, 0, 0, 0, 0, 0 }
};





static int fifo = NULL;

float CheckFifo(
                                   float                inElapsedSinceLastCall,    
                                   float                inElapsedTimeSinceLastFlightLoop,    
                                   int                  inCounter,    
                                   void *               inRefcon)
{
	static vector<char> data;
	fd_set rd, wr, er;
	FD_ZERO(&rd);
	FD_ZERO(&wr);
	FD_ZERO(&er);
	FD_SET(fifo,&rd);
	FD_SET(fifo,&er);
	struct timeval to = { 0, 0 };
	int res = select(fifo+1,&rd,&wr,&er, &to);
	if (res > 0)
	{
		if(FD_ISSET(fifo,&rd))
		{
			char buf[1024];
			int nread = read(fifo,buf,sizeof(buf)-1);
			if(nread > 0)
			{
				data.insert(data.end(),buf,buf+nread);
			}
			
		} else {
			printf("Got discon.\n");
		}
		
		for(int n = 0; n < data.size();++n)
		if(data[n]=='\r' || data[n] == '\n')
		{
			data[n] = 0;
			vector<const char*>	args;
			char * str = &*data.begin();
			char * sep = "\r\n \t";
			while(1)
			{
				char * tok = strtok(str,sep);
				str = NULL;
				if(tok == NULL)
					break;
				args.push_back(tok);
			}
			printf("Eval:\n");
			for (int nn = 0; nn < args.size(); ++nn)
				printf("  %d) '%s'\n", nn,args[nn]);
			printf("Going...\n");
			GISTool_ParseCommands(args);
			data.erase(data.begin(),data.begin()+n+1);
			n = 0;
		}
			
		WED_Notifiable::Notify(wed_Cat_File, wed_Msg_RasterChange, NULL);
		WED_Notifiable::Notify(wed_Cat_File, wed_Msg_VectorChange, NULL);
		WED_Notifiable::Notify(wed_Cat_File, wed_Msg_TriangleHiChange, NULL);
		WED_Notifiable::Notify(wed_Cat_File, wed_Msg_AirportsLoaded, NULL);

		
	}
	return -1;
}



void	XGrindInit(string& outName)
{
	#if APL
	int e = mkfifo("wed_cmds", 0777);
	if(e==-1 && errno != EEXIST)
	{
		fprintf(stderr,"WARNING: unable to make fifo: %d.\n",errno);
	} else {
		fifo=open("wed_cmds",O_RDONLY | O_NONBLOCK, 0);
	}
	#endif
	
	XPLMRegisterFlightLoopCallback(CheckFifo,-1, NULL);

#if APL && defined(__MWERKS__)
//	SIOUXSettings.stubmode = true;
	SIOUXSettings.standalone = false;
	SIOUXSettings.setupmenus = false;
	SIOUXSettings.autocloseonquit = false;
	SIOUXSettings.asktosaveonclose = false;
#endif
	gFailure = CGAL::set_error_handler(cgal_failure);
	XESInit();

	WED_LoadPrefs();
	
	LoadDEMTables();
	LoadObjTables();
	
	int w, h;
	XPLMGetScreenSize(&w, &h);
	RegisterFileCommands();
	RegisterEditCommands();
	WED_MapView *	map_view = new WED_MapView(20, h - 20, w - 20, 20, 1, NULL);
	RegisterProcessingCommands();
	RegisterSpecialCommands();
	
	WED_AssertInit();
	
	XPInitDefaultMargins();
#if 0
	XPWidgetID	foo = XPCreateWidgetLayout(0, XP_DIALOG_BOX, "Title", XP_DIALOG_CLOSEBOX, 1, 0, NULL,
									XP_TABS, "Tab 1;Tab 2;Tab 3", NULL,
											XP_COLUMN,
												XP_CAPTION, "This is a very nice long caption that I made.",
												XP_ROW, XP_POPUP_MENU, "Item a; item B;huge item C is long", NULL, XP_END,
												XP_ROW, XP_CHECKBOX, "Check this", NULL, XP_END,
												XP_ROW, XP_CHECKBOX, "Or Check this", NULL, XP_END,
											XP_END,
											XP_COLUMN,
												XP_ROW,
													XP_CAPTION, "String:",
													XP_EDIT_STRING, XP_EDIT_PASSWORD, 15, 6, NULL, 
												XP_END,
												XP_ROW,
													XP_CAPTION, "Int:",
													XP_EDIT_INT, 6, 6, NULL,
												XP_END,
												XP_ROW,
													XP_CAPTION, "Float:",
													XP_EDIT_FLOAT_, 8, 6, NULL, 
												XP_END,
												XP_ROW,														
													XP_BUTTON_ACTION, "Test 1", NULL,
													XP_BUTTON_ACTION, "Test 2 Very Long Dude", NULL,
												XP_END,
												XP_ROW,
													XP_BUTTON_OK, "OK", 
													XP_BUTTON_CANCEL, "CANCEL",
												XP_END,
											XP_END,
											XP_COLUMN,
												XP_RADIOBUTTON, "Radio Button 1", NULL, 1,
												XP_RADIOBUTTON, "Radio Button 2", NULL, 2,
												XP_RADIOBUTTON, "Radio Button 3", NULL, 3,
												XP_RADIOBUTTON, "Radio Button 4", NULL, 4,
											XP_END,
										XP_END, XP_END);
												
	XPShowWidget(foo);
	XPBringRootWidgetToFront(foo);	
#endif
#if 0	
//////
		TigerMap	tigerMap;

			tigerMap.clear();
			ReadTigerIndex("GIS:data:tiger:tiger_index.txt", tigerMap);
			if (tigerMap.empty())
				printf("Could not open tiger index.\n");
			else {
//				StProfile	profRead("\pProfileRead");
				int	hashNum = 42 * 360 + -72;
				FileList& fl = tigerMap[hashNum];
				if (fl.empty())
					printf("No tiger files available for %d,%d\n", 42, -72);
				string	partial("GIS:data:tiger:");
//				string::size_type div = partial.rfind('/');
//				if (div != partial.npos)
//					partial.erase(div+1);
				for (int n = 0; n < fl.size(); ++n)
				{
					string	full = partial + fl[n].name;
					for (int c = 0; c < full.size(); ++c)
						if (full[c] == '/') full[c] = ':';
					import_tiger_repository(full);
				}
			}
			
			{
//				StProfile	profRead("\pRoughCull");				
				TIGER_RoughCull(-72, 42, -71, 43);
			}
			printf("Sorting...\n");
			{
				StElapsedTime	timer("Sorting");
//				StProfile	postProf("\pPostSort");				
				TIGER_PostProcess(gMap);
			}
			
			printf("Read: %d chains, %d landmarks, %d polygons.\n",
				gChains.size(), gLandmarks.size(), gPolygons.size());

			{
//				StProfile	importProf("\pImport");				
				StElapsedTime	timer("Importing");
				TIGERImport(gChains, gLandmarks, gPolygons, gMap, ConsoleProgressFunc);
			}

			printf("Map contains: %d faces, %d half edges, %d vertices.\n",
				gMap.number_of_faces(),
				gMap.number_of_halfedges(),
				gMap.number_of_vertices());				
#endif	

		GISTool_RegisterCommands(sUtilCmds);

		RegisterDemCmds();
		RegisterCoreCmds();
		RegisterDumpCmds();
		RegisterVectorCmds();
		RegisterProcessingCmds();
		RegisterObsCmds();
		RegisterMiscCmds();
		

}


bool	XGrindCanQuit(void)
{
	return true;
}

void	XGrindDone(void)
{
	WED_SavePrefs();
	if(fifo) close(fifo);
	unlink("wed_cmds");
}