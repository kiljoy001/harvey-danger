#include <u.h>
#include <libc.h>
#include <draw.h>
#include <thread.h>
#include <fcall.h>
#include <9p.h>
#include <bio.h>
#include <ttf.h>

#define ISREALMARK(t, gid, nrunes) (ttfgdefclass((t), (gid)) == 3 && (nrunes) == 1)

static char Egreg[] = "my memory of truetype is fading";
static char Enoent[] = "not found";

static char *fontpath = "/lib/font/ttf";
static int debug = 0;

enum { MAXSUB = 0x100 };
/* enum { MAXCLUSTER = 8 };	/\* longest cluster compileshapewindow will probe *\/ */
enum { IHDRSZ = 5*12, SHDRSZ = 3*12, FCHARSZ = 6 };	/* image(6)/subfont(6) fixed field widths */
enum { SHAPEWIN = 24 };	/* runes fetched per /shape probe */

typedef struct TFont TFont;
typedef struct TSubfont TSubfont;
typedef struct TGlyph TGlyph;
typedef struct TShapeQ TShapeQ;
typedef struct VFont VFont;
typedef struct VSize VSize;

struct TFont {
	int ref;
	Qid qid;
	Qid fileqid;
	Qid glyphqid;
	Qid shapeableqid;
	Qid shapeqid;
	char *fontfile;
	int nfontfile;
	TTFont *ttf;
	char *name9p;
	char *name;
	int size;
	TFont *next, *prev;
	TSubfont *sub[256];
	TGlyph *glyphs;		/* single-cluster glyph cache, see glyph/<seq> */
	int shapeflags;		/* Fgsub|Fgpos|Fctx, -1 = not yet computed */
	int colorflags;		/* 0/1, -1 = not yet computed, see colorcapable() -- folded into shapeable's 4th field */
	TShapeQ *shapeqs;	/* shape-window response cache, see shape/<seq> */
};

enum { Fgsub = 1, Fgpos = 2, Fctx = 4 };


struct TSubfont {
	TFont *font;
	Rune start, end;
	Qid qid;
	char *data;
	int ndata;
	TSubfont *next;
};

/*
 * One entry per distinct rune sequence ever walked under glyph/ -- keyed by
 * the whole sequence, not a single rune, since a ZWJ cluster (e.g. family
 * emoji) only resolves to one glyph via ttfshape() across all its runes at
 * once. Rendered lazily on first read, same pattern as TSubfont/compilesub.
 */
struct TGlyph {
	TFont *font;
	Rune *seq;
	int nseq;
	Qid qid;
	char *data;
	int ndata;
	TGlyph *next;
};

/*
 * One entry per distinct rune window ever walked under shape/ -- keyed by
 * the whole window, same pattern as TGlyph. data holds one decimal
 * consumed-rune-count per cluster found in the window, newline separated,
 * computed lazily by compileshapewindow() on first read.
 */
struct TShapeQ {
	TFont *font;
	Rune *seq;
	int nseq;
	Qid qid;
	char *data;
	int ndata;
	TShapeQ *next;
};

struct VSize {
	VFont *font;
	int size;
	Qid qid;
	Qid fileqid;
	char *data;
	int ndata;
	VSize *next;
};

struct VFont {
	char *name;
	Qid qid;
	char **font;
	int nfont;
	VSize *size;
	VFont *next;
};

static VFont *vfontl;
static Qid ctlqid;

typedef struct FidAux FidAux;

struct FidAux {
	enum {
		FIDROOT,
		FIDCTL,
		FIDFONT,
		FIDVSIZE,
		FIDFONTF,
		FIDSUB,
		FIDGLYPHDIR,
		FIDGLYPH,
		FIDSHAPEABLE,
		FIDSHAPEDIR,
		FIDSHAPE,
	} type;
	TFont *f;
	VFont *vf;
	VSize *vs;
	TSubfont *sub;
	TGlyph *g;
	TShapeQ *sq;
};

TFont fontl = {.next = &fontl, .prev = &fontl};

TFont * tryfont(char *name);

/* debug print helper */
static void
dprint(char *fmt, ...)
{
	va_list arg;

	if(!debug)
		return;
	va_start(arg, fmt);
	vfprint(2, fmt, arg);
	va_end(arg);
}

static void *
emalloc(ulong n)
{
	void *v;

	v = mallocz(n, 1);
	if(v == nil) sysfatal("malloc: %r");
	setmalloctag(v, getcallerpc());
	return v;
}

static uvlong
qidgen(void)
{
	static uvlong x;

	return ++x;
}

/* fixed-width decimal field, as image(6)/subfont(6) write them: %11d + sep */
static long
getfield(char *p)
{
	return strtol(p, nil, 10);
}

static void
fsattach(Req *r)
{
	r->ofcall.qid = (Qid){0, 0, QTDIR};
	r->fid->qid = r->ofcall.qid;
	r->fid->aux = emalloc(sizeof(FidAux));
	respond(r, nil);
}

/* does [k, k+MAXSUB) already have a page? */
static int
haspage(TFont *f, Rune k)
{
	TSubfont *s;

	for(s = f->sub[k >> 8 & 0xff]; s != nil; s = s->next)
		if(s->start == k)
			return 1;
	return 0;
}

static void
addpage(TFont *f, Fmt *fmt, Rune k, int *got0)
{
	TSubfont *s;

	if(haspage(f, k))
		return;
	s = emalloc(sizeof(TSubfont));
	s->start = k;
	if(k == 0) *got0 = 1;
	s->end = k + MAXSUB - 1;
	s->font = f;
	s->qid = (Qid){qidgen(), 0, 0};
	s->next = f->sub[k >> 8 & 0xff];
	f->sub[k >> 8 & 0xff] = s;
	fmtprint(fmt, "%#.4ux\t%#.4ux\ts.%.4ux-%.4ux\n", s->start, s->end, s->start, s->end);
}

void
mksubfonts(TFont *f)
{
	int i, k;
	TTChMap *c;
	TTFontU *u;
	Fmt fmt;
	int got0;

	fmtstrinit(&fmt);
	fmtprint(&fmt, "%d\t%d\n", f->ttf->ascentpx + f->ttf->descentpx, f->ttf->ascentpx);
	got0 = 0;
	u = f->ttf->u;
	for(c = u->cmap; c < u->cmap + u->ncmap; c++)
		for(k = c->start & ~(MAXSUB-1); k < c->end; k += MAXSUB)
			addpage(f, &fmt, k, &got0);
	if(!got0){
		TSubfont *s;

		s = emalloc(sizeof(TSubfont));
		s->start = 0;
		s->end = 0;
		s->font = f;
		s->qid = (Qid){qidgen(), 0, 0};
		s->next = f->sub[0];
		f->sub[0] = s;
		fmtprint(&fmt, "%#.4ux\t%#.4ux\ts.%.4ux-%.4ux\n", 0, 0, 0, 0);
	}
	f->fontfile = fmtstrflush(&fmt);
	f->nfontfile = strlen(f->fontfile);
}

static void
blit(uchar *t, int x, int y, int tstride, uchar *s, int w, int h)
{
	int tx, ty, sx, sy;
	u16int b;
	uchar *tp, *sp;

	if(y < 0) y = 0;
	ty = y;
	sp = s;
	for(sy = 0; sy < h; sy++, ty++){
		tx = x;
		tp = t + ty * tstride + (tx >> 3);
		b = 0;
		for(sx = 0; sx < w; sx += 8){
			b |= *sp++ << 8 - (tx & 7);
			*tp++ |= b >> 8;
			b <<= 8;
		}
		*tp |= b >> 8;
	}
}

/*
 * Sanity-gate a glyph's geometry before it's ever handed to blit(), which
 * does no bounds checking of its own. A row (y = ascentpx - ymaxpx) or
 * height that doesn't fit inside [0, h) means blit() writes past the strip
 * buffer's real row extent -- decorative/handwriting TTFs in particular
 * can report ink extents well past their own declared ascent/descent.
 */
static int
badglyph(TTGlyph *g, int ascentpx, int h)
{
	int y;

	if(g->width < 0 || g->height < 0)
		return 1;
	if(g->width > 8192 || g->height > 8192)
		return 1;
	y = ascentpx - g->ymaxpx;
	if(y < 0 || y + g->height > h)
		return 1;
	return 0;
}

/* "1f468-200d-1f469-..." -> Rune[]. nil on bad syntax (not our name to walk). */
static Rune *
parseseq(char *name, int *np)
{
	Rune *seq;
	char *p, *end;
	long v;
	int n, cap;

	cap = 8;
	seq = emalloc(cap * sizeof(Rune));
	n = 0;
	p = name;
	for(;;){
		v = strtol(p, &end, 16);
		if(end == p || v <= 0 || v > Runemax){
			free(seq);
			return nil;
		}
		if(n == cap){
			cap *= 2;
			seq = realloc(seq, cap * sizeof(Rune));
		}
		seq[n++] = v;
		if(*end == 0)
			break;
		if(*end != '-'){
			free(seq);
			return nil;
		}
		p = end + 1;
	}
	*np = n;
	return seq;
}

/* inverse of parseseq, for fsstat's d.name */
static char *
runeseqname(Rune *seq, int nseq)
{
	Fmt fmt;
	int i;

	fmtstrinit(&fmt);
	for(i = 0; i < nseq; i++)
		fmtprint(&fmt, i == 0 ? "%.4ux" : "-%.4ux", seq[i]);
	return fmtstrflush(&fmt);
}

static int
seqeq(Rune *a, int na, Rune *b, int nb)
{
	if(na != nb)
		return 0;
	return memcmp(a, b, na * sizeof(Rune)) == 0;
}

/* find-or-create the cache entry for this rune sequence; nil only on
 * malformed name (findsubfont's Enoent convention, not a render failure) */
static TGlyph *
findglyph(TFont *f, char *name)
{
	Rune *seq;
	int nseq;
	TGlyph *g;

	if((seq = parseseq(name, &nseq)) == nil)
		return nil;
	for(g = f->glyphs; g != nil; g = g->next)
		if(seqeq(g->seq, g->nseq, seq, nseq)){
			free(seq);
			return g;
		}
	g = emalloc(sizeof(TGlyph));
	g->font = f;
	g->seq = seq;
	g->nseq = nseq;
	g->qid = (Qid){qidgen(), 0, 0};
	g->next = f->glyphs;
	f->glyphs = g;
	return g;
}

static TShapeQ *
findshapeq(TFont *f, char *name)
{
	Rune *seq;
	int nseq;
	TShapeQ *sq;

	if((seq = parseseq(name, &nseq)) == nil)
		return nil;
	for(sq = f->shapeqs; sq != nil; sq = sq->next)
		if(seqeq(sq->seq, sq->nseq, seq, nseq)){
			free(seq);
			return sq;
		}
	sq = emalloc(sizeof(TShapeQ));
	sq->font = f;
	sq->seq = seq;
	sq->nseq = nseq;
	sq->qid = (Qid){qidgen(), 0, 0};
	sq->next = f->shapeqs;
	f->shapeqs = sq;
	return sq;
}

/*
 * Whether this font has any tables worth speculatively shaping for at
 * all. Computed once, cached on TFont. ctx (contextual substitution)
 * is unimplemented upstream in compileglyph, so always reported absent.
 * Read together with colorcapable() as the 4-field shapeable file
 * (gsub gpos ctx colr), one round trip for a client to learn both.
 */
static int
shapeflags(TFont *f)
{
	if(f->shapeflags < 0){
		f->shapeflags = 0;
		if(f->ttf->u->liga != nil)
			f->shapeflags |= Fgsub;
		if(f->ttf->u->gposmark != nil)
			f->shapeflags |= Fgpos;
	}
	return f->shapeflags;
}

/*
 * Whether this font has a COLR table at all -- i.e. whether it's worth
 * a client trying the color-capable per-glyph path (glyph/<seq>) for
 * an ordinary single rune, instead of going straight to the cheaper
 * whole-page subfont (which is always plain outline, see compilesub).
 * Computed once, cached on TFont, same pattern as shapeflags(); its
 * value is served as the 4th field of the shapeable file, not a file
 * of its own -- see FIDSHAPEABLE's read case.
 */
static int
colorcapable(TFont *f)
{
	if(f->colorflags < 0)
		f->colorflags = f->ttf->u->colr != nil;
	return f->colorflags;
}


/*
 * Render one rune sequence to a file. COLR/CPAL data, if the font has any
 * for the shaped glyph, becomes a plain image(6) blob -- RGBA32 when
 * ttfcolrglyph reports real color data, GREY1 otherwise (bm.chan is a
 * has-color flag, not itself a packed chan value -- see the resolve in
 * the branch below). The rect encodes the baseline offset directly -- no
 * separate metrics file needed, see discussion. Falls back to a single
 * k1 cell, same layout compilesub uses per glyph, when ttfcolrglyph
 * reports no COLR data for the glyph at all.
 *
 * ng>1 no longer means "blank". If the base
 * (gidbuf[0]) is a plain outline glyph (not COLR), composite it
 * with each subsequent glyph in the shaped sequence as a
 * GPOS-attached mark, chained: gidbuf[1] attaches to gidbuf[0],
 * gidbuf[2] attaches to gidbuf[1], etc. -- this is what a base
 * consonant + matra (है = U+0939 U+0948) needs, and generalizes to
 * stacked marks.
 *
 * COLR bases with ng>1 still blank -- compositing COLR output with
 * mono marks is a separate problem (different bit depths, alpha
 * compositing) and out of scope.
 */

static int
bitget(u8int *bit, int stride, int x, int y)
{
	return (bit[y * stride + (x >> 3)] >> (7 - (x & 7))) & 1;
}

static void
bitorset(u8int *bit, int stride, int x, int y)
{
	bit[y * stride + (x >> 3)] |= 1 << (7 - (x & 7));
}

/*
 * Blit src's outline bits into dst. originx/originy are the dst
 * canvas coordinates that correspond to src's own (0,0) pixel --
 * i.e. src row 0 (assumed = font-y src->ymaxpx, the top row) col 0
 * (assumed = font-x src->xminpx, the left column). Caller computes
 * originx/originy from the shared pen-origin placement; this
 * function does no coordinate-system math of its own beyond adding
 * src's own row/col index, so there is exactly one place (the call
 * site) to check if placement comes out wrong. OR-composite,
 * clipped to dst's bounds.
 */
static void
blitmono(TTGlyph *src, u8int *dst, int dstride, int dwidth, int dheight, int originx, int originy)
{
	int sstride, x, y, dx, dy;

	sstride = (src->width + 7) >> 3;
	for(y = 0; y < src->height; y++){
		for(x = 0; x < src->width; x++){
			if(!bitget(src->bit, sstride, x, y))
				continue;
			dx = originx + x;
			dy = originy + y;
			if(dx < 0 || dx >= dwidth || dy < 0 || dy >= dheight)
				continue;
			bitorset(dst, dstride, dx, dy);
		}
	}
}

static int
hasvirama(Rune *s, int n)
{
	int i;

	for(i = 0; i < n; i++)
		if(s[i] == 0x094D)
			return 1;
	return 0;
}

static int
syllablebase(TTFontU *u, u16int *gidbuf, int ng)
{
	int k;
	for(k = ng - 1; k >= 0; k--)
		if(ttfgdefclass(u, gidbuf[k]) != 3)
			return k;
	return -1;
}

static int
isindicbase(Rune r)
{
	/*
	 * Devanagari consonants.  This is intentionally character based:
	 * GDEF class 3 is not sufficient to identify Reph.
	 */
	return (r >= 0x0915 && r <= 0x0939);
}

/*
 * Nearest base glyph after index i, within i's OWN syllable. Never
 * crosses a syllable boundary -- a cluster can contain more than one
 * syllable's worth of bases (e.g. "karmna"), and reph / ligature-
 * produced pseudo-marks must never attach to a base that belongs to a
 * different syllable than the mark itself, even if it's the nearest
 * one in the array.
 */
static int
followingbase(TTFontU *u, TTShapeInfo *gi, TTGlyph **seq, int ng, int i)
{
	int j;

	for(j = i + 1; j < ng; j++){
		if(gi[j].syllable != gi[i].syllable)
			break;
		if(seq[j] == nil)
			continue;
		if(ttfgdefclass(u, gi[j].gid) != 3)
			return j;
	}
	return -1;
}

/* syllablebase() removed: it searched the whole cluster for the last
 * non-mark glyph with no notion of *which* syllable a mark belonged
 * to, which was correct only by coincidence for single-syllable
 * clusters and wrong for multi-syllable ones (RA-subjoin in "karmna"
 * attaching to NA instead of MA). Every former syllablebase() call
 * site now just uses followingbase() -- reph and ligature-produced
 * pseudo-marks need identical "nearest base in my own syllable"
 * semantics, so there's no remaining reason for two functions. */

void
compileglyph(TFont *f, TGlyph *g)
{
	TTFont *t;
	TTGlyph *og, **seq;
	TTBitmap bm;
	TTShapeInfo *gi;
	int ng;
	char *d, *p;
	char chanstr[32];
	int nchanstr, dstride, sz, h, ascentpx, descentpx;
	int i, j, *px, *py, dx, dy;
	int xmin, xmax, w;
	u8int *canvas;
	int *pending, npending;

	t = f->ttf;
	ascentpx = t->ascentpx;
	descentpx = t->descentpx;
	if(getenv("GLYPHDEBUG") != nil) {
	  char seqstr[8*MAXCLUSTER];
	  int k;

	  seqstr[0] = 0;
	  for(k = 0; k < g->nseq && k < MAXCLUSTER; k++)
		snprint(seqstr+strlen(seqstr), sizeof(seqstr)-strlen(seqstr),
				"%s%.4ux", k ? "-" : "", g->seq[k]);
	  fprint(2, "compileglyph: font=%s.%d rp=%s ascentpx=%d descentpx=%d h=%d\n",
			   f->name, f->size, seqstr, ascentpx, descentpx, ascentpx+descentpx);
	}
	h = ascentpx + descentpx;	/* fixed line box: every cluster this
					 * font produces gets the SAME height,
					 * so naive image-stitching lines up
					 * baselines by construction */

	gi = emalloc(g->nseq * sizeof(TTShapeInfo));

	ng = ttfshape(t, g->seq, g->nseq, gi);

	if(ng < 1){
		free(gi);
		g->data = emalloc(1);
		g->ndata = 0;
		return;
	}

	if(ng == 1){
		u16int gid;

		gid = gi[0].gid;
		free(gi);

		memset(&bm, 0, sizeof bm);
		if(ttfcolrglyph(t, gid, &bm) > 0){
			ulong chan;

			chan = bm.chan ? ABGR32 : GREY1;
			chantostr(chanstr, chan);
			nchanstr = strlen(chanstr);
			dstride = bm.stride;
			sz = IHDRSZ + dstride * bm.height;

			d = emalloc(sz);
			p = d + sprint(d, "%11.*s %11d %11d %11d %11d ",
				nchanstr, chanstr, 0, 0, bm.width, bm.height);
			memmove(p, bm.bit, bm.height * bm.stride);

			free(bm.bit);

			g->data = d;
			g->ndata = sz;
			return;
		}

		og = ttfgetglyph(t, gid, 1);
		if(og == nil){
			g->data = emalloc(1);
			g->ndata = 0;
			return;
		}

		dstride = (((og->width+7)>>3) + 3) & ~3;
		sz = IHDRSZ + dstride * h;

		d = emalloc(sz);
		p = d + sprint(d, "%11s %11d %11d %11d %11d ", "k1",
			0, 0, og->width, h);
		memset(p, 0, dstride * h);
		blitmono(og, (uchar*)p, ((og->width+7)>>3), og->width, h,
			0, ascentpx - og->ymaxpx);	/* baseline-relative
							 * row, not 0 */

		g->data = d;
		g->ndata = sz;
		ttfputglyph(og);
		return;
	}

	/*
	 * ng > 1: base + chain of marks. If the base is COLR, we don't
	 * handle it -- blank, same as before.
	 */
	memset(&bm, 0, sizeof bm);
	if(ttfcolrglyph(t, gi[0].gid, &bm) > 0){
		free(bm.bit);
		free(gi);
		g->data = emalloc(1);
		g->ndata = 0;
		return;
	}

	seq = emalloc(ng * sizeof(TTGlyph *));
	px = emalloc(ng * sizeof(int));
	py = emalloc(ng * sizeof(int));

	seq[0] = ttfgetglyph(t, gi[0].gid, 1);
	if(seq[0] == nil){
		free(gi); free(seq); free(px); free(py);
		g->data = emalloc(1);
		g->ndata = 0;
		return;
	}
	px[0] = 0;
	py[0] = 0;

	pending = malloc(ng * sizeof(*pending));
	npending = 0;

	/* fprint(2, "\n--- DIAGNOSTIC START: PROCESSING CLUSTER (%d GLYPHS) ---\n", ng); */
	/* for(i = 0; i < ng; i++) { */
	/* 	fprint(2, "  Glyph[%d]: GID=0x%.4x, GDEF=%d, nrunes=%d, syllable=%d, is_real_mark=%d\n", */
	/* 		i, gi[i].gid,  */
	/* 		ttfgdefclass(t->u, gi[i].gid),  */
	/* 		gi[i].nrunes,  */
	/* 		gi[i].syllable,  */
	/* 		ISREALMARK(t->u, gi[i].gid, gi[i].nrunes)); */
	/* } */

	for(i = 1; i < ng; i++){
		seq[i] = ttfgetglyph(t, gi[i].gid, 1);
		if(seq[i] == nil){
			px[i] = px[i-1];
			py[i] = py[i-1];
			continue;
		}

		/*
		 * Reph / Pseudo-marks checks
		 */
		if(ttfgdefclass(t->u, gi[i].gid) == 3 && gi[i].ligprio == 0){
			/*
			 * Reph: reordered before its base. Attach to the
			 * first following real base IN THIS SYLLABLE.
			 */
			j = followingbase(t->u, gi, seq, ng, i);
			if(j >= 0){
				pending[npending++] = i;
				continue;
			}
			/* Fallthrough if no following base inside this isolated window */
		}

		if(ttfgdefclass(t->u, gi[i].gid) == 3 && gi[i].nrunes > 1){
			/*
			 * Ligature-produced pseudo-mark (e.g. subjoined
			 * RA): resolved against its own syllable's base
			 * in pass 2, same as reph.
			 */
			pending[npending++] = i;
			continue;
		}

		if(ttfgdefclass(t->u, gi[i].gid) == 3){
			/*
			 * FIX: Standard lookback must skip ALL marks (GDEF == 3),
			 * not just those flagged by ISREALMARK.
			 */
			j = i - 1;
			while(j >= 0 && (seq[j] == nil ||
				ttfgdefclass(t->u, gi[j].gid) == 3))
				j--;

			if(j >= 0 && seq[j] != nil &&
			   ttfmarkpos(t->u, gi[i].gid, gi[j].gid, 0, &dx, &dy)){
				dx = rounddiv(dx * t->ppem, t->u->emsize);
				dy = rounddiv(dy * t->ppem, t->u->emsize);
				px[i] = px[j] + dx;
				py[i] = py[j] + dy;
			}else if(j >= 0){
				px[i] = px[j];
				py[i] = py[j];
			}else{
				px[i] = px[i-1];
				py[i] = py[i-1];
			}
		}else{
			/* Base advance logic */
			j = i - 1;
			while(j >= 0 && (seq[j] == nil ||
				(ttfgdefclass(t->u, gi[j].gid) == 3)))
				j--;

			if(j >= 0){
				px[i] = px[j] + seq[j]->advanceWidthpx;
				py[i] = py[j];
			}else{
				px[i] = px[i-1];
				py[i] = py[i-1];
			}
		}
	}

	/*
	 * Pass 2: the real bases have now been positioned. Attach reph
	 * and ligature-produced pseudo-marks to the nearest base within
	 * THEIR OWN SYLLABLE -- never "the" base of the whole cluster,
	 * since one shaped cluster can span multiple syllables.
	 */
	for(i = 0; i < npending; i++){
		int pnd, rawdx, rawdy, marked;

		pnd = pending[i];
		j = followingbase(t->u, gi, seq, ng, pnd);

		/* FIX: If no base follows inside this window fragment, look backward instead */
		if(j < 0) {
			j = pnd - 1;
			while(j >= 0 && (seq[j] == nil || ttfgdefclass(t->u, gi[j].gid) == 3))
				j--;
		}

		if(j >= 0 && seq[j] != nil){
			rawdx = rawdy = 0;
			marked = ttfmarkpos(t->u, gi[pnd].gid, gi[j].gid, 0,
				&rawdx, &rawdy);
			if(marked){
				dx = rounddiv(rawdx * t->ppem, t->u->emsize);
				dy = rounddiv(rawdy * t->ppem, t->u->emsize);
				px[pnd] = px[j] + dx;
				py[pnd] = py[j] + dy;
			}else{
				px[pnd] = px[j] + seq[j]->xmaxpx;
				py[pnd] = py[j];
			}
		}else if(pnd > 0){
			px[pnd] = px[pnd-1];
			py[pnd] = py[pnd-1];
		}else{
			px[pnd] = py[pnd] = 0;
		}
	}

	free(pending);

	/*
	 * w still comes from ink extent (horizontal stitching has no
	 * shared-metric equivalent to fall back on the way height does).
	 * h is now the FIXED line box from above -- ymin/ymax are no
	 * longer computed or used for vertical sizing at all.
	 */
	xmin = px[0] + seq[0]->xminpx;
	xmax = px[0] + seq[0]->xmaxpx;
	for(i = 1; i < ng; i++){
		if(seq[i] == nil)
			continue;
		if(px[i] + seq[i]->xminpx < xmin) xmin = px[i] + seq[i]->xminpx;
		if(px[i] + seq[i]->xmaxpx > xmax) xmax = px[i] + seq[i]->xmaxpx;
	}
	w = xmax - xmin;

	dstride = (((w + 7) >> 3) + 3) & ~3;
	canvas = mallocz(dstride * h, 1);

	for(i = 0; i < ng; i++){
		if(seq[i] == nil)
			continue;
		blitmono(seq[i], canvas, ((w + 7) >> 3), w, h,
			px[i] + seq[i]->xminpx - xmin,
			ascentpx - py[i] - seq[i]->ymaxpx);	/* baseline-
								 * relative,
								 * not ymax-
								 * relative */
	}

	sz = IHDRSZ + dstride * h;
	d = emalloc(sz);
	p = d + sprint(d, "%11s %11d %11d %11d %11d ", "k1", 0, 0, w, h);
	memmove(p, canvas, dstride * h);
	free(canvas);

	for(i = 0; i < ng; i++)
		if(seq[i] != nil)
			ttfputglyph(seq[i]);
	free(seq);
	free(px);
	free(py);
	free(gi);

	g->data = d;
	g->ndata = sz;
}

/*
 * Greedily find cluster boundaries across a lookahead window, using the
 * same criteria compileglyph uses to decide a rune sequence renders as
 * one composite unit: either ttfshape genuinely ligates the sequence
 * (ng < candidate length), or the next rune's glyph is a GDEF mark
 * (class 3) attaching onto what came before. Emits one decimal count
 * per cluster found, newline separated; the last cluster is dropped
 * since it may extend past the window edge -- caller re-probes with a
 * shifted window when it runs out.
 */
/*
 * Does adding one more rune (bringing the candidate to gidbuf[0..ng)
 * glyphs, up from ngprev glyphs for the same prefix one rune shorter)
 * genuinely extend the same visual cluster? Two ways it can:
 *   - the new rune got absorbed into a ligature with the tail
 *     (ng <= ngprev -- glyph count didn't grow, or shrank further)
 *   - the new rune became its own glyph, but that glyph is a
 *     combining mark (GDEF class 3) attaching onto what came before
 * ng > ngprev+1 shouldn't normally happen (one more input rune can
 * add at most one more raw glyph) -- treated as "not ok" defensively
 * rather than assumed impossible.
 */
static int
isindicextend(Rune r)
{
	return (r >= 0x093A && r <= 0x094D);
}

static int
extendsok(TTFont *t, Rune *rp, int n, TTShapeInfo *gi, int ng, int ngprev)
{
	if(ng <= ngprev)
		return 1;
	if(ng == ngprev+1){
	  /* Pre-base reordering vowel signs (short I-matra, etc.) get
	   * moved before their base's glyph, so the newly added glyph
	   * is NOT guaranteed to be last -- check the newly consumed
	   * RUNE directly instead of inferring it from output position. */
		if(isindicextend(rp[n-1]))
			return 1;

		/* newly added glyph is itself a mark: matra-follows-base */
		if(ttfgdefclass(t->u, gi[ng-1].gid) == 3)
			return 1;

		/* Previous glyph is a pending rphf/reph output. */
		if(ngprev > 0 && gi[ngprev-1].ligprio == 0)
			return 1;

		/* An ordinary consonant following virama remains in the syllable. */
		if(n >= 2 && rp[n-2] == 0x094D)
			return 1;

		return 0;
	}
	return 0;
}

#define IS_VIRAMA(r)  ((r) == 0x094D)
#define IS_JOINER(r)  ((r) == 0x200C || (r) == 0x200D)
/*
 * Rule 3: Base excludes ALL combining marks.
 * Maps Devanagari dependent vowel signs, viramas, and modifiers.
 */
#define IS_MARK(r)   (((r) >= 0x093A && (r) <= 0x0957) || ((r) >= 0x0901 && (r) <= 0x0903))
#define IS_BASE(r)   (!IS_MARK(r) && !IS_JOINER(r))

typedef enum {
	PROP_BASE,
	PROP_VIRAMA,
	PROP_JOINER,
	PROP_MARK
} RuneProp;

static RuneProp
getprop(TTFont *font, Rune r)
{
	if(IS_VIRAMA(r)) return PROP_VIRAMA;
	if(IS_JOINER(r)) return PROP_JOINER;
	if(IS_MARK(r) || ttfgdefclass(font->u, ttffindchar(font, r)) == 3) return PROP_MARK;
	return PROP_BASE;
}

static int
findclusterlen(TTFont *font, Rune *rp, int nseq)
{
	int cand, last_base_idx, has_pending_virama, liglen, ng;
	TTShapeInfo gi[MAXCLUSTER];
	RuneProp p;

	if(nseq <= 1)
		return nseq;

	/* Keep the first GSUB ligature together as well as script clusters. */
	ng = ttfshape(font, rp, nseq < MAXCLUSTER ? nseq : MAXCLUSTER, gi);
	liglen = ng > 0 ? gi[0].nrunes : 1;
	/* The first rune always anchors the cluster */
	last_base_idx = (getprop(font, rp[0]) == PROP_BASE) ? 0 : -1;
	has_pending_virama = (getprop(font, rp[0]) == PROP_VIRAMA) ? 1 : 0;

	for(cand = 1; cand < nseq; cand++){
		p = getprop(font, rp[cand]);

		if(p == PROP_MARK) {
			/* Marks naturally append to the running cluster */
			continue;
		}

		if(p == PROP_VIRAMA) {
			has_pending_virama = 1;
			continue;
		}

		if(p == PROP_JOINER) {
			/* Rule 2: Joiners are completely transparent to adjacency checks. */
			continue;
		}

		if(p == PROP_BASE) {
			/*
			 * Rule 1: Two consecutive bases start separate clusters,
			 * EXCEPT when the second base is immediately preceded by a virama.
			 */
			if(last_base_idx != -1 && has_pending_virama) {
				/* Legal conjunct bridge: stays in the same cluster */
				last_base_idx = cand;
				has_pending_virama = 0; /* virama consumed by this base */
				continue;
			}

			if(cand < liglen)
				continue;
			/* Rule 1 Fallthrough: Standard base starts a new cluster boundary */
			break;
		}
	}
	return cand;
}

/*
 * Find the last glyph belonging to the base complex beginning at i.
 * The base itself is followed by all marks attached to it.
 */
static int
baseclusterend(TTFontU *u, TTShapeInfo *gi, TTGlyph **seq, int ng, int i)
{
	int j, base;

	base = followingbase(u, gi, seq, ng, i);
	if(base < 0)
		return -1;

	j = base + 1;
	while(j < ng){
		if(seq[j] == nil)
			break;
		if(ttfgdefclass(u, gi[j].gid) != 3)
			break;
		j++;
	}
	return j - 1;
}

void
compileshapewindow(TFont *f, TShapeQ *sq)
{
	Fmt fmt;
	int pos, clen;
	char seqstr[8*MAXCLUSTER];
	int i;

	fmtstrinit(&fmt);

	seqstr[0] = 0;
	for(i = 0; i < sq->nseq && i < MAXCLUSTER; i++)
		snprint(seqstr+strlen(seqstr), sizeof(seqstr)-strlen(seqstr),
			"%s%.4ux", i ? "-" : "", sq->seq[i]);

	dprint("truetypefs: compileshapewindow v3 %s.%d shape/%s (nseq=%d)\n",
		f->name, f->size, seqstr, sq->nseq);

	pos = 0;
	while(pos < sq->nseq){
		/* Find how many runes are bound to this cluster */
		clen = findclusterlen(f->ttf, sq->seq + pos, sq->nseq - pos);

		if(clen <= 0)
			break;

		/* Rule 4: If window is full, withhold trailing un-emitted cluster */
		if(pos + clen >= sq->nseq && sq->nseq == SHAPEWIN){
			dprint("truetypefs: v4 trailing cluster deferred at pos=%d\n", pos);
			break;
		}

		fmtprint(&fmt, "%d\n", clen);
		dprint("truetypefs: v4 emit cluster pos=%d len=%d\n", pos, clen);
		pos += clen;
	}

	sq->data = fmtstrflush(&fmt);
	sq->ndata = strlen(sq->data);
}

static void
compilesub(TFont *f, TSubfont *s)
{
	int n, i, w, x, h, g, sz;
	char *d, *p;
	TTGlyph **gs;
	TTFont *t;

	t = f->ttf;
	n = s->end - s->start + 1;
	gs = emalloc9p(sizeof(TTGlyph *) * n);
	w = 0;
	h = t->ascentpx + t->descentpx;
	for(i = 0; i < n; i++){
		gs[i] = nil;
		if(s->start + i != 0){
			g = ttffindchar(t, s->start + i);
			if(g != 0)
				gs[i] = ttfgetglyph(t, g, 1);
			if(gs[i] != nil && badglyph(gs[i], t->ascentpx, h)){
				ttfputglyph(gs[i]);
				gs[i] = nil;
			}
		}
		if(gs[i] == nil){
			gs[i] = ttfgetglyph(t, 0, 1);
			if(gs[i] != nil && badglyph(gs[i], t->ascentpx, h)){
				ttfputglyph(gs[i]);
				gs[i] = nil;
			}
		}
		if(gs[i] == nil){
			/* every source failed or was rejected: don't take the fileserver
			 * down over one glyph, synthesize an empty zero-width placeholder */
			gs[i] = emalloc(sizeof(TTGlyph));
			gs[i]->bit = emalloc(1);
			gs[i]->ymaxpx = t->ascentpx;
		}
	   w += gs[i]->width;
	}
	sz = 5 * 12 + (w+7>>3) * h + 3 * 12 + (n + 1) * 6;
	d = emalloc(sz);
	p = d + sprint(d, "%11s %11d %11d %11d %11d ", "k1", 0, 0, w, h);
	x = 0;
	for(i = 0; i < n; i++){
		blit((uchar*)p, x, t->ascentpx - gs[i]->ymaxpx, w+7>>3, gs[i]->bit, gs[i]->width, gs[i]->height);
		x += gs[i]->width;
	}
	p += (w+7>>3) * h;
	p += sprint(p, "%11d %11d %11d ", n, h, t->ascentpx);
	x = 0;
	for(i = 0; i < n; i++){
		*p++ = x;
		*p++ = x >> 8;
		*p++ = 0;
		*p++ = h;
		*p++ = gs[i]->xminpx;
		if(gs[i]->advanceWidthpx != 0)
			*p++ = gs[i]->advanceWidthpx;
		else
			*p++ = gs[i]->width;
		x += gs[i]->width;
	}
	*p++ = x;
	*p = x >> 8;
	s->data = d;
	s->ndata = sz;
	for(i = 0; i < n; i++)
		ttfputglyph(gs[i]);
	free(gs);
}

static VFont *
findvfont(char *name)
{
	VFont *v;

	for(v = vfontl; v != nil; v = v->next)
		if(strcmp(v->name, name) == 0)
			return v;
	return nil;
}

static VSize *
findvsize(VFont *v, int size)
{
	VSize *s;

	for(s = v->size; s != nil; s = s->next)
		if(s->size == size)
			return s;
	return nil;
}

/*
 * Emit one range line per still-unclaimed codepoint run inside [lo,hi].
 * Caller guarantees [lo,hi] lies entirely within the single page
 * [pagestart, pagestart+MAXSUB-1] -- a page's glyphs all live in one
 * already-lazily-generated s.<pagestart>-<pageend> subfont blob, and a
 * run starting mid-page reaches into it via buildfont's optional third
 * numeric field (offset into the target subfont) rather than needing
 * new bitmap data of its own. Marks each emitted codepoint claimed so a
 * later, lower-priority component sees it as unavailable.
 */
static void
emitrun(Fmt *fmt, uchar *claimed, char *font, int size, int lo, int hi, int pagestart)
{
	int i, runstart, pageend;

	pageend = pagestart + MAXSUB - 1;
	i = lo;
	while(i <= hi){
		if(claimed[i]){
			i++;
			continue;
		}
		runstart = i;
		while(i <= hi && !claimed[i]){
			claimed[i] = 1;
			i++;
		}
		fmtprint(fmt, "%#.4ux\t%#.4ux\t%d\t/n/ttf/%s.%d/s.%.4ux-%.4ux\n",
			runstart, i-1, runstart - pagestart, font, size, pagestart, pageend);
	}
}

static int
hascp(TFont *f, int cp)
{
	TTChMap *c;

	for(c = f->ttf->u->cmap; c < f->ttf->u->cmap + f->ttf->u->ncmap; c++)
		if(cp >= c->start && cp <= c->end)
			return 1;
	return 0;
}

static VSize *
mkvsize(VFont *v, int size)
{
	Fmt fmt;
	VSize *s;
	TFont **fonts, *f, *tf;
	TTChMap *c;
	uchar *claimed;
	char *name;
	int i, gotheader, k, lo, hi, ti, hi0;

	dprint("truetypefs: mkvsize %s size %d\n", v->name, size);
	if((s = findvsize(v, size)) != nil){
		dprint("truetypefs: cached size %d ndata=%d\n", size, s->ndata);
		return s;
	}

	/* one byte per codepoint: which component (if any) has already
	 * claimed it. Priority is component order in v->font[], i.e. the
	 * order given in the ctl write. */
	claimed = emalloc(Runemax + 1);
	fonts = emalloc(v->nfont * sizeof(TFont*));

	/* resolve all components once; find the text font (first component
	 * covering 'A') and the first successfully-resolved component
	 * (for header metrics) */
	ti = -1;
	hi0 = -1;
	for(i = 0; i < v->nfont; i++){
		name = smprint("%s.%d", v->font[i], size);
		dprint("truetypefs: component[%d] tryfont %s\n", i, name);
		fonts[i] = tryfont(name);
		free(name);
		if(fonts[i] == nil){
			dprint("truetypefs: component[%d] tryfont FAILED\n", i);
			continue;
		}
		if(hi0 < 0)
			hi0 = i;
		if(ti < 0 && hascp(fonts[i], 'A'))
			ti = i;
	}

	fmtstrinit(&fmt);
	gotheader = 0;
	if(hi0 >= 0){
		f = fonts[hi0];
		fmtprint(&fmt, "%d\t%d\n", f->ttf->ascentpx + f->ttf->descentpx, f->ttf->ascentpx);
		gotheader = 1;
	}

	if(ti >= 0){
	  tf = fonts[ti];
	  for(k = 0; k < 0x80; k++)
		if(hascp(tf, k) && !claimed[k])
		  emitrun(&fmt, claimed, v->font[ti], size, k, k, k & ~(MAXSUB-1));
	}

	for(i = 0; i < v->nfont; i++){
		f = fonts[i];
		if(f == nil)
			continue;
		for(c = f->ttf->u->cmap; c < f->ttf->u->cmap + f->ttf->u->ncmap; c++)
			for(k = c->start & ~(MAXSUB-1); k <= c->end; k += MAXSUB){
				lo = c->start > k ? c->start : k;
				hi = c->end < k+MAXSUB-1 ? c->end : k+MAXSUB-1;
				emitrun(&fmt, claimed, v->font[i], size, lo, hi, k);
			}
	}
	free(claimed);
	free(fonts);
	if(!gotheader){
		dprint("truetypefs: mkvsize %s.%d FAILED: no components\n", v->name, size);
		free(fmtstrflush(&fmt));
		return nil;
	}

	s = emalloc(sizeof *s);
	s->font = v;
	s->size = size;
	s->data = fmtstrflush(&fmt);
	s->ndata = strlen(s->data);
	dprint("truetypefs: mkvsize %s.%d created, ndata=%d\n", v->name, size, s->ndata);
	s->qid = (Qid){qidgen(), 0, QTDIR};
	s->fileqid = (Qid){qidgen(), 0, 0};
	s->next = v->size;
	v->size = s;
	return s;
}

static char *
writectl(char *data, int n)
{
	char *p, *a[64];
	VFont *v;
	int i, na;

	p = emalloc(n + 1);
	memmove(p, data, n);
	p[n] = 0;
	na = tokenize(p, a, nelem(a));
	if(na < 2){
		free(p);
		return "usage: name font ...";
	}
	if(findvfont(a[0]) != nil){
		free(p);
		return "font already exists";
	}
	v = emalloc(sizeof *v);
	v->name = strdup(a[0]);
	v->qid = (Qid){qidgen(), 0, QTDIR};
	v->font = emalloc((na - 1) * sizeof(char*));
	v->nfont = na - 1;
	for(i = 1; i < na; i++)
		v->font[i-1] = strdup(a[i]);
	dprint("truetypefs: creating virtual font %s with %d components\n", v->name, v->nfont);
	for(i = 0; i < v->nfont; i++)
		dprint("truetypefs: component[%d] = %s\n", i, v->font[i]);
	v->next = vfontl;
	vfontl = v;
	free(p);
	return nil;
}

TFont *
tryfont(char *name)
{
	TTFont *ttf;
	TFont *f, *g;
	char *d, *buf, *p;
	int sz;

	for(f = fontl.next; f != &fontl; f = f->next)
		if(strcmp(f->name9p, name) == 0)
			return f;
	d = strrchr(name, '.');
	if(d == nil){
	inval:
		werrstr("invalid file name");
		return nil;
	}
	sz = strtol(d + 1, &p, 10);
	if(d[1] == 0 || *p != 0)
		goto inval;
	buf = estrdup9p(name);
	buf[d - name] = 0;
	p = smprint("%s/%s", fontpath, buf);
	if(p == nil)
		sysfatal("smprint: %r");
	for(g = fontl.next; g != &fontl; g = g->next)
	  if(strcmp(g->name, buf) == 0)
		break;
	ttf = g != &fontl ? ttfscale(g->ttf, sz, 0) : ttfopen(p, sz, 0);
	free(p);
	if(ttf == nil){
		free(buf);
		return nil;
	}
	f = emalloc(sizeof(TFont));
	f->ttf = ttf;
	f->name9p = strdup(name);
	f->name = buf;
	f->size = sz;
	f->qid = (Qid){qidgen(), 0, QTDIR};
	f->fileqid = (Qid){qidgen(), 0, 0};
	f->glyphqid = (Qid){qidgen(), 0, QTDIR};
	f->shapeableqid = (Qid){qidgen(), 0, 0};
	f->shapeqid = (Qid){qidgen(), 0, QTDIR};
	f->shapeflags = -1;	/* not yet computed, see shapeflags() */
	f->colorflags = -1;	/* not yet computed, see colorcapable() */
	f->next = &fontl;
	f->prev = fontl.prev;
	f->next->prev = f;
	f->prev->next = f;
	mksubfonts(f);
	return f;
}

static char *
fsclone(Fid *old, Fid *new)
{
	new->aux = emalloc(sizeof(FidAux));
	*(FidAux*)new->aux = *(FidAux*)old->aux;
	return nil;
}

static void
fsdestroyfid(Fid *f)
{
	FidAux *fa;

	fa = f->aux;
	free(fa);
	f->aux = nil;
}

static TSubfont *
findsubfont(TFont *f, char *name)
{
	char *p, *q;
	char buf[16];
	int a, b, n;
	TSubfont *s;

	if(name[0] != 's' || name[1] != '.' || name[2] == '-')
		return nil;
	a = strtol(name + 2, &p, 16);
	if(*p != '-')
		return nil;
	b = strtol(p + 1, &q, 16);
	if(p + 1 == q)
		return nil;
	/*
	 * Accept either an exact match, or the canonical "s.XXXX-YYYY"
	 * prefix followed by a depth-disambiguation suffix (subfontname()
	 * in libdraw appends ".N" when multiple depths of the same
	 * subfont might legitimately be requested). truetypefs only ever
	 * serves one depth per page, so the suffix carries no real
	 * information here -- it must simply not cause ENOENT, which is
	 * what the old strict *q==0 check did for any depth-suffixed
	 * request, including ordinary non-cluster single-rune lookups.
	 */
	n = snprint(buf, nelem(buf), "s.%.4ux-%.4ux", a, b);
	if(strncmp(buf, name, n) != 0)
		return nil;
	if(name[n] != 0 && !(name[n] == '.' && name[n+1] >= '0' && name[n+1] <= '9'))
		return nil;
	for(s = f->sub[a>>8&0xff]; s != nil; s = s->next)
		if(s->start == a && s->end == b)
			break;
	return s;
}

static char *
fswalk(Fid *fid, char *name, Qid *qid)
{
	static char errbuf[ERRMAX];
	FidAux *fa;

	fa = fid->aux;
	assert(fa != nil);
	dprint("truetypefs: walk type=%d name=%s\n", fa->type, name);
	switch(fa->type){
	case FIDROOT:
		if(strcmp(name, "ctl") == 0){
			fa->type = FIDCTL;
			fid->qid = ctlqid;
			*qid = fid->qid;
			return nil;
		}
		fa->f = tryfont(name);
		if(fa->f != nil){
			fa->f->ref++;
			fa->type = FIDFONT;
			fid->qid = fa->f->qid;
			*qid = fa->f->qid;
			return nil;
		}
		if((fa->vf = findvfont(name)) != nil){
			fa->type = FIDFONT;
			fid->qid = fa->vf->qid;
			*qid = fid->qid;
			return nil;
		}
		rerrstr(errbuf, nelem(errbuf));
		return errbuf;
	case FIDFONT:
		if(fa->vf != nil){
			char *p;
			int sz;

			sz = strtol(name, &p, 10);
			if(name[0] == 0 || *p != 0 || sz <= 0)
				return Enoent;
			dprint("truetypefs: virtual %s walk size %d\n", fa->vf->name, sz);
			fa->vs = mkvsize(fa->vf, sz);
			dprint("truetypefs: mkvsize returned %p ndata=%d\n", fa->vs, fa->vs != nil ? fa->vs->ndata : 0);
			if(fa->vs == nil || fa->vs->ndata == 0)
				return Enoent;
			fa->type = FIDVSIZE;
			fid->qid = fa->vs->qid;
			*qid = fa->vs->qid;
			return nil;
		}
		if(strcmp(name, "font") == 0){
			fa->type = FIDFONTF;
			fid->qid = fa->f->fileqid;
			*qid = fa->f->fileqid;
			return nil;
		}
		if(strcmp(name, "glyph") == 0){
			fa->type = FIDGLYPHDIR;
			fid->qid = fa->f->glyphqid;
			*qid = fa->f->glyphqid;
			return nil;
		}
		if(strcmp(name, "shapeable") == 0){
			fa->type = FIDSHAPEABLE;
			fid->qid = fa->f->shapeableqid;
			*qid = fa->f->shapeableqid;
			return nil;
		}
		if(strcmp(name, "shape") == 0){
			fa->type = FIDSHAPEDIR;
			fid->qid = fa->f->shapeqid;
			*qid = fa->f->shapeqid;
			return nil;
		}
		fa->sub = findsubfont(fa->f, name);
		if(fa->sub == nil)
			return Enoent;
		fa->type = FIDSUB;
		fid->qid = fa->sub->qid;
		*qid = fa->sub->qid;
		return nil;
	case FIDVSIZE:
		if(strcmp(name, "font") != 0)
			return Enoent;
		fa->type = FIDFONTF;
		fa->f = nil;
		fid->qid = fa->vs->fileqid;
		*qid = fa->vs->fileqid;
		return nil;
	case FIDGLYPHDIR:
		fa->g = findglyph(fa->f, name);
		if(fa->g == nil)
			return Enoent;
		fa->type = FIDGLYPH;
		fid->qid = fa->g->qid;
		*qid = fa->g->qid;
		return nil;
	case FIDSHAPEDIR:
		fa->sq = findshapeq(fa->f, name);
		if(fa->sq == nil)
			return Enoent;
		fa->type = FIDSHAPE;
		fid->qid = fa->sq->qid;
		*qid = fa->sq->qid;
		return nil;
	default:
		return Egreg;
	}
}

static void
fsstat(Req *r)
{
	FidAux *fa;

	fa = r->fid->aux;
	assert(fa != nil);
	r->d.uid = estrdup9p(getuser());
	r->d.gid = estrdup9p(getuser());
	r->d.muid = estrdup9p(getuser());
	r->d.mtime = r->d.atime = time(0);
	r->d.qid = r->fid->qid;
	switch(fa->type){
	case FIDROOT:
		r->d.mode = 0777;
		r->d.name = estrdup9p("/");
		respond(r, nil);
		break;
	case FIDFONT:
		r->d.mode = 0777;
		r->d.name = estrdup9p(fa->f != nil ? fa->f->name9p : fa->vf->name);
		respond(r, nil);
		break;
	case FIDVSIZE:
		r->d.mode = 0777;
		r->d.name = smprint("%d", fa->vs->size);
		respond(r, nil);
		break;
	case FIDCTL:
		r->d.mode = 0666;
		r->d.name = estrdup9p("ctl");
		respond(r, nil);
		break;
	case FIDFONTF:
		r->d.mode = 0666;
		r->d.name = estrdup9p("font");
		r->d.length = fa->vs != nil ? fa->vs->ndata : fa->f->nfontfile;
		respond(r, nil);
		break;
	case FIDSUB:
		r->d.mode = 0666;
		r->d.name = smprint("s.%.4ux-%.4ux", fa->sub->start, fa->sub->end);
		r->d.length = fa->sub->ndata;
		respond(r, nil);
		break;
	case FIDGLYPHDIR:
		r->d.mode = 0777;
		r->d.name = estrdup9p("glyph");
		respond(r, nil);
		break;
	case FIDGLYPH:
		r->d.mode = 0444;
		r->d.name = runeseqname(fa->g->seq, fa->g->nseq);
		r->d.length = fa->g->ndata;
		respond(r, nil);
		break;
	case FIDSHAPEABLE:
		r->d.mode = 0444;
		r->d.name = estrdup9p("shapeable");
		respond(r, nil);
		break;
	case FIDSHAPEDIR:
		r->d.mode = 0777;
		r->d.name = estrdup9p("shape");
		respond(r, nil);
		break;
	case FIDSHAPE:
		r->d.mode = 0444;
		r->d.name = runeseqname(fa->sq->seq, fa->sq->nseq);
		r->d.length = fa->sq->ndata;
		respond(r, nil);
		break;
	default:
		respond(r, Egreg);
	}
}

static int
vfontdirread(int n, Dir *d, void *aux)
{
	FidAux *fa;
	VSize *s;
	int i;

	fa = aux;
	for(s = fa->vf->size, i = 0; s != nil; s = s->next, i++)
		if(i == n){
			d->name = smprint("%d", s->size);
			d->uid = estrdup9p(getuser());
			d->gid = estrdup9p(getuser());
			d->muid = estrdup9p(getuser());
			d->mode = 0777;
			d->qid = s->qid;
			d->mtime = d->atime = time(0);
			d->length = 0;
			return 0;
		}
	return -1;
}

static int
vsize_read(int n, Dir *d, void *aux)
{
	if(n != 0)
		return -1;
	d->name = estrdup9p("font");
	d->uid = estrdup9p(getuser());
	d->gid = estrdup9p(getuser());
	d->muid = estrdup9p(getuser());
	d->mode = 0666;
	d->qid = ((FidAux*)aux)->vs->fileqid;
	d->mtime = d->atime = time(0);
	d->length = ((FidAux*)aux)->vs->ndata;
	return 0;
}

static int
fontdirread(int n, Dir *d, void *aux)
{
	FidAux *fa;

	fa = aux;
	if(n == 0){
		d->name = estrdup9p("font");
		d->uid = estrdup9p(getuser());
		d->gid = estrdup9p(getuser());
		d->muid = estrdup9p(getuser());
		d->mode = 0666;
		d->qid = fa->f->fileqid;
		d->mtime = d->atime = time(0);
		d->length = fa->f->nfontfile;
		return 0;
	}
	return -1;
}

static void
fsread(Req *r)
{
	FidAux *fa;

	fa = r->fid->aux;
	assert(fa != nil);
	switch(fa->type){
	case FIDROOT:
		respond(r, nil);
		break;
	case FIDFONT:
		if(fa->vf != nil)
			dirread9p(r, vfontdirread, fa);
		else
			dirread9p(r, fontdirread, fa);
		respond(r, nil);
		break;
	case FIDVSIZE:
		dirread9p(r, vsize_read, fa);
		respond(r, nil);
		break;
	case FIDCTL:
		{
			Fmt fmt;
			VFont *v;
			char *data;
			int i, ndata;

			fmtstrinit(&fmt);
			for(v = vfontl; v != nil; v = v->next){
				fmtprint(&fmt, "%s\t", v->name);
				for(i = 0; i < v->nfont; i++)
					fmtprint(&fmt, "%s ", v->font[i]);
				fmtprint(&fmt, "\n");
			}
			data = fmtstrflush(&fmt);
			ndata = strlen(data);
			readbuf(r, data, ndata);
			free(data);
		}
		respond(r, nil);
		break;
	case FIDFONTF:
		if(fa->vs != nil)
			readbuf(r, fa->vs->data, fa->vs->ndata);
		else
			readbuf(r, fa->f->fontfile, fa->f->nfontfile);
		respond(r, nil);
		break;
	case FIDSUB:
		if(fa->sub->data == nil)
			compilesub(fa->f, fa->sub);
		readbuf(r, fa->sub->data, fa->sub->ndata);
		respond(r, nil);
		break;
	case FIDGLYPHDIR:
		respond(r, nil);	/* no listing: access is walk-by-name only, like FIDSUB pages */
		break;
	case FIDGLYPH:
		if(fa->g->data == nil)
			compileglyph(fa->f, fa->g);
		readbuf(r, fa->g->data, fa->g->ndata);
		respond(r, nil);
		break;
	case FIDSHAPEABLE:
		{
			char buf[32];
			int fl, n;
			TTFont *ttf;

			fl = shapeflags(fa->f);
			ttf = fa->f->ttf;
			n = snprint(buf, sizeof buf, "%d %d %d %d %d %d\n",
				(fl & Fgsub) != 0, (fl & Fgpos) != 0, (fl & Fctx) != 0,
						colorcapable(fa->f),
						ttf->ascentpx, ttf->ascentpx + ttf->descentpx);
			readbuf(r, buf, n);
		}
		respond(r, nil);
		break;
	case FIDSHAPEDIR:
		respond(r, nil);	/* no listing: access is walk-by-name only */
		break;
	case FIDSHAPE:
		if(fa->sq->data == nil)
			compileshapewindow(fa->f, fa->sq);
		readbuf(r, fa->sq->data, fa->sq->ndata);
		respond(r, nil);
		break;
	default:
		respond(r, Egreg);
	}
}

static void
fswrite(Req *r)
{
	FidAux *fa;
	char *err;

	fa = r->fid->aux;
	if(fa == nil || fa->type != FIDCTL){
		respond(r, Egreg);
		return;
	}
	err = writectl(r->ifcall.data, r->ifcall.count);
	if(err != nil){
		respond(r, err);
		return;
	}
	r->ofcall.count = r->ifcall.count;
	respond(r, nil);
}

Srv fssrv = {
	.attach = fsattach,
	.walk1 = fswalk,
	.clone = fsclone,
	.stat = fsstat,
	.read = fsread,
	.write = fswrite,
	.destroyfid = fsdestroyfid,
};

static void
usage(void)
{
	fprint(2, "usage: %s [-d] [-F fontpath]\n", argv0);
	exits("usage");
}

void
threadmain(int argc, char **argv)
{
	ARGBEGIN {
	case 'F':
		fontpath = EARGF(usage());
		break;
	case 'd':
		debug = 1;
		break;
	default:
		usage();
	} ARGEND;

	ctlqid = (Qid){qidgen(), 0, 0};
	unmount(nil, "/n/ttf");
	threadpostmountsrv(&fssrv, nil, "/n/ttf", MREPL);
	threadexits(nil);
}
