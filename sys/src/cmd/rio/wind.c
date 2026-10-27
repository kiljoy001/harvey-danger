#include <u.h>
#include <libc.h>
#include <ctype.h>
#include <draw.h>
#include <thread.h>
#include <cursor.h>
#include <mouse.h>
#include <keyboard.h>
#include <frame.h>
#include <fcall.h>
#include <plumb.h>
/* #include <complete.h> */
#include "dat.h"
#include "fns.h"

#define IDLE_TRIGGER 5000

Window*
wlookid(int id)
{
	int i;

	for(i=0; i<nwindow; i++)
		if(window[i]->id == id)
			return window[i];
	return nil;
}

Window*
wpointto(Point pt)
{
	int i;
	Window *v, *w;

	w = nil;
	for(i=0; i<nwindow; i++){
		v = window[i];
		if(ptinrect(pt, v->screenr))
		if(w==nil || v->topped>w->topped)
			w = v;
	}
	return w;
}

static	int	topped;

void
wtopme(Window *w)
{
	if(w!=nil && w->i!=nil && w->topped!=topped){
		w->topped = ++topped;
		topwindow(w->i);
		flushimage(display, 1);
	}
}

void
wbottomme(Window *w)
{
	if(w!=nil && w->i!=nil){
		w->topped = - ++topped;
		bottomwindow(w->i);
		flushimage(display, 1);
	}
}

Window*
wtop(Point pt)
{
	Window *w;

	w = wpointto(pt);
	if(w!=nil){
		incref(w);
		wcurrent(w);
		wtopme(w);
		wsendctlmesg(w, Topped, ZR, nil);
		wclose(w);
	}
	return w;
}

void
wcurrent(Window *w)
{
	Channel *c;

	if(input == nil){
		input = w;
		return;
	}
	if(w == input)
		return;
	incref(input);
	c = chancreate(sizeof(Window*), 0);
	wsendctlmesg(input, Repaint, ZR, c);
	sendp(c, w);		/* send the new input */
	wclose(recvp(c));	/* release old input */
	chanfree(c);
}

void
wuncurrent(Window *w)
{
	Channel *c;

	if(input == nil || w != input)
		return;
	c = chancreate(sizeof(Window*), 0);
	wsendctlmesg(w, Repaint, ZR, c);
	sendp(c, nil);
	recvp(c);
	chanfree(c);
}

static	Cursor	*lastcursor;

void
riosetcursor(Cursor *p)
{
	if(p==lastcursor)
		return;
	setcursor(mousectl, p);
	lastcursor = p;
}

void
wsetcursor(Window *w, int force)
{
	Cursor *p;

	if(menuing || sweeping || (w!=input && wpointto(mouse->xy)!=w))
		return;
	if(w==nil)
		p = nil;
	else {
		p = w->cursorp;
		if(p==nil && w->holding)
			p = &whitearrow;
	}
	if(p && force)	/* force cursor reload */
		lastcursor = nil;
	riosetcursor(p);
}

void
waddraw(Window *w, Rune *r, int nr)
{
	w->raw = runerealloc(w->raw, w->nraw+nr);
	runemove(w->raw+w->nraw, r, nr);
	w->nraw += nr;
}

enum
{
	HiWater	= 640000,	/* max size of history */
	LoWater	= 400000,	/* min size of history after max'ed */
	MinWater	= 20000,	/* room to leave available when reallocating */
};

uint
winsert(Window *w, Rune *r, int n, uint q0)
{
	uint m, n1, p0;

	if(n == 0)
		return q0;

	if (w->nr == 0) q0 = w->q0 = w->q1 = w->nr;
	if (w->file) setmodified(w, 1);
	n1 = 0;
	if (w->mode == M_MULTI_EDIT) {
	  n1 = n;
	  n = n * w->nm;
	}
	if(w->nr+n>HiWater && q0>=w->org && q0>=w->qh){
		m = min(HiWater-LoWater, min(w->org, w->qh));
		w->org -= m;
		w->qh -= m;
		if(w->q0 > m)
			w->q0 -= m;
		else
			w->q0 = 0;
		if(w->q1 > m)
			w->q1 -= m;
		else
			w->q1 = 0;
		w->nr -= m;
		runemove(w->run, w->run+m, w->nr);
		q0 -= m;
	}
	if(w->nr+n > w->maxr){
		/*
		 * Minimize realloc breakage:
		 *	Allocate at least MinWater
		 * 	Double allocation size each time
		 *	But don't go much above HiWater
		 */
		m = max(min(2*(w->nr+n), HiWater), w->nr+n)+MinWater;
		if(m > HiWater)
			m = max(HiWater+MinWater, w->nr+n);
		if(m > w->maxr){
			w->run = runerealloc(w->run, m);
			w->maxr = m;
		}
	}
	if (w->mode == M_MULTI_EDIT) {
	  n = n1;
	  for (int i = w->nm - 1; i >= 0; i--) {
		p0 = w->mq[i];
		/* fprint(2, "%d %d %d,", p0, w->org, w->nr); */
		undoaddinsert(w->undo, r, n, w->mq[i]);
		if (p0 < w->nr)
		runemove(w->run+p0+n, w->run+p0, w->nr-p0);
		runemove(w->run+p0, r, n);
		if(p0 >= w->org && p0 <= w->org+w->nchars) {
		  Point p;
		  p = frptofchar(&w->Frame, p0);
		  frtick(&w->Frame, p, 0);
		  frinsert(&w->Frame, r, r + n, p0 - w->org);
		  frdrawsel(&w->Frame, p, p0, p0 + n, 1);
		  p = frptofchar(&w->Frame, p0 + n);
		  frtick(&w->Frame, p, 1);
		}
		if (w->q0 > w->mq[i]) w->q0 += n;
		if (w->qh > w->mq[i]) w->qh += n;
		w->nr += n;
		w->mq[i] = p0 + n * (i + 1);
	  }
	} else {
	undoaddinsert(w->undo, r, n, q0);
	runemove(w->run+q0+n, w->run+q0, w->nr-q0);
	runemove(w->run+q0, r, n);
	w->nr += n;
	}
	spansadjust(w, q0, n);
	/* if output touches, advance selection, not qh; works best for keyboard and output */
	if(q0 <= w->q1)
		w->q1 += n;
	if(q0 <= w->q0)
		w->q0 += n;
	if(q0 < w->qh)
		w->qh += n;
	if(q0 < w->org)
		w->org += n;
	else if(!n1 && q0 <= w->org+w->nchars)
		frinsert(&w->Frame, r, r+n, q0-w->org);
	return q0;
}

static void
wfill(Window *w)
{
	Rune *rp;
	int i, n, m, nl;

	while(w->lastlinefull == FALSE){
		n = w->nr-(w->org+w->nchars);
		if(n == 0)
			break;
		if(n > 2000)	/* educated guess at reasonable amount */
			n = 2000;
		rp = w->run+(w->org+w->nchars);

		/*
		 * it's expensive to frinsert more than we need, so
		 * count newlines.
		 */
		nl = w->maxlines-w->nlines;
		m = 0;
		for(i=0; i<n; ){
			if(rp[i++] == '\n'){
				m++;
				if(m >= nl)
					break;
			}
		}
		frinsert(&w->Frame, rp, rp+i, w->nchars);
	}
}

void
wsetselect(Window *w, uint q0, uint q1)
{
	int p0, p1;
	Image *fg, *bg;
	int i;
	Span s;

	/* w->p0 and w->p1 are always right; w->q0 and w->q1 may be off */
	w->q0 = q0;
	w->q1 = q1;
	/* compute desired p0,p1 from q0,q1 */
	p0 = q0-w->org;
	p1 = q1-w->org;
	if(p0 < 0)
		p0 = 0;
	if(p1 < 0)
		p1 = 0;
	if(p0 > w->nchars)
		p0 = w->nchars;
	if(p1 > w->nchars)
		p1 = w->nchars;
	if(p0==w->p0 && p1==w->p1)
		return;

	fg = w->cols[TEXT];
	bg = w->cols[BACK];

	/* screen disagrees with desired selection */
	if(w->p1<=p0 || p1<=w->p0 || p0==p1 || w->p1==w->p0){
		/* no overlap or too easy to bother trying */
		frsetcol(&w->Frame, cols[TEXT], cols[BACK]);
		frdrawsel(&w->Frame, frptofchar(&w->Frame, w->p0), w->p0, w->p1, 0);
		wcolordraw(w, w->p0 + w->org, w->p1 + w->org);

		frdrawsel(&w->Frame, frptofchar(&w->Frame, p0), p0, p1, 1);
		goto Return;
	}
	/* overlap; avoid unnecessary painting */
	if(p0 < w->p0){
		/* extend selection backwards */
		frdrawsel(&w->Frame, frptofchar(&w->Frame, p0), p0, w->p0, 1);
	}else if(p0 > w->p0){
		/* trim first part of selection */
		wcolordraw(w, w->p0+w->org, p0+w->org);
		frdrawsel(&w->Frame, frptofchar(&w->Frame, w->p0), w->p0, p0, 0);
	}
	if(p1 > w->p1){
		/* extend selection forwards */
		frdrawsel(&w->Frame, frptofchar(&w->Frame, w->p1), w->p1, p1, 1);
	}else if(p1 < w->p1){
		/* trim last part of selection */
		wcolordraw(w, p1+w->org, w->p1+w->org);
		frdrawsel(&w->Frame, frptofchar(&w->Frame, p1), p1, w->p1, 0);
	}

    Return:
	w->p0 = p0;
	w->p1 = p1;
	frsetcol(&w->Frame, fg, bg);
}

static void
wborder(Window *w, int type)
{
	Image *col;

	if(w->i == nil)
		return;
	if(w->holding){
		if(type == Selborder)
			col = holdcol;
		else
			col = paleholdcol;
	}else{
		if(type == Selborder)
			col = titlecol;
		else
			col = lighttitlecol;
	}
	border(w->i, w->i->r, Selborder, col, ZP);
}

static void
wsetcols(Window *w, int topped)
{
	if(w->holding)
		if(topped)
			w->cols[TEXT] = holdcol;
		else
			w->cols[TEXT] = lightholdcol;
	else
		if(topped)
			w->cols[TEXT] = cols[TEXT];
		else
			w->cols[TEXT] = paletextcol;
}

void
wsetname(Window *w)
{
	int i, n;
	char err[ERRMAX];
	
	n = snprint(w->name, sizeof(w->name)-2, "window.%d.%d", w->id, w->namecount++);
	for(i='A'; i<='Z'; i++){
		if(nameimage(w->i, w->name, 1) > 0)
			return;
		errstr(err, sizeof err);
		if(strcmp(err, "image name in use") != 0)
			break;
		w->name[n] = i;
		w->name[n+1] = 0;
	}
	w->name[0] = 0;
	fprint(2, "rio: setname failed: %s\n", err);
}

char* mtext[] = { "", "Text", "Media", "Debug", "Multi-Edit", "Narrow", nil, "Location",
  "Command", "Search", };

void wtagrepaint(Window *w) {
  Window *w1;
  Rectangle r1;
  Fmt f;
  Rune *rp;
  uint nr, n;
  char sfmt[128], *s;
  Point p;

  if (!w->title)
	return;
  w1 = w->title;
  r1 = w1->screenr;
  runefmtstrinit(&f);
  if (w1->input)
	fmtprint(&f, "%S", w1->input);
  else {
	fmtprint(&f, "%s", w->dir);
	if (w->file)
	  fmtprint(&f, "/%s", w->file);
  }

  if (w1->input) {
	switch(w->mode) {
	case M_CMD_HIST:
	case M_FILE_HIST:
	case M_SEARCH_HIST: s = mtext[w->mode]; break;
	default:
	  snprint(sfmt, sizeof(sfmt), "%S", w1->prompt);
	  s = sfmt;
	}
  } else {
	if (w->mode > M_INV) w->mode = M_TERM;
	s = mtext[w->mode];
  }

  wdelete(w1, 0, w1->nr);
  snprint(sfmt, sizeof(sfmt), "%s %s@%s", s, getenv("user"), getenv("sysname"));
  n = stringwidth(w->font, sfmt);
  draw(w1->i, r1, tagcols[BACK], nil, ZP); // Start a clean slate
  string(w1->i, Pt(r1.max.x - n - Selborder, r1.min.y + Selborder),
		 tagcols[TEXT], ZP, w->font, sfmt);

  rp = runefmtstrflush(&f);
  nr = runestrlen(rp);
  winsert(w1, rp, nr, 0);
  p  = frptofchar(&w1->Frame, nr);
  if (w->file) {
	/* fprint(2, "%d %d, ", w->fmodified, w1->active); */
	string(w1->i, p,
		   w->fmodified && !w1->active ? tagcols[TEXT] : tagcols[BACK],
		   ZP, w->font, "*");
  }
  if (!w1->active) frtick(&w1->Frame, p , 0);
  border(w1->i, r1, 1, w1->active ? display->black : tagcols[BORD], ZP);
  free(rp);
}

static Rectangle wtagresize(Window *w, Image *i) {
  Window *w1;
  Rectangle r, r1;

  if (w->title) {
	w1 = w->title;
	undoclear(w1->undo);
  } else {
	w1 = emalloc(sizeof(Window));
	w1->undo = undostackcreate(100);
	w->title = w1;
  }
  r1 = insetrect(i->r, Selborder);
  r1.max.y = r1.min.y + font->height + 2 * Selborder;
  r = insetrect(i->r, Selborder+1);
  r.min.y = r1.max.y;
  /* w1->i = allocwindow(wscreen, r1, Refnone, DNofill); */
  w1->i = i;
  w1->screenr = r1;
  /* draw(w1->i, r1, tagcols[BACK], nil, ZP); */
  frclear(&w1->Frame, FALSE);
  frinit(&w1->Frame, insetrect(r1, Selborder), w->font, w1->i, tagcols);

  return r;
}

void setmodified(Window *w, int v) {
  int repaint = 0;
  if (v != w->fmodified) repaint = 1;
  w->fmodified = v;
  /* Repaint AFTER setting flag  */
  if (repaint) wtagrepaint(w);
}
	
static void
wresize(Window *w, Image *i)
{
	Rectangle r;
	Image *im;

	w->i = i;
	w->mc.image = i;
	if (w->enable_title)
	  r = wtagresize(w, i);
	else
	  r = insetrect(i->r, Selborder+1);
	if (w->overlay) {
	  if(saved) {
		freeimage(saved);
		saved = nil;
	  }
	  im = w->overlay;
	  freeimage(w->overlay);
	  w->overlay = nil;
	  im = namedimage(display, "odebug");
	  if (im) nameimage(im, "odebug", 0);
	  im = namedimage(display, "omedia");
	  if (im) nameimage(im, "omedia", 0);
	}
	if (w->popup) {
	  w->popup->screenr = ZR;
	}
	w->scrollr = r;
	w->scrollr.max.x = r.min.x+Scrollwid;
	w->lastsr = ZR;
	r.min.x += Scrollwid+Scrollgap;
	frclear(&w->Frame, FALSE);
	frinit(&w->Frame, r, w->font, w->i, cols);
	wsetcols(w, w == input);
	w->maxtab = maxtab*stringwidth(w->font, "0");
	if(!w->mouseopen || !w->winnameread){
		r = insetrect(w->i->r, Selborder);
		draw(w->i, r, cols[BACK], nil, w->entire.min);
		wtagrepaint(w);
		wfill(w);
		wsetselect(w, w->q0, w->q1);
		wscrdraw(w);
	}
	if(w == input)
		wborder(w, Selborder);
	else
		wborder(w, Unselborder);
	flushimage(display, 1);
	wsetname(w);
	w->topped = ++topped;
	w->resized = TRUE;
	w->winnameread = FALSE;
	w->mc.buttons = 0;	/* avoid re-triggering clicks on resize */
	w->mouse.counter++;
	w->wctlready = 1;
}

void
wrepaint(Window *w)
{
	wsetcols(w, w == input);
	if(!w->mouseopen || !w->winnameread) {
		frredraw(w);
		/* Causes selection expansion */
		/* wcolordraw(w, w->org, w->nchars+w->org); */
	}
	if(w == input)
		wborder(w, Selborder);
	else
		wborder(w, Unselborder);
}

static void
wrefresh(Window *w)
{
	Rectangle r;

	if(w == input)
		wborder(w, Selborder);
	else
		wborder(w, Unselborder);
	r = insetrect(w->i->r, Selborder);
	draw(w->i, r, w->cols[BACK], nil, w->entire.min);
	wfill(w);
	w->ticked = 0;
	if(w->p0 > 0)
		frdrawsel(&w->Frame, frptofchar(&w->Frame, 0), 0, w->p0, 0);
	if(w->p1 < w->nchars)
		frdrawsel(&w->Frame, frptofchar(&w->Frame, w->p1), w->p1, w->nchars, 0);
	frdrawsel(&w->Frame, frptofchar(&w->Frame, w->p0), w->p0, w->p1, 1);
	w->lastsr = ZR;
	wscrdraw(w);
}

/*
 * Need to do this in a separate proc because if process we're interrupting
 * is dying and trying to print tombstone, kernel is blocked holding p->debug lock.
 */
void
interruptproc(void *v)
{
	int *notefd;

	notefd = v;
	write(*notefd, "interrupt", 9);
	close(*notefd);
	free(notefd);
}

Completion* completef(char *dir, char *s);

void
completeproc1(void *arg)
{
	Completejob *job;
	Completion *c;

	job = arg;
	threadsetname("namecomplete %s", job->dir);

	c = completef(job->dir, job->str);
	if(c != nil) {
	  c->force = job->force;
	  if (sendp(job->win->complete, c) <= 0)
		freecompletion(c);
	}

	wclose(job->win);

	/* free(job->dir); */
	free(job->str);
	free(job);
}

int
windfilewidth(Window *w, uint q0, int oneelement)
{
	uint q;
	Rune r;

	q = q0;
	while(q > 0){
		r = w->run[q-1];
		if(r<=' ' || r=='=' || r=='^' || r=='(' || r=='{')
			break;
		if(oneelement && r=='/')
			break;
		--q;
	}
	return q0-q;
}

void
namecomplete1(Window *w, int force)
{
	int nstr, npath;
	Rune *path, *str;
	char *dir, *root, *s;
	Completejob *job;
	Window *w1;

	w1 = w;
	if (w->title && w->title->active) {
	  w1 = w->title;
	}
	/* control-f: filename completion; works back to white space or / */
	if(w1->q0<w1->nr && w1->run[w1->q0]>' ')	/* must be at end of word */
		return;
	nstr = windfilewidth(w1, w1->q0, TRUE);
	str = w1->run+(w1->q0-nstr);
	/* npath = windfilewidth(w1, w1->q0-nstr, FALSE); */
	/* path = w1->run+(w1->q0-nstr-npath); */
	npath = windfilewidth(w1, w1->q0, FALSE);
	path = w1->run+(w1->q0-npath);
	if (!(nstr || npath || force)) return;

	/* is path rooted? if not, we need to make it relative to window path */
	s = runetobyte(path, npath, &npath);
	if(npath>0 && path[0]=='/') {
		dir = runetobyte(path, npath - nstr, &npath);
	} else {
		if(strcmp(w->dir, "") == 0)
			root = ".";
		else
			root = w->dir;
		/* dir = smprint("%s/%.*S", root, npath, path); */
		dir = root;
	}
	/* fprint(2, "-%s %s%d%d ", s, dir, nstr, npath); */
	if(dir == nil)
		return;

	/* run in background, winctl will collect the result on w->complete chan */
	job = emalloc(sizeof *job);
	/* job->str = runetobyte(str, nstr, &nstr); */
	job->str = s;
	job->dir = cleanname(dir);
	job->win = w;
	job->force = force;
	w->comp_len = nstr;
	incref(w);
	proccreate(completeproc, job, STACK);
}

void wclosepopup(Window *w) {
  if (w->popup) {
	freecompletion(w->c);
	w->c = nil;
	w = w->popup;
	/* Undo existing artifacts */
	wdelete(w, 0, w->nr);
	wclosewin(w);
  }
}

static Window* mkpopup(Window *w1) {
  Window *w;
  w = emalloc(sizeof(Window));
  w1->popup = w;
  w->screenr = insetrect(w1->i->r, 100);
  w->font = w1->font;
  return w;
}

void
showcandidates(Window *w1, Completion *c)
{
	int i, dx;
	Fmt f;
	Rune *rp;
	uint nr, qline;

	Window *w, *wi;
	Rectangle r;
	Point p;
	if (c->nmatch == 0 && !c->force)
	  return;

	if (w1->popup == nil) {
	  w = emalloc(sizeof(Window));
	  w1->popup = w;
	  r = insetrect(w1->i->r, 100);
	  w->font = w1->font;
	} else {
	  w = w1->popup;
	  if(eqrect(w->screenr, ZR))
		r = w->screenr = insetrect(w1->i->r, 100);
	  else
		r = w->screenr;
	  wclosepopup(w);
	}
	/* Try to place below the search string */
	wi = (w1->title && w1->title->active) ? w1->title : w1;
	if (w1->mode == M_FILE_HIST || w1->mode == M_CMD_HIST || w1->mode == M_SEARCH_HIST)
	  nr = wi->q0 - wi->org;
	else
	nr = windfilewidth(wi, wi->q0, TRUE);
	p = frptofchar(&wi->Frame, wi->q0 - nr - wi->org);
	if (w1->title && w1->title->active)
	  p.y = wi->screenr.max.y - w->font->height;

	runefmtstrinit(&f);
	if (c->nmatch == 0) {
	  qline = 1;
	  /* fmtprint(&f, "No matches in %d files\n", c->nfile); */
	  fmtprint(&f, "No matches\n");
	} else {
	  qline = min(c->nfile, 5);
	  for(i=0; i<c->nfile; i++){
		nr = strlen(c->filename[i]);
		if (nr > 44)
		  fmtprint(&f, "%.*s...\n", 44, c->filename[i]);
		else
		  fmtprint(&f, "%s\n", c->filename[i]);
	  }
	}
	rp = runefmtstrflush(&f);
	nr = runestrlen(rp);

	r.max.y = r.min.y + qline * w->font->height + 2;
	dx = Dx(r);
	if ((p.x + dx) > w1->i->r.max.x) {
	  /* Keep inside window */
	  p.x = r.min.x = w1->i->r.min.x + (Dx(w1->i->r) - dx) - 2 * Selborder;
	  r.max.x = r.min.x + dx;
	}
	if ((p.y + Dy(r) + w->font->height) > w1->i->r.max.y) {
	  /* Above the line */
	  p = subpt(p, Pt(r.min.x, r.max.y));
	  r = rectaddpt(r, p);
	} else {
	  /* Below the line */
	  p = addpt(p, Pt(0, w->font->height));
	  r = rectaddpt(r, subpt(p, r.min));
	}
	freeimage(w->i);
	w->i = allocwindow(wscreen, r, Refnone, DNofill);
	w->screenr = r;
	/* draw(w->i, r, cols[BACK], nil, frptofchar(&w1->Frame, w1->q0)); */
	border(w->i, r, 1, cols[BORD], ZP);
	frclear(&w->Frame, FALSE);
	frinit(&w->Frame, insetrect(r, 1), w->font, w->i, cols);

	winsert(w, rp, nr, 0);
	/* Highlight */
	int q0 = w->org + frcharofpt(&w->Frame, Pt(w->Frame.r.min.x, w->Frame.r.min.y + w->font->height));
	wsetselect(w, 0, q0);
	free(rp);
}

static void
showcandidates1(Window *w, Completion *c)
{
	int i;
	Fmt f;
	Rune *rp;
	uint nr, qline;
	char *s;

	runefmtstrinit(&f);
	if (c->nmatch == 0)
		s = "[no matches in ";
	else
		s = "[";
	if(c->nfile > 32)
		fmtprint(&f, "%s%d files]\n", s, c->nfile);
	else{
		fmtprint(&f, "%s", s);
		for(i=0; i<c->nfile; i++){
			if(i > 0)
				fmtprint(&f, " ");
			fmtprint(&f, "%s", c->filename[i]);
		}
		fmtprint(&f, "]\n");
	}
	rp = runefmtstrflush(&f);
	nr = runestrlen(rp);

	/* place text at beginning of line before cursor and host point */
	qline = min(w->qh, w->q0);
	while(qline>0 && w->run[qline-1] != '\n')
		qline--;

	if(qline == w->qh){
		/* advance host point to avoid readback */
		w->qh = winsert(w, rp, nr, qline)+nr;
	} else {
		winsert(w, rp, nr, qline);
	}
	free(rp);
}

int
wbswidth(Window *w, Rune c)
{
	uint q, eq, stop;
	Rune r;
	int skipping;

	/* there is known to be at least one character to erase */
	if(c == 0x08)	/* ^H: erase character */
		return 1;
	q = w->q0;
	stop = 0;
	if(q > w->qh)
		stop = w->qh;
	skipping = TRUE;
	while(q > stop){
		r = w->run[q-1];
		if(r == '\n'){		/* eat at most one more character */
			if(q == w->q0)	/* eat the newline */
				--q;
			break; 
		}
		if(c == 0x17){
			eq = isalnum(r);
			if(eq && skipping)	/* found one; stop skipping */
				skipping = FALSE;
			else if(!eq && !skipping)
				break;
		}
		--q;
	}
	return w->q0-q;
}

void
wsetorigin(Window *w, uint org, int exact)
{
	int i, a, fixup;
	Rune *r;
	uint n;

	if(org>0 && !exact){
		/* org is an estimate of the char posn; find a newline */
		/* don't try harder than 256 chars */
		for(i=0; i<256 && org<w->nr; i++){
			if(w->run[org] == '\n'){
				org++;
				break;
			}
			org++;
		}
	}
	a = org-w->org;
	fixup = 0;
	if(a>=0 && a<w->nchars){
		frdelete(&w->Frame, 0, a);
		fixup = 1;	/* frdelete can leave end of last line in wrong selection mode; it doesn't know what follows */
	}else if(a<0 && -a<w->nchars){
		n = w->org - org;
		r = w->run+org;
		frinsert(&w->Frame, r, r+n, 0);
	}else
		frdelete(&w->Frame, 0, w->nchars);
	w->org = org;
	wfill(w);
	wscrdraw(w);
	wsetselect(w, w->q0, w->q1);
	if(fixup && w->p1 > w->p0)
		frdrawsel(&w->Frame, frptofchar(&w->Frame, w->p1-1), w->p1-1, w->p1, 1);
}

uint
wbacknl(Window *w, uint p, uint n)
{
	int i, j;

	/* look for start of this line if n==0 */
	if(n==0 && p>0 && w->run[p-1]!='\n')
		n = 1;
	i = n;
	while(i-->0 && p>0){
		--p;	/* it's at a newline now; back over it */
		if(p == 0)
			break;
		/* at 128 chars, call it a line anyway */
		for(j=128; --j>0 && p>0; p--)
			if(w->run[p-1]=='\n')
				break;
	}
	return p;
}

char*
wcontents(Window *w, int *ip)
{
	return runetobyte(w->run, w->nr, ip);
}

void
wshow(Window *w, uint q0)
{
	int qe;
	int nl;
	uint q;

	qe = w->org+w->nchars;
	if(w->org<=q0 && (q0<qe || (q0==qe && qe==w->nr)))
		wscrdraw(w);
	else{
		nl = 4*w->maxlines/5;
		q = wbacknl(w, q0, nl);
		/* avoid going backwards if trying to go forwards - long lines! */
		if(!(q0>w->org && q<w->org))
			wsetorigin(w, q, TRUE);
		while(q0 > w->org+w->nchars)
			wsetorigin(w, w->org+1, FALSE);
	}
}

void
wsnarf(Window *w)
{
	if(w->q1 == w->q0)
		return;
	nsnarf = w->q1-w->q0;
	snarf = runerealloc(snarf, nsnarf);
	snarfversion++;	/* maybe modified by parent */
	runemove(snarf, w->run+w->q0, nsnarf);
	putsnarf();
}

void
wsend(Window *w)
{
	getsnarf();
	wsnarf(w);
	if(nsnarf == 0)
		return;
	if(w->rawing){
		waddraw(w, snarf, nsnarf);
		if(snarf[nsnarf-1]!='\n' && snarf[nsnarf-1]!='\004')
			waddraw(w, L"\n", 1);
	}else{
		winsert(w, snarf, nsnarf, w->nr);
		if(snarf[nsnarf-1]!='\n' && snarf[nsnarf-1]!='\004')
			winsert(w, L"\n", 1, w->nr);
	}
	wsetselect(w, w->nr, w->nr);
	wshow(w, w->nr);
}

void
wdelete(Window *w, uint q0, uint q1)
{
	uint p0, p1, flag;
	int m, n, a;

	n = q1-q0;
	if(n == 0)
		return;

	if (w->nr == 0) { w->q0 = w->q1 = w->nr; return; }
	if (w->file) setmodified(w, 1);
	m = flag = 0;
	if (w->mode == M_MULTI_EDIT) {
	  /* Avoid deletion that happens at the end */
	  flag = 1;
	  a = abs(n);
	  /* Since we can't delete at pos 0, we accumulate total chars in m */
	  for (int i = 0; i < w->nm; i++) {
		w->mq[i] -= m; 
		if (w->mq[i] >= a && w->mq[i] <= w->nr) {
		  /* fprint(2, "%d %d %d %d,", w->mq[i], q0, m, w->nr); */
		  if (n > 0) {
			p0 = w->mq[i] - n;
			p1 = w->mq[i];
			w->mq[i] -= n; 
		  } else {
			p0 = w->mq[i];
			p1 = w->mq[i] - n;
		  }
		  undoadddelete(w->undo, w->run + p0, a, p0);
		  runemove(w->run + p0, w->run + p1, w->nr - p1);
		  if(p0 >= w->org && p0 <= w->org+w->nchars)
			frdelete(&w->Frame, p0, p1);
		  if (w->q0 > p0) w->q0 -= a;
		  if (w->qh > p1) w->qh -= a;
		  m += a;
		  w->nr -= a;
		}
	  }
	} else {
	undoadddelete(w->undo, w->run + q0, n, q0);
	runemove(w->run+q0, w->run+q1, w->nr-q1);
	w->nr -= n;
	}
	spansadjust(w, q0, -n);
	if(q0 < w->q0)
		w->q0 -= min(n, w->q0-q0);
	if(q0 < w->q1)
		w->q1 -= min(n, w->q1-q0);
	if (!flag) {
	if(q1 < w->qh)
		w->qh -= n;
	else if(q0 < w->qh)
		w->qh = q0;
	}
	if(q1 <= w->org)
		w->org -= n;
	else if(!flag && q0 < w->org+w->nchars){
		p1 = q1 - w->org;
		if(p1 > w->nchars)
			p1 = w->nchars;
		if(q0 < w->org){
			w->org = q0;
			p0 = 0;
		}else
			p0 = q0 - w->org;
		frdelete(&w->Frame, p0, p1);
		wfill(w);
	}
}

void
wcut(Window *w)
{
	if(w->q1 == w->q0)
		return;
	wdelete(w, w->q0, w->q1);
	wsetselect(w, w->q0, w->q0);
}

void
wpaste(Window *w)
{
	uint q0;

	if(nsnarf == 0)
		return;
	wcut(w);
	q0 = w->q0;
	if(w->rawing && q0==w->nr){
		waddraw(w, snarf, nsnarf);
		wsetselect(w, q0, q0);
	}else{
		q0 = winsert(w, snarf, nsnarf, w->q0);
		q0 += nsnarf;
		wsetselect(w, q0, q0);
	}
}

int
cirunestrncmp(Rune *s1, Rune *s2, long n)
{
    Rune r1, r2;

    for(; n > 0; n--) {
        r1 = *s1++;
        r2 = *s2++;
        r1 = tolowerrune(r1);
        r2 = tolowerrune(r2);

        if(r1 != r2)
            return r1 - r2;
        if(r1 == 0)
            return 0;
    }
    return 0;
}

void
wlook(Window *w, Rune *s)
{
	int i, n, e, start;

	if (s) {
		i = w->q0;
		n = runestrlen(s);
	} else {
		i = w->q1;
		n = w->q1 - w->q0;
		s = w->run + w->q0;
	}

	if(n <= 0 || w->nr < n)
		return;

	e = w->nr - n;
	if(i > e)
		i = 0;

	start = i;
	do {
		if(cirunestrncmp(s, w->run + i, n) == 0) {
			wsetselect(w, i, i + n);
			wshow(w, i);
			return;
		}
		if(++i > e)
			i = 0;
	} while(i != start);
}

void
wrlook(Window *w)
{
	int i, n, e;

	i = w->q1;
	n = i - w->q0;
	e = w->nr - n;
	if(n <= 0 || e < n)
		return;

	i = w->q0 - 1;
	while(runestrncmp(w->run+w->q0, w->run + i, n) != 0){
		if(i > 0)
			i--;
		else
			i = e;
	}

	wsetselect(w, i, i+n);
	wshow(w, i);
}

/*
 * Parse possibly-quoted token from s.
 * Returns pointer to first non-space after token.
 * Sets *tok and *toklen to token bounds.
 */
/*
 * parsetokrune:
 *   Parse one token from rune array r.
 *   Handles '...' or "..." quoting, skips whitespace, stops at newline.
 *
 *   Input:
 *     r  = pointer to Rune buffer (null or 0-terminated)
 *     tok = output start pointer (into same buffer)
 *     toklen = output length in runes
 *
 *   Returns pointer to first rune after the token.
 */
Rune*
parsetok(Rune *r, Rune **tok, int *toklen)
{
	Rune *p;
	Rune quote = 0;

	/* skip leading spaces and tabs */
	while(*r == ' ' || *r == '\t')
		r++;

	if(*r == '\'' || *r == '"'){
		quote = *r++;
		*tok = r;
		for(p = r; *p != 0; p++){
			if(*p == quote)
				break;
			if(*p == '\n' || *p == '\r')
				break;  /* stop on newline inside quote */
		}
		*toklen = p - *tok;
		if(*p == quote)
			p++;  /* skip closing quote */
	} else {
		*tok = r;
		for(p = r; *p != 0; p++){
			if(*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
				break;
		}
		*toklen = p - *tok;
	}

	/* skip trailing whitespace / newline */
	while(*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
		p++;

	return p;
}


void
wplumb(Window *w)
{
	Plumbmsg *m;
	static int fd = -2;
	char buf[32];
	uint p0, p1;
	Cursor *c;

	if(fd == -2)
		fd = plumbopen("send", OWRITE|OCEXEC);
	if(fd < 0)
		return;
	m = emalloc(sizeof(Plumbmsg));
	m->src = estrdup("rio");
	m->dst = nil;
	m->wdir = estrdup(w->dir);
	m->type = estrdup("text");
	p0 = w->q0;
	p1 = w->q1;
	if((w->q1 > w->q0) && (w->run[p0] == '\'' || w->run[p0] == '"')) {
		/* Quoted string */
		/* Rune r[256]; */
		Rune *tok, *p, *r1;
		int len;

		/* runestrcpy(r, L"'a b.mp3'  \"c d.ogg\"\n"); */
		/* p = r; */

		len = p1 - p0;
		r1 = malloc((len + 1) * sizeof(Rune));
		runestrncpy(r1, w->run + p0, len);
		p = r1;
		while(*p){
		  p = parsetok(p, &tok, &len);
		  if(len == 0)
			break;
		  /* fprint(2, "token: %d %.*S\n", len, len, tok); */
		  m->data = runetobyte(tok, len, &m->ndata);
		  plumbsend(fd, m);
		}
		p1 = p0 + messagesize; // Force skipping
		free(r1);
		m->attr = nil;
	} else{
		while(p0>0 && w->run[p0-1]!=' ' && w->run[p0-1]!='\t' && w->run[p0-1]!='\n')
			p0--;
		while(p1<w->nr && w->run[p1]!=' ' && w->run[p1]!='\t' && w->run[p1]!='\n')
			p1++;
		/* snprint(buf, sizeof(buf), "click=%d", w->q0-p0); */
		/* m->attr = plumbunpackattr(buf); */
	}
	if(p1-p0 > messagesize-1024){
		plumbfree(m);
		return;	/* too large for 9P */
	}
	m->data = runetobyte(w->run+p0, p1-p0, &m->ndata);
	if(plumbsend(fd, m) < 0){
	  /* % indicates the command prompt */
	  if (w->run[p0] != '%') {
		c = lastcursor;
		riosetcursor(&query);
		sleep(300);
		riosetcursor(c);
	  } else {
		/* Plumbing output goes to the window running the plumber
		   process. This code ensures that output is added in the
		   current window. */
		p0++;
		Rune s = '\n';
		winsert(w, &s, 1, p1);
		w->qh = p0;
		w->qh1 = p1;
		wsetselect(w, p0, p1);
	  }
	}
	plumbfree(m);
}

void
wkeyctl(Window *w, Rune r);

static void
wkeyctl1(Window *w, Rune r)
{
	uint q0 ,q1;
	int n, nb;
	int *notefd;

	switch(r){
	case 0:
	case Kcaps:
	case Knum:
	case Kshift:
	case Kalt:
	case Kctl:
	case Kaltgr:
		return;
	}

	if(w->i==nil)
		return;
	/* navigation keys work only when mouse and kbd is not open */
	if(!w->mouseopen)
		switch(r){
		case Kdown:
			n = shiftdown ? 1 : w->maxlines/3;
			goto case_Down;
		case Kscrollonedown:
			n = mousescrollsize(w->maxlines);
			if(n <= 0)
				n = 1;
			goto case_Down;
		case Kpgdown:
			n = 2*w->maxlines/3;
		case_Down:
			q0 = w->org+frcharofpt(&w->Frame, Pt(w->Frame.r.min.x, w->Frame.r.min.y+n*w->font->height));
			wsetorigin(w, q0, TRUE);
			return;
		case Kup:
			n = shiftdown ? 1 : w->maxlines/3;
			goto case_Up;
		case Kscrolloneup:
			n = mousescrollsize(w->maxlines);
			if(n <= 0)
				n = 1;
			goto case_Up;
		case Kpgup:
			n = 2*w->maxlines/3;
		case_Up:
			q0 = wbacknl(w, w->org, n);
			wsetorigin(w, q0, TRUE);
			return;
		case Kleft:
			if(w->q0 > 0){
				q0 = w->q0-1;
				wsetselect(w, q0, q0);
				wshow(w, q0);
			}
			return;
		case Kright:
			if(w->q1 < w->nr){
				q1 = w->q1+1;
				wsetselect(w, q1, q1);
				wshow(w, q1);
			}
			return;
		case Khome:
			wshow(w, 0);
			return;
		case Kend:
			wshow(w, w->nr);
			return;
		case Kscroll:
			w->scrolling ^= 1;
			wshow(w, w->nr);
			return;
		case Ksoh:	/* ^A: beginning of line */
			if(w->q0==0 || w->q0==w->qh || w->run[w->q0-1]=='\n')
				return;
			nb = wbswidth(w, 0x15 /* ^U */);
			wsetselect(w, w->q0-nb, w->q0-nb);
			wshow(w, w->q0);
			return;
		case Kenq:	/* ^E: end of line */
			q0 = w->q0;
			while(q0 < w->nr && w->run[q0]!='\n')
				q0++;
			wsetselect(w, q0, q0);
			wshow(w, w->q0);
			return;
		case Kstx:	/* ^B: output point */
			wsetselect(w, w->qh, w->qh);
			wshow(w, w->q0);
			return;
		}
	if(w->rawing && (w->q0==w->nr || w->mouseopen)){
		waddraw(w, &r, 1);
		return;
	}
	if(r==Kesc || (w->holding && r==Kdel)){	/* toggle hold */
		if(w->holding)
			--w->holding;
		else
			w->holding++;
		wsetcursor(w, FALSE);
		wrepaint(w);
		if(r == Kesc)
			return;
	}
	if(r != Kdel){
		wsnarf(w);
		wcut(w);
	}
	switch(r){
	case Kdel:	/* send interrupt */
		w->qh = w->nr;
		wshow(w, w->qh);
		if(w->notefd < 0)
			return;
		notefd = emalloc(sizeof(int));
		*notefd = dup(w->notefd, -1);
		proccreate(interruptproc, notefd, 4096);
		return;
	case Kack:	/* ^F: file name completion */
	case Kins:	/* Insert: file name completion */
		namecomplete(w, 0);
		return;
	case Kbs:	/* ^H: erase character */
	case Knack:	/* ^U: erase line */
	case Ketb:	/* ^W: erase word */
		if(w->q0==0 || w->q0==w->qh)
			return;
		nb = wbswidth(w, r);
		q1 = w->q0;
		q0 = q1-nb;
		if(q0 < w->org){
			q0 = w->org;
			nb = q1-q0;
		}
		if(nb > 0){
			wdelete(w, q0, q0+nb);
			wsetselect(w, q0, q0);
		}
		return;
	}
	/* otherwise ordinary character; just insert */
	q0 = w->q0;
	q0 = winsert(w, &r, 1, q0);
	wshow(w, q0+1);
}

static Window	*clickwin;
static uint	clickmsec;
static Point	clickpt;
static uint	clickcount;
static Window	*selectwin;
static uint	selectq;

static void
wframescroll(Window *w, int dl)
{
	uint q0;

	if(dl == 0){
		wscrsleep(w, 100);
		return;
	}
	if(dl < 0){
		q0 = wbacknl(w, w->org, -dl);
		if(selectq > w->org+w->p0)
			wsetselect(w, w->org+w->p0, selectq);
		else
			wsetselect(w, selectq, w->org+w->p0);
	}else{
		if(w->org+w->nchars == w->nr)
			return;
		q0 = w->org+frcharofpt(&w->Frame, Pt(w->Frame.r.min.x, w->Frame.r.min.y+dl*w->font->height));
		if(selectq >= w->org+w->p1)
			wsetselect(w, w->org+w->p1, selectq);
		else
			wsetselect(w, selectq, w->org+w->p1);
	}
	wsetorigin(w, q0, TRUE);
}

/*
 * called from frame library
 */
static void
framescroll(Frame *f, int dl)
{
	if(f != &selectwin->Frame)
		error("frameselect not right frame");
	wframescroll(selectwin, dl);
}

static Rune left1[] =  { L'{', L'[', L'(', L'<', L'«', 0 };
static Rune right1[] = { L'}', L']', L')', L'>', L'»', 0 };
static Rune left2[] =  { L'\n', 0 };
static Rune left3[] =  { L'\'', L'"', L'`', 0 };

static Rune *left[] = {
	left1,
	left2,
	left3,
	nil
};
static Rune *right[] = {
	right1,
	left2,
	left3,
	nil
};

static int
wclickmatch(Window *w, int cl, int cr, int dir, uint *q)
{
	Rune c;
	int nest;

	nest = 1;
	for(;;){
		if(dir > 0){
			if(*q == w->nr)
				break;
			c = w->run[*q];
			(*q)++;
		}else{
			if(*q == 0)
				break;
			(*q)--;
			c = w->run[*q];
		}
		if(c == cr){
			if(--nest==0)
				return 1;
		}else if(c == cl)
			nest++;
	}
	return cl=='\n' && nest==1;
}

static int
inmode(Rune r, int mode)
{
	return (mode == 1) ? isalnum(r) : r && !isspace(r);
}

static void
wstretchsel(Window *w, uint pt, uint *q0, uint *q1, int mode)
{
	int c, i, rc, lc;
	Rune *r, *l, *p;
	uint q;

	if(mode){
		lc = *q0 > 0     ? w->run[*q0-1] : '\n';
		rc = *q1 < w->nr ? w->run[*q1]   : '\n';
		for(i=0; left[i]; i++){
			l = left[i];
			r = right[i];
			p = strrune(l, lc);
			if(p && r[p-l] == rc){
				*q0 -= *q0 > 0 && lc != '\n';
				*q1 += *q1 < w->nr;
				return;
			}
		}
	}

	*q0 = pt;
	*q1 = pt;
	for(i=0; left[i]!=nil; i++){
		q = *q0;
		l = left[i];
		r = right[i];
		/* try matching character to left, looking right */
		if(q == 0)
			c = '\n';
		else
			c = w->run[q-1];
		p = strrune(l, c);
		if(p != nil){
			if(wclickmatch(w, c, r[p-l], 1, &q))
				*q1 = q-(c!='\n');
			return;
		}
		/* try matching character to right, looking left */
		if(q == w->nr)
			c = '\n';
		else
			c = w->run[q];
		p = strrune(r, c);
		if(p != nil){
			if(wclickmatch(w, c, l[p-r], -1, &q)){
				*q1 = *q0+(*q0<w->nr && c=='\n');
				*q0 = q;
				if(c!='\n' || q!=0 || w->run[0]=='\n')
					(*q0)++;
			}
			return;
		}
	}
	/* try filling out word to right */
	while(*q1<w->nr && inmode(w->run[*q1], mode))
		(*q1)++;
	/* try filling out word to left */
	while(*q0>0 && inmode(w->run[*q0-1], mode))
		(*q0)--;
}

int isdoubleclick(Mouse mc)
{
	int dx, dy;

	dx = abs(clickpt.x - mc.xy.x);
	dy = abs(clickpt.y - mc.xy.y);
	clickpt = mc.xy;
	clickcount++;
	/* fprint(2, "%d%d%d%d ", mc.msec-clickmsec >= 500, clickcount, dx > 3, dy > 3); */
	if(mc.msec-clickmsec >= 500 || clickcount > 3 || dx > 3 || dy > 3)
		clickcount = 0;
	clickmsec = mc.msec;
	return (clickcount >= 1);
}

static void
wselect(Window *w)
{
	uint q0, q1;
	int b, x, y, dx, dy, mode, first;

	first = 1;
	selectwin = w;
	w->preview = 0;
	if (w->title) w->title->active = 0;
	/* wtagrepaint(w); */
	/*
	 * Double-click immediately if it might make sense.
	 */
	b = w->mc.buttons;
	q0 = w->q0;
	q1 = w->q1;
	dx = abs(clickpt.x - w->mc.xy.x);
	dy = abs(clickpt.y - w->mc.xy.y);
	clickpt = w->mc.xy;
	selectq = w->org+frcharofpt(&w->Frame, w->mc.xy);
	clickcount++;
	if(w->mc.msec-clickmsec >= 500 || clickwin != w || clickcount > 3 || dx > 3 || dy > 3)
		clickcount = 0;
	if(clickwin == w && clickcount >= 1 && w->mc.msec-clickmsec < 500){
	if (ptinrect(w->mc.xy, w->title->screenr)) {
	  w->title->active = 1;
	  wtagrepaint(w);
	}
		mode = (clickcount > 2) ? 2 : clickcount;
		wstretchsel(w, selectq, &q0, &q1, mode);
		wsetselect(w, q0, q1);
		x = w->mc.xy.x;
		y = w->mc.xy.y;
		/* stay here until something interesting happens */
		while(1){
			readmouse(&w->mc);
			dx = abs(w->mc.xy.x-x);
			dy = abs(w->mc.xy.y-y);
			if(w->mc.buttons != b || dx >= 3 && dy >= 3)
				break;
			clickcount++;
			clickmsec = w->mc.msec;
		}
		w->mc.xy.x = x;	/* in case we're calling frselect */
		w->mc.xy.y = y;
		q0 = w->q0;	/* may have changed */
		q1 = w->q1;
		selectq = w->org+frcharofpt(&w->Frame, w->mc.xy);
	}
	if(w->mc.buttons == b && clickcount == 0){
		w->scroll = framescroll;
		frselect(&w->Frame, &w->mc);
		/* horrible botch: while asleep, may have lost selection altogether */
		if(selectq > w->nr)
			selectq = w->org + w->p0;
		w->Frame.scroll = nil;
		if(selectq < w->org)
			q0 = selectq;
		else
			q0 = w->org + w->p0;
		if(selectq > w->org+w->nchars)
			q1 = selectq;
		else
			q1 = w->org+w->p1;
	}
	if(q0 == q1){
		mode = (clickcount > 2) ? 2 : clickcount;
		if(q0==w->q0 && clickwin==w && w->mc.msec-clickmsec<500)
			wstretchsel(w, selectq, &q0, &q1, mode);
		else
			clickwin = w;
		clickmsec = w->mc.msec;
	}
	/* fprint(2, "%d %d", q0, q1); */
	if (mod & Mctl) {
	  w->q0 = q0;
	  multimark(w, q0);
	  drawmmark(w, 0, 0);
	} else {
	  if (w->mode == M_MULTI_EDIT) {
		w->nm = 0;
		w->mode = M_TERM;
		wrepaint(w);
		undoboundary(w->undo);
	  }

	wsetselect(w, q0, q1);
	while(w->mc.buttons){
		w->mc.msec = 0;
		b = w->mc.buttons;
		if(b & 6){
			if(b & 2){
				wsnarf(w);
				wcut(w);
			}else{
				if(first){
					first = 0;
					getsnarf();
				}
				wpaste(w);
			}
		}
		wscrdraw(w);
		while(w->mc.buttons == b)
			readmouse(&w->mc);
		if(w->mc.msec-clickmsec >= 500)
			clickwin = nil;
	}
	}
	wtagrepaint(w); // Refresh mode status
}

/*
 * Convert back to physical coordinates
 */
static void
wmovemouse(Window *w, Point p)
{
	if(w != input || menuing || sweeping)
		return;
	p.x += w->screenr.min.x-w->i->r.min.x;
	p.y += w->screenr.min.y-w->i->r.min.y;
	moveto(mousectl, p);
}


Window*
wmk(Image *i, Mousectl *mc, Channel *ck, Channel *cctl, int scrolling)
{
	static int id;

	Window *w;
	Rectangle r;

	w = emalloc(sizeof(Window));
	w->screenr = i->r;
	w->font = font;
	w->i = i;
	w->mc = *mc;
	w->ck = ck;
	w->cctl = cctl;
	w->cursorp = nil;
	w->popup = nil; //mkpopup(w);
	w->title = nil;//emalloc(sizeof(Window));
	w->dir = estrdup(startdir);
	w->label = estrdup("<unnamed>");
	w->preview = 1;
	w->file = nil;
	w->overlay = nil;
	w->mode = M_TERM;
	w->undo = w->scratch = undostackcreate(100);
	w->hfile = undostackcreate(100);
	w->hcmd = undostackcreate(100);
	w->hsearch = undostackcreate(100);
	w->enable_title = title;
	w->title = nil;
	if (w->enable_title)
	  r = wtagresize(w, i);
	else
	  r = insetrect(i->r, Selborder+1);
	w->conswrite = chancreate(sizeof(Conswritemesg), 0);
	w->consread =  chancreate(sizeof(Consreadmesg), 0);
	w->kbdread =  chancreate(sizeof(Consreadmesg), 0);
	w->mouseread =  chancreate(sizeof(Mousereadmesg), 0);
	w->wctlread =  chancreate(sizeof(Consreadmesg), 0);
	w->complete = chancreate(sizeof(Completion*), 0);
	w->gone = chancreate(sizeof(char*), 0);
	w->scrollr = r;
	w->scrollr.max.x = r.min.x+Scrollwid;
	w->lastsr = ZR;
	r.min.x += Scrollwid+Scrollgap;
	frinit(&w->Frame, r, font, i, cols);
	w->maxtab = maxtab*stringwidth(font, "0");
	w->topped = ++topped;
	w->id = ++id;
	w->notefd = -1;
	w->scrolling = scrolling;
	r = insetrect(w->i->r, Selborder);
	draw(w->i, r, cols[BACK], nil, w->entire.min);
	wtagrepaint(w);
	wborder(w, Selborder);
	wscrdraw(w);
	threadcreate(openproc, w, STACK);
	incref(w);	/* ref will be removed after mounting; avoids delete before ready to be deleted */
	return w;
}

void
wclosewin(Window *w)
{
	Image *i = w->i;
	if(i == nil)
		return;
	w->i = nil;
	/* move it off-screen to hide it, in case client is slow in letting it go */
	originwindow(i, i->r.min, view->r.max);
	freeimage(i);
}

static void
wclunk(Window *w)
{
	int i;

	if(w->deleted)
		return;
	w->deleted = TRUE;
	if(w == input){
		input = nil;
		riosetcursor(nil);
	}
	if(w == wkeyboard)
		wkeyboard = nil;
	for(i=0; i<nhidden; i++)
		if(hidden[i] == w){
			--nhidden;
			memmove(hidden+i, hidden+i+1, (nhidden-i)*sizeof(hidden[0]));
			break;
		}
	for(i=0; i<nwindow; i++)
		if(window[i] == w){
			--nwindow;
			memmove(window+i, window+i+1, (nwindow-i)*sizeof(window[0]));
			break;
		}
}

int
wclose(Window *w)
{
	int i;

	i = decref(w);
	if(i > 0)
		return 0;
	if(i < 0)
		error("negative ref count");
	wclunk(w);
	/* wsendctlmesg(w->popup, Exited, ZR, nil); */
	/* wsendctlmesg(w->title, Exited, ZR, nil); */
	wsendctlmesg(w, Exited, ZR, nil);
	return 1;
}

void
wsendctlmesg(Window *w, int type, Rectangle r, void *p)
{
	Wctlmesg wcm;

	wcm.type = type;
	wcm.r = r;
	wcm.p = p;
	send(w->cctl, &wcm);
}

static int
wctlmesg(Window *w, int m, Rectangle r, void *p)
{
	Image *i = p;
	Window *w1;

	switch(m){
	default:
		error("unknown control message");
		break;
	case Wakeup:
		break;
	case Reshaped:
		if(w->deleted){
			freeimage(i);
			break;
		}
		w->screenr = r;
		wclosewin(w);
		wresize(w, i);
		wsetcursor(w, FALSE);
		break;
	case Topped:
		if(w->deleted)
			break;
		w->wctlready = 1;
		wsetcursor(w, FALSE);
		/* fall thrugh for redraw after input change */
	case Repaint:
		if(p != nil){
			/* sync with input change from wcurrent()/wuncurrent() */
			Channel *c = p;
			input = recvp(c);

			/* when we lost input, release mouse and keyboard buttons */
			if(w->mc.buttons){
				w->mc.buttons = 0;
				w->mouse.counter++;
			}
			w->keyup = w->kbdopen;
			w->wctlready = 1;

			sendp(c, w);
		}
		if(w->i==nil || Dx(w->screenr)<=0)
			break;
		wrepaint(w);
		flushimage(display, 1);
		break;
	case Refresh:
		if(w->i==nil || Dx(w->screenr)<=0)
			break;
		wrefresh(w);
		wtagrepaint(w);
		flushimage(display, 1);
		break;
	case Movemouse:
		if(w->i==nil || Dx(w->screenr)<=0 || !ptinrect(r.min, w->i->r))
			break;
		wmovemouse(w, r.min);
	case Rawon:
		break;
	case Rawoff:
		while(w->nraw > 0){
			wkeyctl(w, w->raw[0]);
			--w->nraw;
			runemove(w->raw, w->raw+1, w->nraw);
		}
		break;
	case Holdon:
	case Holdoff:
		if(w->i==nil)
			break;
		wsetcursor(w, FALSE);
		wrepaint(w);
		flushimage(display, 1);
		break;
	case Truncate:
		wdelete(w, 0, w->nr);
		break;
	case Deleted:
		wclunk(w);
		if(w->notefd >= 0)
			write(w->notefd, "hangup", 6);
		wclosewin(w);
		flushimage(display, 1);
		break;
	case Exited:
	  if (w1 = w->popup) {
	  wclosewin(w1);
	  frclear(&w1->Frame, TRUE);
	  free(w1);
	  }

	  if (w1 = w->title) {
	  /* wclosewin(w1); */
	  frclear(&w1->Frame, TRUE);
	  free(w1);
	  }

		wclosewin(w);
		frclear(&w->Frame, TRUE);
		flushimage(display, 1);
		if(w->notefd >= 0)
			close(w->notefd);
		undostackfree(w->undo);
		undostackfree(w->hfile);
		undostackfree(w->hcmd);
		chanfree(w->mc.c);
		chanfree(w->ck);
		chanfree(w->cctl);
		chanfree(w->conswrite);
		chanfree(w->consread);
		chanfree(w->mouseread);
		chanfree(w->wctlread);
		chanfree(w->kbdread);
		chanfree(w->complete);
		chanfree(w->gone);
		free(w->raw);
		free(w->run);
		free(w->mq);
		free(w->buttons);
		freespan(w);
		/* free(w->file); */
		/* free(w->dir); */
		/* free(w->label); */
		free(w);
		break;
	}
	return m;
}

static void
wmousectl(Window *w)
{
	int but;

	for(but=1;; but++){
		if(but > 5)
			return;
		if(w->mc.buttons == 1<<(but-1))
			break;
	}

	incref(w);		/* hold up window while we track */
	if(w->i != nil){
		if(shiftdown && but > 3)
			wkeyctl(w, but == 4 ? Kscrolloneup : Kscrollonedown);
		else if(ptinrect(w->mc.xy, w->scrollr) || (but > 3))
			wscroll(w, but);
		else if(but == 1) {
		  if (w->hovering)
			oaction(w);
		  else
			wselect(w);
		}
	}
	wclose(w);
}

void
winctl(void *arg)
{
	Rune *rp, *up, r;
	uint qh, q0;
	int nr, nb, c, wid, i, npart, initial, lastb, skip, winit, select, israw;
	char *s, *t, part[3];
	Window *w;
	Mousestate *mp, m;
	enum { WKbd, WKbdread, WMouse, WMouseread, WCtl, WCwrite, WCread, WWread, WComplete,
	  Wgone, NWALT };
	Alt alts[NWALT+1];
	Consreadmesg crm;
	Mousereadmesg mrm;
	Conswritemesg cwm;
	Stringpair pair, cmd;
	Wctlmesg wcm;
	Completion *cr;
	/* Timer *timer; */
	char *kbdq[32], *kbds;
	uint kbdqr, kbdqw;

	w = arg;
	threadsetname("winctl-id%d", w->id);

	mrm.cm = chancreate(sizeof(Mouse), 0);
	crm.c1 = chancreate(sizeof(Stringpair), 0);
	crm.c2 = chancreate(sizeof(Stringpair), 0);
	cwm.cw = chancreate(sizeof(Stringpair), 0);
	/* timer = timerstart(IDLE_TRIGGER); */
	
	alts[WKbd].c = w->ck;
	alts[WKbd].v = &kbds;
	alts[WKbd].op = CHANRCV;
	alts[WKbdread].c = w->kbdread;
	alts[WKbdread].v = &crm;
	alts[WKbdread].op = CHANSND;
	alts[WMouse].c = w->mc.c;
	alts[WMouse].v = (Mouse*)&w->mc;
	alts[WMouse].op = CHANRCV;
	alts[WMouseread].c = w->mouseread;
	alts[WMouseread].v = &mrm;
	alts[WMouseread].op = CHANSND;
	alts[WCtl].c = w->cctl;
	alts[WCtl].v = &wcm;
	alts[WCtl].op = CHANRCV;
	alts[WCwrite].c = w->conswrite;
	alts[WCwrite].v = &cwm;
	alts[WCwrite].op = CHANSND;
	alts[WCread].c = w->consread;
	alts[WCread].v = &crm;
	alts[WCread].op = CHANSND;
	alts[WWread].c = w->wctlread;
	alts[WWread].v = &crm;
	alts[WWread].op = CHANSND;
	alts[WComplete].c = w->complete;
	alts[WComplete].v = &cr;
	alts[WComplete].op = CHANRCV;
	alts[Wgone].c = w->gone;
	alts[Wgone].v = "window deleted";
	alts[Wgone].op = CHANNOP;
	/* alts[WTimer].c = timer->c; */
	/* alts[WTimer].v = nil; */
	/* alts[WTimer].op = CHANRCV; */
	alts[NWALT].op = CHANEND;

	kbdqr = kbdqw = 0;
	npart = 0;
	lastb = -1;
	skip = select = 0;
	winit = 2;
	for(;;){
		if(w->i==nil){
			/* window deleted */
			alts[Wgone].op = CHANSND;

			alts[WKbdread].op = CHANNOP;
			alts[WMouseread].op = CHANNOP;
			alts[WCwrite].op = CHANNOP;
			alts[WWread].op = CHANNOP;
			alts[WCread].op = CHANNOP;
		} else {
			alts[WKbdread].op = w->kbdopen && (kbdqw != kbdqr || w->keyup) ?
				CHANSND : CHANNOP;
			alts[WMouseread].op = w->mouseopen && w->mouse.counter != w->mouse.lastcounter ? 
				CHANSND : CHANNOP;
			/* A file buffer must not receive the attached shell's output. */
			alts[WCwrite].op = w->file == nil && (w->scrolling || w->mouseopen || (w->qh <= w->org+w->nchars)) ?
				CHANSND : CHANNOP;
			alts[WWread].op = w->wctlready ?
				CHANSND : CHANNOP;
			/* print("e%d-%d-%d-%d ", w->id, w->qh, w->nr, alts[WCread].op); */
			if (winit == 2 && alts[WCread].op == CHANNOP) {
			  /* Window init happens in two steps: */
			  /* 	1. Prompt is inserted */
			  /* 	2. Char written to /dev/cons in inserted */
			  /* winit=0 after first cmd (possibly remote) is executed */
			  winit--;
			  w->qh = w->nr;
			}
			/* this code depends on NL and EOT fitting in a single byte */
			/* kind of expensive for each loop; worth precomputing? */
			/* Editing a file is not input for the window's rc process. */
			if(w->file != nil || w->holding || w->mode == M_MULTI_EDIT)
				alts[WCread].op = CHANNOP;
			else if(npart || (w->rawing && w->nraw>0))
				alts[WCread].op = CHANSND;
			else{
				alts[WCread].op = CHANNOP;
				for(i=w->qh; i<w->nr; i++){
					c = w->run[i];
					winit = 0;
					if(c=='\n' || c=='\004'){
					  /* print("d%d-%d-%d-%d ", w->id, w->qh, w->nr, w->qh1); */
						alts[WCread].op = CHANSND;
						/* Avoid storing cancelled/interrupted commands */
						if (c=='\n' && w->qh + 1 < i && (w->qh1 <= w->qh)) {
						  undoaddfile(w->hcmd, w->run + w->qh, i - w->qh);
						  w->qh1 = i;
						}
						break;
					}
				}
			}
		}
		switch(alt(alts)){
		case WKbd:
			if(kbdqw - kbdqr < nelem(kbdq))
				kbdq[kbdqw++ % nelem(kbdq)] = kbds;
			else
				free(kbds);
			if(w->kbdopen)
				continue;
			while(kbdqr != kbdqw){
				kbds = kbdq[kbdqr++ % nelem(kbdq)];
				/* fprint(2, "key %s ", (kbds)); */
				alarm(w->mode == M_DEBUG ? 1000 : IDLE_TRIGGER);
				idle = 0;
				if(*kbds == 'k' && (s = utfrune(kbds + 1, Kctl)) != nil
				   /* Only handle combinations  */
				   && utflen(s) > 1) {
				  char *p;
				  for (p = s; *p;) {
					p += chartorune(&r, p);
					/* fprint(2, "%x ", r); */
					if (!(r == Kctl || r == Kshift || r == Kalt))
					  wkeyctl(w, r);
				  }
				  skip = 1;
				}
				if (*kbds == 'c') {
				  if (!skip) {
					s = kbds + 1;
					chartorune(&r, s);
					wkeyctl(w, r);
					/* fprint(2, "key1 %C %s ", r, (s)); */
				  }
				  skip = 0;
				}
				free(kbds);
			}
			break;
		case WKbdread:
			recv(crm.c1, &pair);
			nb = 0;
			while(kbdqr != kbdqw){
				kbds = kbdq[kbdqr % nelem(kbdq)];
				i = strlen(kbds)+1;
				if(nb+i > pair.ns)
					break;
				memmove((char*)pair.s + nb, kbds, i);
				free(kbds);
				nb += i;
				kbdqr++;
			}
			if(w->keyup && nb+2 <= pair.ns){
				w->keyup = 0;
				memmove((char*)pair.s + nb, "K", 2);
				nb += 2;
			}
			pair.ns = nb;
			send(crm.c2, &pair);
			continue;
		case WMouse:
			if(w->mouseopen) {
				w->mouse.counter++;

				/* queue click events */
				if(!w->mouse.qfull && lastb != w->mc.buttons) {	/* add to ring */
					mp = &w->mouse.queue[w->mouse.wi];
					if(++w->mouse.wi == nelem(w->mouse.queue))
						w->mouse.wi = 0;
					if(w->mouse.wi == w->mouse.ri)
						w->mouse.qfull = TRUE;
					*(Mouse*)mp = *(Mouse*)&w->mc;
					mp->counter = w->mouse.counter;
					lastb = w->mc.buttons;
				}
			} else
				wmousectl(w);
			break;
		case WMouseread:
			/* send a queued event or, if the queue is empty, the current state */
			/* if the queue has filled, we discard all the events it contained. */
			/* the intent is to discard frantic clicking by the user during long latencies. */
			w->mouse.qfull = FALSE;
			if(w->mouse.wi != w->mouse.ri) {
				m = w->mouse.queue[w->mouse.ri];
				if(++w->mouse.ri == nelem(w->mouse.queue))
					w->mouse.ri = 0;
			} else
				m = (Mousestate){*(Mouse*)&w->mc, w->mouse.counter};

			w->mouse.lastcounter = m.counter;
			send(mrm.cm, (Mouse*)&m);
			continue;
		case WCtl:
			if(wctlmesg(w, wcm.type, wcm.r, wcm.p) == Exited){
				while(kbdqr != kbdqw)
					free(kbdq[kbdqr++ % nelem(kbdq)]);
				chanfree(crm.c1);
				chanfree(crm.c2);
				chanfree(mrm.cm);
				chanfree(cwm.cw);
				threadexits(nil);
			}
			continue;
		case WCwrite:
			recv(cwm.cw, &pair);
			rp = pair.s;
			nr = pair.ns;
			for(i=0; i<nr; i++)
				if(rp[i] == '\b'){
					up = rp+i;
					initial = 0;
					for(; i<nr; i++){
						if(rp[i] == '\b'){
							if(up == rp)
								initial++;
							else
								up--;
						}else
							*up++ = rp[i];
					}
					if(initial){
						if(initial > w->qh)
							initial = w->qh;
						qh = w->qh-initial;
						wdelete(w, qh, qh+initial);
						w->qh = qh;
					}
					nr = up - rp;
					break;
				}
			/* w->qh = winsert(w, rp, nr, w->qh)+nr; */
			/* print("d%d-%d-%d ", w->id, w->qh, w->nr); */
			if (!select) 
			  q0 = winsert(w, rp, nr, w->qh)+nr;
			if (w->mode == M_DEBUG && tip.qh2 == w->qh) {
			  if (runestrncmp(L"acid:", w->run + tip.qh2, 5) == 0) {
				/* Blank output indicates invalid variable */
				/* free(tip.r); */
				/* tip.r = nil; */
			  } else {
				for (i = tip.qh2; i < q0; i++)
				  if (w->run[i] == '\n') break;
				  nr = (i - tip.qh2) + 1;
				  /* fprint(2, "c%d %.*S ", nr, nr, w->run + tip.qh2); */
				  /* Reading a hex format value */
				  tip.r = mallocz(nr * sizeof(Rune), 1);
				  runestrncpy(tip.r, w->run + tip.qh2, nr - 1); // Skip \n
				  /* fprint(2, "d%d %.*S ", nr, nr, tip.r); */
			  }
			  wdelete(w, tip.qh1, q0);
			  w->hovering = 0; // Allow keyboard input now
			  idle = 1;
			  q0 = w->qh;
			  tip.qh2 = -1;
			}
			if (!winit || !w->holding) w->qh = q0;
			if (w->q0 < w->q1) {
			  if (select) {
				w->qh = w->nr;
				select--;
			  } else select = 1;
			}

			if(w->scrolling || w->mouseopen)
				wshow(w, w->qh);
			wsetselect(w, w->q0, w->q1);
			wscrdraw(w);
			free(rp);
			break;
		case WCread:
			recv(crm.c1, &pair);
			t = pair.s;
			nb = pair.ns;
			i = npart;
			npart = 0;
			if (w->nraw>0) {
			  cmd.ns = w->nraw;
			  cmd.s  = runemalloc(cmd.ns);
			  runemove(cmd.s, w->raw, cmd.ns);
			  /* print("%.*S", cmd.ns, cmd.s); */
			  israw = 1;
			} else {
			  cmd.ns = 0;
			  cmd.s  = w->run + w->qh;
			  israw = 0;
			}
			if(i)
				memmove(t, part, i);
			while(i<nb && (w->qh<w->nr || w->nraw>0)){
				if(w->qh == w->nr){
					wid = runetochar(t+i, &w->raw[0]);
					w->nraw--;
					runemove(w->raw, w->raw+1, w->nraw);
				}else {
					wid = runetochar(t+i, &w->run[w->qh++]);
					cmd.ns++;
				}
				c = t[i];	/* knows break characters fit in a byte */
				i += wid;
				if(!w->rawing && (c == '\n' || c=='\004')){
					/* Avoid storing cancelled/interrupted commands */
					if (cmd.ns > 1) undoaddfile(w->hcmd, cmd.s, cmd.ns-1);
					if (israw) free(cmd.s);
					if(c == '\004')
						i--;
					break;
				}
			}
			if(i==nb && w->qh<w->nr && w->run[w->qh]=='\004')
				w->qh++;
			if(i > nb){
				npart = i-nb;
				memmove(part, t+nb, npart);
				i = nb;
			}
			pair.s = t;
			pair.ns = i;
			send(crm.c2, &pair);
			continue;
		case WWread:
			w->wctlready = 0;
			recv(crm.c1, &pair);
			s = Dx(w->screenr) > 0 ? "visible" : "hidden";
			t = "notcurrent";
			if(w == input)
				t = "current";
			Point p = stringsize(w->font, "m");
			pair.ns = snprint(pair.s, pair.ns+1,
							  "%11d %11d %11d %11d %11s %11s %11d %11d",
							  w->i->r.min.x, w->i->r.min.y,
							  w->i->r.max.x, w->i->r.max.y, t, s,
							  /* Exclude header line */
							  Dx(w->i->r)/p.x, Dy(w->i->r)/p.y-1);
			send(crm.c2, &pair);
			continue;
		case WComplete:
			if(w->i!=nil){
				if(cr->advance && cr->nmatch > 1 && w->comp_len > 1 && !cr->force) {
				  /* Don't trigger on single letter */
				  /* Don't trigger on single match. User might want to type a new word */
					rp = runesmprint("%s", cr->string);
					if(rp) {
					  Window *w1 = w;
					  if (w->title && w->title->active)
						w1 = w->title;
					  nr = runestrlen(rp);
					  q0 = w1->q0;
					  q0 = winsert(w1, rp, nr, q0);
					  w->comp_len += nr;
					  wshow(w1, q0+nr);
					  free(rp);
					}
				} else {
					Completion *c;
					c = w->c;
					showcandidates(w, cr);
					w->c = cr;
					cr = c;
				}
			}
			freecompletion(cr);
			break;
		/* case WTimer: */
		  /* fprint(2, "saving %d", w->preview); */
		  /* savehistory(w); */
		  /* if (w->preview) */
		  /* 	wsendctlmesg(w, Deleted, ZR, nil); */
		  /* Rescheduling idle timer is unnecessary since no further
			 change is expected till keyboard activity */
		  /* timercancel(timer); */
		  /* timer = timerstart(IDLE_TRIGGER); */
		  /* alts[WTimer].c = timer->c; */
		  /* break; */
		}
		if(w->i!=nil && Dx(w->screenr) > 0 && display->bufp > display->buf)
			flushimage(display, 1);
	}
}

void
wsetpid(Window *w, int pid, int dolabel)
{
	char buf[32];
	int ofd;

	ofd = w->notefd;
	if(pid <= 0)
		w->notefd = -1;
	else {
		if(dolabel){
			snprint(buf, sizeof(buf), "rc %lud", (ulong)pid);
			free(w->label);
			w->label = estrdup(buf);
		}
		snprint(buf, sizeof(buf), "/proc/%lud/notepg", (ulong)pid);
		w->notefd = open(buf, OWRITE|OCEXEC);
	}
	if(ofd >= 0)
		close(ofd);
}

void
winshell(void *args)
{
	Window *w;
	Channel *pidc;
	void **arg;
	char *cmd, *dir;
	char **argv;

	arg = args;
	w = arg[0];
	pidc = arg[1];
	cmd = arg[2];
	argv = arg[3];
	dir = arg[4];
	rfork(RFNAMEG|RFFDG|RFENVG);
	if(filsysmount(filsys, w->id) < 0){
		fprint(2, "mount failed: %r\n");
		sendul(pidc, 0);
		threadexits("mount failed");
	}
	close(0);
	if(open("/dev/cons", OREAD) < 0){
		fprint(2, "can't open /dev/cons: %r\n");
		sendul(pidc, 0);
		threadexits("/dev/cons");
	}
	close(1);
	if(open("/dev/cons", OWRITE) < 0){
		fprint(2, "can't open /dev/cons: %r\n");
		sendul(pidc, 0);
		threadexits("open");	/* BUG? was terminate() */
	}
	if(wclose(w) == 0){	/* remove extra ref hanging from creation */
		notify(nil);
		dup(1, 2);
		if(dir)
			chdir(dir);
		procexec(pidc, cmd, argv);
		_exits("exec failed");
	}
}
