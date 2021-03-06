/*
 * x2x: Uses the XTEST extension to forward keystrokes from a window on
 *      one display to another display.  Useful for desks
 *      with multiple keyboards.
 *
 * Copyright (c) 1997
 * Digital Equipment Corporation.  All rights reserved.
 * 
 * By downloading, installing, using, modifying or distributing this
 * software, you agree to the following:
 * 
 * 1. CONDITIONS. Subject to the following conditions, you may download,
 * install, use, modify and distribute this software in source and binary
 * forms:
 * 
 * a) Any source code, binary code and associated documentation
 * (including the online manual) used, modified or distributed must
 * reproduce and retain the above copyright notice, this list of
 * conditions and the following disclaimer.
 * 
 * b) No right is granted to use any trade name, trademark or logo of
 * Digital Equipment Corporation.  Neither the "Digital Equipment
 * Corporation" name nor any trademark or logo of Digital Equipment
 * Corporation may be used to endorse or promote products derived from
 * this software without the prior written permission of Digital
 * Equipment Corporation.
 * 
 * 2.  DISCLAIMER.  THIS SOFTWARE IS PROVIDED BY DIGITAL "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.IN NO EVENT SHALL DIGITAL BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/Xatom.h> /* for selection */
#include <X11/XKBlib.h> /* XkbKeycodeToKeysym */
#include <sys/types.h> /* for select */
#include <sys/time.h> /* for select */
#include <unistd.h> /* for sleep() */
#include "format.h"

/*#define DEBUG*/

/* Hack for solaris 2008/12/15 ADT */
#ifdef __stderrp
#define __stderrp stderr
#endif

/**********
 * definitions for edge
 **********/
#define EDGE_NONE   0 /* don't transfer between edges of screens */
#define EDGE_EAST   1 /* from display is on the east side of to display */
#define EDGE_WEST   2 /* from display is on the west side of to display */
#define EDGE_NORTH	3 /* from display is on the north side of to display */
#define EDGE_SOUTH	4 /* from display is on the west side of to display */

/**********
 * functions
 **********/
static void    ParseCommandLine();
static Display *OpenAndCheckDisplay();
static Bool    CheckTestExtension();
static void    DoX2X();
static void    InitDpyInfo();
static void    DoConnect();
static void    DoDisconnect();
static void    RegisterEventHandlers();
static Bool    ProcessEvent();
static Bool    ProcessMotionNotify();
static Bool    ProcessExpose();
static Bool    ProcessEnterNotify();
static Bool    ProcessButtonPress();
static Bool    ProcessButtonRelease();
static Bool    ProcessKeyEvent();
static Bool    ProcessConfigureNotify();
static Bool    ProcessClientMessage();
static Bool    ProcessSelectionRequest();
static void    SendPing();
static Bool    ProcessPropertyNotify();
static Bool    ProcessSelectionNotify();
static void    SendSelectionNotify();
static Bool    ProcessSelectionClear();
static Bool    ProcessVisibility();
static Bool    ProcessMapping();
static void    FakeThingsUp();
static void    FakeAction();
static void    RefreshPointerMapping();
static void    Usage();

/**********
 * text formatting instructions
 **********/
#define toDpyFormatLength (sizeof(toDpyFormat) / sizeof(Format))
static Format toDpyFormat[] = {
		FormatMeasureText,
		FormatSetLeft,      0,
		FormatSetTop,       0,
		FormatAddHalfTextX, 1,
		FormatAddHalfTextY, 3,
		FormatString, (Format)"unknown",
		FormatAddHalfTextX, 1,
		FormatAddHalfTextY, 1
};
/* indexes of values to be filled in at runtime */
#define toDpyLeftIndex    2
#define toDpyTopIndex     4
#define toDpyStringIndex 10 

/**********
 * stuff for selection forwarding
 **********/
typedef struct _dpyxtra {
		Display *otherDpy;
		int  sState;
		Atom pingAtom;
		Bool pingInProg;
		Window propWin;
} DPYXTRA, *PDPYXTRA;

/**********
 * structures for recording state of buttons and keys
 **********/
typedef struct _fakestr {
		struct _fakestr *pNext;
		int type;
		unsigned int thing;
} FAKE, *PFAKE;

#define FAKE_KEY    0
#define FAKE_BUTTON 1

#define MAX_BUTTONS 20

#define GETDPYXTRA(DPY,PDPYINFO)\
		(((DPY) == (PDPYINFO)->fromDpy) ?\
		 &((PDPYINFO)->fromDpyXtra) : &((PDPYINFO)->toDpyXtra))

/* values for sState */
#define SELSTATE_ON     0
#define SELSTATE_OFF    1
#define SELSTATE_WAIT   2

/* special values for translated coordinates */
#define COORD_INCR     -1
#define COORD_DECR     -2
#define SPECIAL_COORD(COORD) (((COORD) < 0) ? (COORD) : 0)

/**********
 * display information
 **********/
typedef struct {
		/* stuff on "from" display */
		Display *fromDpy;
		Window  root;
		Window  trigger;
		Window  big;
		GC      textGC;
		Atom    wmpAtom, wmdwAtom;
		Cursor  grabCursor;
		XFS     *font;
		int     twidth, theight;
		int     lastFromX;
		int     lastFromY;
		int     unreasonableDelta;

		/* stuff on "to" display */
		Display *toDpy;
		Window  selWin;
		unsigned int buttonCount;
		unsigned int inverseMap[MAX_BUTTONS]; /* inverse of button mapping */

		/* state of connection */
		int     mode;			/* connection */
		int     eventMask;		/* trigger */

		/* coordinate conversion stuff */
		int     toScreen;
		int     nScreens;
		short   **xTables; /* precalculated conversion tables */
		short   **yTables;
		int     fromXConn, fromXDisc; /* location of cursor after conn/disc ops */
		int     fromYConn, fromYDisc; /* location of cursor after conn/disc ops */
		int     fromXIncr, fromXDecr; /* location of cursor after incr/decr ops */
		int     fromYIncr, fromYDecr; /* location of cursor after incr/decr ops */

		/* selection forwarding info */
		DPYXTRA fromDpyXtra;
		DPYXTRA toDpyXtra;
		Display *sDpy;
		XSelectionRequestEvent sEv;
		Time    sTime;

		/* for recording state of buttons and keys */
		PFAKE   pFakeThings;
		
} DPYINFO, *PDPYINFO;

/* shadow displays */
typedef struct _shadow {
		struct _shadow *pNext;
		char    *name;
		Display *dpy;
} SHADOW, *PSHADOW;

/* sticky keys */
typedef struct _sticky {
		struct _sticky *pNext;
		KeySym keysym;
} STICKY, *PSTICKY;

typedef int  (*HANDLER)(); /* event handler function */

/**********
 * top-level variables
 **********/
static char    *programStr = "x2x";
static char    *fromDpyName = NULL;
static char    *toDpyName   = NULL;
static char    *defaultFN   = "-*-times-bold-r-*-*-*-180-*-*-*-*-*-*";
static char    *fontName    = "-*-times-bold-r-*-*-*-180-*-*-*-*-*-*";
static char    *pingStr     = "PING"; /* atom for ping request */
static char    *geomStr     = NULL;
static Bool    waitDpy      = False;
static Bool    doBig        = False;
static Bool    doMouse      = True;
static int     doEdge       = EDGE_NONE;
static Bool    doSel        = True;
static Bool    doAutoUp     = True;
static Bool    doResurface  = False;
static PSHADOW shadows      = NULL;
static int     triggerw     = 2;
static Bool    doPointerMap = True;
static PSTICKY stickies     = NULL;
static Bool    doBtnBlock   = False;

/**********
 * main
 **********/
int main(int argc, char **argv)
{
		Display *fromDpy;
		PSHADOW pShadow;

		XrmInitialize();
		ParseCommandLine(argc, argv);

		fromDpyName = XDisplayName(fromDpyName);
		toDpyName   = XDisplayName(toDpyName);
		if (!strcasecmp(toDpyName, fromDpyName)) {
				if (strlen(toDpyName))
					fprintf(stderr, "%s: display names are both %s\n", programStr, toDpyName);
				else
					fprintf(stderr, "%s: display names are both empty\n", programStr);
				exit(1);
		}

		/* no OS independent wat to stop Xlib from complaining via stderr,
		   but can always pipe stdout/stderr to /dev/null */
		/* convert to real name: */
		while ((fromDpy = XOpenDisplay(fromDpyName)) == NULL) {
				if (!waitDpy) {
						fprintf(stderr, "%s - error: can not open display %s\n", programStr, fromDpyName);
						exit(2);
				} /* END if */
				sleep(10);
		} /* END while fromDpy */

		/* toDpy is always the first shadow */
		pShadow = (PSHADOW)malloc(sizeof(SHADOW));
		pShadow->name = toDpyName;
		/* link into the global list */
		pShadow->pNext = shadows;
		shadows = pShadow;

		/* initialize all of the shadows, including the toDpy */
		for (pShadow = shadows; pShadow; pShadow = pShadow->pNext)
				if (!(pShadow->dpy = OpenAndCheckDisplay(pShadow->name)))
						exit(3);

		/* run the x2x loop */
		DoX2X(fromDpy, shadows->dpy);

		/* shut down gracefully */
		XCloseDisplay(fromDpy);
		for (pShadow = shadows; pShadow; pShadow = pShadow->pNext)
				XCloseDisplay(pShadow->dpy);
		exit(0);

} /* END main */

static Display *OpenAndCheckDisplay(char* name)
{
		Display *openDpy;

		/* convert to real name: */
		name = XDisplayName(name);
		while ((openDpy = XOpenDisplay(name)) == NULL) {
				if (!waitDpy) {
						fprintf(stderr, "%s - error: can not open display %s\n", 
										programStr, name);
						return NULL;
				} /* END if */
				sleep(10);
		} /* END while openDpy */

		if (!CheckTestExtension(openDpy)) {
				fprintf(stderr,
								"%s - error: display %s does not support the test extension\n",
								programStr, name);
				return NULL;
		}
		return (openDpy);

} /* END OpenAndCheckDisplay */

/**********
 * use standard X functions to parse the command line
 **********/
static void ParseCommandLine(argc, argv)
		int  argc;
		char **argv;
{
		int     arg;
		PSHADOW pShadow;
		extern  char *lawyerese;
		PSTICKY pNewSticky;
		KeySym  keysym;

#ifdef DEBUG
		printf ("programStr = %s\n", programStr);
#endif  

		for (arg = 1; arg < argc; ++arg) {
				if (!strcasecmp(argv[arg], "-from")) {
						if (++arg >= argc) Usage();
						fromDpyName = argv[arg];

#ifdef DEBUG
						printf ("fromDpyName = %s\n", fromDpyName);
#endif  
				} else if (!strcasecmp(argv[arg], "-to")) {
						if (++arg >= argc) Usage();
						toDpyName = argv[arg];

#ifdef DEBUG
						printf ("toDpyName = %s\n", toDpyName);
#endif
				} else if (!strcasecmp(argv[arg], "-font")) {
						if (++arg >= argc) Usage();
						fontName = argv[arg];

#ifdef DEBUG
						printf ("fontName = %s\n", fontName);
#endif
				} else if (!strcasecmp(argv[arg], "-geometry")) {
						if (++arg >= argc) Usage();
						geomStr = argv[arg];

#ifdef DEBUG
						printf ("geometry = %s\n", geomStr);
#endif
				} else if (!strcasecmp(argv[arg], "-wait")) {
						waitDpy = True;

#ifdef DEBUG
						printf("will wait for displays\n");
#endif
				} else if (!strcasecmp(argv[arg], "-big")) {
						doBig = True;

#ifdef DEBUG
						printf("will create big window on from display\n");
#endif
				} else if (!strcasecmp(argv[arg], "-nomouse")) {
						doMouse = False;

#ifdef DEBUG
						printf("will not capture mouse (eek!)\n");
#endif
				} else if (!strcasecmp(argv[arg], "-nopointermap")) {
						doPointerMap = False;

#ifdef DEBUG
						printf("will not do pointer mapping\n");
#endif
				} else if (!strcasecmp(argv[arg], "-east")) {
						doEdge = EDGE_EAST;
#ifdef DEBUG
						printf("\"from\" is on the east side of \"to\"\n");
#endif
				} else if (!strcasecmp(argv[arg], "-west")) {
						doEdge = EDGE_WEST;
#ifdef DEBUG
						printf("\"from\" is on the west side of \"to\"\n");
#endif
				} else if (!strcasecmp(argv[arg], "-north")) {
						doEdge = EDGE_NORTH;
#ifdef DEBUG
						printf("\"from\" is on the north side of \"to\"\n");
#endif
				} else if (!strcasecmp(argv[arg], "-south")) {
						doEdge = EDGE_SOUTH;
#ifdef DEBUG
						printf("\"from\" is on the south side of \"to\"\n");
#endif
				} else if (!strcasecmp(argv[arg], "-nosel")) {
						doSel = False;
#ifdef DEBUG
						printf("will not transmit X selections between displays\n");
#endif
				} else if (!strcasecmp(argv[arg], "-noautoup")) {
						doAutoUp = False;
#ifdef DEBUG
						printf("will not automatically lift keys and buttons\n");
#endif
				} else if (!strcasecmp(argv[arg], "-buttonblock")) {
						doBtnBlock = True;
#ifdef DEBUG
						printf("mouse buttons down will block disconnects\n");
#endif
				} else if (!strcasecmp(argv[arg], "-sticky")) {
						if (++arg >= argc) Usage();
						if ((keysym = XStringToKeysym(argv[arg])) != NoSymbol) {
								pNewSticky = (PSTICKY)malloc(sizeof(STICKY));
								pNewSticky->pNext  = stickies;
								pNewSticky->keysym = keysym;
								stickies = pNewSticky;
#ifdef DEBUG
								printf("will press/release sticky key: %s\n", argv[arg]);
#endif
						} else {
								printf("x2x: warning: can't translate %s\n", argv[arg]);
						}
				} else if (!strcasecmp(argv[arg], "-resurface")) {
						doResurface = True;
#ifdef DEBUG
						printf("will resurface the trigger window when obscured\n");
#endif
				} else if (!strcasecmp(argv[arg], "-shadow")) {
						if (++arg >= argc) Usage();
						pShadow = (PSHADOW)malloc(sizeof(SHADOW));
						pShadow->name = argv[arg];

						/* into the global list of shadows */
						pShadow->pNext = shadows;
						shadows = pShadow;

				} else if (!strcasecmp(argv[arg], "-triggerw")) {
						if (++arg >= argc) Usage();
						triggerw = atoi(argv[arg]);
				} else if (!strcasecmp(argv[arg], "-copyright")) {
						printf(lawyerese);
				} else {
						Usage();
				} /* END if... */
		} /* END for */

} /* END ParseCommandLine */

static void Usage()
{
		printf("Usage: x2x [-to <DISPLAY> | -from <DISPLAY>] options...\n");
		printf("       -copyright\n");
		printf("       -font <FONTNAME>\n");
		printf("       -geometry <GEOMETRY>\n");
		printf("       -wait\n");
		printf("       -big\n");
		printf("       -buttonblock\n");
		printf("       -nomouse\n");
		printf("       -east\n");
		printf("       -west\n");
		printf("       -nosel\n");
		printf("       -noautoup\n");
		printf("       -resurface\n");
		printf("       -shadow <DISPLAY>\n");
		printf("       -sticky <sticky key>\n");
		exit(4);

} /* END Usage */

/**********
 * call the library to check for the test extension
 **********/
static Bool CheckTestExtension(Display* dpy)
{
		int eventb, errorb;
		int vmajor, vminor;

		return (XTestQueryExtension(dpy, &eventb, &errorb, &vmajor, &vminor));

} /* END CheckTestExtension */

#define X2X_DISCONNECTED    0
#define X2X_AWAIT_RELEASE   1
#define X2X_CONNECTED       2
#define X2X_CONN_RELEASE    3

static void DoX2X(Display* fromDpy, Display* toDpy)
{
		DPYINFO   dpyInfo;
		int       nfds;
		fd_set    *fdset;
		Bool      fromPending;
		int       fromConn, toConn;

		/* set up displays */
		dpyInfo.fromDpy = fromDpy;
		dpyInfo.toDpy = toDpy;
		InitDpyInfo(&dpyInfo);
		RegisterEventHandlers(&dpyInfo);

		/* set up for select */
		nfds = getdtablesize();
		fdset = (fd_set *)malloc(sizeof(fd_set));
		fromConn = XConnectionNumber(fromDpy);
		toConn   = XConnectionNumber(toDpy);

		while (True) { /* FOREVER */
				if ((fromPending = XPending(fromDpy)))
						if (ProcessEvent(fromDpy, &dpyInfo)) /* done! */
								break;

				if (XPending(toDpy)) {
						if (ProcessEvent(toDpy, &dpyInfo)) /* done! */
								break; 
				} else if (!fromPending) {
						FD_ZERO(fdset);
						FD_SET(fromConn, fdset);
						FD_SET(toConn, fdset);
						select(nfds, fdset, NULL, NULL, NULL);
				}

		} /* END FOREVER */

		free(fdset);

} /* END DoX2X() */

static void InitDpyInfo(PDPYINFO pDpyInfo)
{
		Display   *fromDpy, *toDpy;
		Screen    *fromScreen, *toScreen;
		long      black, white;
		int       fromHeight, fromWidth, toHeight, toWidth;
		Pixmap    nullPixmap;
		XColor    dummyColor;
		Window    root, big, rret, toRoot, propWin;
		Window    trigger = (Window) 0;
		short     *xTable, *yTable; /* short: what about dimensions > 2^15? */
		int       *heights, *widths;
		int       counter;
		int       nScreens, screenNum;
		int       twidth, theight; /* text dimensions */
		int       xoff, yoff; /* window offsets */
		unsigned int width, height; /* window width, height */
		int       geomMask;		/* mask returned by parse */
		int       gravMask;
		int       gravity = NorthWestGravity;
		int       xret, yret;
		unsigned int wret, hret, bret, dret;
		XSetWindowAttributes xswa;
		XSizeHints *xsh;
		int       eventMask;
		GC        textGC;
		char      *windowName;  
		XFS       *font;
		PSHADOW   pShadow;
		int       triggerLoc = 0;

		/* cache commonly used variables */
		fromDpy = pDpyInfo->fromDpy;
		toDpy   = pDpyInfo->toDpy;

		fromScreen = XDefaultScreenOfDisplay(fromDpy);
		black      = XBlackPixelOfScreen(fromScreen);
		white      = XWhitePixelOfScreen(fromScreen);
		fromHeight = XHeightOfScreen(fromScreen);
		fromWidth  = XWidthOfScreen(fromScreen);
		toRoot     = XDefaultRootWindow(toDpy); 

		/* values also in dpyinfo */
		root       = pDpyInfo->root      = XDefaultRootWindow(fromDpy); 
		nScreens   = pDpyInfo->nScreens  = XScreenCount(toDpy);

		/* other dpyinfo values */
		pDpyInfo->mode        = X2X_DISCONNECTED;
		pDpyInfo->unreasonableDelta = fromWidth / 2;
		pDpyInfo->pFakeThings = NULL;

		/* window init structures */
		xswa.override_redirect = True;
		xsh = XAllocSizeHints();
		eventMask = KeyPressMask | KeyReleaseMask;

		/* cursor locations for moving between screens */
		pDpyInfo->fromXIncr = triggerw;
		pDpyInfo->fromYIncr = triggerw;
		pDpyInfo->fromXDecr = fromWidth - triggerw - 1;
		pDpyInfo->fromYDecr = fromHeight - triggerw - 1;
		if (doEdge) { /* edge triggers x2x */
				nullPixmap = XCreatePixmap(fromDpy, root, 1, 1, 1);
				eventMask |= EnterWindowMask;
				pDpyInfo->grabCursor = 
						XCreatePixmapCursor(fromDpy, nullPixmap, nullPixmap,
										&dummyColor, &dummyColor, 0, 0);
				if (doEdge == EDGE_EAST) {
						/* trigger window location */
						triggerLoc = fromWidth - triggerw;
						toHeight = XHeightOfScreen(XScreenOfDisplay(toDpy, nScreens - 1));
						toWidth = 0;
						pDpyInfo->fromXConn = triggerw;
						pDpyInfo->fromXDisc = fromWidth - triggerw - 1;
				} else if (doEdge == EDGE_WEST) {
						/* trigger window location */
						triggerLoc = 0;
						toHeight = XHeightOfScreen(XScreenOfDisplay(toDpy, nScreens - 1));
						toWidth  = XWidthOfScreen(XScreenOfDisplay(toDpy, nScreens - 1));
						pDpyInfo->fromXConn = fromWidth - triggerw - 1;
						pDpyInfo->fromXDisc = triggerw;
				} else if (doEdge == EDGE_NORTH) {
						/* trigger window location */
						triggerLoc = 0;
						toHeight = XHeightOfScreen(XScreenOfDisplay(toDpy, nScreens - 1));
						toWidth  = XWidthOfScreen(XScreenOfDisplay(toDpy, nScreens - 1));
						pDpyInfo->fromYConn = fromHeight - triggerw - 1;
						pDpyInfo->fromYDisc = triggerw;
				} else if (doEdge == EDGE_SOUTH) {
						/* trigger window location */
						triggerLoc = fromHeight - triggerw;
						toHeight = 0;
						toWidth  = XWidthOfScreen(XScreenOfDisplay(toDpy, nScreens - 1));
						pDpyInfo->fromYConn = 1;
						pDpyInfo->fromYDisc = fromHeight - triggerw - 1;
				} /* END if doEdge == ... */

				xswa.background_pixel = black;
				/* fromWidth - 1 doesn't seem to work for some reason */
				if ((doEdge == EDGE_WEST) || (doEdge == EDGE_EAST)) {
						trigger = pDpyInfo->trigger = 
								XCreateWindow(fromDpy, root, triggerLoc, 0, triggerw, fromHeight,
												0, 0, InputOutput, 0, 
												CWBackPixel | CWOverrideRedirect, &xswa);
				} else if ((doEdge == EDGE_NORTH) || (doEdge == EDGE_SOUTH)) {
						trigger = pDpyInfo->trigger = 
								XCreateWindow(fromDpy, root, 0, triggerLoc, fromWidth, triggerw,
												0, 0, InputOutput, 0, 
												CWBackPixel | CWOverrideRedirect, &xswa);
				}
				font = NULL;

		} else { /* normal window for text: do size grovelling */
				pDpyInfo->grabCursor = XCreateFontCursor(fromDpy, XC_exchange);
				eventMask |= StructureNotifyMask | ExposureMask;
				if (doMouse) eventMask |= ButtonPressMask | ButtonReleaseMask;

				/* determine size of text */
				if (((font = XLoadQueryFont(fromDpy, fontName)) != NULL) ||
								((font = XLoadQueryFont(fromDpy, defaultFN)) != NULL) ||
								((font = XLoadQueryFont(fromDpy, "fixed")) != NULL)) { 
						/* have a font */
						toDpyFormat[toDpyStringIndex] = (Format)toDpyName;
						formatText(NULL, NULL, NULL, font, 
										toDpyFormatLength, toDpyFormat, &twidth, &theight);

						textGC = pDpyInfo->textGC = XCreateGC(fromDpy, root, 0, NULL);
						XSetState(fromDpy, textGC, black, white, GXcopy, AllPlanes);
						XSetFont(fromDpy, textGC, font->fid);

				} else { /* should not have to execute this clause: */
						twidth = theight = 100; /* default window size */
				} /* END if have a font ... else ... */

				/* determine size of window */
				xoff = yoff = 0;
				width = twidth;
				height = theight;
				geomMask = XParseGeometry(geomStr, &xoff, &yoff, &width, &height);
				switch (gravMask = (geomMask & (XNegative | YNegative))) {
						case (XNegative | YNegative): gravity = SouthEastGravity; break;
						case XNegative:               gravity = NorthEastGravity; break;
						case YNegative:               gravity = SouthWestGravity; break;
						default:                      gravity = NorthWestGravity; break;
				}
				if (gravMask) {
						XGetGeometry(fromDpy, root, &rret, &xret, &yret, &wret, &hret, &bret, &dret);
						if ((geomMask & (XValue | XNegative)) == (XValue | XNegative)) {
								xoff = wret - width + xoff;
						}
						if ((geomMask & (YValue | YNegative)) == (YValue | YNegative)) {
								yoff = hret - height + yoff;
						}
				} /* END if geomMask */

				trigger = pDpyInfo->trigger = 
						XCreateSimpleWindow(fromDpy, root, xoff, yoff, width, height, 
										0, black, white);
		} /* END if doEdge ... else ...*/

		/* size hints stuff: */
		xsh->x           = xoff;
		xsh->y           = yoff;
		xsh->base_width  = width;
		xsh->base_height = height;
		xsh->win_gravity = gravity;
		xsh->flags       = (PPosition|PBaseSize|PWinGravity);
		XSetWMNormalHints(fromDpy, trigger, xsh);

		windowName = (char *)malloc(strlen(programStr) + strlen(toDpyName) + 2);
		strcpy(windowName, programStr);
		strcat(windowName, " ");
		strcat(windowName, toDpyName);
		XStoreName(fromDpy, trigger, windowName);
		XSetIconName(fromDpy, trigger, windowName);

		/* register for WM_DELETE_WINDOW protocol */
		pDpyInfo->wmpAtom = XInternAtom(fromDpy, "WM_PROTOCOLS", True);
		pDpyInfo->wmdwAtom = XInternAtom(fromDpy, "WM_DELETE_WINDOW", True);
		XSetWMProtocols(fromDpy, trigger, &(pDpyInfo->wmdwAtom), 1);

		if (doBig) {
				big = pDpyInfo->big = 
						XCreateWindow(fromDpy, root, 0, 0, fromWidth, fromHeight, 0,
										0, InputOutput, 0, CWOverrideRedirect, &xswa);
				/* size hints stuff: */
				xsh->x           = 0;
				xsh->y           = 0;
				xsh->base_width  = fromWidth;
				xsh->base_height = fromHeight;
				xsh->min_width   = fromWidth;
				xsh->min_height  = fromHeight;
				xsh->flags       = (PMinSize|PPosition|PBaseSize);

				XSetWMNormalHints(fromDpy, big, xsh);
				XStoreName(fromDpy, big, windowName);
				XSetIconName(fromDpy, big, windowName);
		} else {
				pDpyInfo->big = None;
		}

		XFree((char *) xsh);
		free(windowName);

		/* conversion stuff */
		if ((doEdge == EDGE_WEST) || (doEdge == EDGE_EAST)) {
			pDpyInfo->toScreen = (doEdge == EDGE_WEST) ? (nScreens - 1) : 0;
		} else if ((doEdge == EDGE_NORTH) || (doEdge == EDGE_SOUTH)) {
			pDpyInfo->toScreen = (doEdge == EDGE_NORTH) ? (nScreens - 1) : 0;
		}

		/* construct table lookup for screen coordinate conversion */
		pDpyInfo->xTables = (short **)malloc(sizeof(short *) * nScreens);
		pDpyInfo->yTables = (short **)malloc(sizeof(short *) * nScreens);
		heights = (int *)malloc(sizeof(int *) * nScreens);
		widths  = (int *)malloc(sizeof(int *) * nScreens);

		for (screenNum = 0; screenNum < nScreens; ++screenNum) {
				widths[screenNum] = toWidth  = 
						XWidthOfScreen(XScreenOfDisplay(toDpy, screenNum));
				heights[screenNum] = toHeight = 
						XHeightOfScreen(XScreenOfDisplay(toDpy, screenNum));
#ifdef DEBUG
				printf("Target screen dimensions: %dx%d\n", toWidth, toHeight);
#endif

				pDpyInfo->xTables[screenNum] = xTable =
						(short *)malloc(sizeof(short) * fromWidth);
				pDpyInfo->yTables[screenNum] = yTable =
						(short *)malloc(sizeof(short) * fromHeight);

				/* vertical conversion table */
				for (counter = 0; counter < fromHeight; ++counter)
						yTable[counter] = (counter * toHeight) / fromHeight;

				/* horizontal conversion table entries */
				for (counter = 0; counter < fromWidth; ++counter)
						xTable[counter] = (counter * toWidth) / fromWidth;

				/* adjustment for boundaries */
				if ((screenNum != 0) || (doEdge == EDGE_EAST))
						xTable[0] = COORD_DECR;
				if (((screenNum + 1) < nScreens) || (doEdge == EDGE_WEST)) {
						xTable[fromWidth - 1] = COORD_INCR;
						/* work-around for bug: on at least one tested screen, cursor
						   never moved past fromWidth - 2 */
						xTable[fromWidth - 2] = COORD_INCR;
				}

				if ((screenNum != 0) || (doEdge == EDGE_SOUTH))
						yTable[0] = COORD_DECR;
				if (((screenNum + 1) < nScreens) || (doEdge == EDGE_NORTH)) {
						yTable[fromHeight - 1] = COORD_INCR;
						/* work-around for bug: on at least one tested screen, cursor
						   never moved past fromWidth - 2 */
						yTable[fromHeight - 2] = COORD_INCR;
				}

		} /* END for screenNum */

		free(heights);
		free(widths);

		/* always create propWin for events from toDpy */
		propWin = XCreateWindow(toDpy, toRoot, 0, 0, 1, 1, 0, 0, InputOutput,
						CopyFromParent, 0, NULL);
		pDpyInfo->toDpyXtra.propWin = propWin;

		/* initialize pointer mapping */
		RefreshPointerMapping(toDpy, pDpyInfo);

		if (doSel) {
				pDpyInfo->sDpy = NULL;
				pDpyInfo->sTime = 0;

				pDpyInfo->fromDpyXtra.otherDpy   = toDpy;
				pDpyInfo->fromDpyXtra.sState     = SELSTATE_OFF;
				pDpyInfo->fromDpyXtra.pingAtom   = XInternAtom(fromDpy, pingStr, False);
				pDpyInfo->fromDpyXtra.pingInProg = False;
				pDpyInfo->fromDpyXtra.propWin    = trigger;
				eventMask |= PropertyChangeMask;

				pDpyInfo->toDpyXtra.otherDpy     = fromDpy;
				pDpyInfo->toDpyXtra.sState       = SELSTATE_OFF;
				pDpyInfo->toDpyXtra.pingAtom     = XInternAtom(toDpy, pingStr, False);
				pDpyInfo->toDpyXtra.pingInProg   = False;
				XSelectInput(toDpy, propWin, PropertyChangeMask);
				XSetSelectionOwner(toDpy, XA_PRIMARY, propWin, CurrentTime);
		} /* END if doSel */

		if (doResurface) /* get visibility events */
				eventMask |= VisibilityChangeMask;

		XSelectInput(fromDpy, trigger, eventMask);
		pDpyInfo->eventMask = eventMask; /* save for future munging */
		if (doSel) XSetSelectionOwner(fromDpy, XA_PRIMARY, trigger, CurrentTime);
		XMapRaised(fromDpy, trigger);
		if ((pDpyInfo->font = font)) { /* paint text */
				/* position text */
				pDpyInfo->twidth = twidth;
				pDpyInfo->theight = theight;
				toDpyFormat[toDpyLeftIndex] = MAX(0,((width - twidth) / 2));
				toDpyFormat[toDpyTopIndex]  = MAX(0,((height - theight) / 2));

				formatText(fromDpy, trigger, &(textGC), font, 
								toDpyFormatLength, toDpyFormat, NULL, NULL);
		} /* END if font */

		for (pShadow = shadows; pShadow; pShadow = pShadow->pNext)
				XTestGrabControl(pShadow->dpy, True); /* impervious to grabs! */

} /* END InitDpyInfo */

static void DoConnect(PDPYINFO pDpyInfo)
{
		Display *fromDpy = pDpyInfo->fromDpy;
		Window  trigger = pDpyInfo->trigger;

#ifdef DEBUG
		printf("connecting\n");
#endif
		pDpyInfo->mode = X2X_CONNECTED;
		if (pDpyInfo->big != None) XMapRaised(fromDpy, pDpyInfo->big);
		XGrabPointer(fromDpy, trigger, True,
						PointerMotionMask | ButtonPressMask | ButtonReleaseMask,
						GrabModeAsync, GrabModeAsync,
						None, pDpyInfo->grabCursor, CurrentTime);
		XGrabKeyboard(fromDpy, trigger, True, 
						GrabModeAsync, GrabModeAsync,
						CurrentTime);
		XSelectInput(fromDpy, trigger, pDpyInfo->eventMask | PointerMotionMask);
		XFlush(fromDpy);
} /* END DoConnect */

static void DoDisconnect(PDPYINFO pDpyInfo)
{
		Display *fromDpy = pDpyInfo->fromDpy;
		PDPYXTRA pDpyXtra;

#ifdef DEBUG
		printf("disconnecting\n");
#endif
		pDpyInfo->mode = X2X_DISCONNECTED;
		if (pDpyInfo->big != None) XUnmapWindow(fromDpy, pDpyInfo->big);
		XUngrabKeyboard(fromDpy, CurrentTime);
		XUngrabPointer(fromDpy, CurrentTime);
		XSelectInput(fromDpy, pDpyInfo->trigger, pDpyInfo->eventMask);

		if (doSel) {
				pDpyXtra = GETDPYXTRA(fromDpy, pDpyInfo);
				if (pDpyXtra->sState == SELSTATE_ON) {
						XSetSelectionOwner(fromDpy, XA_PRIMARY, pDpyXtra->propWin, CurrentTime);
				}
		} /* END if */

		XFlush(fromDpy);

		/* force normal state on to display: */
		if (doAutoUp)
				FakeThingsUp(pDpyInfo);
} /* END DoDisconnect */

static void RegisterEventHandlers(PDPYINFO pDpyInfo)
{
		Display *fromDpy = pDpyInfo->fromDpy;
		Window  trigger = pDpyInfo->trigger;
		Display *toDpy;
		Window  propWin;

#define XSAVECONTEXT(A, B, C, D) XSaveContext(A, B, C, (XPointer)(D))

		XSAVECONTEXT(fromDpy, trigger, MotionNotify,    ProcessMotionNotify);
		XSAVECONTEXT(fromDpy, trigger, Expose,          ProcessExpose);
		XSAVECONTEXT(fromDpy, trigger, EnterNotify,     ProcessEnterNotify);
		XSAVECONTEXT(fromDpy, trigger, ButtonPress,     ProcessButtonPress);
		XSAVECONTEXT(fromDpy, trigger, ButtonRelease,   ProcessButtonRelease);
		XSAVECONTEXT(fromDpy, trigger, KeyPress,        ProcessKeyEvent);
		XSAVECONTEXT(fromDpy, trigger, KeyRelease,      ProcessKeyEvent);
		XSAVECONTEXT(fromDpy, trigger, ConfigureNotify, ProcessConfigureNotify);
		XSAVECONTEXT(fromDpy, trigger, ClientMessage,   ProcessClientMessage);
		XSAVECONTEXT(fromDpy, trigger, ClientMessage,   ProcessClientMessage);
		XSAVECONTEXT(fromDpy, trigger, ClientMessage,   ProcessClientMessage);
		XSAVECONTEXT(fromDpy, None,    MappingNotify,   ProcessMapping);

		toDpy = pDpyInfo->toDpy;
		propWin = pDpyInfo->toDpyXtra.propWin;
		XSAVECONTEXT(toDpy, None, MappingNotify, ProcessMapping);

		if (doResurface)
				XSAVECONTEXT(fromDpy, trigger, VisibilityNotify, ProcessVisibility);

		if (doSel) {
				XSAVECONTEXT(fromDpy, trigger, SelectionRequest, ProcessSelectionRequest);
				XSAVECONTEXT(fromDpy, trigger, PropertyNotify,   ProcessPropertyNotify);
				XSAVECONTEXT(fromDpy, trigger, SelectionNotify,  ProcessSelectionNotify);
				XSAVECONTEXT(fromDpy, trigger, SelectionClear,   ProcessSelectionClear);

				XSAVECONTEXT(toDpy,   propWin, SelectionRequest, ProcessSelectionRequest);
				XSAVECONTEXT(toDpy,   propWin, PropertyNotify,   ProcessPropertyNotify);
				XSAVECONTEXT(toDpy,   propWin, SelectionNotify,  ProcessSelectionNotify);
				XSAVECONTEXT(toDpy,   propWin, SelectionClear,   ProcessSelectionClear);
		} /* END if doSel */

} /* END RegisterEventHandlers */

static Bool ProcessEvent(Display *dpy, PDPYINFO pDpyInfo)
{
		XEvent    ev;
		XAnyEvent *pEv = (XAnyEvent *)&ev;
		HANDLER   handler;

#define XFINDCONTEXT(A, B, C, D) XFindContext(A, B, C, (XPointer *)(D))

		XNextEvent(dpy, &ev);
		handler = 0;
		if ((!XFINDCONTEXT(dpy, pEv->window, pEv->type, &handler)) || 
			(!XFINDCONTEXT(dpy, None, pEv->type, &handler))) {
				/* have handler */
				return ((*handler)(dpy, pDpyInfo, &ev));
		} else {
#ifdef DEBUG
				printf("no handler for window 0x%x, event type %d\n", (unsigned int)pEv->window, pEv->type);
#endif
		} /* END if/else */

		return False;

} /* END ProcessEvent */

static Bool ProcessMotionNotify(Display *unused, PDPYINFO pDpyInfo, XMotionEvent *pEv)
{
		/* XXX: caution! pEv might be pseudo-event!!! */

		/* Note: ProcessMotionNotify is sometimes called from inside x2x to
		 *       simulate a motion event.  Any new references to pEv fields
		 *       must be checked carefully!
		 */

		int       toScreenNum;
		PSHADOW   pShadow;
		int       toX = 0;
		int       toY = 0;
		int       fromX, deltaX;
		int       fromY, deltaY;
		Display   *fromDpy;
		Bool      bAbortedDisconnect;

		/* find the screen */
		toScreenNum = pDpyInfo->toScreen;
		fromX = pEv->x_root;
		fromY = pEv->y_root;

		/* check to make sure the cursor is still on the from screen */
		if ((doEdge == EDGE_WEST) || (doEdge == EDGE_EAST)) {
				#ifdef DEBUG
				printf("In ProcessMotionNotify(), WEST/EAST\n");
				#endif
				if (!(pEv->same_screen)) {
						toX = (pDpyInfo->lastFromX < fromX) ? COORD_DECR : COORD_INCR;
				} else {
						toX = pDpyInfo->xTables[toScreenNum][fromX];
				}
				/* sanity check motion: necessary for nondeterminism surrounding warps */
				deltaX = pDpyInfo->lastFromX - fromX;
				if (deltaX < 0)
						deltaX = -deltaX;
				if (deltaX > pDpyInfo->unreasonableDelta) {
						return False;
				}

				if (SPECIAL_COORD(toX) != 0) { /* special coordinate */
						bAbortedDisconnect = False;
						if (toX == COORD_INCR) {
								if (toScreenNum != (pDpyInfo->nScreens - 1)) { /* next screen */
										toScreenNum = ++(pDpyInfo->toScreen);
										fromX = pDpyInfo->fromXIncr;
										toX = pDpyInfo->xTables[toScreenNum][fromX];
								} else { /* disconnect! */
										if (doBtnBlock &&
														(pEv->state & (Button1Mask | Button2Mask | Button3Mask |
																	   Button4Mask | Button5Mask)))
												bAbortedDisconnect = True;
										else {
												DoDisconnect(pDpyInfo);
												fromX = pDpyInfo->fromXDisc;
										}
										toX = pDpyInfo->xTables[toScreenNum][pDpyInfo->fromXConn];
								}
						} else { /* DECR */
								if (toScreenNum != 0) { /* previous screen */
										toScreenNum = --(pDpyInfo->toScreen);
										fromX = pDpyInfo->fromXDecr;
										toX = pDpyInfo->xTables[toScreenNum][fromX];
								} else { /* disconnect! */
										if (doBtnBlock &&
														(pEv->state & (Button1Mask | Button2Mask | Button3Mask |
																	   Button4Mask | Button5Mask)))
												bAbortedDisconnect = True;
										else {
												DoDisconnect(pDpyInfo);
												fromX = pDpyInfo->fromXDisc;
										}
										toX = pDpyInfo->xTables[toScreenNum][pDpyInfo->fromXConn];
								}
						} /* END if toX */
						if (!bAbortedDisconnect) {
								fromDpy = pDpyInfo->fromDpy;
								XWarpPointer(fromDpy, None, pDpyInfo->root, 0, 0, 0, 0, 
												fromX, pEv->y_root);
								XFlush(fromDpy);
						}
				} /* END if SPECIAL_COORD */
				pDpyInfo->lastFromX = fromX;

				for (pShadow = shadows; pShadow; pShadow = pShadow->pNext) {
					if (pEv->y_root <= XHeightOfScreen(XScreenOfDisplay(pDpyInfo->toDpy, toScreenNum))) {
						XTestFakeMotionEvent(pShadow->dpy, toScreenNum, toX, pDpyInfo->yTables[toScreenNum][pEv->y_root], 0);
						XFlush(pShadow->dpy);
					} else {
#ifdef DEBUG
						printf("bullshit Y coordinate: %d\n", pEv->y_root);
#endif
					}
				} /* END for */

		} else if ((doEdge == EDGE_NORTH) || (doEdge == EDGE_SOUTH)) {
				#ifdef DEBUG
				printf("In ProcessMotionNotify(), NORTH/SOUTH\n");
				#endif
				if (!(pEv->same_screen)) {
						toY = (pDpyInfo->lastFromY < fromY) ? COORD_DECR : COORD_INCR;
				} else {
						toY = pDpyInfo->yTables[toScreenNum][fromY];
				}
				/* sanity check motion: necessary for nondeterminism surrounding warps */
				deltaY = pDpyInfo->lastFromY - fromY;
				if (deltaY < 0)
						deltaY = -deltaY;
				if (deltaY > pDpyInfo->unreasonableDelta) {
						return False;
				}

				if (SPECIAL_COORD(toY) != 0) { /* special coordinate */
						bAbortedDisconnect = False;
						if (toY == COORD_INCR) {
								if (toScreenNum != (pDpyInfo->nScreens - 1)) { /* next screen */
										toScreenNum = ++(pDpyInfo->toScreen);
										fromY = pDpyInfo->fromYIncr;
										toY = pDpyInfo->yTables[toScreenNum][fromY];
								} else { /* disconnect! */
										if (doBtnBlock &&
														(pEv->state & (Button1Mask | Button2Mask | Button3Mask |
																	   Button4Mask | Button5Mask)))
												bAbortedDisconnect = True;
										else {
												DoDisconnect(pDpyInfo);
												fromY = pDpyInfo->fromYDisc;
										}
										toY = pDpyInfo->yTables[toScreenNum][pDpyInfo->fromYConn];
								}
						} else { /* DECR */
								if (toScreenNum != 0) { /* previous screen */
										toScreenNum = --(pDpyInfo->toScreen);
										fromY = pDpyInfo->fromYDecr;
										toY = pDpyInfo->yTables[toScreenNum][fromY];
								} else { /* disconnect! */
										if (doBtnBlock &&
														(pEv->state & (Button1Mask | Button2Mask | Button3Mask |
																	   Button4Mask | Button5Mask)))
												bAbortedDisconnect = True;
										else {
												DoDisconnect(pDpyInfo);
												fromY = pDpyInfo->fromYDisc;
										}
										toY = pDpyInfo->yTables[toScreenNum][pDpyInfo->fromYConn];
								}
						} /* END if toY */
						if (!bAbortedDisconnect) {
								fromDpy = pDpyInfo->fromDpy;
								XWarpPointer(fromDpy, None, pDpyInfo->root, 0, 0, 0, 0, 
												pEv->x_root, fromY);
								XFlush(fromDpy);
						}
				} /* END if SPECIAL_COORD */
				pDpyInfo->lastFromY = fromY;

				for (pShadow = shadows; pShadow; pShadow = pShadow->pNext) {
						if (pEv->x_root <= XWidthOfScreen(XScreenOfDisplay(pDpyInfo->toDpy, toScreenNum))) {
							XTestFakeMotionEvent(pShadow->dpy, toScreenNum, pDpyInfo->xTables[toScreenNum][pEv->x_root], toY, 0);
							XFlush(pShadow->dpy);
						} else {
#ifdef DEBUG
							printf("bullshit X coordinate: %d\n", pEv->x_root);
#endif
						}
				} /* END for */
		}

		return False;

} /* END ProcessMotionNotify */

static Bool ProcessExpose(Display *dpy, PDPYINFO pDpyInfo, XExposeEvent *pEv)
{
		XClearWindow(pDpyInfo->fromDpy, pDpyInfo->trigger);
		if (pDpyInfo->font)
				formatText(pDpyInfo->fromDpy, pDpyInfo->trigger, 
								&(pDpyInfo->textGC), pDpyInfo->font, 
								toDpyFormatLength, toDpyFormat, NULL, NULL);
		return False;

} /* END ProcessExpose */

static Bool ProcessEnterNotify(Display *dpy, PDPYINFO pDpyInfo, XCrossingEvent *pEv)
{
		Display *fromDpy = pDpyInfo->fromDpy;
		XMotionEvent xmev;

		if ((pEv->mode == NotifyNormal) &&
						(pDpyInfo->mode == X2X_DISCONNECTED) && (dpy == pDpyInfo->fromDpy)) {
				DoConnect(pDpyInfo);
				if ((doEdge == EDGE_EAST) || (doEdge == EDGE_WEST)) {
					XWarpPointer(fromDpy, None, pDpyInfo->root, 0, 0, 0, 0, 
							pDpyInfo->fromXConn, pEv->y_root); 
				} else if ((doEdge == EDGE_NORTH) || (doEdge == EDGE_SOUTH)) {
					XWarpPointer(fromDpy, None, pDpyInfo->root, 0, 0, 0, 0, 
							pEv->x_root, pDpyInfo->fromYConn);
				}
				xmev.x_root = pDpyInfo->lastFromX = pDpyInfo->fromXConn;
				xmev.y_root = pDpyInfo->lastFromY = pDpyInfo->fromYConn;
				/* xmev.y_root = pEv->y_root; */
				xmev.same_screen = True;
				ProcessMotionNotify(NULL, pDpyInfo, &xmev);
		}  /* END if NotifyNormal... */
		return False;

} /* END ProcessEnterNotify */

static Bool ProcessButtonPress(Display *dpy, PDPYINFO pDpyInfo, XButtonEvent *pEv)
{
		int state;
		PSHADOW   pShadow;
		unsigned int toButton;

		switch (pDpyInfo->mode) {
				case X2X_DISCONNECTED:
						pDpyInfo->mode = X2X_AWAIT_RELEASE;
#ifdef DEBUG
						printf("awaiting button release before connecting\n");
#endif
						break;
				case X2X_CONNECTED:
						if (pEv->button <= pDpyInfo->buttonCount) {
								toButton = pDpyInfo->inverseMap[pEv->button];
						} else {
								/* TODO: actually handle this */
								printf("Unknown button %d pressed!\n", pEv->button);
								return False;
						}

						for (pShadow = shadows; pShadow; pShadow = pShadow->pNext) {
								XTestFakeButtonEvent(pShadow->dpy, toButton, True, 0);
#ifdef DEBUG
								printf("from button %d down, to button %d down\n", pEv->button,toButton);
#endif
								XFlush(pShadow->dpy);
						} /* END for */
						if (doAutoUp)
								FakeAction(pDpyInfo, FAKE_BUTTON, toButton, True);
						if (doEdge) break;

						/* check if more than one button pressed */
						state = pEv->state;
						switch (pEv->button) {
								case Button1: state &= ~Button1Mask; break;
								case Button2: state &= ~Button2Mask; break;
								case Button3: state &= ~Button3Mask; break;
								case Button4: state &= ~Button4Mask; break;
								case Button5: state &= ~Button5Mask; break;
								default:
#ifdef DEBUG
											  printf("unknown button %d\n", pEv->button);
#endif	
											  break;
						} /* END switch button */
						if (state) { /* then more than one button pressed */
#ifdef DEBUG
								printf("awaiting button release before disconnecting\n");
#endif
								pDpyInfo->mode = X2X_CONN_RELEASE;
						}
						break;
		} /* END switch mode */
		return False;
} /* END ProcessButtonPress */

static Bool ProcessButtonRelease(Display *dpy, PDPYINFO pDpyInfo, XButtonEvent *pEv)
{
		int state;
		PSHADOW   pShadow;
		XMotionEvent xmev;
		unsigned int toButton;

		if ((pDpyInfo->mode == X2X_CONNECTED) || 
						(pDpyInfo->mode == X2X_CONN_RELEASE)) {
				if (pEv->button <= pDpyInfo->buttonCount) {
						toButton = pDpyInfo->inverseMap[pEv->button];
				} else {
						/* TODO: actually handle this */
						printf("Unknown button %d released!\n", pEv->button);
						return False;
				}

				for (pShadow = shadows; pShadow; pShadow = pShadow->pNext) {
						XTestFakeButtonEvent(pShadow->dpy, toButton, False, 0);
#ifdef DEBUG
						printf("from button %d up, to button %d up\n", pEv->button, toButton);
#endif
						XFlush(pShadow->dpy);
				} /* END for */
				if (doAutoUp)
						FakeAction(pDpyInfo, FAKE_BUTTON, toButton, False);
		} /* END if */

		if (doEdge) {
				return False;
		}
		if ((pDpyInfo->mode == X2X_AWAIT_RELEASE) || 
						(pDpyInfo->mode == X2X_CONN_RELEASE)) {
				/* make sure that all buttons are released */
				state = pEv->state;
				switch (pEv->button) {
						case Button1: state &= ~Button1Mask; break;
						case Button2: state &= ~Button2Mask; break;
						case Button3: state &= ~Button3Mask; break;
						case Button4: state &= ~Button4Mask; break;
						case Button5: state &= ~Button5Mask; break;
						default:
#ifdef DEBUG
									  printf("unknown button %d\n", pEv->button);
#endif	
									  break;
				} /* END switch button */
				if (!state) { /* all buttons up: time to (dis)connect */
						if (pDpyInfo->mode == X2X_AWAIT_RELEASE) { /* connect */
								DoConnect(pDpyInfo);
								xmev.x_root = pDpyInfo->lastFromX = pEv->x_root;
								xmev.y_root = pDpyInfo->lastFromY = pEv->y_root;
								/* xmev.y_root = pEv->y_root; */
								xmev.same_screen = True;
								ProcessMotionNotify(NULL, pDpyInfo, &xmev);
						} else { /* disconnect */
								DoDisconnect(pDpyInfo);
						} /* END if mode */
				} /* END if !state */
		} /* END if mode */
		return False;

} /* END ProcessButtonRelease */

static Bool ProcessKeyEvent(Display *dpy, PDPYINFO pDpyInfo, XKeyEvent *pEv)
{
		KeyCode   keycode;
		KeySym    keysym;
		PSHADOW   pShadow;
		Bool      bPress;
		PSTICKY   pSticky;

		keysym = XkbKeycodeToKeysym(pDpyInfo->fromDpy, 0, pEv->keycode, 0);
		bPress = (pEv->type == KeyPress);

		for (pSticky = stickies; pSticky; pSticky = pSticky->pNext)
				if (keysym == pSticky->keysym)
						break;

		if (pSticky) {
				for (pShadow = shadows; pShadow; pShadow = pShadow->pNext) {
						if ((keycode = XKeysymToKeycode(pShadow->dpy, keysym))) {
								XTestFakeKeyEvent(pShadow->dpy, keycode, True, 0);
								XTestFakeKeyEvent(pShadow->dpy, keycode, False, 0);
								XFlush(pShadow->dpy);
						} /* END if */
				} /* END for */
		} else {
				for (pShadow = shadows; pShadow; pShadow = pShadow->pNext) {
						if ((keycode = XKeysymToKeycode(pShadow->dpy, keysym))) {
								XTestFakeKeyEvent(pShadow->dpy, keycode, bPress, 0);
								XFlush(pShadow->dpy);
						} /* END if */
				} /* END for */
				if (doAutoUp) {
						FakeAction(pDpyInfo, FAKE_KEY, keysym, bPress);
				}
		}

		return False;

} /* END ProcessKeyEvent */ 

static Bool ProcessConfigureNotify(Display *dpy, PDPYINFO pDpyInfo, XConfigureEvent *pEv)
{
		if (pDpyInfo->font) {
				/* reposition text */
				toDpyFormat[toDpyLeftIndex] = 
						MAX(0,((pEv->width - pDpyInfo->twidth) / 2));
				toDpyFormat[toDpyTopIndex]  = 
						MAX(0,((pEv->height - pDpyInfo->theight) / 2));
		} /* END if font */
		return False;

} /* END ProcessConfigureNotify */

static Bool ProcessClientMessage(Display *dpy, PDPYINFO pDpyInfo, XClientMessageEvent *pEv)
{
		/* terminate if atoms match! */
		return ((pEv->message_type == pDpyInfo->wmpAtom) &&
						(pEv->data.l[0]    == pDpyInfo->wmdwAtom));

} /* END ProcessClientMessage */

static Bool ProcessSelectionRequest(Display *dpy, PDPYINFO pDpyInfo, XSelectionRequestEvent *pEv)
{
		PDPYXTRA pDpyXtra = GETDPYXTRA(dpy, pDpyInfo);
		Display *otherDpy;

#ifdef DEBUG
		printf("selection request\n");
#endif

		/* bribe me to support more general selection requests, 
		   or send me the code to do it. */
		if ((pDpyXtra->sState != SELSTATE_ON) ||
						(pEv->selection != XA_PRIMARY) || 
						(pEv->target > XA_LAST_PREDEFINED)) { /* bad request, punt request */
				pEv->property = None;
				SendSelectionNotify(pEv); /* blam! */
		} else {
				otherDpy = pDpyXtra->otherDpy;
				SendPing(otherDpy, GETDPYXTRA(otherDpy, pDpyInfo)); /* get started */
				if (pDpyInfo->sDpy) {
						/* nuke the old one */
						pDpyInfo->sEv.property = None;
						SendSelectionNotify(&(pDpyInfo->sEv)); /* blam! */
				} /* END if InProg */
				pDpyInfo->sDpy  = otherDpy;
				pDpyInfo->sEv = *pEv;
		} /* END if relaySel */
		return False;

} /* END ProcessSelectionRequest */

static void SendPing(Display *dpy, PDPYXTRA pDpyXtra)
{
		if (!(pDpyXtra->pingInProg)) {
				XChangeProperty(dpy, pDpyXtra->propWin, pDpyXtra->pingAtom, XA_PRIMARY,
								8, PropModeAppend, NULL, 0);
				pDpyXtra->pingInProg = True;
		} /* END if */
} /* END SendPing */

static Bool ProcessPropertyNotify(Display *dpy, PDPYINFO pDpyInfo, XPropertyEvent *pEv)
{
		PDPYXTRA pDpyXtra = GETDPYXTRA(dpy, pDpyInfo);

#ifdef DEBUG
		printf("property notify\n");
#endif

		if (pEv->atom == pDpyXtra->pingAtom) { /* acking a ping */
				pDpyXtra->pingInProg = False;
				if (pDpyXtra->sState == SELSTATE_WAIT) {
						pDpyXtra->sState = SELSTATE_ON;
						XSetSelectionOwner(dpy, XA_PRIMARY, pDpyXtra->propWin, pEv->time);
				} else if (dpy == pDpyInfo->sDpy) {
						if (pDpyInfo->sTime == pEv->time) { 
								/* oops, need to ensure uniqueness */
								SendPing(dpy, pDpyXtra); /* try for another time stamp */
						} else {
								pDpyInfo->sTime = pEv->time;
								XConvertSelection(dpy, pDpyInfo->sEv.selection, pDpyInfo->sEv.target,
												XA_PRIMARY, pDpyXtra->propWin, pEv->time);
						} /* END if ... ensure uniqueness */
				} /* END if sState... */
		} /* END if ping */
		return False;

} /* END ProcessPropertyNotify */

static Bool ProcessSelectionNotify(Display *dpy, PDPYINFO pDpyInfo, XSelectionEvent *pEv)
{
		Atom type;
		int  format;
		unsigned long nitems, after;
		unsigned char *prop;
		Bool success;
		XSelectionRequestEvent *pSelReq;

#define DEFAULT_PROP_SIZE 1024L

#ifdef DEBUG
		printf("selection notify\n");
#endif

		if ((dpy == pDpyInfo->sDpy) && (pDpyInfo->sTime == pEv->time)) {
				success = False;
				/* corresponding select */
				if (XGetWindowProperty(dpy, pEv->requestor, XA_PRIMARY, 0L, 
										DEFAULT_PROP_SIZE, True, AnyPropertyType,
										&type, &format, &nitems, &after, &prop)
								== Success) { /* got property */
						if ((type != None) && (format != None) && (nitems != 0) && 
										(prop != None) && (type <= XA_LAST_PREDEFINED)) { /* known type */
								if (after == 0L) { /* got everything */
										success = True;
								} else { /* try to get everything */
										XFree(prop);
										success = 
												((XGetWindowProperty(dpy, pEv->requestor, XA_PRIMARY, 0L,
																	 DEFAULT_PROP_SIZE + after + 1,
																	 True, AnyPropertyType,
																	 &type, &format, &nitems, &after, &prop)
												  == Success) && 
												 (type != None) && (format != None) && (nitems != 0) && 
												 (after == 0L) && (prop != None));
								} /* END if got everything ... else ...*/
						} /* END if known type */
				} /* END if got property */

				pSelReq = &(pDpyInfo->sEv);
				if (success) { /* send bits to the requesting dpy/window */
						XChangeProperty(pSelReq->display, pSelReq->requestor,
										pSelReq->property, type, format, PropModeReplace,
										prop, nitems);
						XFree(prop);
						SendSelectionNotify(pSelReq);
				} else {
						pSelReq->property = None;
						SendSelectionNotify(pSelReq);
				} /* END if success */
				pDpyInfo->sDpy = NULL;
		} /* END if corresponding select */
		return False;

} /* END ProcessSelectionNotify */

static void SendSelectionNotify(XSelectionRequestEvent *pSelReq)
{
		XSelectionEvent sendEv;

		sendEv.type      = SelectionNotify;
		sendEv.display   = pSelReq->display;
		sendEv.requestor = pSelReq->requestor;
		sendEv.selection = pSelReq->selection;
		sendEv.target    = pSelReq->target;
		sendEv.property  = pSelReq->property;
		sendEv.time      = pSelReq->time;
		XSendEvent(pSelReq->display, pSelReq->requestor, False, 0, 
						(XEvent *)&sendEv);

} /* END SendSelectionNotify */

static Bool ProcessSelectionClear(Display *dpy, PDPYINFO pDpyInfo, XSelectionClearEvent *pEv)
{
		Display  *otherDpy;
		PDPYXTRA pDpyXtra, pOtherXtra;

#ifdef DEBUG
		printf("selection clear\n");
#endif

		if (pEv->selection == XA_PRIMARY) {
				/* track primary selection */
				pDpyXtra = GETDPYXTRA(dpy, pDpyInfo);
				pDpyXtra->sState = SELSTATE_OFF;
				otherDpy = pDpyXtra->otherDpy;
				pOtherXtra = GETDPYXTRA(otherDpy, pDpyInfo);
				pOtherXtra->sState = SELSTATE_WAIT;
				SendPing(otherDpy, pOtherXtra);
				if (pDpyInfo->sDpy) { /* nuke the selection in progress */
						pDpyInfo->sEv.property = None;
						SendSelectionNotify(&(pDpyInfo->sEv)); /* blam! */
						pDpyInfo->sDpy = NULL;
				} /* END if nuke */
		} /* END if primary */
		return False;

} /* END ProcessSelectionClear */

/**********
 * process a visibility event
 **********/
static Bool ProcessVisibility(Display *dpy, PDPYINFO pDpyInfo, XVisibilityEvent *pEv)
{
		/* might want to qualify, based on other messages.  otherwise,
		   this code might cause a loop if two windows decide to fight
		   it out for the top of the stack */
		if (pEv->state != VisibilityUnobscured)
				XRaiseWindow(dpy, pEv->window);

		return False;

} /* END ProcessVisibility */

/**********
 * process a keyboard mapping event
 **********/
static Bool ProcessMapping(dpy, pDpyInfo, pEv)
		Display             *dpy;
		PDPYINFO            pDpyInfo;
		XMappingEvent       *pEv;
{
#ifdef DEBUG
		printf("mapping\n");
#endif

		switch (pEv->request) {
				case MappingModifier:
				case MappingKeyboard:
						XRefreshKeyboardMapping(pEv);
						break;
				case MappingPointer:
						RefreshPointerMapping(dpy, pDpyInfo);
						break;
		} /* END switch */

		return False;

} /* END ProcessMapping */

static void FakeAction(PDPYINFO pDpyInfo, int type, unsigned int thing, Bool bDown)
{
		PFAKE *ppFake;
		PFAKE pFake;

		/* find the associated button, or the last record, whichever comes first */
		for (ppFake = &(pDpyInfo->pFakeThings);
						(*ppFake && 
						 (((*ppFake)->type != type) || ((*ppFake)->thing != thing)));
						ppFake = &((*ppFake)->pNext));

		if (bDown) { /* key down */
				if (*ppFake == NULL) { /* need a new record */
						pFake = (PFAKE)malloc(sizeof(FAKE));
						pFake->pNext = NULL; /* always at the end of the list */
						pFake->type = type;
						pFake->thing = thing;
						*ppFake = pFake;
				} /* END if */
		} else { /* key up */
				if (*ppFake != NULL) { /* get rid of the record */
						/* splice out of the list */
						pFake = *ppFake;
						*ppFake = pFake->pNext;
						free(pFake); /* blam! */
				} /* END if */
		} /* END if */

} /* END FakeAction */

static void FakeThingsUp(PDPYINFO pDpyInfo)
{
		PFAKE pFake, pNext;
		PSHADOW pShadow;
		unsigned int type;
		KeyCode keycode;

		if (pDpyInfo->pFakeThings) { /* everything goes up! */
				for (pFake = pDpyInfo->pFakeThings; pFake; pFake = pNext) {
						type = pFake->type;
						/* send up to all shadows */
						for (pShadow = shadows; pShadow; pShadow = pShadow->pNext) {
								if (type == FAKE_KEY) { /* key goes up */
										if ((keycode = XKeysymToKeycode(pShadow->dpy, pFake->thing))) {
												XTestFakeKeyEvent(pShadow->dpy, keycode, False, 0);
#ifdef DEBUG
												printf("key 0x%x up\n", pFake->thing);
#endif
										} /* END if */
								} else { /* button goes up */
										XTestFakeButtonEvent(pShadow->dpy, pFake->thing, False, 0);
#ifdef DEBUG
										printf("button %d up\n", pFake->thing);
#endif
								} /* END if/else */
						} /* END for */

						/* flush everything at once */
						for (pShadow = shadows; pShadow; pShadow = pShadow->pNext)
								XFlush(pShadow->dpy);

						/* get next and free current */
						pNext = pFake->pNext;
						free(pFake);
				} /* END for */

				pDpyInfo->pFakeThings = NULL;
		} /* END if */

} /* END FakeThingsUp */

static void RefreshPointerMapping(Display *dpy, PDPYINFO pDpyInfo)
{
		unsigned int buttCtr;
		unsigned char buttonMap[MAX_BUTTONS];

		if (dpy == pDpyInfo->toDpy) { /* only care about toDpy */
				/* straightforward mapping */
				for (buttCtr = 0; buttCtr < MAX_BUTTONS; buttCtr++) {
						pDpyInfo->inverseMap[buttCtr] = buttCtr;
				} /* END for */

				if (doPointerMap) {
						pDpyInfo->buttonCount = XGetPointerMapping(dpy, buttonMap, MAX_BUTTONS);

#ifdef DEBUG
						printf("number of buttons: %d\n", pDpyInfo->buttonCount);
#endif
						for (buttCtr = 0; buttCtr < pDpyInfo->buttonCount; ++buttCtr) {
								/* check if button is out of range on remote side */
								if (buttonMap[buttCtr] > pDpyInfo->buttonCount) {
									pDpyInfo->inverseMap[buttCtr +1] = buttCtr + 1;
									printf("Warning: display %s has out of index pointer mapping: %d -> %d\n",
											toDpyName, buttCtr+1, buttonMap[buttCtr]);
#ifdef DEBUG
									printf("button %d -> %d\n", buttCtr + 1, buttCtr+1);
#endif
								} else {
									pDpyInfo->inverseMap[buttonMap[buttCtr]] = buttCtr + 1; 
#ifdef DEBUG
									printf("button %d -> %d\n", buttCtr + 1, buttonMap[buttCtr]);
#endif
								}
						} /* END for */
				} /* END if */
		} /* END if toDpy */

} /* END RefreshPointerMapping */
