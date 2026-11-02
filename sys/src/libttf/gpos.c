/*
 * GPOS mark-attachment support: MarkToBase (type 4), MarkToLigature
 * (type 5), MarkToMark (type 6). Format 2 GSUB/GPOS header (with
 * featureVariationsOffset) is not handled, matching the assumption
 * already made in ttfgsubinit. Anchor format 2 (contour point) and
 * format 3 (device tables) are read as flat x,y and their hinting
 * data is ignored, same tradeoff colr.c makes for gradients.
 */

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ttf.h>
#include "impl.h"

static int debug = -1;

/*
 * Anchor table: format 1 is x,y only. Format 2 adds a hinting
 * contour point index; format 3 adds device-table offsets for x
 * and y. All three start with the same x,y pair, which is all we
 * use -- ppem-independent device correction is a quality nicety,
 * not correctness, same call made for COLR gradients.
 */
static void
readanchor(TTFontU *f, vlong base, u16int off, int *x, int *y)
{
	u16int fmt;
	u32int ux, uy;

	if(off == 0){
		*x = 0;
		*y = 0;
		return;
	}
	Bseek(f->bin, base + off, 0);
	ttfunpack(f, "w", &fmt);
	ttfunpack(f, "SS", &ux, &uy);
	USED(fmt);
	*x = (int)ux;
	*y = (int)uy;
}

/*
 * MarkArray: markCount u16, then markCount x {markClass u16;
 * markAnchorOffset u16} MarkRecords, offsets relative to the start
 * of this MarkArray table. Shared by MarkBasePos, MarkToLigature's
 * mark side, and MarkToMark's mark1 side.
 *
 * Reads all (class, offset) pairs first, then resolves anchors --
 * same read-offsets-then-resolve shape ttfgsubinit uses for
 * ligsetoffs, since resolving an anchor mid-loop would Bseek away
 * from the MarkRecord array we're still walking.
 */
static int
readmarkarray(TTFontU *f, vlong arraybase, u16int **classp, int **xp, int **yp)
{
	u16int n, *cls, *offs;
	int *xs, *ys;
	int i;

	Bseek(f->bin, arraybase, 0);
	ttfunpack(f, "w", &n);
	cls = malloc(n * sizeof(u16int));
	offs = malloc(n * sizeof(u16int));
	for(i = 0; i < n; i++)
		ttfunpack(f, "ww", &cls[i], &offs[i]);
	xs = malloc(n * sizeof(int));
	ys = malloc(n * sizeof(int));
	for(i = 0; i < n; i++)
		readanchor(f, arraybase, offs[i], &xs[i], &ys[i]);
	free(offs);
	*classp = cls;
	*xp = xs;
	*yp = ys;
	return n;
}

/*
 * BaseArray / Mark2Array: baseCount u16, then baseCount records of
 * nclass anchor offsets (u16 each), relative to this array table's
 * own start. Identical shape for MarkBasePos's BaseArray and
 * MarkToMark's Mark2Array -- only the field names differ in the
 * spec. Returns a flat nbase*nclass offset table; 0 means "no
 * anchor for this class on this base", per spec.
 *
 * nbase is clamped to the record count actually present, same
 * malformed-font tolerance ttfgsubinit applies to ncov vs nligset.
 */
static u16int *
readbasearray(TTFontU *f, vlong arraybase, int nbase, int nclass)
{
	u16int n, *offs;
	int i, j;

	Bseek(f->bin, arraybase, 0);
	ttfunpack(f, "w", &n);
	if(n < nbase)
		nbase = n;
	offs = malloc(nbase * nclass * sizeof(u16int));
	for(i = 0; i < nbase; i++)
		for(j = 0; j < nclass; j++)
			ttfunpack(f, "w", &offs[i * nclass + j]);
	return offs;
}

static TTGPOSMark *
growmarks(TTGPOSMark *mk, int *cap, int n)
{
	if(n >= *cap){
		*cap *= 2;
		mk = realloc(mk, *cap * sizeof(TTGPOSMark));
	}
	return mk;
}

/* MarkBasePos (4) and MarkToMark (6): identical subtable shape,
 * only the spec's field names ("Base" vs "Mark2") differ. */
static TTGPOSMark *
parsemarktobase(TTFontU *f, vlong subbase, int type, TTGPOSMark *mk, int *cap, int *np, int debug, int qmark, int qbase)
{
	u16int markcovoff, basecovoff, nmarkclass, markarrayoff, basearrayoff;
	u16int nmark, *markglyphs, nbase, *baseglyphs;
	u16int *mclass, *baseoffs;
	int *mx, *my;
	int mi, bi, off;
	vlong basearraybase;

	ttfunpack(f, "wwww", &markcovoff, &basecovoff, &nmarkclass, &markarrayoff);
	ttfunpack(f, "w", &basearrayoff);

	Bseek(f->bin, subbase + markcovoff, 0);
	nmark = readcoverage(f, &markglyphs);
	Bseek(f->bin, subbase + basecovoff, 0);
	nbase = readcoverage(f, &baseglyphs);

	if(debug){
		fprint(2, "  subtable: type=%d nmarkclass=%d nmark=%d nbase=%d\n",
			type, nmarkclass, nmark, nbase);
		fprint(2, "    markcov:");
		for(mi = 0; mi < nmark; mi++)
			fprint(2, " %d", markglyphs[mi]);
		fprint(2, "\n    basecov:");
		for(bi = 0; bi < nbase; bi++)
			fprint(2, " %d", baseglyphs[bi]);
		fprint(2, "\n");
	}

	nmark = readmarkarray(f, subbase + markarrayoff, &mclass, &mx, &my);
	basearraybase = subbase + basearrayoff;
	baseoffs = readbasearray(f, basearraybase, nbase, nmarkclass);

	for(mi = 0; mi < nmark; mi++){
		for(bi = 0; bi < nbase; bi++){
			off = baseoffs[bi * nmarkclass + mclass[mi]];
			if(qmark >= 0 && markglyphs[mi] == qmark && baseglyphs[bi] == qbase)
				fprint(2, "query: mark=%d base=%d found in type=%d subtable, class=%d, "
					"baseoff=%d (%s), markxy=(%d,%d)\n",
					qmark, qbase, type, mclass[mi], off,
					off == 0 ? "ZERO -- covered but no anchor for this class" : "nonzero",
					mx[mi], my[mi]);
			if(off == 0)
				continue;
			mk = growmarks(mk, cap, *np);
			mk[*np].mark = markglyphs[mi];
			mk[*np].markx = mx[mi];
			mk[*np].marky = my[mi];
			mk[*np].base = baseglyphs[bi];
			mk[*np].type = type;
			mk[*np].component = 0;
			readanchor(f, basearraybase, off, &mk[*np].basex, &mk[*np].basey);
			(*np)++;
		}
	}

	free(markglyphs);
	free(baseglyphs);
	free(mclass);
	free(mx);
	free(my);
	free(baseoffs);
	return mk;
}

/* MarkToLigature (5): the ligature side has one more level than
 * MarkToBase -- each ligature glyph has componentCount anchor sets
 * (one per component position, e.g. the 2nd consonant of a
 * conjunct), each with nclass anchor offsets, relative to that
 * ligature's own LigatureAttach table, NOT the LigatureArray. */
static TTGPOSMark *
parsemarktolig(TTFontU *f, vlong subbase, TTGPOSMark *mk, int *cap, int *np, int debug, int qmark, int qbase)
{
	u16int markcovoff, ligcovoff, nmarkclass, markarrayoff, ligarrayoff;
	u16int nmark, *markglyphs, nlig, *ligglyphs;
	u16int *mclass;
	int *mx, *my;
	u16int nligrec, *ligoffs;
	u16int ncomp, off;
	vlong ligarraybase, attachbase;
	int mi, li, ci;

	USED(qmark); USED(qbase);	/* 41/88 case is a type-4 lookup; add query support here if needed later */

	ttfunpack(f, "wwww", &markcovoff, &ligcovoff, &nmarkclass, &markarrayoff);
	ttfunpack(f, "w", &ligarrayoff);

	Bseek(f->bin, subbase + markcovoff, 0);
	nmark = readcoverage(f, &markglyphs);
	Bseek(f->bin, subbase + ligcovoff, 0);
	nlig = readcoverage(f, &ligglyphs);

	if(debug){
		fprint(2, "  subtable: type=5 nmarkclass=%d nmark=%d nlig=%d\n",
			nmarkclass, nmark, nlig);
		fprint(2, "    markcov:");
		for(mi = 0; mi < nmark; mi++)
			fprint(2, " %d", markglyphs[mi]);
		fprint(2, "\n    ligcov:");
		for(li = 0; li < nlig; li++)
			fprint(2, " %d", ligglyphs[li]);
		fprint(2, "\n");
	}

	nmark = readmarkarray(f, subbase + markarrayoff, &mclass, &mx, &my);

	ligarraybase = subbase + ligarrayoff;
	Bseek(f->bin, ligarraybase, 0);
	ttfunpack(f, "w", &nligrec);
	if(nligrec < nlig)
		nlig = nligrec;
	ligoffs = malloc(nlig * sizeof(u16int));
	for(li = 0; li < nlig; li++)
		ttfunpack(f, "w", &ligoffs[li]);

	for(li = 0; li < nlig; li++){
		attachbase = ligarraybase + ligoffs[li];
		Bseek(f->bin, attachbase, 0);
		ttfunpack(f, "w", &ncomp);
		for(ci = 0; ci < ncomp; ci++){
			for(mi = 0; mi < nmark; mi++){
				Bseek(f->bin, attachbase + 2 + ci * nmarkclass * 2 + mclass[mi] * 2, 0);
				ttfunpack(f, "w", &off);
				if(off == 0)
					continue;
				mk = growmarks(mk, cap, *np);
				mk[*np].mark = markglyphs[mi];
				mk[*np].markx = mx[mi];
				mk[*np].marky = my[mi];
				mk[*np].base = ligglyphs[li];
				mk[*np].type = 5;
				mk[*np].component = ci;
				readanchor(f, attachbase, off, &mk[*np].basex, &mk[*np].basey);
				(*np)++;
			}
		}
	}

	free(markglyphs);
	free(ligglyphs);
	free(mclass);
	free(mx);
	free(my);
	free(ligoffs);
	return mk;
}

static int
inset(u16int *set, int n, u16int v)
{
	int i;

	for(i = 0; i < n; i++)
		if(set[i] == v)
			return 1;
	return 0;
}

static TTGPOSMark *
processlookup(TTFontU *f, vlong lookupbase, u16int li, TTGPOSMark *mk,
	int *cap, int *np, int debug, int qmark, int qbase)
{
	vlong lbase, subbase;
	u16int type, flag, nsub, *suboffs, posfmt, markfilterjunk, si;

	lbase = lookupbase;
	Bseek(f->bin, lbase, 0);
	ttfunpack(f, "www", &type, &flag, &nsub);
	if(debug)
		fprint(2, "lookup %d: type=%d flag=%#x nsub=%d\n", li, type, flag, nsub);
	if(type != 4 && type != 5 && type != 6)
		return mk;

	suboffs = malloc(nsub * sizeof(u16int));
	for(si = 0; si < nsub; si++)
		ttfunpack(f, "w", &suboffs[si]);
	if(flag & 0x0010)
		ttfunpack(f, "w", &markfilterjunk);

	for(si = 0; si < nsub; si++){
		subbase = lbase + suboffs[si];
		Bseek(f->bin, subbase, 0);
		ttfunpack(f, "w", &posfmt);
		if(posfmt != 1){
			if(debug)
				fprint(2, "  subtable %d: unhandled format %d\n", si, posfmt);
			continue;
		}
		if(type == 5)
			mk = parsemarktolig(f, subbase, mk, cap, np, debug, qmark, qbase);
		else
			mk = parsemarktobase(f, subbase, type, mk, cap, np, debug, qmark, qbase);
	}
	free(suboffs);
	return mk;
}

int
ttfgposinit(TTFontU *f)
{
	vlong gposbase, lookupbase;
	u16int major, minor, scriptoff, featoff, lookupoff, nlookup, *lookupoffs;
	u16int *abvmlut, *blwmlut, *marklut, *mkmklut, li;
	int nabvm, nblwm, nmark, nmkmk, cap, n, qmark, qbase;
	char *abvmtag[] = {"abvm"};
	char *blwmtag[] = {"blwm"};
	char *marktag[] = {"mark"};
	char *mkmktag[] = {"mkmk"};
	char *e;
	int *done;
	TTGPOSMark *mk;

	if(f->gposmark != nil || f->ngposmark != 0)
		return 1;	/* already parsed via a shared TTFontU */
	if(debug < 0)
		debug = getenv("GPOSDEBUG") != nil;
	qmark = qbase = -1;
	if((e = getenv("GPOSQMARK")) != nil)
		qmark = atoi(e);
	if((e = getenv("GPOSQBASE")) != nil)
		qbase = atoi(e);
	if(ttfgototable(f, "GPOS") < 0){
		f->gposmark = nil;
		f->ngposmark = 0;
		if(debug)
			fprint(2, "gpos: no GPOS table\n");
		return 0;
	}
	gposbase = Bseek(f->bin, 0, 1);
	ttfunpack(f, "wwwww", &major, &minor, &scriptoff, &featoff, &lookupoff);
	USED(major); USED(minor);
	lookupbase = gposbase + lookupoff;
	Bseek(f->bin, lookupbase, 0);
	ttfunpack(f, "w", &nlookup);
	lookupoffs = malloc(nlookup * sizeof(u16int));
	for(li = 0; li < nlookup; li++)
		ttfunpack(f, "w", &lookupoffs[li]);

	abvmlut = ttffeaturelookups(f, gposbase, scriptoff, featoff, abvmtag, 1, &nabvm);
	blwmlut = ttffeaturelookups(f, gposbase, scriptoff, featoff, blwmtag, 1, &nblwm);
	marklut = ttffeaturelookups(f, gposbase, scriptoff, featoff, marktag, 1, &nmark);
	mkmklut = ttffeaturelookups(f, gposbase, scriptoff, featoff, mkmktag, 1, &nmkmk);

	if(debug)
		fprint(2, "gpos: abvm=%d blwm=%d mark=%d mkmk=%d lookups by feature\n",
			nabvm, nblwm, nmark, nmkmk);

	cap = 64;
	n = 0;
	mk = malloc(cap * sizeof(TTGPOSMark));
	done = mallocz(nlookup * sizeof(int), 1);

	/* abvm first: an above-base mark (reph included) whose mark/base
	 * pair also happens to appear in a blwm/mkmk subtable must resolve
	 * to its abvm anchor, since ttfmarkpos returns the first match. */
	for(li = 0; li < nlookup; li++){
		if(!inset(abvmlut, nabvm, li) || done[li])
			continue;
		mk = processlookup(f, lookupbase + lookupoffs[li], li, mk, &cap, &n, debug, qmark, qbase);
		done[li] = 1;
	}
	for(li = 0; li < nlookup; li++){
		if(!inset(blwmlut, nblwm, li) || done[li])
			continue;
		mk = processlookup(f, lookupbase + lookupoffs[li], li, mk, &cap, &n, debug, qmark, qbase);
		done[li] = 1;
	}
	for(li = 0; li < nlookup; li++){
		if(!inset(marklut, nmark, li) || done[li])
			continue;
		mk = processlookup(f, lookupbase + lookupoffs[li], li, mk, &cap, &n, debug, qmark, qbase);
		done[li] = 1;
	}
	for(li = 0; li < nlookup; li++){
		if(!inset(mkmklut, nmkmk, li) || done[li])
			continue;
		mk = processlookup(f, lookupbase + lookupoffs[li], li, mk, &cap, &n, debug, qmark, qbase);
		done[li] = 1;
	}
	/* fallback: no abvm/blwm/mkmk found at all (ScriptList/FeatureList
	 * missing or unparseable) -- revert to old behavior rather than
	 * silently producing zero mark attachments. */
	if(nabvm == 0 && nblwm == 0 && nmark == 0 && nmkmk == 0){
		for(li = 0; li < nlookup; li++)
			mk = processlookup(f, lookupbase + lookupoffs[li], li, mk, &cap, &n, debug, qmark, qbase);
	}

	free(done);
	free(abvmlut);
	free(blwmlut);
	free(marklut);
	free(mkmklut);
	free(lookupoffs);

	f->gposmark = mk;
	f->ngposmark = n;
	if(debug)
		fprint(2, "gpos: %d mark-attachment entries total\n", n);
	return 1;
}

/*
 * Look up the (dx,dy) a mark glyph needs, in font design units,
 * relative to a specific base/ligature-component/mark2 glyph it's
 * being attached to. component is the 0-based ligature component
 * index for type-5 attachment; pass 0 for types 4 and 6.
 *
 * Linear scan, not sorted+bsearch -- ttfgsubinit's qsort() on
 * liga[] blew the interpreter's call stack on this same font
 * family because of heavy duplicate keys (see the ligtest stack
 * overflow earlier in this conversation); mark/base glyph ids will
 * have the same duplicate-heavy shape (one mark glyph attaches to
 * many different bases), so this deliberately doesn't sort.
 * ngposmark is typically small (tens to low hundreds of entries)
 * so the linear scan cost is not a real concern.
 *
 * dx/dy is (baseAnchor - markAnchor): the offset to apply to the
 * mark's own placement so its anchor point lands on the base's
 * anchor point. Caller still owns scaling font design units to
 * ppem/26.6 and combining with the base glyph's own pen position --
 * this only resolves the GPOS-level offset.
 */
int
ttfmarkpos(TTFontU *f, u16int mark, u16int base, int component, int *dx, int *dy)
{
	TTGPOSMark *m;
	int i;

	if(debug)
		fprint(2, "ttfmarkpos: mark=%d base=%d component=%d, ngposmark=%d, gposmark=%s\n",
			mark, base, component, f->ngposmark, f->gposmark == nil ? "nil" : "set");
	for(i = 0; i < f->ngposmark; i++){
		m = &f->gposmark[i];
		if(m->mark != mark || m->base != base)
			continue;
		if(debug)
		  fprint(2, "ttfmarkpos: FOUND i=%d type=%d component=%d "
				 "markxy=(%d,%d) basexy=(%d,%d)\n",
				 i, m->type, m->component,
				 m->markx, m->marky, m->basex, m->basey);
		if(m->type == 5 && m->component != component)
			continue;
		*dx = m->basex - m->markx;
		*dy = m->basey - m->marky;
		return 1;
	}
	return 0;
}
