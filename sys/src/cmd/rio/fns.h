/* Harvey compat (in compat.c/frcol.c): 9front libc/libdraw/libframe helpers */
int	min(int, int);
int	max(int, int);
int	badrect(Rectangle);
void	frsetcol(Frame*, Image*, Image*);
int	whide(Window*);
int	wunhide(Window*);
int wbswidth(Window *w, Rune c);
void wsetselect(Window *w, uint q0, uint q1);
void wdelete(Window *w, uint q0, uint q1);
uint winsert(Window *w, Rune *r, int n, uint q0);
void wclosewin(Window *w);
void wclosepopup(Window *w);
void waddraw(Window *w, Rune *r, int nr);
void setmodified(Window *w, int v);
void showcandidates(Window *w1, Completion *c);
void openproc(void *arg);
Completion* complete1(char **all, int n, char *s, int fuzzy, int dedup, int fold);
int windfilewidth(Window *w, uint q0, int oneelement);
void completeproc(void *arg);
void savehistory(Window *w);
uint msec(void);
void idlewatch(void);
void idlewatch1(void*);
int idlehandler(void *, char *msg);
void wrepaint(Window *w);
int intpcmp(const void *va, const void *vb);
void buffer_close_all(void);
ulong blendcolor(ulong c1, ulong c2, double t);
void multimark(Window *w, int q0);
void drawmmark(Window *w, int n, int offset);
void hoverexit(Window *w);
void oaction(Window *w);
Keymap *keymap_load(Keydef key_map[]);
void execprim(Window *w, char *p);
char* cwd(void);

/* props.c */
void waddspan(Window *w, Span s);
void waddcolorspan(Window *w, uint pos, uint len);
void spansadjust(Window *w, uint pos, int delta);
void wcolordraw(Window *w, uint q0, uint q1);
int wcolinuse(Window *w, Image *col);
void freespan(Window *w);

void		winctl(void*);
void		winshell(void*);
Window*	wlookid(int);
Window*	wmk(Image*, Mousectl*, Channel*, Channel*, int);
Window*	wpointto(Point);
Window*	wtop(Point);
void		wtopme(Window*);
void		wbottomme(Window*);
char*	wcontents(Window*, int*);
int		wclose(Window*);
uint		wbacknl(Window*, uint, uint);
void		wcurrent(Window*);
void		wuncurrent(Window*);
void		wcut(Window*);
void		wpaste(Window*);
void		wplumb(Window*);
void		wlook(Window*, Rune*);
void		wrlook(Window*);
void		wscrdraw(Window*);
void		wscroll(Window*, int);
void		wsend(Window*);
void		wsendctlmesg(Window*, int, Rectangle, void*);
void		wsetcursor(Window*, int);
void		wsetname(Window*);
void		wsetorigin(Window*, uint, int);
void		wsetpid(Window*, int, int);
void		wshow(Window*, uint);
void		wsnarf(Window*);
void 		wscrsleep(Window*, uint);
void 		wtagrepaint(Window*);

void 		interruptproc(void*);
void 		namecomplete(Window*, int);

Channel*	xfidinit(void);
void		xfidctl(void*);
void		xfidflush(Xfid*);
void		xfidattach(Xfid*);
void		xfidopen(Xfid*);
void		xfidclose(Xfid*);
void		xfidread(Xfid*);
void		xfidwrite(Xfid*);

Filsys*	filsysinit(Channel*);
int		filsysmount(Filsys*, int);
Xfid*		filsysrespond(Filsys*, Xfid*, Fcall*, char*);
void		filsyscancel(Xfid*);

void		deletetimeoutproc(void*);


void	freescrtemps(void);
int	parsewctl(char**, Rectangle, Rectangle*, int*, int*, int*, int*, char**, char*, char*);
int	writewctl(Xfid*, char*);
Window *new(Image*, int, int, int, char*, char*, char**);
void	riosetcursor(Cursor*);
/* int	min(int, int); */
/* int	max(int, int); */
Rune*	strrune(Rune*, Rune);
/* int	isalnum(Rune); */
/* int	isspace(Rune); */
void	timerstop(Timer*);
void	timercancel(Timer*);
Timer*	timerstart(int);
void	error(char*);
void	killprocs(void);
int	shutdown(void*, char*);
void	iconinit(void);
void	*erealloc(void*, uint);
void *emalloc(uint);
char *estrdup(char*);
void	button3menu(void);
void	button2menu(Window*);
void	cvttorunes(char*, int, Rune*, int*, int*, int*);
/* was (byte*,int)	runetobyte(Rune*, int); */
char* runetobyte(Rune*, int, int*);
void	putsnarf(void);
void	getsnarf(void);
void	timerinit(void);
int	goodrect(Rectangle);
int	inborder(Rectangle, Point);

#define	runemalloc(n)		malloc((n)*sizeof(Rune))
#define	runerealloc(a, n)	realloc(a, (n)*sizeof(Rune))
#define	runemove(a, b, n)	memmove(a, b, (n)*sizeof(Rune))
