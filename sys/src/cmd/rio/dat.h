#include <complete.h>
#include "undo.h"

enum
{
	Qdir,			/* /dev for this window */
	Qscreen,
	Qsnarf,
	Qwctl,
	Qtap,
	Qwsys,		/* directory of window directories */
	Qwsysdir,		/* window directory, child of wsys */

	Qcons,
	Qconsctl,
	Qcursor,
	Qwdir,
	Qwinid,
	Qwinname,
	Qlabel,
	Qkbd,
	Qmouse,
	Qtext,
	Qwindow,

	QMAX,
	Qglobal = Qcons,	/* anything >= must have non nil window */
};

/* Keep mtext in sync with this */
enum { M_TERM, M_TEXT, M_MEDIA, M_DEBUG, M_MULTI_EDIT, M_NARROW, M_INPUT, M_FILE_HIST,
  M_CMD_HIST, M_SEARCH_HIST, M_INV
};

#define	STACK	8192
#define	MAXSNARF	100*1024
#define FILE_HIST "/file_history"
#define CMD_HIST  "/cmd_history"

typedef	struct	Consreadmesg Consreadmesg;
typedef	struct	Conswritemesg Conswritemesg;
typedef struct	Kbdreadmesg Kbdreadmesg;
typedef	struct	Stringpair Stringpair;
typedef	struct	Dirtab Dirtab;
typedef	struct	Fid Fid;
typedef	struct	Filsys Filsys;
typedef	struct	Mouseinfo	Mouseinfo;
typedef	struct	Mousereadmesg Mousereadmesg;
typedef	struct	Mousestate	Mousestate;
typedef	struct	Ref Ref;
typedef	struct	Timer Timer;
typedef	struct	Wctlmesg Wctlmesg;
typedef	struct	Window Window;
typedef	struct	Xfid Xfid;
typedef struct	Tapmesg Tapmesg;
typedef struct  Completejob Completejob;
typedef struct  Buffer Buffer;
typedef struct Buflist Buflist;
typedef struct Button Button;
typedef struct Span Span;

struct Span {
    uint pos;
    uint len;
    int  type;
    union {
        struct { Image *fg, *bg; };   /* SpanColor */
        int   ulthick;                /* SpanUnderline */
        char *url;                    /* SpanURL */
    };
};

struct  Button
{
  Rectangle r;
  Rune      *label;
  Rune      *cmd;
  Rune      *tooltip;
};

struct Buffer
{
	int 	id;
	char	*dir, *file;
	uint 	q0, q1;
	Rune 	*r;
	uint 	nr;
	int 	modified;
	int 	mode;
	UndoStack   *undo;
	Buflist		*narrow;
};

typedef struct Buflist {
  Buffer *buf;
  Buflist *next;
} Buflist;

struct Completejob
{
	char	*dir;
	char	*str;
	Window	*win;
	int 	force;
};

enum
{
	Selborder		= 4,		/* border of selected window */
	Unselborder	= 1,		/* border of unselected window */
	Scrollwid 		= 12,		/* width of scroll bar */
	Scrollgap 		= 4,		/* gap right of scroll bar */
	BIG			= 3,		/* factor by which window dimension can exceed screen */
	TRUE		= 1,
	FALSE		= 0,
};

#define	QID(w,q)	((w<<8)|(q))
#define	WIN(q)	((((ulong)(q).path)>>8) & 0xFFFFFF)
#define	FILE(q)	(((ulong)(q).path) & 0xFF)

enum	/* control messages */
{
	Wakeup,
	Reshaped,
	Topped,
	Repaint,
	Refresh,
	Movemouse,
	Rawon,
	Rawoff,
	Holdon,
	Holdoff,
	Truncate,
	Deleted,
	Exited,
};

struct Wctlmesg
{
	int		type;
	Rectangle	r;
	void		*p;
};

struct Conswritemesg
{
	Channel	*cw;		/* chan(Stringpair) */
};

struct Consreadmesg
{
	Channel	*c1;		/* chan(tuple(char*, int) == Stringpair) */
	Channel	*c2;		/* chan(tuple(char*, int) == Stringpair) */
};

struct Mousereadmesg
{
	Channel	*cm;		/* chan(Mouse) */
};

struct Stringpair	/* rune and nrune or byte and nbyte */
{
	void		*s;
	int		ns;
};

struct Mousestate
{
	Mouse;
	ulong	counter;	/* serial no. of mouse event */
};

struct Mouseinfo
{
	Mousestate	queue[16];
	int	ri;	/* read index into queue */
	int	wi;	/* write index */
	ulong	counter;	/* serial no. of last mouse event we received */
	ulong	lastcounter;	/* serial no. of last mouse event sent to client */
	int	lastb;	/* last button state we received */
	uchar	qfull;	/* filled the queue; no more recording until client comes back */	
};

struct Window
{
	Ref;
	QLock;
	Frame;
	Image		*i;		/* window image, nil when deleted */
	Mousectl	mc;
	Mouseinfo	mouse;
	Channel		*ck;		/* chan(char*) */
	Channel		*cctl;		/* chan(Wctlmesg)[4] */
	Channel		*conswrite;	/* chan(Conswritemesg) */
	Channel		*consread;	/* chan(Consreadmesg) */
	Channel		*mouseread;	/* chan(Mousereadmesg) */
	Channel		*wctlread;	/* chan(Consreadmesg) */
	Channel		*kbdread;	/* chan(Consreadmesg) */
	Channel		*complete;	/* chan(Completion*) */
	Channel		*gone;		/* chan(char*) */
	uint			nr;			/* number of runes in window */
	uint			maxr;		/* number of runes allocated in r */
	Rune			*run;
	uint			nraw;
	Rune			*raw;
	uint			org;
	uint			q0;
	uint			q1;
	uint			qh;
	uint			qh1;
	uint			*mq;
	uint			nm;
	uint			maxnm;
	uint			comp_len; /* Length of completion prefix */
	uint			line, col;
	int			id;
	char			name[32];
	uint			namecount;
	Rectangle		scrollr;
	Window		    *popup;
	Window		    *title;
	UndoStack   *scratch;
	UndoStack   *undo;
	UndoStack   *hfile;
	UndoStack   *hcmd;
	UndoStack   *hsearch;
	int			bufid;
	Rune			*input;
	Rune			*prompt;
	void			(*cmd)(Window*, Rune*);
	int		    enable_title;
	int		    active;
	int		    saving;
	uint	    mode, prev_mode;
	int		    hovering;
	Image		*overlay;
	Button		*buttons;
	int		    nbtns;
	Rectangle	orect;
	int		    mpreview; /* Preview mode is activated */
	int		    preview;  /* Window is a live preview window */
	Completion    *c;
	Span *spans;
	uint  nspans;
	uint  maxspans;

	/*
	 * Rio once used originwindow, so screenr could be different from i->r.
	 * Now they're always the same but the code doesn't assume so.
	*/
	Rectangle		screenr;	/* screen coordinates of window */
	int			resized;
	int			wctlready;
	Rectangle		lastsr;
	int			topped;
	int			notefd;
	uchar		scrolling;
	Cursor		cursor;
	Cursor		*cursorp;
	uchar		holding;
	uchar		rawing;
	uchar		ctlopen;
	uchar		wctlopen;
	uchar		deleted;
	uchar		mouseopen;
	uchar		kbdopen;
	uchar		keyup;
	uchar		winnameread;
	char			*label;
	char			*dir;
	char			*file;
	int			fmodified; /* If loaded file is modified (renamed: Frame embeds modified) */
};

struct Dirtab
{
	char		*name;
	uchar	type;
	uint		qid;
	uint		perm;
};

struct Fid
{
	int		fid;
	int		busy;
	int		open;
	int		mode;
	Qid		qid;
	Window	*w;
	Dirtab	*dir;
	Fid		*next;
	int		nrpart;
	uchar	rpart[UTFmax];
};

struct Xfid
{
		Ref;
		Xfid		*next;
		Xfid		*free;
		Fcall;
		Channel	*c;	/* chan(void(*)(Xfid*)) */
		Fid		*f;
		uchar	*buf;
		Filsys	*fs;
		int		flushtag;	/* our tag, so flush can find us */
		Channel	*flushc;	/* channel(int) to notify us we're being flushed */
};

Channel*	xfidinit(void);
void		xfidctl(void*);
void		xfidflush(Xfid*);
void		xfidattach(Xfid*);
void		xfidopen(Xfid*);
void		xfidclose(Xfid*);
void		xfidread(Xfid*);
void		xfidwrite(Xfid*);

enum
{
	Nhash	= 16,
};

struct Filsys
{
		int		cfd;
		int		sfd;
		int		pid;
		char		*user;
		Channel	*cxfidalloc;	/* chan(Xfid*) */
		Channel	*csyncflush;	/* chan(int) */
		Fid		*fids[Nhash];
};

Filsys*	filsysinit(Channel*);
int		filsysmount(Filsys*, int);
Xfid*		filsysrespond(Filsys*, Xfid*, Fcall*, char*);
void		filsyscancel(Xfid*);

void		deletetimeoutproc(void*);

struct Timer
{
	int		dt;
	int		cancel;
	Channel	*c;	/* chan(int) */
	Timer	*next;
};

Font		*font;
Mousectl	*mousectl;
Mouse	*mouse;
Display	*display;
Image	*view;
Screen	*wscreen;
Cursor	boxcursor;
Cursor	crosscursor;
Cursor	sightcursor;
Cursor	whitearrow;
Cursor	query;
Cursor	*corners[9];
Cursor	skull;

Image	*background;
Image	*tagcols[NCOL];
Image	*cols[NCOL];
Image	*titlecol;
Image	*lighttitlecol;
Image	*dholdcol;
Image	*holdcol;
Image	*lightholdcol;
Image	*paleholdcol;
Image	*paletextcol;
Image	*sizecol;
int	reverse;	/* there are no pastel paints in the dungeons and dragons world -- rob pike */

Window	**window;
Window	*wkeyboard;	/* window of simulated keyboard */
int		nwindow;
int		snarffd;
int		gotscreen;
int		servekbd;

Channel *opentap;	/* open fromtap or totap */
Channel *closetap;	/* close fromtap or totap */
Channel	*fromtap;	/* keyboard output from the tap program */
Channel *totap;		/* our keyboard input to tap program */
Channel *wallchan;  /* wallpaper */

Window	*input;
QLock	all;			/* BUG */
Filsys	*filsys;
Window	*hidden[100];
int		nhidden;
int		nsnarf;
Rune*	snarf;
int		scrolling;
int		maxtab;
int		title;
Channel*	winclosechan;
char		*startdir;
int		sweeping;
char		srvpipe[];
char		srvwctl[];
int		errorshouldabort;
int		menuing;		/* menu action is pending; waiting for window to be indicated */
int		snarfversion;	/* updated each time it is written */
int		messagesize;		/* negotiated in 9P version setup */
int		shiftdown;
int		debug;
int		idle;
Point   mp;
Image   *saved;

enum {
	Mmod4 = 1<<0,
	Mctl = 1<<1,
	Malt = 1<<2,
	Mshift = 1<<3,
};
int mod;
typedef struct {
  char *k, *cmd;
} Keydef;
Keydef key_map[];

typedef int bool;

typedef struct {
  const char *s;
  void (*f0)(void);
  void (*f1)(Window *w);
  void (*f2)(Window *w, int n);
  int argc;
} Command;

typedef struct keymap Keymap;
struct keymap {
  Rune *k;
  Keymap **val; // Array of Keymap pointer
  int n, len;

  bool iscmd;
  Command *cmd;
};
Keymap *global_map;

typedef struct {
  int q0, qh1, qh2;
  Point p;
  Rune *r;
} Tooltip;
Tooltip tip;
