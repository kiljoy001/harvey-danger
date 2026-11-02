/*
 * This file is part of the UCB release of Plan 9. It is subject to the license
 * terms in the LICENSE file found in the top-level directory of this
 * distribution and at http://akaros.cs.berkeley.edu/files/Plan9License. No
 * part of the UCB release of Plan 9, including this file, may be copied,
 * modified, propagated, or distributed except according to the terms contained
 * in the LICENSE file.
 */

#include <u.h>
#include <libc.h>
#include <draw.h>

static int	fontresize(Font*, int, int, int, u32);
static char*	findsub(Font*, Rune);

#define	PJW	0	/* use NUL==pjw for invisible characters */

/*
 * Set FONTDEBUG=1 (or any nonzero value) in the environment before
 * the process starts to enable the shaping/clustering trace fprints
 * below. getenv() only happens once, cached in fontdebug, so normal
 * runs pay one lookup and nothing after.
 */
int fontdebug = -1;

static int
fdebug(void)
{
	char *e;

	if(fontdebug < 0){
		e = getenv("FONTDEBUG");
		fontdebug = e != nil && *e != '\0' && *e != '0';
		free(e);
	}
	return fontdebug;
}

/* enum { MAXCLUSTER = 8 }; */
enum { Fgsub = 1, Fgpos = 2, Fctx = 4 };
enum { SHAPEWIN = 24 };	/* runes fetched per /shape probe */

typedef struct Shapecap Shapecap;
struct Shapecap
{
	char		*prefix;	/* subfontname minus trailing /s.XXXX-YYYY */
	int		flags;		/* -1 = probe failed, else Fgsub|Fgpos|Fctx bits */
	int		colorflags;	/* -1 = not yet probed, else 0/1, see colorcapable() */
	int ascent, height;
	Shapecap	*next;
};

typedef struct Shapeq Shapeq;
struct Shapeq
{
	int	len[SHAPEWIN];
	int	blen[SHAPEWIN];		/* byte length of cluster i, parallel to len[] -- filled by nextclusterutf's refill, unused/zero in rune-mode */
	int	nlen;
	int	pos;
	Rune	winbuf[SHAPEWIN];	/* runes of the last fetched window, for freshness check */
	int	nwin;			/* count valid in winbuf */
	Rune	basecut[MAXCLUSTER];	/* dropped edge-cluster runes, refed next refill */
	int	basecutbytes[MAXCLUSTER];	/* byte width of each basecut rune, parallel array -- unused in rune-mode */
	int	nbasecut;
	char	*subfont;		/* copy of subfontname that owns the queued /shape result */
};

typedef struct Clusterimg Clusterimg;
typedef struct Shapeaux Shapeaux;
struct Shapeaux
{
	Shapecap	*cap;
	Shapeq		q;
	Clusterimg	*cluster;
};
struct Clusterimg
{
	Rune		r[MAXCLUSTER];
	int		n;
	Image		*img;
	Clusterimg	*next;
};

static Shapeaux*
shapeaux(Font *f)
{
	if(f->shapeaux == nil)
		f->shapeaux = mallocz(sizeof(Shapeaux), 1);
	return f->shapeaux;
}

/* strip trailing "/s.XXXX-YYYY", matching Cachefont.subfontname's shape
 * for a real ttf-backed page -- keys the capability cache per real
 * component directory, not per page. */
static char*
shapeprefix(char *subfontname)
{
	char *p, *s;
	int n;

	for(p = subfontname; *p; p++)
		if(p[0] == '/' && p[1] == 's' && p[2] == '.'){
			n = p - subfontname;
			s = malloc(n + 1);
			memmove(s, subfontname, n);
			s[n] = 0;
			return s;
		}
	return strdup(subfontname);
}

/*
 * Find (or create) the per-prefix capability cache entry shared by
 * shapecapable() and colorcapable() -- both are keyed on the same
 * subfontname prefix, so one entry holds both, each probed lazily and
 * independently the first time either is asked for.
 */
static Shapecap*
findcap(Font *f, char *subfontname)
{
	Shapeaux *sx;
	Shapecap *c;
	char *prefix;

	sx = shapeaux(f);
	prefix = shapeprefix(subfontname);
	for(c = sx->cap; c != nil; c = c->next)
		if(strcmp(c->prefix, prefix) == 0){
			free(prefix);
			return c;
		}

	c = mallocz(sizeof(Shapecap), 1);
	c->prefix = prefix;
	c->flags = -1;
	c->colorflags = -1;
	c->next = sx->cap;
	sx->cap = c;
	return c;
}

/*
 * 0 = not shapeable (or probe failed -- treated the same: fall back to
 * ordinary single-rune behavior). Only ever opens anything under
 * /n/ttf/ -- any other subfontname (plain .bit fonts) costs zero round
 * trips, matching the mount-point gate discussed earlier.
 *
 * The first three fields of /shapeable are GSUB, GPOS, and contextual
 * shaping capability; the fourth is whether the font has COLR glyphs.
 * Both results are cached by prefix from this one probe.
 */
static int
shapecapable(Font *f, char *subfontname)
{
	Shapecap *c;
	char buf[64], path[256];
	int fd, n, gsub, gpos, ctx, color;

	if(strncmp(subfontname, "/n/ttf/", 7) != 0)
		return 0;

	c = findcap(f, subfontname);
	if(c->flags >= 0)
		return c->flags;

	c->flags = 0;
	c->colorflags = 0;
	snprint(path, sizeof path, "%s/shapeable", c->prefix);
	fd = open(path, OREAD);
	if(fd >= 0){
		n = read(fd, buf, sizeof buf - 1);
		close(fd);
		if(n > 0){
			char *a[6];

			buf[n] = 0;
			if (fdebug())
			  fprint(2, "shapecapable: sfn=%s path=%s buf=%s\n",
					 subfontname, path, buf);
			if(tokenize(buf, a, nelem(a)) == nelem(a)){
				gsub = atoi(a[0]);
				gpos = atoi(a[1]);
				ctx = atoi(a[2]);
				color = atoi(a[3]);
				c->flags = (gsub ? Fgsub : 0) |
					(gpos ? Fgpos : 0) |
					(ctx ? Fctx : 0);
				c->colorflags = color != 0;
				c->ascent = atoi(a[4]);
				c->height = atoi(a[5]);
			}
		}
	}
	return c->flags;
}

/*
 * 0/1 = whether this font has any COLR (color) glyphs at all, per
 * /n/ttf/.../colorcapable. Probed once per prefix, same caching as
 * shapecapable(). Lets cachecharscluster() decide whether a single, non-
 * clustered rune is worth trying loadcluster()'s color-capable
 * per-glyph fetch before falling back to loadchar()'s cheaper but
 * always-monochrome whole-page subfont -- plain text fonts never pay
 * the extra round trip.
 */
static int
colorcapable(Font *f, char *subfontname)
{
	Shapecap *c;

	if(strncmp(subfontname, "/n/ttf/", 7) != 0)
		return 0;

	c = findcap(f, subfontname);
	if(c->colorflags == -1)
	  shapecapable(f, subfontname);
	return c->colorflags;
}

/*
 * Cluster length for rp[0], given n runes still available in the
 * string. Drains a queued /shape response; refills with one round
 * trip carrying up to SHAPEWIN runes (any leftover from a previous
 * window's withheld trailing cluster first, then fresh runes from rp)
 * when the queue runs dry. Falls back to 1 whenever the font isn't
 * shapeable, the fetch fails, or there's too little left to probe
 * usefully -- ordinary single-rune behavior, unchanged from today.
 */
/*
 * Append up to n runes from rp as dash-hex ("XXXX-YYYY-...") to buf,
 * bounded by bufsz. Used to build the trailing path component under
 * both /shape/ and /glyph/ -- same encoding truetypefs.c's
 * runeseqname() produces server-side, so paths built here always
 * resolve. Returns the number of runes actually encoded, which may be
 * less than n if bufsz ran out first.
 */
static int
seqtohex(char *buf, int bufsz, Rune *rp, int n)
{
	char *p, *e;
	int i;

	p = buf;
	e = buf + bufsz;
	for(i = 0; i < n && p < e-6 && rp[i]; i++)
		p += snprint(p, e-p, "%s%.4ux", i ? "-" : "", rp[i]);
	return i;
}

/*
 * shaperefill() only records cluster lengths, never which runes
 * they cover. subfont-name equality is not sufficient to trust a
 * leftover queue -- the same subfont page is shared across unrelated
 * strings/draws, so a queue left half-consumed by one draw can be
 * silently misapplied to a completely different string that happens
 * to reuse the same subfont right after. Record the window runes and
 * check the rune actually at rp/sp matches what the queue expects at
 * its current position before trusting q->pos < q->nlen.
 */
static int
shapeqfresh(Shapeq *q, Rune r0)
{
	int i, off;

	off = 0;
	for(i = 0; i < q->pos; i++)
		off += q->len[i];
	return off < q->nwin && q->winbuf[off] == r0;
}

/*
 * Given an assembled window, fetch /shape and fill q->len/q->nlen.
 * Returns total runes accounted for (sum of q->len[0..nlen)), which
 * may be less than nwindow if a trailing cluster was withheld by the
 * server; returns 0 if the fetch failed outright (caller should treat
 * the whole window as a single-rune fallback). Shared by nextcluster
 * (rune-mode) and nextclusterutf (byte-string mode) -- window content
 * is identical either way, only how the caller assembles/consumes it
 * differs.
 */
static int
shaperefill(char *subfontname, Rune *window, int nwindow, Shapeq *q)
{
	char path[256], *prefix, *data, *p, *e;
	int fd, nd, total, v;

	if(nwindow < 2)
		return 0;

	prefix = shapeprefix(subfontname);
	p = path;
	e = path + sizeof path;
	p += snprint(p, e-p, "%s/shape/", prefix);
	seqtohex(p, e-p, window, nwindow);
	free(prefix);

	fd = open(path, OREAD);
	if(fd < 0)
		return 0;
	data = malloc(8192);
	nd = readn(fd, data, 8191);
	close(fd);
	if(nd <= 0){
		free(data);
		return 0;
	}
	data[nd] = 0;

	q->nlen = 0;
	q->pos = 0;
	free(q->subfont);
	q->subfont = strdup(subfontname);
	q->nwin = nwindow;
	memmove(q->winbuf, window, nwindow*sizeof(Rune));
	total = 0;
	p = data;
	while(*p && q->nlen < SHAPEWIN){
		v = strtol(p, &p, 10);
		if(v <= 0)
			break;
		q->len[q->nlen++] = v;
		total += v;
		while(*p == '\n' || *p == ' ')
			p++;
	}
	free(data);
	q->pos = 0;
	return total;
}

/*
 * True for runes in the regional-indicator block (paired to spell
 * two-letter flag codes, e.g. 0x1F1EF 0x1F1F5 = JP). This is Unicode
 * grapheme-cluster segmentation (UAX #29), not font GSUB shaping --
 * emoji fonts have no lookup for it, so shapecapable()/shaperefill()
 * can never discover it via /shape. Must stay a hard-coded check
 * ahead of the shapecapable() gate. Shared by nextcluster (rune-mode)
 * and nextclusterutf (byte-string mode) so they can't drift.
 */
static int
isregind(Rune r)
{
	return r >= 0x1F1E6 && r <= 0x1F1FF;
}

static int
isvs(Rune r)
{
	return r >= 0xFE00 && r <= 0xFE0F;
}

/*
 * Shared window-builder for nextcluster/nextclusterutf. Exactly one
 * of rp/sp is non-nil, selecting rune-array vs byte-string input.
 * Stops at maxrunes, SHAPEWIN, nul, or the first rune whose subfont
 * (by findsub()) differs from subfontname -- see the comment above
 * nextcluster() for why that boundary matters. windowbytes may be
 * nil (rune-mode callers don't need per-rune byte widths); when
 * non-nil it receives each window rune's UTF-8 width, for byte-mode
 * callers translating shaperefill's rune-count clusters back to
 * bytes. Having one copy of this loop means the subfont-comparison
 * bug fixed here can't reappear in only one of the two callers.
 */
static int
buildwindow(Font *f, char *subfontname, Rune *rp, char *sp, int maxrunes, Rune *window, int *windowbytes)
{
	int nwindow, i, bw;
	Rune vr;
	char *p;
	char *owner;

	nwindow = 0;
	p = sp;
	for(i = 0; i < maxrunes && nwindow < SHAPEWIN; i++){
		if(rp != nil){
			vr = rp[i];
			if(vr == 0)
				break;
			bw = 0;
		}else{
			if(*p == 0)
				break;
			bw = chartorune(&vr, p);
		}

		if(vr == ' '){
		  if(nwindow > 0)
            break;              /* don't extend an existing window into a space */
		  /* window starting on a space: it's a window of exactly one */
		  window[nwindow] = vr;
		  if(windowbytes != nil)
            windowbytes[nwindow] = bw;
		  nwindow = 1;
		  if(rp == nil)
            p += bw;
		  break;
		}

		owner = findsub(f, vr);
		if(nwindow > 0 && (owner == nil || findcap(f, owner) != findcap(f, subfontname)))
			break;
		window[nwindow] = vr;
		if(windowbytes != nil)
			windowbytes[nwindow] = bw;
		nwindow++;
		if(rp == nil)
			p += bw;
	}
	return nwindow;
}

static int
nextcluster(Font *f, char *subfontname, Rune *rp, int n, Shapeq *q, int *fromqueue)
{
	int i, total;
	Rune window[SHAPEWIN];
	int nwindow;

	*fromqueue = 0;
	if(fdebug()){
		int dbgn, dbgi;

		dbgn = n < 3 ? n : 3;
		fprint(2, "nextcluster: sfn=%s rp=", subfontname);
		for(dbgi = 0; dbgi < dbgn; dbgi++)
			fprint(2, "%s%.4ux", dbgi ? " " : "", rp[dbgi]);
		fprint(2, "\n");
	}
	if(n >= 2 && ((isregind(rp[0]) && isregind(rp[1])) || isvs(rp[1]))){
	  *fromqueue = 1;
	  return 2;
	}
	if(!shapecapable(f, subfontname))
		return 1;

	if(fdebug())
		fprint(2, "nextcluster: owner=%s current=%s pos=%d nlen=%d\n",
			q->subfont != nil ? q->subfont : "<nil>",
			subfontname, q->pos, q->nlen);
	if(q->pos < q->nlen &&
	   (q->subfont == nil || strcmp(q->subfont, subfontname) != 0 ||
	    !shapeqfresh(q, rp[0]))){
		if(fdebug())
			fprint(2, "nextcluster: INVALIDATE stale queue\n");
		q->pos = q->nlen;
		q->nbasecut = 0;
	}

	if(q->pos < q->nlen){
		if(q->len[q->pos] > MAXCLUSTER || q->len[q->pos] > n)
			return 1;
		*fromqueue = 1;
		return q->len[q->pos];	/* peek -- caller commits via shapeqadvance() */
	}

	/*
	 * rp already points at the first rune not consumed by the queued
	 * cluster lengths. Do not prepend basecut here: those runes are
	 * still present at rp, and doing so duplicates them on the next
	 * refill.
	 *
	 * Stop the window at the first rune that isn't in this same font
	 * (findsub() names a different subfont than subfontname). Asking
	 * one font's /shape to judge whether it "ligates" with a rune it
	 * doesn't even contain a glyph for is meaningless -- ttfshape has
	 * to do something with the unmapped codepoint, and whatever that
	 * is can trip extendsok()'s heuristic into reporting a bogus
	 * cross-font cluster.
	 */
	nwindow = buildwindow(f, subfontname, rp, nil, n, window, nil);
	q->nbasecut = 0;

	total = shaperefill(subfontname, window, nwindow, q);
	if(total == 0)
		return 1;

	if(total < nwindow){
		q->nbasecut = nwindow - total;
		if(q->nbasecut > MAXCLUSTER)
			q->nbasecut = MAXCLUSTER;	/* shouldn't happen -- defensive clamp */
		for(i = 0; i < q->nbasecut; i++)
			q->basecut[i] = window[total+i];
	}

	if(q->nlen == 0 || q->len[q->pos] > MAXCLUSTER)
		return 1;
	*fromqueue = 1;
	return q->len[q->pos];	/* peek -- caller commits via shapeqadvance() */
}

/*
 * Byte-string analog of nextcluster, for cachechars' ss-mode. Only
 * ever decodes past the first rune when shapecapable() is true --
 * plain ASCII/Latin text through a non-shaping font pays nothing
 * beyond the single chartorune() cachechars already needed. Decodes
 * up to SHAPEWIN runes ahead from sp (stopping at nul or maxrunes),
 * tracking each rune's byte width so shaperefill's rune-count clusters
 * translate back into byte counts. Writes the returned cluster's own
 * decoded runes into outrunes (caller-sized >= MAXCLUSTER) so
 * downstream cmatch/loadcluster have a Rune* to operate on, same as
 * rune-mode callers get for free from rp. *nbytes receives the
 * cluster's byte length. Returns the cluster's rune count, or 1 (with
 * *nbytes set to the first rune's own width) on any fallback path.
 */
static int
nextclusterutf(Font *f, char *subfontname, char *sp, int maxrunes,
	Rune *outrunes, int *nbytes, Shapeq *q, int *fromqueue)
{
	Rune window[SHAPEWIN];
	int windowbytes[SHAPEWIN];
	int nwindow, i, k, total, wpos, n, b;
	char *p;
	Rune r0, r1;
	int w0, w1;

	*fromqueue = 0;
	w0 = chartorune(&r0, sp);
	if(fdebug()){
		Rune dbg[3];
		int dbgn, dbgi, dw;
		char *dp;

		dbg[0] = r0;
		dbgn = maxrunes > 0 ? 1 : 0;
		dp = sp + w0;
		while(dbgn < 3 && dbgn < maxrunes && *dp){
			dw = chartorune(&dbg[dbgn], dp);
			dp += dw;
			dbgn++;
		}
		fprint(2, "nextclusterutf: sfn=%s rp=", subfontname);
		for(dbgi = 0; dbgi < dbgn; dbgi++)
			fprint(2, "%s%.4ux", dbgi ? " " : "", dbg[dbgi]);
		fprint(2, "\n");
	}

	if(maxrunes >= 2 && sp[w0] != 0){
	  w1 = chartorune(&r1, sp+w0);
	  if((isregind(r0) && isregind(r1)) || isvs(r1)){
		outrunes[0] = r0;
		outrunes[1] = r1;
		*nbytes = w0 + w1;
		return 2;
	  }
	}

	if(!shapecapable(f, subfontname)){
		*nbytes = chartorune(&outrunes[0], sp);
		return 1;
	}

	if(fdebug())
		fprint(2, "nextclusterutf: owner=%s current=%s pos=%d nlen=%d\n",
			q->subfont != nil ? q->subfont : "<nil>",
			subfontname, q->pos, q->nlen);
	if(q->pos < q->nlen &&
	   (q->subfont == nil || strcmp(q->subfont, subfontname) != 0 ||
	    !shapeqfresh(q, r0))){
		if(fdebug())
			fprint(2, "nextclusterutf: INVALIDATE stale queue\n");
		q->pos = q->nlen;
		q->nbasecut = 0;
	}

	if(q->pos < q->nlen){
		n = q->len[q->pos];
		if(n > MAXCLUSTER || n > maxrunes){
			*nbytes = chartorune(&outrunes[0], sp);
			return 1;
		}
		b = q->blen[q->pos];
		*fromqueue = 1;		/* peek -- caller commits via shapeqadvance() */
		p = sp;
		for(k = 0; k < n; k++)
			p += chartorune(&outrunes[k], p);
		*nbytes = b;
		return n;
	}

	/*
	 * sp already points at the first rune not consumed by q. Stop the
	 * window at the first rune outside this same font -- see the
	 * matching comment in nextcluster().
	 */
	nwindow = buildwindow(f, subfontname, nil, sp, maxrunes, window, windowbytes);
	q->nbasecut = 0;

	total = shaperefill(subfontname, window, nwindow, q);
	if(total == 0){
		*nbytes = chartorune(&outrunes[0], sp);
		return 1;
	}

	wpos = 0;
	for(k = 0; k < q->nlen; k++){
		b = 0;
		for(i = 0; i < q->len[k]; i++)
			b += windowbytes[wpos+i];
		q->blen[k] = b;
		wpos += q->len[k];
	}
	if(total < nwindow){
		q->nbasecut = nwindow - total;
		if(q->nbasecut > MAXCLUSTER)
			q->nbasecut = MAXCLUSTER;
		for(i = 0; i < q->nbasecut; i++){
			q->basecut[i] = window[total+i];
			q->basecutbytes[i] = windowbytes[total+i];
		}
	}

	if(q->nlen == 0){
		*nbytes = chartorune(&outrunes[0], sp);
		return 1;
	}

	n = q->len[q->pos];
	if(n > MAXCLUSTER || n > maxrunes){
		*nbytes = chartorune(&outrunes[0], sp);
		return 1;
	}
	b = q->blen[q->pos];
	*fromqueue = 1;		/* peek -- caller commits via shapeqadvance() */
	p = sp;
	for(k = 0; k < n; k++)
		p += chartorune(&outrunes[k], p);
	*nbytes = b;
	return n;
}

/*
 * Commit the queue-peek nextcluster()/nextclusterutf() handed back
 * (see *fromqueue there). Called only once cachechars has confirmed
 * the cluster was actually cached -- never on a "must retry" (-1)
 * outcome -- so a failed attempt replays the identical decision
 * instead of silently desyncing every cluster after it.
 */
static void
shapeqadvance(Font *f, char *subfontname, Shapeq *q)
{
	if(!shapecapable(f, subfontname))
		return;
	if(q->subfont != nil && strcmp(q->subfont, subfontname) == 0 &&
	   q->pos < q->nlen)
		q->pos++;
}

/*
 * Called instead of shapeqadvance() whenever a queued multi-rune
 * cluster couldn't actually be fetched/drawn and cachechars is
 * downgrading to consuming just rp[0] on its own. The queue slot
 * still represents N runes' worth of position, but we're only
 * advancing the rune stream by 1 -- committing it normally (as N)
 * would desync every later decision in the string. Rather than try
 * to patch the slot's rune/byte-length bookkeeping in place (fragile,
 * and wrong for ss-mode's blen[] in particular), just invalidate the
 * rest of this shaping window so the next call is forced into a
 * fresh shaperefill() from the caller's new position -- correctness
 * over saving one round trip.
 */
static void
shapeqreset(Font *f, char *subfontname, Shapeq *q)
{
	if(!shapecapable(f, subfontname))
		return;
	q->pos = q->nlen;
}

static int
cmatch(Cacheinfo *c, Rune *rp, int n)
{
	if(c->nval != n)
		return 0;
	return memcmp(c->value, rp, n*sizeof(Rune)) == 0;
}

static char *
cfsubfontname(Cachefont *cf, Font *f)
{
	int depth;
	char *name;

	name = cf->subfontname;
	if(name != nil)
		return name;

	if(f->display != nil && f->display->screenimage != nil)
		depth = f->display->screenimage->depth;
	else
		depth = 8;

	name = subfontname(cf->name, f->name, depth);
	if(name == nil)
		return nil;

	cf->subfontname = name;
	return name;
}

static char *
findsub(Font *f, Rune r)
{
	int i;
	Cachefont *cf;

	for(i = 0; i < f->nsub; i++){
		cf = f->sub[i];
		if(cf->min <= r && r <= cf->max)
			return cfsubfontname(cf, f);
	}
	return nil;
}

/*
 * Public: how many runes starting at rp[0] (up to n available) form
 * one cluster, per the same shaping machinery cachechars uses to
 * decide draw-time cache slots. Exposed so libframe's _frcanfit can
 * make box-split decisions that agree with what actually gets drawn
 * later -- a split boundary chosen without this would risk landing in
 * the middle of a cluster, which cachechars itself would then have no
 * way to recover from (each half is a separate box, drawn separately,
 * with no lookahead into the other half). Returns 1 for any rune not
 * in a shapeable font's range, matching ordinary unclustered behavior.
 */
int
nextclusterlen(Font *f, Rune *rp, int n)
{
	char *sfn;
	Shapeq lq;
	int fromqueue, length;	/* pure peek, never a commit */

	if(n <= 0)
		return 0;
	sfn = findsub(f, *rp);
	if(sfn == nil)
		return 1;
	memset(&lq, 0, sizeof lq);
	length = nextcluster(f, sfn, rp, n, &lq, &fromqueue);
	free(lq.subfont);
	return length;
}

static Clusterimg*
findclusterimg(Shapeaux *sx, Rune *rp, int n)
{
	Clusterimg *ci;
	int i, debug;

	debug = getenv("GLYPHDEBUG") != nil;

	if(debug){
		fprint(2, "findclusterimg: lookup n=%d rp=", n);
		for(i = 0; i < n; i++)
			fprint(2, "%.4ux%s", rp[i], i+1 < n ? "-" : "");
		fprint(2, "\n");
	}

	for(ci = sx->cluster; ci != nil; ci = ci->next){
		if(ci->n != n)
			continue;

		if(memcmp(ci->r, rp, n*sizeof(Rune)) == 0){
			if(debug){
				fprint(2, "findclusterimg: HIT n=%d rp=", n);
				for(i = 0; i < n; i++)
					fprint(2, "%.4ux%s", rp[i],
						i+1 < n ? "-" : "");
				fprint(2, " ci=%p img=%p\n",
					ci, ci->img);
			}
			return ci;
		}
	}

	if(debug){
		fprint(2, "findclusterimg: MISS n=%d rp=", n);
		for(i = 0; i < n; i++)
			fprint(2, "%.4ux%s", rp[i], i+1 < n ? "-" : "");
		fprint(2, "\n");
	}

	return nil;
}

static Clusterimg*
newclusterimg(Shapeaux *sx, Rune *rp, int n)
{
	Clusterimg *ci;
	int i, debug;

	debug = getenv("GLYPHDEBUG") != nil;

	if(debug){
		fprint(2, "newclusterimg: n=%d rp=", n);
		for(i = 0; i < n; i++)
			fprint(2, "%.4ux%s", rp[i], i+1 < n ? "-" : "");
		fprint(2, "\n");
	}

	ci = mallocz(sizeof *ci, 1);
	if(ci == nil)
		return nil;

	memmove(ci->r, rp, n*sizeof(Rune));
	ci->n = n;
	ci->next = sx->cluster;
	sx->cluster = ci;

	if(debug)
		fprint(2, "newclusterimg: ci=%p\n", ci);

	return ci;
}

/*
 * Cluster analog of loadchar -- n>1 runes, fetched as a single
 * image(6) blob from /glyph/<hex>-...-<hex> rather than indexed out
 * of a shared page's Subfont. No Cachefont/Cachesubf involved: caches
 * the decoded Image directly, keyed by the rune sequence, in its own
 * small aged list (see Shapeaux/Clusterimg below) mirroring Cachesubf's
 * shape but for a standalone Image instead of a whole Subfont.
 */
int
loadcluster(Font *f, Rune *rp, int n, Cacheinfo *c, int h, int noflush)
{
	Shapeaux *sx;
	Clusterimg *ci;
	Image *img;
	char path[256], *sfn, *prefix, *p;
	int i, fd, wid, top, bottom;
	uchar *b;
	int debug;

	debug = getenv("GLYPHDEBUG") != nil;

	if(debug){
		fprint(2, "loadcluster: request f=%p n=%d rp=",
			f, n);
		for(i = 0; i < n; i++)
			fprint(2, "%.4ux%s", rp[i], i+1 < n ? "-" : "");
		fprint(2, "\n");
	}

	sfn = findsub(f, rp[0]);
	if(sfn == nil){
		if(debug)
			fprint(2, "loadcluster: no subfont for first rune %.4ux\n",
				rp[0]);
		return 0;
	}

	if(debug)
		fprint(2, "loadcluster: sfn=%s\n", sfn);

	sx = shapeaux(f);
	ci = findclusterimg(sx, rp, n);
	if(ci == nil){
		if(debug)
			fprint(2, "loadcluster: cache miss; building path\n");

		prefix = shapeprefix(sfn);
		p = path;
		p += snprint(p, path+sizeof path-p, "%s/glyph/", prefix);
		free(prefix);
		i = seqtohex(p, path+sizeof path-p, rp, n);
		if(i != n){
			if(debug)
				fprint(2,
					"loadcluster: seqtohex failed: got %d want %d\n",
					i, n);
			return 0;
		}

		if(debug)
			fprint(2, "loadcluster: fetching %s\n", path);

		fd = open(path, OREAD);
		if(fd < 0){
			if(debug)
				fprint(2, "loadcluster: open failed %s\n", path);
			return 0;
		}

		img = readimage(f->display, fd, 0);
		close(fd);

		if(img == nil){
			if(debug)
				fprint(2,
					"loadcluster: readimage failed %s\n",
					path);
			return -1;
		}

		if(debug)
			fprint(2,
				"loadcluster: fetched img=%p r=(%d,%d,%d,%d) "
				"chan=%#lux depth=%d\n",
				img,
				img->r.min.x, img->r.min.y,
				img->r.max.x, img->r.max.y,
				img->chan, img->depth);

		ci = newclusterimg(sx, rp, n);
		if(ci == nil){
			if(debug)
				fprint(2,
					"loadcluster: newclusterimg failed\n");
			return 0;
		}

		ci->img = img;

		if(debug)
			fprint(2,
				"loadcluster: cached ci=%p img=%p "
				"chan=%#lux depth=%d cache depth=%d\n",
				ci, ci->img,
				ci->img->chan, ci->img->depth,
				f->cacheimage != nil ?
				f->cacheimage->depth : -1);
	}else if(debug)
		fprint(2,
			"loadcluster: cache hit ci=%p img=%p\n",
			ci, ci->img);

	wid = Dx(ci->img->r);

	if(debug){
		int _i;

		fprint(2,
			"loadcluster: f=%p width=%d maxdepth=%d "
			"cacheimage=%p c=%p\n",
			f, f->width, f->maxdepth, f->cacheimage, c);
		fprint(2, "loadcluster: rp=");
		for(_i = 0; _i < n; _i++)
			fprint(2, "%.4ux%s", rp[_i],
				_i+1 < n ? "-" : "");
		fprint(2, " n=%d wid=%d\n", n, wid);
	}

	if(f->width < wid || f->width == 0 ||
	   f->maxdepth < ci->img->depth ||
	   (f->cacheimage != nil &&
	    f->cacheimage->chan != ci->img->chan) ||
	   (f->display != nil && f->cacheimage == nil)){
		if(debug)
			fprint(2,
				"loadcluster: resize needed "
				"fontwidth=%d imgwidth=%d "
				"maxdepth=%d imgdepth=%d\n",
				f->width, wid,
				f->maxdepth, ci->img->depth);

		if(noflush)
			return -1;
		if(f->width < wid)
			f->width = wid;
		if(fontresize(f, f->width, f->ncache,
			ci->img->depth, ci->img->chan) <= 0)
			return -1;
	}

	if(debug)
		fprint(2,
			"loadcluster: before cache c=%p rp=%p n=%d\n",
			c, rp, n);

	c->nval = n;
	memmove(c->value, rp, n*sizeof(Rune));
	c->width = wid;
	c->x = h*f->width;
	c->left = ci->img->r.min.x;

	if(debug){
		fprint(2,
			"loadcluster: cache entry nval=%d value=",
			c->nval);
		for(i = 0; i < c->nval; i++)
			fprint(2, "%.4ux%s", c->value[i],
				i+1 < c->nval ? "-" : "");
		fprint(2,
			" width=%d x=%d left=%d\n",
			c->width, c->x, c->left);
	}

	if(f->display == nil)
		return 1;

	if(debug)
		fprint(2,
			"loadcluster: display=%p cacheimage=%p "
			"ci=%p img=%p\n",
			f->display, f->cacheimage, ci, ci->img);

	b = bufimage(f->display, 37);
	if(b == nil)
		return 0;

	Shapecap *cap = findcap(f, sfn);
	int d = (f->ascent - cap->ascent);
	top = ci->img->r.min.y;
	bottom = ci->img->r.max.y;

	if(debug)
		fprint(2,
			"loadcluster: imgr=(%d,%d,%d,%d) "
			"ascent=%d height=%d top=%d bottom=%d\n",
			ci->img->r.min.x, ci->img->r.min.y,
			ci->img->r.max.x, ci->img->r.max.y,
			f->ascent, f->height, top, bottom);

	b[0] = 'l';
	BPLONG(b+1, f->cacheimage->id);
	BPLONG(b+5, ci->img->id);
	BPSHORT(b+9, h);
	BPLONG(b+11, c->x);
	BPLONG(b+15, top);
	BPLONG(b+19, c->x+wid);
	BPLONG(b+23, bottom);
	BPLONG(b+27, ci->img->r.min.x);
	BPLONG(b+31, ci->img->r.min.y - d);
	b[35] = ci->img->r.min.x;
	b[36] = wid;
	return 1;
}

/* return number of translated cache indices, 0 must retry, -1 on error */
int
_cachecharsn(Font *f, char **ss, Rune **rr, ushort *cp, int max, int *wp,
	char **subfontname, int *cumx, int *clusterlen, int nrunes)
{
	int i, j, h, w, rw, wid, nc, n, fromqueue, left;
	char *sp, *sfn;
	Rune r, *rp, vr;
	Rune clusterbuf[MAXCLUSTER];
	u32 a;
	Cacheinfo *c, *tc, *ec;

	if(ss){
		sp = *ss;
		rp = (Rune*)L"";
	}else{
		sp = "";
		rp = *rr;
	}
	wid = 0;
	*subfontname = nil;
	if (0) fprint(2, "cachechars: START max=%d ss=%p rr=%p rp=%p first=%#.4ux\n",
		max, ss, rr, rp, rp[0]);

	/* Bound input separately: an output batch must not split a ligature. */
	for(i=0, left=nrunes; i<max && left>0 && (ss ? *sp != 0 : *rp != 0); sp+=w, rp+=rw){
		if(ss){
			r = *(uchar*)sp;
			if(r < Runeself)
				w = 1;
			else{
				w = chartorune(&vr, sp);
				r = vr;
			}
			n = 1;
			fromqueue = 0;
			clusterbuf[0] = r;
			sfn = findsub(f, r);
			if (0) fprint(2, "cachechars: UTF i=%d r=%#.4ux w=%d sfn=%s\n",
				i, r, w, sfn != nil ? sfn : "<nil>");
			if(sfn != nil)
				n = nextclusterutf(f, sfn, sp, left < SHAPEWIN ? left : SHAPEWIN, clusterbuf, &w,
					&shapeaux(f)->q, &fromqueue);
			rw = n;
			rp = clusterbuf;
		}else{
			r = *rp;
			w = 0;
			n = 1;
			fromqueue = 0;
			sfn = findsub(f, r);
			if (0)
			  fprint(2, "cachechars: RUNE i=%d r=%#.4ux sfn=%s\n",
				i, r, sfn != nil ? sfn : "<nil>");
			if(sfn != nil)
				n = nextcluster(f, sfn, rp, left < SHAPEWIN ? left : SHAPEWIN,
					&shapeaux(f)->q, &fromqueue);
			rw = n;
		}

		if (0) fprint(2, "cachechars: CLUSTER i=%d r=%#.4ux n=%d rw=%d fromqueue=%d sfn=%s\n",
			i, r, n, rw, fromqueue, sfn != nil ? sfn : "<nil>");

		a = ~0;
		h = (17 * (uint)r) & (f->ncache-NFLOOK-1);
		c = &f->cache[h];
		tc = c;
		ec = c+NFLOOK;

		while(c < ec){
			if(cmatch(c, rp, n) && c->age){
				if (0) fprint(2, "cachechars: FOUND-CACHE i=%d h=%d r=%#.4ux n=%d width=%d age=%lud\n",
					i, h, r, n, c->width, c->age);
				goto Found;
			}
			if(c->age < a){
				a = c->age;
				tc = c;
			}
			c++;
			h++;
		}

		c = tc;
		h = tc - f->cache;

		if (0) fprint(2, "cachechars: CACHE-MISS i=%d h=%d r=%#.4ux n=%d a=%lud cage=%lud fage=%lud\n",
			i, h, r, n, a, c->age, f->age);

		if(a && (f->age-a)<500){
			nc = 2*(f->ncache-NFLOOK) + NFLOOK;
			if (0) fprint(2, "cachechars: RESIZE-CHECK i=%d nc=%d\n", i, nc);
			if(nc <= MAXFCACHE){
				if(i == 0)
					fontresize(f, f->width, nc, f->maxdepth,
						f->cacheimage != nil ?
						f->cacheimage->chan : CHAN1(CGrey, f->maxdepth));
				if (0) fprint(2, "cachechars: RESIZE/BREAK i=%d\n", i);
				break;
			}
		}

		if(i > 0 && c->age == f->age){
			if (0) fprint(2, "cachechars: FLUSH/BREAK i=%d h=%d\n", i, h);
			break;
		}

		if(fdebug())
			if (0) fprint(2, "cachechars: i=%d rp=%p r=%#.4ux rw=%d n=%d sfn=%s\n",
				i, rp, r, rw, n, sfn != nil ? sfn : "<nil>");

		if(n > 1 || (sfn != nil && colorcapable(f, sfn))){
			if (0) fprint(2, "cachechars: LOADCLUSTER i=%d r=%#.4ux n=%d sfn=%s\n",
				i, r, n, sfn != nil ? sfn : "<nil>");

			j = loadcluster(f, rp, n, c, h, i);

			if (0) fprint(2, "cachechars: LOADCLUSTER-RESULT i=%d j=%d\n",
				i, j);

			if(j == 0){
				if (0) fprint(2, "cachechars: LOADCLUSTER-FALLBACK i=%d r=%#.4ux\n",
					i, r);
				if(fromqueue)
					shapeqreset(f, sfn, &shapeaux(f)->q);
				fromqueue = 0;
				n = 1;
				rw = 1;
				if(ss)
					w = chartorune(&r, sp);
				j = loadchar(f, r, c, h, i, subfontname);
				if (0) fprint(2, "cachechars: FALLBACK-LOADCHAR i=%d j=%d subfont=%s\n",
					i, j,
					*subfontname != nil ? *subfontname : "<nil>");
			}
		}else{
			if (0) fprint(2, "cachechars: LOADCHAR i=%d r=%#.4ux sfn=%s color=%d\n",
				i, r, sfn != nil ? sfn : "<nil>",
				sfn != nil ? colorcapable(f, sfn) : 0);

			j = loadchar(f, r, c, h, i, subfontname);

			if (0) fprint(2, "cachechars: LOADCHAR-RESULT i=%d j=%d subfont=%s\n",
				i, j,
				*subfontname != nil ? *subfontname : "<nil>");
		}

		if(j == 1 && fromqueue){
			if (0) fprint(2, "cachechars: QUEUE-ADVANCE i=%d sfn=%s\n",
				i, sfn != nil ? sfn : "<nil>");
			shapeqadvance(f, sfn, &shapeaux(f)->q);
		}

		if(j == 0){
			if (0) fprint(2, "cachechars: MISSING-CLUSTER-FALLBACK i=%d r=%#.4ux\n",
				i, r);
			if(fromqueue)
				shapeqreset(f, sfn, &shapeaux(f)->q);
			n = 1;
			if(ss){
				w = chartorune(&r, sp);
				clusterbuf[0] = r;
				rp = clusterbuf;
			}else
				rw = 1;
			j = loadchar(f, r, c, h, i, subfontname);
			if (0) fprint(2, "cachechars: MISSING-FALLBACK-RESULT i=%d j=%d subfont=%s\n",
				i, j,
				*subfontname != nil ? *subfontname : "<nil>");
		}

		if (0) fprint(2, "cachechars: FINAL-J i=%d j=%d r=%#.4ux n=%d\n",
			i, j, r, n);

		if(j <= 0){
			if (0) fprint(2, "cachechars: RETURN-FAIL i=%d j=%d wid=%d\n",
				i, j, wid);
			if(i == 0)
			  return -1;
			break;
		}

	Found:
		if (0) fprint(2, "cachechars: FOUND i=%d h=%d width=%d n=%d\n",
			i, h, c->width, n);

		wid += c->width;
		/* fprint(2, "cachechars: ACCEPT i=%d r=%#.4ux n=%d width=%d wid=%d\n", */
		/* i, r, n, c->width, wid); */
		c->age = f->age;
		cp[i] = h;
		if(cumx)
			cumx[i] = wid;
		if(clusterlen)
			clusterlen[i] = n;
		i++;
		left -= n;

		if (0) fprint(2, "cachechars: ACCEPT i=%d wid=%d next-rp=%p\n",
			i, wid, rp+rw);
	}

	if(ss)
		*ss = sp;
	else
		*rr = rp;
	*wp = wid;

	/* if (!ss) fprint(2, "cachechars: RETURN i=%d wid=%d rp=%p\n", */
	/* 	i, wid, rp); */

	return i;
}

void
agefont(Font *f)
{
	Cacheinfo *c, *ec;
	Cachesubf *s, *es;

	f->age++;
	if(f->age == 65536){
		/*
		 * Renormalize ages
		 */
		c = f->cache;
		ec = c+f->ncache;
		while(c < ec){
			if(c->age){
				c->age >>= 2;
				c->age++;
			}
			c++;
		}
		s = f->subf;
		es = s+f->nsubf;
		while(s < es){
			if(s->age){
				if(s->age<SUBFAGE && s->cf->name != nil){
					/* clean up */
					if(f->display == nil || s->f != f->display->defaultsubfont)
						freesubfont(s->f);
					s->cf = nil;
					s->f = nil;
					s->age = 0;
				}else{
					s->age >>= 2;
					s->age++;
				}
			}
			s++;
		}
		f->age = (65536>>2) + 1;
	}
}

static Subfont*
cf2subfont(Cachefont *cf, Font *f)
{
	char *name;
	Subfont *sf;

	name = cfsubfontname(cf, f);
	if(name == nil)
		return nil;
	sf = lookupsubfont(f->display, name);
	if(sf != nil)
		return sf;
	return _getsubfont(f->display, name);
}

/* return 1 if load succeeded, 0 if failed, -1 if must retry */
int
loadchar(Font *f, Rune r, Cacheinfo *c, int h, int noflush, char **subfontname)
{
	int i, oi, wid, top, bottom;
	Rune pic;
	Fontchar *fi;
	Cachefont *cf;
	Cachesubf *subf, *of;
	uchar *b;

	pic = r;
    Again:
	for(i=0; i<f->nsub; i++){
		cf = f->sub[i];
		if(cf->min<=pic && pic<=cf->max)
			goto Found;
	}
    TryPJW:
	if(pic != PJW){
		pic = PJW;
		goto Again;
	}
	return 0;

    Found:
	/*
	 * Choose exact or oldest
	 */
	oi = 0;
	subf = &f->subf[0];
	for(i=0; i<f->nsubf; i++){
		if(cf == subf->cf)
			goto Found2;
		if(subf->age < f->subf[oi].age)
			oi = i;
		subf++;
	}
	subf = &f->subf[oi];

	if(subf->f){
		if(f->age-subf->age>SUBFAGE || f->nsubf>MAXSUBF){
    Toss:
			/* ancient data; toss */
			freesubfont(subf->f);
			subf->cf = nil;
			subf->f = nil;
			subf->age = 0;
		}else{				/* too recent; grow instead */
			of = f->subf;
			f->subf = realloc(of, (f->nsubf+DSUBF)*sizeof *subf);
			if(f->subf == nil){
				f->subf = of;
				goto Toss;
			}
			subf = &f->subf[f->nsubf];
			memset(subf, 0, DSUBF*sizeof *subf);
			f->nsubf += DSUBF;
		}
	}
	subf->age = 0;
	subf->cf = nil;
	subf->f = cf2subfont(cf, f);
	if(subf->f == nil){
		if(cf->subfontname == nil)
			goto TryPJW;
		*subfontname = cf->subfontname;
		return -1;
	}

	subf->cf = cf;
	if(subf->f->ascent > f->ascent && f->display){
		/* should print something? this is a mistake in the font file */
		/* must prevent c->top from going negative when loading cache */
		Image *b;
		int d, t;
		d = subf->f->ascent - f->ascent;
		b = subf->f->bits;
		draw(b, b->r, b, nil, addpt(b->r.min, Pt(0, d)));
		draw(b, Rect(b->r.min.x, b->r.max.y-d, b->r.max.x, b->r.max.y), f->display->black, nil, b->r.min);
		for(i=0; i<subf->f->n; i++){
			t = subf->f->info[i].top-d;
			if(t < 0)
				t = 0;
			subf->f->info[i].top = t;
			t = subf->f->info[i].bottom-d;
			if(t < 0)
				t = 0;
			subf->f->info[i].bottom = t;
		}
		subf->f->ascent = f->ascent;
	}

    Found2:
	subf->age = f->age;

	/* possible overflow here, but works out okay */
	pic += cf->offset;
	pic -= cf->min;
	if(pic >= subf->f->n)
		goto TryPJW;
	fi = &subf->f->info[pic];
	if(fi->width == 0)
		goto TryPJW;
	wid = (fi+1)->x - fi->x;
	if(f->width < wid || f->width == 0 || f->maxdepth < subf->f->bits->depth
	|| (f->cacheimage != nil && f->cacheimage->chan != subf->f->bits->chan)
	|| (f->display != nil && f->cacheimage == nil)){
		/*
		 * Flush, free, reload (easier than reformatting f->b)
		 */
		if(noflush)
			return -1;
		if(f->width < wid)
			f->width = wid;
		if(fontresize(f, f->width, f->ncache,
			subf->f->bits->depth, subf->f->bits->chan) <= 0)
			return -1;
		/* c is still valid as didn't reallocate f->cache */
	}
	c->value[0] = r;
	c->nval = 1;
	c->width = fi->width;
	c->x = h*f->width;
	c->left = fi->left;
	if(f->display == nil)
		return 1;
	b = bufimage(f->display, 37);
	if(b == nil)
		return 0;
	top = fi->top + (f->ascent-subf->f->ascent);
	bottom = fi->bottom + (f->ascent-subf->f->ascent);
	b[0] = 'l';
	BPLONG(b+1, f->cacheimage->id);
	BPLONG(b+5, subf->f->bits->id);
	BPSHORT(b+9, h);
	BPLONG(b+11, c->x);
	BPLONG(b+15, top);
	BPLONG(b+19, c->x+wid);
	BPLONG(b+23, bottom);
	BPLONG(b+27, fi->x);
	BPLONG(b+31, fi->top);
	b[35] = fi->left;
	b[36] = fi->width;
	return 1;
}

/* returns whether resize succeeded && f->cache is unchanged */
static int
fontresize(Font *f, int wid, int ncache, int depth, u32 chan)
{
	Cacheinfo *i;
	int ret;
	Image *new;
	uchar *b;
	Display *d;

	ret = 0;
	if(depth <= 0)
		depth = 1;
	if(wid <= 0)
		wid = 1;

	d = f->display;
	if(d == nil)
		goto Nodisplay;

	if(chan == 0)
		chan = CHAN1(CGrey, depth);
	new = allocimage(d, Rect(0, 0, ncache*wid, f->height), chan, 0, 0);
	if(new == nil){
		fprint(2, "font cache resize failed: %r\n");
		goto Return;
	}
	b = bufimage(d, 1+4+4+1);
	if(b == nil){
		freeimage(new);
		goto Return;
	}
	b[0] = 'i';
	BPLONG(b+1, new->id);
	BPLONG(b+5, ncache);
	b[9] = f->ascent;
	freeimage(f->cacheimage);
	f->cacheimage = new;
    Nodisplay:
	f->width = wid;
	f->maxdepth = depth;
	ret = 1;
	if(f->ncache != ncache){
		i = malloc(ncache*sizeof f->cache[0]);
		if(i != nil){
			ret = 0;
			free(f->cache);
			f->ncache = ncache;
			f->cache = i;
		}
		/* else just wipe the cache clean and things will be ok */
	}
    Return:
	memset(f->cache, 0, f->ncache*sizeof f->cache[0]);
	return ret;
}

/* Preserve Harvey's seven-argument API until libframe is cluster-aware. */
int
cachecharscluster(Font *f, char **s, Rune **r, ushort *c, int n, int *w,
	char **sub, int *cumx, int *clusterlen)
{
	return _cachecharsn(f, s, r, c, n, w, sub, cumx, clusterlen, n);
}

int
cachechars(Font *f, char **s, Rune **r, u16 *c, int n, int *w, char **sub)
{
	return cachecharscluster(f, s, r, c, n, w, sub, nil, nil);
}

void
_freefontshape(Font *f)
{
    Shapeaux *sx;
    Shapecap *c, *next;
    Clusterimg *g, *gnext;
    sx = f->shapeaux;
    if(sx == nil)
        return;
    for(c = sx->cap; c != nil; c = next){
        next = c->next;
        free(c->prefix);
        free(c);
    }
    for(g = sx->cluster; g != nil; g = gnext){
        gnext = g->next;
        freeimage(g->img);
        free(g);
    }
    free(sx->q.subfont);
    free(sx);
    f->shapeaux = nil;
}
