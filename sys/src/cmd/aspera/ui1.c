/**
Support file for handling keybindings.

Copyright (C) 2025  Anand Tamariya

Author: Anand Tamariya <anand@gmail.com>
Keywords: plan9, keybinding

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.

Commentary:
 - move char word line screen buffer
 - del  char word line
 - window obj n
 - n is repeat - derive from prefix
*/

/* Code: */
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
/* #include <plumb.h> */
/* #include <complete.h> */
#include "dat.h"
#include "fns.h"

#define NULL ((void*)0)
#define MIN(a,b) ((a)<=(b)?(a):(b))

/* typedef int bool; */

/* typedef struct { */
/*   const char *s; */
/*   void (*f0)(void); */
/*   void (*f1)(Window *w); */
/*   void (*f2)(Window *w, int n); */
/*   int argc; */
/* } Command; */

/* typedef struct keymap Keymap; */
/* struct keymap { */
/*   Rune *k; */
/*   Keymap **val; // Array of Keymap pointer */
/*   int n, len; */

/*   bool iscmd; */
/*   Command *cmd; */
/* }; */

/* typedef struct Buflist Buflist; */
/* typedef struct Buflist { */
/*   Buffer *buf; */
/*   Buflist *next; */
/* } Buflist; */
  
Keymap *cur_map, *mode_map;
static int prefix, map_mode;
static Buflist *buffers;
static int nbuf;
static Image *cover;
static int plast, pcur;

void wundo(Window *w);
void wredo(Window *w);
Buffer* buffer_find(int id);
void buffer_save(Window *w, Buffer *buf);
void buffer_restore(Window *w, Buffer *buf);
Buffer* buffer_locate(char *dir, char *file);
void pulseline(Window *w, ulong src, ulong dst);
void keymap_reset(void);
Keymap* keymap_new(int n);
void keymap_dump(Keymap *map);
void fload(Window *w, Rune *s, int ns);
Image *fetchart(int);
void initui(void);
void charright(Window *w, int n);
void cleanreg(int id);

int runecmp(Rune r1, Rune r2) {
  char str[4]; // UTFmax + 1
  if (runetochar(str, &r1) == Runeerror)
	return 0;
  return utfrune(str, r2) != nil;
}

void spawnstage(char **argv, int infd, int outfd) {
	switch(rfork(RFPROC|RFFDG|RFNOTEG|RFCENVG|RFREND|RFNOWAIT)){
	case -1:
		sysfatal("rfork: %r");
	case 0:
		if(infd >= 0){
			dup(infd, 0);
			close(infd);
		}
		if(outfd >= 0){
			dup(outfd, 1);
			close(outfd);
		}
		exec(argv[0], argv);
		sysfatal("exec %s: %r", argv[0]);
	default:
		break;
	}
}

void bol(Window *w) {
  int nb;
  if(w->q0 == 0 || w->r[w->q0-1] == '\n')
  /* if(w->q0 == 0 || w->q0 == w->qh || w->r[w->q0-1] == '\n') */
	return;
  nb = wbswidth(w, 0x15 /* ^U */);
  wsetselect(w, w->q0-nb, shiftdown ? w->q1 : w->q0-nb);
  wshow(w, w->q0);
}

void eol(Window *w) {
  int q1 = w->q1;
  while(q1 < w->nr && w->r[q1] != '\n')
	q1++;
  wsetselect(w, shiftdown ? w->q0 : q1, q1);
  wshow(w, w->q1);
}

void bob(Window *w) {
  if (w->popup && w->popup->i) {
	w = w->popup;
  }
  wsetselect(w, 0, shiftdown ? w->q1 : 0);
  wshow(w, w->q0);
}

void eob(Window *w) {
  if (w->popup && w->popup->i) {
	w = w->popup;
  }
  wsetselect(w, shiftdown ? w->q0 : w->nr, w->nr);
  wshow(w, w->q1);
}

void scrollup(Window *w, int n) {
  int q0, l;
  if (w->popup && w->popup->i) {
	w = w->popup;
  }
  l = (w->Frame.r.max.y - w->Frame.r.min.y) / w->font->height;
  q0 = wbacknl(w, w->org, n * l);
  wsetorigin(w, q0, TRUE);
}  

void scrolldown(Window *w, int n) {
  int q0, l;
  if (w->popup && w->popup->i) {
	w = w->popup;
  }
  l = (w->Frame.r.max.y - w->Frame.r.min.y) / w->font->height;
  q0 = w->org + frcharofpt(w, Pt(w->Frame.r.min.x, w->Frame.r.min.y + n*l*w->font->height));
  if (q0 < w->nr)
	wsetorigin(w, q0, TRUE);
}

void togglescroll(Window *w) {
  w->scrolling ^= 1;
}

void lineup(Window *w, int n) {
  int q0, org, h;

  if (w->popup && w->popup->i) {
	w = w->popup;
	if (w->q0 > w->org)
	  w->q1 = w->q0;
	else
	  w->q1 = w->org;
	shiftdown = 1;
  } else {
	/* If we are scrolling page, go to cursor. */
	wshow(w, w->q0);
  }

  org = w->org;
  q0 = w->q0 - org;
  h = w->font->height;
  Point p = frptofchar(w, q0);
  p.y -= n * h;
  if (p.y < w->Frame.r.min.y) {
	org = wbacknl(w, w->org, n);
	wsetorigin(w, org, TRUE);
	p.y += n * h;
  }
  q0 = w->org + frcharofpt(w, p);
  wsetselect(w, q0, shiftdown ? w->q1 : q0);
  wshow(w, q0);
}

void linedown(Window *w, int n) {
  int q0, org, h;

  if (w->popup && w->popup->i) {
	w = w->popup;
	w->q0 = w->q1;
	shiftdown = 1;
  } else {
	/* If we are scrolling page, go to cursor. */
	wshow(w, w->q0);
  }

  org = w->org;
  q0 = w->q1 - org;
  h = w->font->height;
  Point p = frptofchar(w, q0);
  p.y += n * h;
  if (p.y >= w->Frame.r.max.y) {
	p.y -= n * h;
	org = w->org + frcharofpt(w, Pt(w->Frame.r.min.x, w->Frame.r.min.y + w->font->height));
	wsetorigin(w, org, TRUE);
  }
  q0 = w->org + frcharofpt(w, p);
  wsetselect(w, shiftdown ? w->q0 : q0, q0);
  wshow(w, q0);
}

void prompt(Window *w, Rune *prompt, int mode, void (*fn)(Window *w, Rune *s)) {
  Window *w1;
  Rune name[128];
  jmp_buf savedjb;
  int ret;

  w1 = w->title;
  if (!w1) return;
  if (!w1->active) {
	/* Activate input */
	w->mode = mode;
	w1->active = 1;
	w1->input = L"";
	w1->prompt = prompt;
	w1->cmd = fn;
	wtagrepaint(w);
	if (mode != M_INPUT) namecomplete(w, 0);
  }
}

int curline(Window *w) {
  int q0, i, n;

  q0 = i = n = 0;
  while (q0 < w->q0)
	/* Stop looking after 128 chars */
	if(w->r[q0++]=='\n' || q0 - i > 128) {
	  n++;
	  i = q0;
	}
  return n;
}

void pulseline1(Window *w) {  pulseline(w, DPaleyellow, DWhite); }
	
void gotoline2(Window *w, int n) {
  int q0, i;

  q0 = i = 0;
  n--;
  while (n > 0 && q0 < w->nr)
	/* Stop looking after 128 chars */
	if(w->r[q0++]=='\n' || q0 - i > 128) {
	  n--;
	  i = q0;
	}
  wsetselect(w, q0, q0);
  wshow(w, q0);
  pulseline1(w);
}

/* Highlight a frame line. Useful as a line cursor. */
int sp0, sp1;
void selectfrline(Window *w, int n) {
  int q0, i, j;
  Point p;

  p = frptofchar(w, sp0);
  frdrawsel(w, p, sp0, sp1, 0);
  /* fprint(2, "d%d %d, ", sp0, sp1); */

  i = j = 0;
  q0 = w->org;
  n++;
  while (n > 0 && q0 < w->nchars)
	/* Stop looking after 128 chars */
	if(w->r[q0++]=='\n' || q0 - i > 128) {
	  n--;
	  j = i;
	  i = q0;
	}
  /* fprint(2, "a%d %d, ", j, i); */
  sp0 = j, sp1 = i;
  p = frptofchar(w, sp0);
  frdrawsel(w, p, sp0, sp1, 1);
}

void gotoline1(Window *w, Rune *input) {
  int n;
  char s[128];

  snprint(s, sizeof(s), "%S", input);
  n = atoi(s);
  gotoline2(w, n);
}

void gotochar2(Window *w, int q0) {
  wsetselect(w, q0, q0);
  wshow(w, q0);
  pulseline1(w);
}

void gotochar1(Window *w, Rune *input) {
  int q0;
  char s[128];

  snprint(s, sizeof(s), "%S", input);
  q0 = atoi(s);
  gotochar2(w, q0);
}

void gotocol2(Window *w, int q0) {
  bol(w);
  charright(w, q0);
  wsetselect(w, q0, q0);
  wshow(w, q0);
  pulseline1(w);
}

void gotocol1(Window *w, Rune *input) {
  int q0;
  char s[128];

  snprint(s, sizeof(s), "%S", input);
  q0 = atoi(s);
  gotocol2(w, q0);
}

void gotoline(Window *w, int n) {
  if (!prefix && n)
	gotoline2(w, n);
  else
	prompt(w, L"Line", M_INPUT, gotoline1);
}

void gotochar(Window *w, int n) {
  if (n)
	gotochar2(w, n);
  else
	prompt(w, L"Char", M_INPUT, gotochar1);
}

void gotocol(Window *w, int n) {
  if (!prefix && n)
	gotocol2(w, n);
  else
	prompt(w, L"Column", M_INPUT, gotocol1);
}

void multimove(Window *w, int n) {
  Point p1, p2;
  uint p0;

  for (int i = w->nm - 1; i >= 0; i--) {
	p0 = w->mq[i];
	if(p0 >= w->org && p0 <= w->org+w->nchars) {
	  p1 = frptofchar(w, p0);
	  p2 = frptofchar(w, p0 + n);
	  frtick(w, p1, 0);
	  if (n > 0) {
		frdrawsel(w, p1, p0, p0 + n, 1);
	  } else
		frdrawsel(w, p2, p0 + n, p0 + 1, 1);
	  frtick(w, p2, 1);
	}
	w->mq[i] += n;
  }
}

void charleft(Window *w, int n) {
  /* print("left\n"); */
  int q0 = w->q0;
  if(q0 > 0) {
	q0 = q0 - n;
  }
  if (w->mode == M_MULTI_EDIT)
	multimove(w, -n);
  else {
  wsetselect(w, q0, shiftdown ? w->q1 : q0);
  wshow(w, q0);
  }
}

void charright(Window *w, int n) {
  /* print("right\n"); */
  int q1 = w->q0;
  if(q1 < w->nr) {
	q1 = q1 + n;
  }
  if (w->mode == M_MULTI_EDIT)
	multimove(w, n);
  else {
  wsetselect(w, shiftdown ? w->q0 : q1, q1);
  wshow(w, q1);
  }
}

void cut(Window *w) {
  /* print("cut\n"); */
  wsnarf(w);
  wcut(w);
}

void copy(Window *w) {
  /* print("copy\n"); */
  wsnarf(w);
}

void paste(Window *w) {
  /* print("paste\n"); */
  wpaste(w);
}

/* void exit() { */
/* 	int f; */

/* 	wsupdate(); */
/* 	if(wcur == nil || (f = wwctl(wcur->id, OWRITE)) < 0) */
/* 		return; */
/* 	fprint(f, "delete"); */
/* 	close(f); */
/* } */

void delete_fn(Window *w, Rune r) {
  /* switch(r) */
  /* case Kbs:	/\* ^H: erase character *\/ */
  /* case Knack:	/\* ^U: erase line *\/ */
  /* case Ketb:	/\* ^W: erase word *\/ */
  if(w->q0 == 0 || w->q0 == w->qh)
	return;

  int nb, q0, q1;
  nb = wbswidth(w, r);
  q1 = w->q0;
  q0 = q1 - nb;
  if(q0 < w->org){
	q0 = w->org;
	nb = q1 - q0;
  }
  if(nb > 0){
	wdelete(w, q0, q0 + nb);
	wsetselect(w, q0, q0);
  }
}

void delcharl(Window *w1, int i) {
  int q0, q1;
  Window *w;
  if (i > 0) {
	w = (w1->title && w1->title->active) ? w1->title : w1;
	
	q0 = q1 = w->q0;
	q0 = max(q0 - i, 0);
	/* Avoid deleting selection here i.e. no w->q1 */
	if (w->mode != M_MULTI_EDIT)
	  q1 = w->q1;
	wdelete(w, q0, q1);
	/* wsetselect(w, q0, q0); */
	if (!isspace(w->r[q0 - 1]))
	namecomplete(w1, 1);
  }
}

void delcharr(Window *w, int i) {
  int q0, q1;
  if (i > 0) {
	q0 = w->q0;
	/* Avoid deleting selection here i.e. no w->q1 */
	q1 = min(q0 + i, w->nr);
	if (w->mode == M_MULTI_EDIT)
	  wdelete(w, q1, q0);
	else
	wdelete(w, q0, q1);
	/* wsetselect(w, q0, q0); */
  }
}

void dellinel(Window *w, int i) {
  shiftdown = 1;
  bol(w);
  wdelete(w, w->q0, w->q1);
  wsetselect(w, w->q0, w->q0);
}

void delliner(Window *w, int i) {
  shiftdown = 1;
  eol(w);
  wdelete(w, w->q0, w->q1);
  wsetselect(w, w->q0, w->q0);
}

void wordleft(Window *w, int i) {
  uint q0, q1;

  q0 = w->q0;
  q1 = w->q1;
  /* print("%d %d, ", q0, q1); */
  while (i-- > 0 && q0 > 0) {
	/* print("i%d %d, ", i, shiftdown); */
	while(q0 > 0 && !isalnum(w->r[q0 - 1])) {
	  q0--;
	}
	while(q0 > 0 && (isalnum(w->r[q0 - 1]))) {
	  q0--;
	}
  }
  /* print("%d %d, ", q0, q1); */
  w->q0 = q0;
  w->q1 = q1;
  wsetselect(w, q0, shiftdown ? w->q1 : q0);
  wshow(w, q1);
}

void wordright(Window *w, int i) {
  uint q0, q1;

  q0 = w->q0;
  q1 = w->q1;
  /* print("%d %d, ", q0, q1); */
  while (i-- > 0) {
	/* print("i%d %d, ", i, shiftdown); */
	while(q1 < w->nr && !isalnum(w->r[q1])) {
	  q1++;
	}
	while(q1 < w->nr && (isalnum(w->r[q1]))) {
	  q1++;
	}
  }
  /* print("%d %d, ", q0, q1); */
  w->q0 = q0;
  w->q1 = q1;
  wsetselect(w, shiftdown ? w->q0 : q1, q1);
  wshow(w, q1);
}

void delwordl(Window *w, int i) {
  shiftdown = 1;
  wordleft(w, i);
  cut(w);
}

void delwordr(Window *w, int i) {
  shiftdown = 1;
  wordright(w, i);
  cut(w);
  /* shiftdown = 0; */
}

void interrupt(Window *w) {
  w->qh = w->nr;
  wshow(w, w->qh);
  if(w->notefd < 0)
	return;
  int *notefd = emalloc(sizeof(int));
  *notefd = dup(w->notefd, -1);
  proccreate(interruptproc, notefd, 4096);
}

void selectall(Window *w) {
  wsetselect(w, 0, w->nr);
}

void clear(Window *w) {
  wdelete(w, 0, w->nr);
}

static void
spawn(char **s)
{
  if(rfork(RFPROC|RFNOWAIT|RFNAMEG|RFENVG|RFCFDG|RFREND) == 0) {
	  exec("/bin/rc", s);
	  fprint(2, "exec failed %s: %r\n", s[0]);
	  exits("exec");
  }
}

void cropscreen() {
  /* Create a screenshot before opening the editor window */
  char *name;
  char buf[128];
  char *s[] = {"rc", "-c", nil, nil};
  int fd;

  name = mktemp("/tmp/vcropXXXX");

  fd = create(name, OWRITE, 0666);
  if(fd < 0)
	sysfatal("create: %r");
  if(writeimage(fd, screen, 0) < 0)
	sysfatal("writeimage: %r");
  close(fd);

  snprint(buf, sizeof(buf), "window vcrop %s", name);
  s[2] = buf;
  spawn(s);
}

void cropwindow(Window *w) {
  /* Create a screenshot before opening the editor window */
  char *name;
  char buf[128];
  char *s[] = {"rc", "-c", nil, nil};
  int fd;

  name = mktemp("/tmp/vcropXXXX");

  fd = create(name, OWRITE, 0666);
  if(fd < 0)
	sysfatal("create: %r");
  if(writeimage(fd, w->i, 0) < 0)
	sysfatal("writeimage: %r");
  close(fd);

  snprint(buf, sizeof(buf), "window vcrop %s", name);
  s[2] = buf;
  spawn(s);
}

char* basename(char *s) {
  char *pr;
  pr = utfrrune(s, '/');
  if(pr)
	pr++;
  else
	pr = s;
  return pr;
}

char* dirname(char *s) {
  char *pr, *d;
  int n;
  d = nil;
  pr = utfrrune(s, '/');
  if(pr) {
	n = (pr - s);
	d = malloc(n * sizeof(char));
	memcpy(d, s, n);
	/* d = s; */
	d[n] = 0;
  }
  return d;
}

void fopen(Window *w) {
  /* Open file or directory */
  Window *w1;
  char *s;
  int n;

  w1 = w->title;
  if (!w1) return;
  if (!w1->active) {
	/* Activate input */
	w1->active = 1;
	wtagrepaint(w);
	return;
  }

  fload(w, w1->r, w1->nr);
}

void fload(Window *w, Rune *fs, int ns) {
  Dir *d;
  char buf[4096], *s;
  int n;
  Rune r;
  Buffer *b;

  s = smprint("%.*S", ns, fs);
  d = dirstat(s);
  if(d == nil) return;

  /* Save old one if it's a file buffer */
  b = buffer_find(w->bufid);
  if (b) buffer_save(w, b);

  w->undo = nil;
  w->bufid = 0;
  clear(w);
  undoaddfile(w->hfile, fs, ns);
  if(d->mode & DMDIR) {
	/* Change to dir and list content */
	winsert(w, L"cd ", 3, w->nr);
	winsert(w, fs, ns, w->nr);
	/* Single \n causes segfault on delcharl */
	winsert(w, L" && lc\n", 7, w->nr);
	w->file = nil;
	w->undo = w->scratch;
  } else {
	int fd;
	fd = open(s, OREAD);
	if (fd < 0) {
	  winsert(w, fs, ns, w->nr);
	  winsert(w, L"\n", 2, w->nr);
	  w->undo = w->scratch;
	} else {
	  char *p, *end;
	  w->file = basename(s);
	  /* free(w->dir); */
	  w->dir  = dirname(s);
	  if ((b = buffer_locate(w->dir, w->file))) {
		buffer_restore(w, b);
		/* fprint(2, "%d ", w->bufid); */
	  } else {
	  /* fprint(2, "a%sb", basename(s)); */
	  while ((n = read(fd, buf, sizeof(buf))) > 0) {
		p = buf;
        end = buf + n;

        while(p < end){
		  n = chartorune(&r, p);
		  if(n <= 0)
			sysfatal("bad UTF-8");

		  winsert(w, &r, 1, w->nr);
		  p += n;
        }
	  }
	  setmodified(w, 0);
	  close(fd);
	  w->bufid = ++nbuf;
	  buffer_save(w, nil);
	  }
	}
	w->qh = w->nr;
  }

  free(d);
}

void fsave(Window *w) {
  char *name, *s;
  Window *w1;
  Dir *d;
  int n;

  /* if(!w->modified) return; */
  w1 = w->title;
  /* fprint(2, "s%d %d,", w->saving, w->nr); */
  if (!w1) return;
  if (!w1->active && !w->file) {
	/* Activate input */
	w->saving = 1;
	w1->active = 1;
	wtagrepaint(w);
	return;
  }

  name = wcontents(w1, &n);
  /* fprint(2, "%s ", name); */
  d = dirstat(name);
		
  if(d && d->mode & DMDIR) return; // directory

  int fd;
  fd = create(name, OWRITE, 0666);
  if (fd < 0) {
	/* winsert(w, L"Error: ", 7, w->nr); */
	winsert(w, w1->r, w1->nr, w->nr);
	winsert(w, L"\n", 2, w->nr);
	return;
  }
  s = wcontents(w, &n);
  /* fprint(2, "%s %d, ", s, n); */
  write(fd, s, n);
  close(fd);

  w->file = basename(name);
  setmodified(w, 0);
}

void find(Window *w) {
  Rune name[128];
  Window *w1;

  if (w->q0 != w->q1) {
	wlook(w, nil);
	return;
  }

  w1 = w->title;
  if (!w1) return;
  if (!w1->active) {
	/* Activate input */
	w->prev_mode = w->mode;
	w->mode = M_SEARCH_HIST;
	w1->active = 1;
	w1->input = L"";
	wtagrepaint(w);
	namecomplete(w, 0);
	return;
  }

  runestrncpy(name, w1->r, w1->nr);
  name[w1->nr] = 0;
  undoaddfile(w->hsearch, w1->r, w1->nr);
  /* fprint(2, "%.*S", w1->nr, name); */
  wlook(w, name);
}

static int
longestprefixlength(char *a, char *b, int n)
{
	int i, w;
	Rune ra, rb;

	for(i=0; i<n; i+=w){
		w = chartorune(&ra, a);
		chartorune(&rb, b);
		if(ra != rb)
			break;
		a += w;
		b += w;
	}
	return i;
}

int
intpcmp(const void *va, const void *vb)
{
	char *a, *b;

	a = *(char**)va;
	b = *(char**)vb;
	return (a - b);
}

static int
strpcmp(const void *va, const void *vb)
{
	char *a, *b;

	a = *(char**)va;
	b = *(char**)vb;
	return strcmp(a, b);
}

static int
cistrpcmp(const void *va, const void *vb)
{
	char *a, *b;

	a = *(char**)va;
	b = *(char**)vb;
	return cistrcmp(a, b);
}

int
fuzzymatch(char *target, char *pattern)
{
    char patbuf[256], tgtbuf[512];
    char *tokens[64], *words[64];
    int ntok = 0, nwords = 0;
    int ok, k;

    strncpy(patbuf, pattern, sizeof patbuf);
    strncpy(tgtbuf, target, sizeof tgtbuf);
    patbuf[sizeof patbuf-1] = 0;
    tgtbuf[sizeof tgtbuf-1] = 0;

    ntok   = tokenize(patbuf, tokens, nelem(tokens));
    nwords = tokenize(tgtbuf, words,  nelem(words));

    ok = k = 0;
    for(int i = 0; i < ntok; i++) {
	  for(int j = k; j < nwords; j++) {
		if(strstr(words[j], tokens[i])) {
		  k = j + 1;
		  ok++;
		  break;
		}
	  }
    }

	/* if (ok == ntok) */
	/*   print("%d %d %d %s %s, ", ok, ntok, nwords, target, pattern); */
    return ok == ntok;
}


Completion*
complete1(char **all, int n, char *s, int fuzzy, int dedup, int fold)
{
	long i, l, nfile, len, nbytes;
	int minlen;
	char **name, *p;
	Completion *c;

	/* find longest string, for allocation */
	/* len = 0; */
	/* for(i=0; i<n; i++){ */
	/* 	l = strlen(dirp[i].name) + 1 + 1; /\* +1 for /   +1 for \0 *\/ */
	/* 	if(l > len) */
	/* 		len = l; */
	/* } */

	/* fprint(2, "%d %ld, ", n, l); */
	name = malloc(n*sizeof(char*));
	/* c = malloc(sizeof(Completion) + len); */
	/* if(name == nil || c == nil) */
	if(name == nil)
		goto Return;
	/* memset(c, 0, sizeof(Completion)); */

	/* find the matches */
	len = strlen(s);
	nfile = nbytes = 0;
	minlen = 1000000;
	for(i=0; i<n; i++)
	  if(fuzzy && fuzzymatch(all[i], s) ||
		 (fold && cistrncmp(s, all[i], len) == 0) ||
		 (strncmp(s, all[i], len) == 0)) {
		/* fprint(2, "%s %s %d %d, ", s, all[i], minlen, strlen(all[i])); */
		name[nfile] = all[i];
		if(minlen > strlen(all[i]))
		  minlen = strlen(all[i]);
		nfile++;
		l = strlen(all[i]) + 1 + 1; /* +1 for /   +1 for \0 */
		if(l > nbytes)
		  nbytes = l;
	  }

	c = malloc(sizeof(Completion) + nbytes);
	if(c == nil)
		goto Return;
	memset(c, 0, sizeof(Completion));
	/* fprint(2, "%d ", nfile); */
	if(nfile > 0) {
		/* report interesting results */
		/* trim length back to longest common initial string */
	  if (fuzzy) minlen = len;
	  else
		for(i=1; i<nfile; i++)
			minlen = longestprefixlength(name[0], name[i], minlen);

		/* fprint(2, "%d %d %d, ", nfile, minlen, len); */
		/* build the answer */
		c->complete = (nfile == 1);
		c->advance = c->complete || (minlen > len);
		/* c->string = (char*)(c+1); */
		/* memmove(c->string, name[0]+len, minlen-len); */
		/* if(c->complete) */
		/* 	c->string[minlen++ - len] = (mode[0]&DMDIR)? '/' : ' '; */
		/* c->string[minlen - len] = '\0'; */
		c->nmatch = nfile;
	} else {
		/* no match, so return all possible strings */
		for(i=0; i<n; i++){
			name[i] = all[i];
		}
		minlen = 0;
		nfile = n;
		c->nmatch = 0;
	}

	/* attach list of names */
	nbytes = nfile * sizeof(char*);
	for(i=0; i<nfile; i++)
		nbytes += strlen(name[i]) + 1 + 1;
	c->filename = malloc(nbytes);
	if(c->filename == nil)
		goto Return;
	p = (char*)(c->filename + nfile);
	for(i=0; i<nfile; i++){
		c->filename[i] = p;
		strcpy(p, name[i]);
		p += strlen(p);
		/* if(mode[i] & DMDIR) */
		/* 	*p++ = '/'; */
		*p++ = '\0';
	}
	c->nfile = nfile;
	qsort(c->filename, c->nfile, sizeof(c->filename[0]), fold ? cistrpcmp : strpcmp);
	if (dedup) {
	  free(name);
	  name = c->filename;
	  nfile = 0;
	  for(i = 1; i < c->nfile; i++){
        if(strcmp(name[nfile], name[i]) != 0)
		  name[++nfile] = name[i];
	  }
	  c->nfile = nfile + 1;
	  name = nil;
	}
	if (nfile && minlen > len) {
	  /* Use the topmost name after sorting */
	  c->string = (char*)(c+1);
	  memmove(c->string, name[0]+len, minlen-len);
	  c->string[minlen - len] = '\0';
	}

  Return:
	free(name);
	return c;
}

Completion*
completef(char *dir, char *s)
{
	long i, n, n1;
	int fd;
	Dir *dirp, *dirp1;
	char **filename, *path, *s1;
	Completion *c;

	s1 = s;
	path = dirname(s);
	s = basename(s);
	if (!path || path[0] == '/')
	  path = "";
	if(strchr(s, '/') != nil){
		werrstr("slash character in name argument to complete()");
		return nil;
	}
	/* fprint(2, "%s %s %s %s %d, ", dir, s1, path, s, strcmp(s1, "/")); */
	/* fprint(2, "%s %s, ", smprint("%s/%s", dir, path), smprint("/bin/%s", path)); */

	c = nil;
	dirp = dirp1 = nil;
	n = n1 = 0;
	fd = open(smprint("%s/%s", dir, path), OREAD|OCEXEC);
	if(fd >= 0) {
	  n = dirreadall(fd, &dirp);
	  if (n < 0) n = 0;
	  close(fd);
	}

	/* Include commands from /bin  */
	if (!(s1[0] == '/' || s1[0] == '.')) {
	  fd = open(smprint("/bin/%s", path), OREAD|OCEXEC);
	  if(fd >= 0) {
		n1 = dirreadall(fd, &dirp1);
		if (n1 < 0) n1 = 0;
		close(fd);
	  }
	}

	filename = malloc((n + n1) * sizeof(char*));
	/* fprint(2, "%d %d %x ", n, n1, filename); */
	if(filename == nil)
		goto Return;
	for(i=0; i<n; i++){
		filename[i] = dirp[i].name;
		if (dirp[i].mode & DMDIR) {
		  strcat(filename[i], "/");
		}
		/* fprint(2, "%s ", filename[i]); */
	}
	for(i=0; i<n1; i++){
		filename[i + n] = dirp1[i].name;
		if (dirp1[i].mode & DMDIR) {
		  strcat(filename[i + n], "/");
		}		
		/* fprint(2, "%s ", filename[i]); */
	}
	n += n1;
	c = complete1(filename, n, s, 0, 0, 0);

  Return:
	free(filename);
	free(dirp);
	if (dirp1) free(dirp1);
	return c;
}

void filehistory(Window *w) {
  /* Open file history */
  Window *w1;

  w1 = w->title;
  if (!w1) return;
  if (!w1->active) {
	/* Activate input */
	w->prev_mode = w->mode;
	w->mode = M_FILE_HIST;
	w1->active = 1;
	w1->input = L"";
	wtagrepaint(w);
	namecomplete(w, 0);
  }
}

void cmdhistory(Window *w) {
  /* Open command history */
  Window *w1;

  w1 = w->title;
  if (!w1) return;
  if (!w1->active) {
	/* Activate input */
	w->prev_mode = w->mode;
	w->mode = M_CMD_HIST;
	w1->active = 1;
	w1->input = L"";
	wtagrepaint(w);
	namecomplete(w, 0);
  }
}

void
dictcomplete(Completion *c, char *s)
{
	int pfd[2];   // pipe file descriptors
	char buf[256], *data, *p;
	int n, i, nfile, nbytes;
	char **name;

	if(*s && pipe(pfd) < 0)
		sysfatal("pipe: %r");

	switch(fork()){
	case -1:
		sysfatal("fork: %r");

	case 0:   // child
		close(pfd[0]);             // close read end
		dup(pfd[1], 1);            // connect pipe write -> fd1 (stdout)
		close(pfd[1]);

		// run command: execl(path, argv0, argv1, ..., nil)
		execl("/bin/look", "look", "-f", s, nil);

		// only reached if execl fails
		fprint(2, "execl failed: %r\n");
		exits("execfail");

	default:   // parent
		close(pfd[1]);             // close write end

		nfile = nbytes = 0;
		name = malloc(50 * sizeof(char*));
		while((n = read(pfd[0], buf, sizeof(buf))) > 0) {
		/* while((n = read(pfd[0], buf, sizeof(buf))) > 0 && nfile < 10) { */
		  p = data = buf;
		  while (n > p - buf) {
			if (p = strchr(data, '\n')) {
			  *p = 0;
			  if (nfile > sizeof(name))
				name = realloc(name, (nfile + 50) * sizeof(char*));
			  name[nfile] = strdup(data);
			  i = p - data;
			  /* fprint(2, "%d %d %.*s, ", n, i, i, data); */
			  data = p + 1;
			  nbytes += strlen(name[nfile]) + 1;
			  nfile++;
			} else {
			  seek(pfd[0], -(buf + n - p), 1);
			  p = buf + n;
			  /* fprint(2, "%d %d %.*s, ", n, p - buf, i, data); */
			}
		  }
		}
		if (nfile) {
		  nbytes += nfile * sizeof(char*);
		  c->filename = malloc(nbytes);
		  if (!c->filename) {
			return;
		  }

		  c->nmatch = c->nfile = nfile;
		  p = (char*)(c->filename + nfile);
		  for(i = 0; i < nfile; i++) {
			c->filename[i] = p;
			strcpy(p, name[i]);
			p += strlen(p);
			*p++ = '\0';
			free(name[i]);
		  }
		}

		free(name);
		close(pfd[0]);
		waitpid();
	}
}

void save_mode(Window *w) {
  Buffer *buf;
  buf = buffer_find(w->bufid);
  if (buf) buf->mode = w->mode;
}

void restore_mode(Window *w) {
  Buffer *buf;
  buf = buffer_find(w->bufid);
  if (!buf) {
	w->mode = M_TERM;
	return;
  }
  if (buf->narrow)
	w->mode = M_NARROW;
  else
	w->mode = buf->mode;
}

void switch_mode(Window *w, int mode) {
	w->mode = mode;
	map_mode = 0;
	save_mode(w);
	if (mode == M_MEDIA) pcur = -1;
	wtagrepaint(w);
}

#define REGSWITCHFN(N) \
	void switch_mode##N(Window *w) { switch_mode(w, N); }

REGSWITCHFN(0)
REGSWITCHFN(1)
REGSWITCHFN(2)
REGSWITCHFN(3)

void toggleterm(Window *w) {
  static Keydef key_map[] = {
	{"0", "switch_mode0"}, {"1", "switch_mode1"}, {"2", "switch_mode2"}, {"3", "switch_mode3"},
	nil
  };
  if (!mode_map) {
	mode_map = keymap_load(key_map);
	/* keymap_dump(mode_map); */
  }
  map_mode = 1;
  cur_map = mode_map;
}

void
completeproc(void *arg)
{
	Completejob *job;
	Completion *c, *c1;
	UndoStack *base;
	Window *w;

	job = arg;
	threadsetname("namecomplete %s", job->dir);

	c = c1 = nil;
	w = job->win;
	switch(w->mode) {
	case M_TEXT:
	  if (!*job->str) goto Done;
	  c1 = malloc(sizeof(Completion));
	  memset(c1, 0, sizeof(Completion)); 
	  dictcomplete(c1, job->str);
	  c = complete1(c1->filename, c1->nfile, job->str, 0, 0, 1);
	  break;
	case M_FILE_HIST:
	  base = w->hfile;
	  goto Search;
	case M_CMD_HIST:
	  base = w->hcmd;
	  goto Search;
	case M_SEARCH_HIST:
	  base = w->hsearch;
	Search:
	  c1 = malloc(sizeof(Completion));
	  undolist(base, c1);
	  if (*job->str)
		c = complete1(c1->filename, c1->nfile, job->str, 1, 1, 0);
	  else {
		c = c1; // Reverse chronological order
		c1 = nil;
	  }
	  break;
	default: c = completef(job->dir, job->str); break;
	}

	if(c != nil) {
	  c->force = job->force;
	  if (sendp(job->win->complete, c) <= 0)
		freecompletion(c);
	}

 Done:
	/* wclose(job->win); */

	/* free(job->dir); */
	free(job->str);
	free(job);
	freecompletion(c1);
}

void
cmdhistcomplete(Window *w, int force) {
  /* Read from title window */
  int npath;
  Rune *path;
  char *str;
  Window *w1;

  w1 = w->title;
  /* npath = windfilewidth(w1, w1->q0, FALSE); */
  /* path = w1->r+(w1->q0-npath); */
  /* str = runetobyte(path, npath, &npath); */
  str = runetobyte(w1->r, w1->nr, &npath);

  /* run in background, winctl will collect the result on w->complete chan */
  Completejob *job;
  job = emalloc(sizeof *job);
  job->str = str;
  job->dir = cleanname(w->dir);
  job->win = w;
  job->force = force;
  w->comp_len = npath;
  incref(w);
  proccreate(completeproc, job, STACK);
}

void namecomplete1(Window *w, int force);
void
namecomplete(Window *w, int force) {
  /* fprint(2, "%d%d%d ", w->mode, M_TERM, force); */
  switch(w->mode) {
  case M_TEXT: namecomplete1(w, force); break;
  case M_FILE_HIST: cmdhistcomplete(w, force); break;
  case M_CMD_HIST: cmdhistcomplete(w, force); break;
  case M_SEARCH_HIST: cmdhistcomplete(w, force); break;
  default: namecomplete1(w, force); break;
  }
}

Buffer* buffer_find(int id) {
  Buflist *bl;
  Buffer *buf;
  bl = buffers;
  buf = nil;
  while (bl) {
	if (bl->buf->id == id) {
	  buf = bl->buf;
	  break;
	}
	bl = bl->next;
  }
  return buf;
}

Buffer* buffer_locate(char *dir, char *file) {
	Buflist *bl, *prev, *next;
	Buffer *buf;

	if (!(file && dir))
		return nil;

	buf = nil;
	prev = nil;
	bl = buffers;
	while (bl) {
		next = bl->next;
		if (bl->buf->id == -1) {
			if (prev)
				prev->next = next;
			else
				buffers = next;
			free(bl->buf->r);
			free(bl->buf);
			free(bl);
		} else {
			if (strcmp(bl->buf->file, file) == 0 &&
				strcmp(bl->buf->dir, dir) == 0) {
				buf = bl->buf;
				break;
			}
			prev = bl;
		}
		bl = next;
	}
	return buf;
}

void buffer_save(Window *w, Buffer *buf) {
  if (buf == nil) {
	/* buf = buffer_find(w->bufid); */
	/* if (!buf) */
	  {
	  Buflist *bl;
	  bl  = malloc(sizeof(Buflist));
	  buf = malloc(sizeof(Buffer));
	  buf->id = w->bufid;
	  buf->r  = malloc(w->nr * sizeof(Rune));
	  buf->narrow = nil;
	  bl->buf  = buf;
	  bl->next = buffers;
	  buffers = bl;
	  /* Buffer must have it's own undo stack */
	  w->undo = undostackcreate(100);
	}
  } else
  buf->id = w->bufid;
  buf->q0 = w->q0;
  buf->q1 = w->q1;
  /* buf->r  = w->r; */
  buf->nr = w->nr;
  buf->dir  = w->dir;
  buf->file = w->file;
  buf->modified = w->modified;
  buf->undo = w->undo;
  buf->mode = w->mode;
  buf->r  = runerealloc(buf->r, w->nr);
  runemove(buf->r, w->r, w->nr);
}

void buffer_restore(Window *w, Buffer *buf) {
  /* print("%d %d", w->bufid, buf->id); */
  /* print("%s %d", buf->file, w->bufid); */
  /* if (w->bufid != buf->id) */
	{
	w->undo = nil;
	clear(w);
	winsert(w, buf->r, buf->nr, 0);
  }
  w->bufid = buf->id;
  w->q0 = buf->q0;
  w->q1 = buf->q1;
  w->nr = buf->nr;
  w->qh = buf->nr; // Avoid command execution
  w->dir  = buf->dir;
  w->file = buf->file;
  w->mode = buf->mode;
  w->modified = buf->modified;
  w->undo = buf->undo;
  w->mode = buf->mode;
  wsetselect(w, w->q0, w->q1);
  if (w->enable_title)
	wtagrepaint(w);
}

void buffer_close(Window *w) {
  Buffer *buf;
  buf = buffer_find(w->bufid);
  if (!buf) return; // Buffer deleted

  clear(w);
  w->file = nil;
  w->dir = cwd();
  setmodified(w, 0);
  w->undo = w->scratch;
  undostackfree(buf->undo);
  cleanreg(buf->id);
  buf->id = -1;
  /* free in buffer_locate */
  /* free(buf); */
  /* if (w->enable_title) */
  /* 	wtagrepaint(w); */
}

void buffer_close_all(void) {
  Buflist *bl, *prev;
  Buffer *buf;
  bl = buffers;
  buf = nil;
  while (bl) {
	if ((buf = bl->buf) == nil) {
	  prev->next = bl->next;
	  if (buf) {
		undostackfree(buf->undo);
		free(buf->r);
		free(buf);
	  }
	  free(bl);
	  bl = prev->next;
	} else {
	  prev = bl;
	  bl = bl->next;
	}
  }
}

void pulseline(Window *w, ulong src, ulong dst) {
  Point p0, p1;
  Rectangle r;
  ulong col;
  Image *c;
  int q0, r0, r1;

  q0 = w->q0;
  p1 = frptofchar(w, q0);
  bol(w);
  p0 = frptofchar(w, w->q0);
  r0 = w->q0;
  eol(w);
  r1 = w->q0;
  r = w->Frame.r;
  r.min.y = p0.y;
  r.max.y = p0.y + w->font->height;
  for (double t = 0; t < 1; t += 0.1) {
	col = blendcolor(src, dst, t);
	c = allocimage(display, Rect(0,0,1,1), RGB24, 1, col);
	draw(w->i, r, c, nil, ZP); // Cover complete line
	frdrawsel0(w, p0, r0, r1, c, cols[TEXT]);
	/* frtick(w, p1, 1); */
	flushimage(display, 1);
	sleep(50);
	freeimage(c);
  }
  frdrawsel(w, p0, q0, q0, 0);
  frtick(w, p1, 1);
  wsetselect(w, q0, q0);
  wshow(w, q0);
}

Buffer reg[10];

void savereg(Window *w, int i) {
  Buffer *buf;
  buf = &reg[i];
  buf->id = w->bufid;
  buf->q0 = w->q0;
  /* print("save %d %d %d %d", i, w->q0, buf->id, w->bufid); */
}

void gotoreg(Window *w, int i) {
  int q0, id;
  Buffer *buf;
  buf = &reg[i];
  id = buf->id;
  q0 = buf->q0;
  /* print("goto %d %d %d %d", i, q0, id, w->bufid); */
  if (id < 0) return;
  
  if (id != w->bufid) {
	buf = buffer_find(buf->id);
	if (!buf) return; // Buffer deleted

	buffer_restore(w, buf);
  }
  // Bring q0 to view
  wsetselect(w, q0, q0);
  wshow(w, q0);
  pulseline1(w);
}

#define REGFN(N) \
	void savereg##N(Window *w) { savereg(w, N); } \
	void gotoreg##N(Window *w) { gotoreg(w, N); }

REGFN(0)
REGFN(1)
REGFN(2)
REGFN(3)
REGFN(4)
REGFN(5)
REGFN(6)
REGFN(7)
REGFN(8)
REGFN(9)

void
savehistory(Window *w)
{
	char *f, *c;

	f = strcat(getenv("home"), FILE_HIST);
	c = strcat(getenv("home"), CMD_HIST);
	undofilesave(w->hfile, f);
	undofilesave(w->hcmd , c);
}

void
openproc(void *arg)
{
	Window *w;

	w = arg;
	undofileload(w->hfile, strcat(getenv("home"), FILE_HIST));
	undofileload(w->hcmd , strcat(getenv("home"), CMD_HIST));

	/* threadcreate(saveproc, w, STACK); */
	/* proccreate(saveproc, w, STACK); */
}

void multimark(Window *w, int q0) {
  w->mode = M_MULTI_EDIT;
  keymap_reset();
  if (!w->mq) {
	w->maxnm = 10;
	w->mq = malloc(w->maxnm * sizeof(uint));
  }
  if (w->nm > w->maxnm) {
	w->maxnm += 10;
	w->mq = realloc(w->mq, w->maxnm * sizeof(uint));
  }
  w->mq[w->nm++] = q0;
  qsort(w->mq, w->nm, sizeof(uint), intpcmp);
  undoboundary(w->undo);
}

/* wsetselect removes the marks. Use drawmmark() to redraw. */
void drawmmark(Window *w, int n, int offset) {
  Point p1;
  uint q0;

  for (int i = w->nm - 1; i >= 0; i--) {
	/* Multiple ticks work only after frdrawsel */
	q0 = w->mq[i];
	w->mq[i] += offset;
	p1 = frptofchar(w, q0);
	frdrawsel(w, p1, q0, q0 + n, 1);
	p1 = frptofchar(w, q0 + offset);
	frtick(w, p1, 1);
  }
}

void multiedit_end(Window *w) {
  uint q0;

  if (w->mode != M_MULTI_EDIT) return;

  q0 = w->q0;
  wsetselect(w, q0, q0);
  wshow(w, q0);
  w->nm = 0;
  restore_mode(w);
  wrepaint(w);
  wtagrepaint(w);
  undoboundary(w->undo);
  return;
}

void wordatpt(Window *w) {
	uint q0, q1;
  
	if (w->q0 > 0 && !isspace(w->r[w->q0 - 1]))
	  wordleft(w, 1);
	q0 = w->q0;
	wordright(w, 1);
	q1 = w->q0;
	wsetselect(w, q0, q1);
}

void multiedit(Window *w) {
  uint q0, q1, qi, offset, n;
  Point p1;

  /* fprint(2, "a%d %d-%d ", q0, w->q0, w->q1); */
  if (w->nr == 0 || w->mode == M_MULTI_EDIT) {
	multiedit_end(w);
	return;
  }

  qi = q0 = w->q0;
  if (w->q0 == w->q1) {
	if (w->r[w->q0 - 1] != ' ')
	  wordleft(w, 1);
	q0 = w->q0;
	wordright(w, 1);
	q1 = w->q0;
	w->q0 = q0; //wlook() searches for text between w->q0 & w->q1
  }

  n = q1 - q0;
  offset = qi - w->q0;
  do {
	multimark(w, w->q0);
	wlook(w, nil);
	/* fprint(2, "a%d %d-%d ", q0, w->q0, w->q1); */
  } while (q0 != w->q0);

  w->q0 = qi;
  wtagrepaint(w);
  drawmmark(w, n, offset);
}

void narrow(Window *w) {
  Buffer *buf, *buf1;
  Buflist *bl;
  int n;

  if (w->q0 == w->q1) return;

  buf1 = buffer_find(w->bufid);
  n = w->q1 - w->q0;
  bl  = malloc(sizeof(Buflist));
  buf = mallocz(sizeof(Buffer), 1);
  bl->buf  = buf;
  bl->next = buf1->narrow;

  if (buf1->narrow)
	buffer_save(w, buf1->narrow->buf);
  else
	buffer_save(w, buf1);

  buf1->narrow = bl;
  buf->id = w->bufid;
  buf->r    = malloc(n * sizeof(Rune));
  /* Buffer must have it's own undo stack */
  buf->undo = undostackcreate(100);
  buf->nr   = n;
  buf->dir  = w->dir;
  buf->file = w->file;
  buf->mode = M_NARROW;
  buf->modified = w->modified;
  runemove(buf->r, w->r + w->q0, n);
  buffer_restore(w, buf);
}

void widen(Window *w) {
  Buffer *buf, *buf1;
  Buflist *bl;
  Rune *r;
  uint n;

  if (w->mode != M_NARROW) return;
  buf1 = buffer_find(w->bufid);
  if (!(bl = buf1->narrow)) {
	w->mode = buf1->mode;
	return;
  }

  buf = bl->buf;
  buffer_save(w, buf);
  r = buf->r;
  n = buf->nr;
  buf1->narrow = bl->next;
  free(bl);
  undostackfree(buf->undo);
  free(buf);

  buf = nil;
  if ((bl = buf1->narrow))
	buf = bl->buf;
  else buf = buf1;
  buffer_restore(w, buf);
  wdelete(w, buf->q0, buf->q1);
  winsert(w, r, n, buf->q0);
  w->qh = w->nr;
  free(r);
}

void drawtooltip(Image *screen, Point p, Rune *text) {
	Point size, tp;
	Rectangle r;
	Image *bg, *fg;
	int padding;

	bg = allocimagemix(display, DPaleyellow, DWhite);
	fg = display->black;
	padding = 5;

	size = runestringsize(font, text);
	r.min = addpt(p, Pt(10, 10));
	r.max = addpt(r.min, addpt(size, Pt(padding*2, padding*2)));

	/* Draw shaded box */
	draw(screen, r, bg, nil, ZP);
	border(screen, r, 1, fg, ZP);

	/* Draw text */
	tp = addpt(r.min, Pt(padding, padding));
	runestring(screen, tp, fg, ZP, font, text);
	freeimage(bg);
}

void initbuttons(Window *w, Rectangle r) {
  int i, n, w1, h, pad;
	Button *b;
	Point pb;
	Rune **l, **c, **t;

	Rune *l1[] = { L"↺", L"►", L"■", L"⇾", L"↳", L"↰", L"≣", L"×" };
	/* Rune *l1[] = { L"r", L"c", L"s", L"ov", L"i", L"ou", L"bt", L"q" }; */
	Rune *c1[] = { L"lnew()", L"lcont()", L"stop(pid)", L"next()", L"stmnt()", L"lfunc()",
	  L"stk()", L"quit()" };
	Rune *t1[] = { L"Restart", L"Continue", L"Stop", L"Step Over", L"Step Into", L"Step Out",
	  L"Stacktrace", L"Quit" };

	Rune *l2[] = { L"⏮", L"⤨", L"⏯", L"↺", L"⏭" };
	/* Rune *l2[] = { L"pr", L"s", L"p", L"r", L"n" }; */
	Rune *c2[] = { L"prev", L"shuffle", L"play", L"repeat", L"next" };
	Rune *t2[] = { L"Previous", L"Shuffle", L"Play", L"Repeat", L"Next" };

	l = l2;
	c = c2;
	t = t2;
	n = nelem(l2);
	if (w->mode == M_DEBUG) {
		l = l1;
		c = c1;
		t = t1;
		n = nelem(l1);
	}

	w->nbtns   = n;
	w->buttons = malloc(w->nbtns * sizeof(Button));
	pad = 5;

	w1 = (Dx(r) - (n - 1) * pad) / n;
	h  = Dy(r);
	pb = r.min;
	/* print("%d %d", w1, h); */
	for(i = 0; i < w->nbtns; i++) {
	  b = &w->buttons[i];
	  b->label = l[i];
	  b->tooltip = t[i];
	  b->cmd = c[i];
	  b->r = Rpt(pb, addpt(pb, Pt(w1, h)));
	  pb.x += w1 + pad;
	}
}

void clearoverlay(Window *w) {
	Rectangle ro;
	if (!(saved && w->overlay)) return;

	ro = w->overlay->r;
	draw(screen, ro, saved, nil, ro.min);
	/* flushimage(display, 1); */
	freeimage(saved);
	saved = nil;
}

/* return index of character at point p in frame fr,
 * or -1 if p is not on any rune glyph.
 */
int frcharhit(Frame *fr, Point p) {
	int n;
	int x0, x1;

	/* find nearest character index */
	n = frcharofpt(fr, p);

	/* before first char? */
	if (n <= 0) {
		if(fr->nchars == 0)
			return -1; /* empty frame */

		/* is p inside first glyph box? */
		x0 = frptofchar(fr, 0).x;
		x1 = frptofchar(fr, 1).x;
		if(p.x >= x0 && p.x < x1)
			return 0;

		return -1;
	}

	/* after last char? */
	if (n >= fr->nchars)
		return -1;

	/* check x bounds of glyph n */
	x0 = frptofchar(fr, n).x;
	x1 = frptofchar(fr, n+1).x;

	if(p.x >= x0 && p.x < x1)
		return n;

	return -1;
}

extern Channel *kbdchan;
void showvar(Window *w) {
  int q0, q1, qh, qh1, n;
  Rune *k, *v;
  Fmt f;
  char *cmd;

  q0 = frcharhit(w, mp);
  if (q0 == -1) {
	/* w->hovering = 0; */
	return;
  }

  q0 += w->org;
  w->hovering = 1;
  if (tip.q0 == q0 && tip.r) {
	drawtooltip(screen, tip.p, tip.r);
	/* Don't free tip here to be able to re-use while the cursor is in the glyph region  */
	return;
  }
  free(tip.r);
  tip.r = nil;
  tip.q0 = q0;
  tip.p  = mp;
  
  qh = q0;
  wsetselect(w, q0, q0);
  wordatpt(w);
  q0 = w->q0;
  q1 = w->q1;
  if (q1 < qh) {
	// i++ point is on +; ignore
	q0 = w->qh;
	wsetselect(w, q0, q0);
	return;
  }
  qh = w->qh;
  /* cmd = smprint("lv(\"%.*S\")\n", (w->q1 - w->q0), w->r + w->q0); */
  /* print("%d %s", (w->q1 - w->q0), cmd); */
  runefmtstrinit(&f);
  fmtprint(&f, "lv(\"%.*S\")\n", (w->q1 - w->q0), w->r + w->q0);
  k = runefmtstrflush(&f);
  n = runestrlen(k);
  wsetselect(w, qh, qh);
  winsert(w, k, n, w->q0);

  tip.qh1 = qh;
  tip.qh2 = qh + n;
  /* print("a%d ", tip.qh2); */
  /* Simulate keypress */
  cmd = smprint("k \n");
  nbsend(kbdchan, &cmd);

  free(k);
}

int mountmediasrv(void) {
	char *srv = "/srv/mplayerctl";
	char *mnt = "/mnt/mplayer";
	int fd;

	/* open the service file posted by the server */
	fd = open(srv, ORDWR);
	if(fd < 0) return 0;

	/* ensure the mountpoint exists */
	if(access(mnt, AEXIST) < 0)
		create(mnt, OREAD, DMDIR|0555);

	/* mount it into our namespace */
	if(mount(fd, -1, mnt, MREPL, "") < 0)
		sysfatal("mount %s: %r", mnt);

	/* print("mounted %s to %s\n", srv, mnt); */
	return 1;
}

void oaction(Window *w) {
	int i, fd, n;
	Rune *s;
	char cmd[64];
	char *mtpt = "/mnt/mplayer/ctl";
	Rectangle ro;

	for(i = 0; i < w->nbtns; i++) {
	  if(ptinrect(w->mc.xy, w->buttons[i].r)) {
		/* fprint(2, "Clicked %s\n", w->buttons[i].label); */
		idle = 0;
		s = w->buttons[i].cmd;
		/* clearoverlay(w); */
		ro = insetrect(w->i->r, Selborder);
		draw(screen, ro, saved, nil, ro.min);
		if (w->mode == M_MEDIA) {
		  if(access(mtpt, AEXIST) == 0 || mountmediasrv()) {
		  fd = open(mtpt, OWRITE);
		  if (fd >= 0) {
			snprint(cmd, sizeof(cmd), "%S", s);
			/* fprint(2, "2 %s ", cmd); */
			write(fd, cmd, strlen(cmd));
			pcur = -1;
			/* if ((n = read(fd, cmd, sizeof(cmd))) > 0) { */
			/*   cmd[n] = 0; */
			/*   pcur = atoi(cmd); */
			/* } */
			close(fd);
		  }
		  }
		} else {
		  /* Pass to the current program */
		  clear(w);
		  winsert(w, s, runestrlen(s), w->q0);
		  winsert(w, L"\n", 1, w->q0);
		  w->hovering = 0;
		  idle = 1;
		}
		break;
	  }
	}
}

Image *fetchart(int px) {
	int fd;
	Image *art;
	int pipe1[2];

	char *res[] = { "/bin/resample", nil, "/mnt/mplayer/artwork", nil };

	if(pipe(pipe1) < 0) sysfatal("pipe1: %r");

	res[1] = smprint("-x %d", px);
	spawnstage(res, -1, pipe1[1]);
	close(pipe1[1]);

	art = readimage(display, pipe1[0], 0);
	close(pipe1[0]);
	free(res[1]);

	return art;
}

/* Draw cover art from /mnt/mplayer/artwork into Image *b */
void
drawart(Image *b)
{
	Image *art;
	Rectangle r, br;
	int aw, ah, bw, bh;
	Point p;

	if(b == nil) return;
	
	br = b->r;
	bw = Dx(br);
	bh = Dy(br);
	art = cover;
	if(art == nil) return;

	/* Center artwork on target image */
	r = art->r;
	aw = Dx(r);
	ah = Dy(r);

	p.x = br.min.x + (bw - aw)/2;
	p.y = br.min.y + (bh - ah)/2;

	draw(b, rectaddpt(r, subpt(p, r.min)), art, nil, r.min);
}

void drawoverlay(Window *w) {
    Button *b, *tip;
    Rectangle r, ro, rp;
    Image *o, *ocol, *ocol1;
	Point p, size;
	int h;

	ro = insetrect(w->i->r, Selborder);
	r  = insetrect(w->i->r, 150);
	rp = insetrect(r, 30);
	if (w->mode == M_DEBUG) {
	  r  = insetrect(w->i->r, 180);
	  h = 0;
	  if (w->title)
		h = Dy(w->title->screenr);
	  r  = rectsubpt(r, Pt(0, (Dy(ro) - Dy(r))/2 - h));
	  rp = insetrect(r, 3);
	}
	w->orect = r;
	if(saved)
	  freeimage(saved);
	saved = allocimage(display, ro, screen->chan, 0, DNofill);
	draw(saved, ro, screen, nil, ro.min);
	o = allocimage(display, ro, RGBA32, 0, setalpha(DRed, (w->mode == M_DEBUG) ? 0 : 0x88));
	ocol = allocimage(display, Rect(0,0,1,1), RGBA32, 1, setalpha(DPalegreygreen, 0x88));
	ocol1 = allocimage(display, Rect(0,0,1,1), RGBA32, 1, DPalegreygreen);

	w->overlay = namedimage(display, (w->mode == M_DEBUG) ? "odebug" : "omedia");
	/* preallocate overlay */
	if (!w->overlay) {
	  w->overlay = ocol;

	  replclipr(o, 0, r);
	  draw(o, r, w->overlay, nil, ZP);
	  /* replclipr(o, 0, ro); */

	  /* rp = insetrect(r, 30); */
	  initbuttons(w, rp);
	  for (int i = 0; i < w->nbtns; i++) {
		b = &w->buttons[i];
		border(o, b->r, 1, cols[TEXT], ZP);
		size = runestringsize(font, b->label);
		p = addpt(b->r.min, divpt(Pt(Dx(b->r) - size.x, Dy(b->r) - size.y), 2));
		runestring(o, p, cols[TEXT], ZP, font, b->label);
	  }

	  /* Save a copy */
	  w->overlay = allocimage(display, r, RGBA32, 0,
							  setalpha(DOpaque, (w->mode == M_DEBUG) ? 0 : 0x88));
	  draw(w->overlay, r, o, nil, r.min);
	  nameimage(w->overlay, (w->mode == M_DEBUG) ? "odebug" : "omedia", 1);
	}

	/* Layers: Transparent, artwork, panel, highlight */
	if (ptinrect(mp, r)) {
	  tip = nil;
	  w->hovering = 1;
	  replclipr(o, 0, ro);
	  if (cover && w->mode == M_MEDIA) {
		drawart(o);
	  }
	draw(o, r, w->overlay, nil, r.min);
	for (int i = 0; i < w->nbtns; i++) {
	  b = &w->buttons[i];
	  if (ptinrect(mp, b->r)) {
		/* Draw button again to preserve all features */
		draw(o, b->r, ocol1, nil, ZP); // Highlight button
		border(o, b->r, 1, cols[TEXT], ZP);
		size = runestringsize(font, b->label);
		p = addpt(b->r.min, divpt(Pt(Dx(b->r) - size.x, Dy(b->r) - size.y), 2));
		runestring(o, p, cols[TEXT], ZP, font, b->label);
		tip = b;
		break;
	  }
	}
	/* draw tooltip on top of buttons */
	if (tip) drawtooltip(o, mp, tip->tooltip);

	/* draw overlay on screen only */
	draw(screen, ro, o, nil, ro.min);
	} else if (w->mode == M_DEBUG)
	  showvar(w);
	flushimage(display, 1);

	freeimage(o);
	freeimage(ocol);
}

void hoverexit(Window *w) {
  /* fprint(2, "h1 %x %d, ", w, w->hovering); */
  Rectangle r, ro;

  w->overlay = namedimage(display, (w->mode == M_DEBUG) ? "odebug" : "omedia");
  if (w->hovering && w->overlay) {
	/* clearoverlay(w); */
	ro = insetrect(w->i->r, Selborder);
	draw(screen, ro, saved, nil, ro.min);
	r  = w->orect;
	if (ptinrect(mp, r)) {
	  /* fprint(2, "h1 %x %d, ", w, ptinrect(mp, r)); */
	  idle = 0;
	  drawoverlay(w);
	} else {
	  w->hovering = 0;
	  flushimage(display, 1);
	}
  }
}

void
idlewatch1(void*)
{
    Window *w;
    int i, j, drawing;
    Rectangle r;
	char cmd[64];
	int fd, n;

    j = drawing = 0;
	pcur = -1;
    for (;;) {
        if (!idle) {
            sleep(500);
            continue;
        }

        w = input;
        if (!w) {
		  idle = 0;
		  continue;
		}

		/* Delay cover update to ensure new one is available */
		if (w->mode == M_MEDIA && pcur != plast) {
		  char *mtpt = "/mnt/mplayer/ctl";
		  /* fprint(2, "b1 %d %d %d ", cover == nil, pcur, plast); */
		  if (mountmediasrv() && (fd = open(mtpt, OREAD)) >= 0) {
			if ((n = read(fd, cmd, sizeof(cmd))) > 0) {
			  cmd[n] = 0;
			  pcur = atoi(cmd);
			}
			close(fd);
			/* fprint(2, "b2 %x %d %d ", cover, pcur, plast); */
			if (pcur != plast)
			{
			  freeimage(cover);
			  cover = fetchart(Dy(w->i->r));
			  /* fprint(2, "%x ", cover); */
			  if (cover) plast = pcur;
			}
		  }
		}

		/* Activate hover */
		r  = insetrect(w->i->r, 150);
        if (!drawing && !w->hovering && (w->mode == M_MEDIA || w->mode == M_DEBUG)) {
		  /* fprint(2, "h %x %d, ", w, ptinrect(mp, r)); */
		  drawing = 1;
		  drawoverlay(w);
		  if (!saved) w->hovering = 0;
		  drawing = 0;
        }

        /* Run idle tasks */
        if (j >= nwindow - 1) j = 0;
        for (i = j; i < nwindow && idle; i++) {
            w = window[i];
            savehistory(w);
            j = i;
        }
        idle = 0;
    }
}

int idlehandler(void *, char *msg) {
  /* fprint(2, "notified: %s\n", msg); */
  if (strcmp(msg, "alarm") == 0) {
	/* idlewatch(); */
	idle = 1;
	return 1;
  }
  return 0;
}

void self_insert(Window *w, Rune s, int n) {
  /* if (s == '\n') print("insert %c %d\n", s, w->qh); */
  Window *w1 = w;
  if (w->title && w->title->active) {
	w1 = w->title;
	if (s == '\n') {
	  if (w->mode == M_CMD_HIST) {
		winsert(w, w1->r, w1->nr, w->q0);
		winsert(w, &s, 1, w->q0);
	  } else if (w->mode == M_SEARCH_HIST) {
		find(w);
	  } else if (w->mode == M_INPUT) {
		w1->cmd(w, w1->r);
	  } else {
	  if (w->saving) fsave(w);
	  else fopen(w);
	  }
	}
	if (s == '\n' || s == Kesc) {
	  w->title->active = 0;
	  w->title->input = nil;
	  w->saving = 0;
	  w->mode = (s == Kesc) ? M_TERM : w->prev_mode;
	  s = -1;
	  wclosepopup(w);
	  wtagrepaint(w);
	}
  }
  uint q0 = w1->q0;
  if (s != -1) {
  if (s == '\t' && w->popup && w->popup->i) {
	Window *p = w->popup;
	int i1, i2, n, nr;
	Fmt f;
	Rune *rp;
	if (w->c && p->q0 < p->nr) {
	  i1 = 0;
	  if (w->mode == M_FILE_HIST || w->mode == M_CMD_HIST || w->mode == M_SEARCH_HIST)
		wdelete(w1, 0, w1->nr);
	  else
		i1 += w->comp_len;
	
	  /* i1 = p->q0; */
	  runefmtstrinit(&f);
	  n = curline(p);
	  /* print("%d %d %s", p->q0, n, (w->c->filename[n])); */
	  fmtprint(&f, "%s", w->c->filename[n]);
	  rp = runefmtstrflush(&f);
	  nr = runestrlen(rp);
	  /* if (w->mode == M_TERM || w->mode == M_TEXT) */
	  /* 	i1 += w->comp_len; */
	  /* i2 = p->q1 - 1; */
	  /* wsetselect(p, i1, i2); */
	  /* winsert(w1, p->r + i1, i2 - i1, w1->q0); */
	  winsert(w1, rp + i1, nr - i1, w1->q0);
	  q0 = w1->q0 - 1;
	  /* fprint(2, "%d %d %d, ", w1->nr, w1->q0); */
	  if (q0 > 0 && w1->nr > 0 && w1->r[q0] == '/' && w->mode == M_TERM)
		s = '/';
	  free(rp);
	}
  } else if (w->mode == M_MULTI_EDIT) {
	if (s == Kesc) {
	  multiedit_end(w);
	  s = ' ';
	} else {
	  winsert(w1, &s, n, q0);
	}
  } else if (s == Kesc) {
	w->holding = !w->holding;
	/* Handles painting for both cases */
	wsendctlmesg(w, Holdoff, ZR, nil);
  } else {
	/* Replace existing selection */
	cut(w1);
	winsert(w1, &s, n, q0);
  }
  /* wshow(w, q0 + 1); */
  
  wclosepopup(w);
  if (!isspace(s))
	namecomplete(w, 0);
  }
}

void confirmexit(void);

struct { const char *s; int argc; void (*f); } prim[] = {
  {"bol", 1, bol}, {"eol", 1, eol}, {"bob", 1, bob}, {"eob", 1, eob},
  {"left", 2, charleft}, {"right", 2, charright}, {"up", 2, lineup}, {"down", 2, linedown},  
  {"wordleft", 2, wordleft}, {"wordright", 2, wordright},
  {"cut", 1, cut}, {"copy", 1, copy}, {"paste", 1, paste},
  {"find", 1, find}, {"rfind", 1, wrlook}, {"plumb", 1, wplumb},
  {"delcharl", 2, delcharl}, {"delcharr", 2, delcharr},
  {"delwordl", 2, delwordl}, {"delwordr", 2, delwordr},
  {"dellinel", 2, dellinel}, {"delliner", 2, delliner},
  {"interrupt", 1, interrupt}, {"autosuggest", 2, namecomplete},
  {"scrollup", 2, scrollup}, {"scrolldown", 2, scrolldown}, {"togglescroll", 1, togglescroll},
  {"selectall", 1, selectall}, {"clear", 2, clear},
  {"cropwindow", 1, cropwindow}, {"cropscreen", 0, cropscreen},
  {"undo", 1, wundo}, {"redo", 1, wredo}, {"open", 1, fopen}, {"save", 1, fsave},
  {"filehistory", 1, filehistory}, {"cmdhistory", 1, cmdhistory},
  {"toggleterm", 1, toggleterm}, {"bufferclose", 1, buffer_close},
  {"gotoline", 2, gotoline}, {"gotochar", 2, gotochar},
  {"gotocol", 2, gotocol}, {"selectfrline", 2, selectfrline},
  { "gotoreg0", 1, gotoreg0 }, { "savereg0", 1, savereg0 },
  { "gotoreg1", 1, gotoreg1 }, { "savereg1", 1, savereg1 },
  { "gotoreg2", 1, gotoreg2 }, { "savereg2", 1, savereg2 },
  { "gotoreg3", 1, gotoreg3 }, { "savereg3", 1, savereg3 },
  { "gotoreg4", 1, gotoreg4 }, { "savereg4", 1, savereg4 },
  { "gotoreg5", 1, gotoreg5 }, { "savereg5", 1, savereg5 },
  { "gotoreg6", 1, gotoreg6 }, { "savereg6", 1, savereg6 },
  { "gotoreg7", 1, gotoreg7 }, { "savereg7", 1, savereg7 },
  { "gotoreg8", 1, gotoreg8 }, { "savereg8", 1, savereg8 },
  { "gotoreg9", 1, gotoreg9 }, { "savereg9", 1, savereg9 },
  {"multiedit", 1, multiedit}, {"narrow", 1, narrow}, {"widen", 1, widen},
  { "switch_mode0", 1, switch_mode0 },
  { "switch_mode1", 1, switch_mode1 },
  { "switch_mode2", 1, switch_mode2 },
  { "switch_mode3", 1, switch_mode3 },
  {"exit", 0, confirmexit},
};

/* Special keys mapped for handling. */
struct {char *s; Rune r;} keys_mapped[] = {
  {"C", Kctl}, {"M", Kalt}, {"S", Kshift}, {"mod4", Kmod4},
  {"left", Kleft}, {"right", Kright}, {"up", Kup}, {"down", Kdown},
  {"backspace", Kbs}, {"tab", 0x09}, {"ret", 0x0a}, {"del", Kdel},
  {"spc", ' '}, {"pgup", Kpgup}, {"pgdown", Kpgdown},
  {"home", Khome}, {"end", Kend}, {"print", Kprint}, {"kscroll", Kscroll},
};

Keydef key_map[] = {
  /* Use S + lower case letter for uppercase letters eg. "C-S-q" */
  {"C-x", "cut"}, {"C-c", "copy"}, {"C-v", "paste"}, {"C-S-q", "exit"},
  {"left", "left"}, {"right", "right"}, {"up", "up"}, {"down", "down"},
  {"C-left", "wordleft"}, {"C-right", "wordright"},
  {"C-a", "bol"}, {"C-e", "eol"}, {"M-<", "bob"}, {"C-b", "eob"},
  {"C-f", "find"}, {"C-r", "rfind"}, {"C-ret", "plumb"},
  {"C-tab", "autosuggest"},
  {"backspace", "delcharl"}, {"C-d", "delcharr"},
  {"C-backspace", "delwordl"}, {"C-S-d", "delwordr"},
  {"C-u", "dellinel"}, {"C-k", "delliner"},
  {"del", "interrupt"}, {"C-g-g", "gotoline"}, {"C-g-c", "gotocol"},
  {"pgup", "scrollup"}, {"pgdown", "scrolldown"}, {"kscroll", "togglescroll"},
  {"C-home", "bob"}, {"C-end", "eob"},
  {"M-a", "selectall"}, {"C-l", "clear"},
  {"print", "cropscreen"}, {"C-print", "cropwindow"},
  {"C-z", "undo"}, {"C-y", "redo"}, {"C-o", "open"}, {"C-s", "save"},
  {"C-m", "filehistory"}, {"C-n", "cmdhistory"}, {"C-t", "toggleterm"},
  { "C-0", "gotoreg0" }, { "C-S-0", "savereg0" },
  { "C-1", "gotoreg1" }, { "C-S-1", "savereg1" },
  { "C-2", "gotoreg2" }, { "C-S-2", "savereg2" },
  { "C-3", "gotoreg3" }, { "C-S-3", "savereg3" },
  { "C-4", "gotoreg4" }, { "C-S-4", "savereg4" },
  { "C-5", "gotoreg5" }, { "C-S-5", "savereg5" },
  { "C-6", "gotoreg6" }, { "C-S-6", "savereg6" },
  { "C-7", "gotoreg7" }, { "C-S-7", "savereg7" },
  { "C-8", "gotoreg8" }, { "C-S-8", "savereg8" },
  { "C-9", "gotoreg9" }, { "C-S-9", "savereg9" },
  {"C-q", "bufferclose"}, {"C-;", "multiedit"},
  {"C-[", "narrow"}, {"C-]", "widen"},
  nil
};

int keymap_find(Keymap* cur_map, Rune k) {
  int found = -1;
  for (int j = 0; j < cur_map->n; ++j) {
	if ((k == cur_map->k[j])) {
	  found = j;
	  break;
	}
  }
  return found;
}

void keymap_set_key(Keymap *map, Rune k, char *cmd) {
  int n = map->n, match = 0;
  for (int i = 0; prim[i].s && !match; ++i) {
	/* print("%d %s\n", i, prim[i].s); */
	if (strcmp(prim[i].s, cmd) == 0) {
	  Keymap  *cmap;// = emalloc(sizeof(Keymap));
	  Command *c = emalloc(sizeof(Command));
	  cmap = map;
	  c->s = prim[i].s;
	  c->argc = prim[i].argc;
	  switch(prim[i].argc) {
	  case 0:
		c->f0 = prim[i].f;
		break;
	  case 1:
		c->f1 = prim[i].f;
		break;
	  case 2:
		c->f2 = prim[i].f;
		break;
	  }
	  cmap->iscmd = 1;
	  cmap->cmd = c;
	  
	  map->k[n] = k;
	  map->val[n] = cmap;
	  map->n = ++n;
	  match = 1;
	}
  }
}

void keymap_reset(void) {
  /* Reset after command execution */
  cur_map = NULL;
  prefix = 0;
  map_mode = 0;
}

int
shiftedchar(int c)
{
	switch(c){
	case '1': return '!';
	case '2': return '@';
	case '3': return '#';
	case '4': return '$';
	case '5': return '%';
	case '6': return '^';
	case '7': return '&';
	case '8': return '*';
	case '9': return '(';
	case '0': return ')';
	case '-': return '_';
	case '=': return '+';
	case '[': return '{';
	case ']': return '}';
	case '\\': return '|';
	case ';': return ':';
	case '\'': return '"';
	case ',': return '<';
	case '.': return '>';
	case '/': return '?';
	case '`': return '~';
	default:
		if(c >= 'a' && c <= 'z')
			return c - 'a' + 'A';
		else if(c >= 'A' && c <= 'Z')
			return c + ' ';
		return c;
	}
}

void keymap_exec(Window *win, Rune seq) {
  Keymap *m, *m_shift;
  int j, modifier;

  if (cur_map == NULL)
	cur_map = global_map;
  m = cur_map;

  modifier = mod;
  /* print("%x %x - ", seq, modifier); */
  if (modifier) {
	/* Don't handle SHIFT here */
	if (modifier & Mctl) {
	  j = keymap_find(cur_map, Kctl);
	  modifier ^= Mctl;
	  /* Exclude TAB and RET */
	  /* if (seq <= 26 && seq != 0x09 && seq != 0x0a) */
	  /* 	seq += 96; */
	} else if (modifier & Malt) {
	  j = keymap_find(cur_map, Kalt);
	  modifier ^= Malt;
	} else if (modifier & Mmod4) {
	  j = keymap_find(cur_map, Kmod4);
	  modifier ^= Mmod4;
	}
	if (j > -1)
	  m = m->val[j];
  }

  j = -1;
  if (shiftdown) {
	/* compare after switching case: Ctrl + Shift + e */
	j = keymap_find(m, Kshift);
	if (j > -1) {
	  m_shift = m->val[j];
	  j = keymap_find(m_shift, shiftedchar(seq));
	  /* print("S %d", j); */
	  if(j == -1) {
		j = keymap_find(m_shift, (seq));
		/* print("S1 %d", j); */
	  }
	  if (j > -1)
		m = m_shift;
	}
	if(j == -1) {
	  j = keymap_find(m, shiftedchar(seq));
	  /* print("S2 %d", j); */
	}
  }
  if(j == -1)
	j = keymap_find(m, seq);
  /* print(" %d %c %c, ", j, seq, shiftedchar(seq)); */
  if(j == -1) {
	/* print("insert "); */
	self_insert(win, seq, prefix ? prefix : 1);
	keymap_reset();
	return;	// no such path
  }

  prefix = 1;
  m = m->val[j];
  if (m->iscmd) {
	/* print("cmd %s ", m->cmd->s); */
	Command *cmd = m->cmd;
	int updown = 0;
	if (strcmp(cmd->s, "up") == 0 || strcmp(cmd->s, "down") == 0
		|| strcmp(cmd->s, "scrollup") == 0 || strcmp(cmd->s, "scrolldown") == 0)
	  updown = 1;
	else
	  /* if (!(updown */
	  /* 		|| strcmp(cmd->s, "delcharl") == 0 */
	  /* 		|| strcmp(cmd->s, "bob") == 0 || strcmp(cmd->s, "eob") == 0)) */
	  wclosepopup(win);
	if (win->title && win->title->active && !updown && !(strcmp(cmd->s, "delcharl") == 0))
	  win = win->title;
	switch (cmd->argc) {
	case 0:
	  (*cmd->f0)();
	  break;
	case 1:
	  (*cmd->f1)(win);
	  break;
	case 2:
	  (*cmd->f2)(win, prefix ? prefix : 1);
	  break;
	}
	/* map_mode=1 indicates command has replaced cur_map; hence DO NOT reset */
	if (!map_mode) keymap_reset();
  } else
	cur_map = m;
}

Keymap* keymap_new(int n) {
  Keymap* map;
  map = emalloc(sizeof(Keymap));
  map->n   = 0;
  map->len = n;
  map->iscmd = 0;
  map->k   = emalloc(n * sizeof(int));
  map->val = emalloc(n * sizeof(Keymap));
  return map;
}

void keymap_free(Keymap* map) {
  for(int i = 0; i < map->n; i++)
	keymap_free(map->val[i]);
	free(map->k);
	free(map);
}

void keymap_copy(Keymap* dest, Keymap* map) {
  if(dest->n < map->n) {
	keymap_free(dest);
	dest = keymap_new(map->n + 3);
  }
  for(int i = 0; i < map->n; i++) {
	dest->k[i]   = map->k[i];
	dest->val[i] = map->val[i];
  }
}

Keymap* keymap_grow(Keymap* map) {
  Keymap* dest;
  dest = keymap_new(map->n + 3);
  dest->n = map->n;
  for(int i = 0; i < map->n; i++) {
	dest->k[i]   = map->k[i];
	dest->val[i] = map->val[i];
  }
  free(map);
  return dest;
}

/* Helper to print runes nicely */
void print_rune(Rune r) {
	int i = 0;
	for (int m = 0; m < nelem(keys_mapped); m++) {
	  if ((r == keys_mapped[m].r)) {
		print("%s ", keys_mapped[m].s);
		i = 1;
		break;
	  }
	}
	if (!i) print("%c ", r);
}

/* Recursive helper to dump a keymap */
void keymap_dump_recursive(Keymap *map, Rune *prefix, int depth) {
  if(map == nil)
	return;

  for(int i = 0; i < map->n; i++){
	Rune key = map->k[i];
	Keymap *child = map->val[i];

	prefix[depth] = key;
	prefix[depth+1] = 0;

	/* if this node itself has a command, print it */
	if(child && child->iscmd){
	  print("Sequence: ");
	  for(int j = 0; j <= depth; j++)
		print_rune(prefix[j]);
		fprint(2, "%20s\n", child->cmd->s);
	}

	/* recurse to explore deeper sequences */
	if(child != nil && !child->iscmd)
	  keymap_dump_recursive(child, prefix, depth+1);
  }
}

/* Keymap dump function */
void keymap_dump(Keymap *map) {
  Rune buf[32];  // Max depth of 32 keys
  keymap_dump_recursive(map, buf, 0);
}

Keymap *keymap_load(Keydef key_map[]) {
  Keymap *global_map;
  Keymap *prev_map;
  Keymap *cur_map;
  Rune r;

  initui();
  /* if(global_map == NULL) */
  /* 	global_map = keymap_new(nelem(keys_mapped)); */
  global_map = keymap_new(1);

  for(int i = 0; key_map[i].k; i++) {
	prev_map = global_map;
	cur_map = NULL;

	char *seq = key_map[i].k;
	char *tok;
	for(int j = 0; seq[j]; j++) {
	  tok = seq + j;
	  if ((*tok == '-') || (*tok == ' '))
		continue;

	  // Convert token to Rune
	  r = *tok;
	  for (int m = 0; m < nelem(keys_mapped); m++) {
		int l = strlen(keys_mapped[m].s);
		if (strncmp(tok, keys_mapped[m].s, l) == 0) {
		  r = keys_mapped[m].r;
		  j += l - 1;
		  break;
		}
	  }

	  int n = keymap_find(prev_map, r);
	  /* print("%c %d, ", *tok, n); */
	  if(n == -1){
		cur_map = keymap_new(1);
		n = prev_map->n++;
		prev_map->k[n] = r;
		prev_map->val[n] = cur_map;
	  } else {
		cur_map = prev_map->val[n];
		if(cur_map->n >= cur_map->len){
		  cur_map = keymap_grow(cur_map);
		  prev_map->val[n] = cur_map;
		}
	  }
	  prev_map = cur_map;
	}

	// assign command to the leaf node
	keymap_set_key(cur_map, r, key_map[i].cmd);
	/* print("a %C %s, ", r, key_map[i].cmd); */
  }
  /* keymap_dump(global_map); */
  /* exits("loaded"); */

  cur_map = NULL;
  return global_map;
}

 
void wkeyctl(Window *w, Rune r) {
  if(w->i==nil || !r || w->hovering) return;

  /* Password entry uses raw mode - don't echo */
  if(!(w->mouseopen || w->rawing)) {
	if(w->holding && mod == Mctl && r == 'd') {
	  /* libsys provides holdon(). This will pass EOT to the holding application.  */
	  Rune s = '\004';
	  winsert(w, &s, 1, w->q0);
	  w->holding = 0;
	  wsendctlmesg(w, Holdoff, ZR, nil);
	  
	} else
	keymap_exec(w, r);
  }
  if(w->rawing && (w->q0==w->nr || w->mouseopen)){
	waddraw(w, &r, 1);
	return;
  }
}

void wundo(Window *w) {
	Rune *data = nil;
	int datalen = 0, cursorpos = 0, type = -1, group, i;

	group = undoisboundary(w->undo);
	while (undoundo(w->undo, &data, &datalen, &cursorpos, &type) == 0) {
	  /* fprint(2, " undo t=%d %d %d, ", type, i, datalen); */
	  /* fprint(2, " undo t=%d %d %d, ", cursorpos, datalen, i); */
	  undopause(w->undo);
	  if (!type)
		wdelete(w, cursorpos, cursorpos + datalen);
	  else
		winsert(w, data, datalen, cursorpos);
	  undoresume(w->undo);
	  i = undoisboundary(w->undo);
	  if (group && i == 0) continue; else break;
	}
}

void wredo(Window *w) {
	Rune *data = nil;
	int datalen = 0, cursorpos = 0, type = -1, group, i;

	group = undoisboundary(w->undo);
	while (undoredo(w->undo, &data, &datalen, &cursorpos, &type) == 0) {
	  /* fprint(2, " redo t=%d, ", type); */
	  /* write(2, data, datalen); */
	  undopause(w->undo);
	  if (type)
		wdelete(w, cursorpos, cursorpos + datalen);
	  else
		winsert(w, data, datalen, cursorpos);
	  undoresume(w->undo);
	  i = undoisboundary(w->undo);
	  if (group && i == 0) continue; else break;
	}
}

void initui(void) {
  for (int i = 0; i < 10; i++) reg[i].id = -1;
}

void cleanreg(int id) {
  for (int i = 0; i < 10; i++)
	if (id == reg[i].id) reg[i].id = -1;
}

void execprim(Window *w, char *p) {
  int i, n;
  char *args[2];

  n = tokenize(p, args, nelem(args));
  if (!n) return;

  prefix = 0;
  for(i = 0; i < nelem(prim); i++) {
	if(strcmp(prim[i].s, args[0]) == 0) {
	  Command cmd;
	  n = atoi(args[1]);
	  switch(prim[i].argc) {
	  case 0:
		cmd.f0 = prim[i].f;
		cmd.f0();
		break;
	  case 1:
		cmd.f1 = prim[i].f;
		cmd.f1(w);
		break;
	  case 2:
		cmd.f2 = prim[i].f;
		cmd.f2(w, n);
		break;
	  }
	}
  }
}	
