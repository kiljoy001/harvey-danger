#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ttf.h>
#include "impl.h"

typedef struct TTColrState TTColrState;
typedef struct TTColrBitmap TTColrBitmap;

static int colrdebug;

struct TTColrState {
	TTFont *font;
	TTFontU *f;
	TTColr *c;
	TTColrBitmap *bm;
	ulong rgba;

	int xminpx;
	int yminpx;
	int ymaxpx;

	double xx, xy;
	double yx, yy;
	double dx, dy;
};

struct TTColrBitmap {
	uchar *rgba;
	int width, height, stride, chan;
};

/*
 * Read CPAL palette 0.
 *
 * CPAL stores colors as BGRA.  We keep them as RGBA.
 * COLRv1 paletteIndex refers to an entry within the selected
 * palette; for now palette 0 is used.
 */
static int
readcpal(TTFontU *f, TTColr *c)
{
	vlong base, p;
	u16int version, nent, npal, nrec;
	u32int recoff;
	u16int *indices;
	int i, j, ri;

	if(ttfgototable(f, "CPAL") < 0)
		return 0;

	base = Bseek(f->bin, 0, 1);
	c->cpal = base;

	ttfunpack(f, "wwwwl",
		&version, &nent, &npal, &nrec, &recoff);

	if(version != 0 && version != 1){
		werrstr("bad CPAL version %d", version);
		return -1;
	}

	if(nent == 0 || npal == 0 || nrec == 0)
		return 0;

	/*
	 * For now use palette 0.  Its first color record is given
	 * by colorRecordIndices[0].
	 */
	indices = malloc(npal * sizeof(u16int));
	if(indices == nil)
		return -1;

	for(i = 0; i < npal; i++)
		ttfunpack(f, "w", &indices[i]);

	ri = indices[0];

	if(ri < 0 || ri + nent > nrec){
		free(indices);
		werrstr("bad CPAL colorRecordIndices");
		return -1;
	}

	c->palette = malloc(nent * sizeof(TTColor));
	if(c->palette == nil){
		free(indices);
		return -1;
	}

	p = base + recoff + 4 * ri;
	Bseek(f->bin, p, 0);

	for(j = 0; j < nent; j++){
		/*
		 * ColorRecord is BGRA, not RGBA.
		 */
		ttfunpack(f, "bbbb",
			&c->palette[j].b,
			&c->palette[j].g,
			&c->palette[j].r,
			&c->palette[j].a);
	}

	c->npalette = nent;
	free(indices);

	return 1;
}

/*
 * Parse COLR version 1.
 *
 * We only build the BaseGlyphList index and remember LayerList.
 * Paint tables themselves are deliberately read lazily.
 */
int
ttfcolrinit(TTFontU *f)
{
	colrdebug = getenv("COLRDEBUG") != nil;
	vlong colrbase, base;
	u16int version;
	u16int nbase;
	u32int baseoff, layeroff;
	u16int nlayer0;
	u32int baseglyphoff, layerlistoff;
	u32int clipoff, varmapoff, varstoreoff;
	u32int paint;
	u32int i;
	u32int nbaseglyph;
	TTColr *c;

	if(f->colr != nil)
		return 1;

	if(ttfgototable(f, "COLR") < 0)
		return 0;

	colrbase = Bseek(f->bin, 0, 1);

	/*
	 * CPAL is required for COLR.
	 */
	c = malloc(sizeof(*c));
	if(c == nil)
		return -1;
	memset(c, 0, sizeof(*c));

	c->base = colrbase;

	/*
	 * Read COLR header.
	 *
	 * Version 1:
	 *   uint16 version
	 *   uint16 numBaseGlyphRecords
	 *   Offset32 baseGlyphRecordsOffset
	 *   Offset32 layerRecordsOffset
	 *   uint16 numLayerRecords
	 *   Offset32 baseGlyphListOffset
	 *   Offset32 layerListOffset
	 *   Offset32 clipListOffset
	 *   Offset32 varIndexMapOffset
	 *   Offset32 itemVariationStoreOffset
	 */
	Bseek(f->bin, colrbase, 0);
	ttfunpack(f, "wwllwllll",
		&version,
		&nbase,
		&baseoff,
		&layeroff,
		&nlayer0,
		&baseglyphoff,
		&layerlistoff,
		&clipoff,
		&varmapoff,
		&varstoreoff);

	USED(baseoff);
	USED(layeroff);
	USED(nlayer0);
	USED(varmapoff);
	USED(varstoreoff);

	if(version != 1){
		free(c);
		return 0;	/* only COLRv1 here */
	}

	if(baseglyphoff == 0){
		free(c);
		return 0;
	}

	if(readcpal(f, c) < 0){
		free(c);
		return -1;
	}

	if(c->palette == nil){
		free(c);
		return 0;
	}

	/*
	 * BaseGlyphList:
	 *
	 * uint32 numBaseGlyphPaintRecords
	 * BaseGlyphPaintRecord records[]
	 *
	 * BaseGlyphPaintRecord:
	 * uint16 glyphID
	 * Offset32 paintOffset
	 *
	 * paintOffset is relative to BaseGlyphList.
	 */
	base = colrbase + baseglyphoff;
	c->base = base;

	Bseek(f->bin, base, 0);
	ttfunpack(f, "l", &nbaseglyph);

	if(nbaseglyph == 0)
		goto done;

	c->glyph = malloc(f->numGlyphs * sizeof(TTColrGlyph));
	if(c->glyph == nil){
		free(c->palette);
		free(c);
		return -1;
	}
	memset(c->glyph, 0, f->numGlyphs * sizeof(TTColrGlyph));

	for(i = 0; i < nbaseglyph; i++){
		u16int gid;

		ttfunpack(f, "wl", &gid, &paint);
		if(gid >= f->numGlyphs)
			continue;

		c->glyph[gid].paint = paint;
		c->glyph[gid].present = 1;
	}

	c->nglyph = f->numGlyphs;

	/*
	 * LayerList:
	 *
	 * uint32 numLayers
	 * Offset32 paintOffsets[numLayers]
	 *
	 * Offsets are relative to LayerList.
	 */
	if(layerlistoff != 0){
	  c->layer = colrbase + layerlistoff;
	}

	if(clipoff != 0)
		c->clip = colrbase + clipoff;

done:
	f->colr = c;

	if(colrdebug)
		fprint(2,
			"COLR: base=%#llux glyphs=%d layer=%#llux clip=%#llux palette=%d emsize=%d\n",
			c->base, c->nglyph, c->layer, c->clip, c->npalette, f->emsize);

	return 1;
}

static u32int
colroff24(TTFontU *f)
{
	u32int off;

	off = Bgetc(f->bin) << 16;
	off |= Bgetc(f->bin) << 8;
	off |= Bgetc(f->bin);
	return off;
}

static int
colrlayers(TTFontU *f, TTColr *c, u32int first, u32int n, u32int *paint)
{
	u32int nlayer;
	int i;

	if(c->layer == 0){
		if(colrdebug)
			fprint(2, "COLR colrlayers: no LayerList\n");
		return -1;
	}

	Bseek(f->bin, c->layer, 0);
	ttfunpack(f, "l", &nlayer);

	if(first > nlayer || n > nlayer - first){
		if(colrdebug)
			fprint(2, "COLR colrlayers: first=%ud n=%ud nlayer=%ud out of range\n",
				first, n, nlayer);
		return -1;
	}

	Bseek(f->bin, c->layer + 4 + first * 4, 0);

	for(i = 0; i < n; i++){
		ttfunpack(f, "l", &paint[i]);
		paint[i] += c->layer;
	}

	return n;
}

/*
 * Look up gid's clip box in COLR's clipList, if present.
 *
 * ClipList:
 *   uint8    format (=1)
 *   uint32   numClips
 *   Clip records[numClips], sorted by startGlyphID:
 *     uint16   startGlyphID
 *     uint16   endGlyphID
 *     Offset24 clipBoxOffset   (relative to ClipList start)
 *
 * ClipBox format 1:
 *   uint8  format (=1)
 *   int16  xMin, yMin, xMax, yMax   (FUnits)
 * format 2 adds a var-index base for variable fonts; we treat it
 * the same as format 1 and ignore variation.
 *
 * Returns 1 and fills in the pixel bbox if gid has a clip box,
 * 0 if COLR has no clipList or gid isn't covered by one.
 */
static int
colrclip(TTFont *fs, TTColr *c, u16int gid,
	int *xminpx, int *yminpx, int *xmaxpx, int *ymaxpx)
{
	TTFontU *f;
	vlong lo, hi, mid, recs;
	u32int nclip, cliboxoff;
	u16int start, end, uxmin, uymin, uxmax, uymax;
	uchar fmt;
	double scale;

	if(c->clip == 0)
		return 0;

	f = fs->u;

	Bseek(f->bin, c->clip, 0);
	ttfunpack(f, "b", &fmt);
	if(fmt != 1)
		return 0;
	ttfunpack(f, "l", &nclip);

	recs = c->clip + 5;
	lo = 0;
	hi = nclip;

	while(lo < hi){
		mid = (lo + hi) / 2;

		Bseek(f->bin, recs + mid * 7, 0);
		ttfunpack(f, "ww", &start, &end);

		if(gid < start){
			hi = mid;
			continue;
		}
		if(gid > end){
			lo = mid + 1;
			continue;
		}

		cliboxoff = colroff24(f);

		Bseek(f->bin, c->clip + cliboxoff, 0);
		ttfunpack(f, "b", &fmt);
		if(fmt != 1 && fmt != 2)
			return 0;

		ttfunpack(f, "wwww", &uxmin, &uymin, &uxmax, &uymax);

		/*
		 * ClipBox coords are FUnits; scale to 26.6 the same
		 * way PaintTransform's dx/dy are, then round exactly
		 * like colrbounds (xminpx = x>>6, xmaxpx = (x+63)>>6)
		 * so the canvas origin can't drift from individual
		 * layers' own bounds by a rounding-path mismatch.
		 */
		scale = (double)fs->ppem * 64.0 / f->emsize;

		*xminpx = (s32int)((s16int)uxmin * scale) >> 6;
		*yminpx = (s32int)((s16int)uymin * scale) >> 6;
		*xmaxpx = ((s32int)((s16int)uxmax * scale) + 63) >> 6;
		*ymaxpx = ((s32int)((s16int)uymax * scale) + 63) >> 6;

		return 1;
	}

	return 0;
}


static double
colrfixed(TTFontU *f)
{
	u32int x;

	ttfunpack(f, "l", &x);
	return (s32int)x / 65536.0;
}

static void
colrcompose(TTColrState *s,
	double xx, double xy, double yx, double yy,
	double dx, double dy)
{
	double a, b, c, d, e, f;

	a = s->xx * xx + s->xy * yx;
	b = s->xx * xy + s->xy * yy;
	c = s->yx * xx + s->yy * yx;
	d = s->yx * xy + s->yy * yy;
	e = s->xx * dx + s->xy * dy + s->dx;
	f = s->yx * dx + s->yy * dy + s->dy;

	s->xx = a;
	s->xy = b;
	s->yx = c;
	s->yy = d;
	s->dx = e;
	s->dy = f;
}

static int
colrsolid(TTColrState *s, u16int palette, u16int alpha)
{
	TTColor *c;
	ulong a;

	if(palette >= s->c->npalette)
		return -1;

	c = &s->c->palette[palette];

	/*
	 * alpha is F2DOT14. Clamp to [0, 1], then convert
	 * to 8-bit alpha.
	 */
	a = alpha;
	if(a > 0x4000)
		a = 0x4000;
	a = (a * 255 + 0x2000) / 0x4000;

	s->rgba =
		((ulong)c->r << 24) |
		((ulong)c->g << 16) |
		((ulong)c->b << 8) |
		a;

	if(colrdebug)
		fprint(2, "COLR solid palette=%ud rgba=%#8.8lux\n",
			palette, s->rgba);

	return 0;
}

/*
 * ColorLine, flat-color fallback: true gradient interpolation along the
 * line/circle isn't implemented, so this picks a single representative
 * stop -- the one with the largest stopOffset, i.e. the gradient's "far"
 * color -- and renders it as a flat PaintSolid would. Called from
 * PaintLinearGradient/PaintRadialGradient in place of actually walking
 * the gradient; see colrpaint's case 4/6.
 *
 * Per spec, a ColorLine with zero stops means fully transparent -- that's
 * a real, valid result, not an error, so it succeeds with s->rgba left
 * at 0 rather than returning -1. Returning -1 here would make a
 * PaintColrLayers parent abort every remaining sibling layer over one
 * legitimately-empty gradient, which is worse than just rendering
 * nothing for this one paint.
 *
 * ColorLine:
 *   uint8      extend
 *   uint16     numStops
 *   ColorStop  stops[numStops]
 * ColorStop:
 *   F2DOT14    stopOffset
 *   uint16     paletteIndex
 *   F2DOT14    alpha
 */
static int
colrcolorline(TTColrState *s, vlong off)
{
	TTFontU *f;
	uchar extend;
	u16int nstop, i;
	u16int ustop, palette, ualpha;
	s16int stopoff, bestoff;
	u16int bestpalette, bestalpha;
	int have;

	f = s->f;

	Bseek(f->bin, off, 0);
	ttfunpack(f, "b", &extend);
	ttfunpack(f, "w", &nstop);
	USED(extend);

	have = 0;
	bestoff = 0;
	bestpalette = 0;
	bestalpha = 0x4000;

	for(i = 0; i < nstop; i++){
		ttfunpack(f, "w", &ustop);
		ttfunpack(f, "w", &palette);
		ttfunpack(f, "w", &ualpha);

		stopoff = (s16int)ustop;
		if(!have || stopoff > bestoff){
			have = 1;
			bestoff = stopoff;
			bestpalette = palette;
			bestalpha = ualpha;
		}
	}

	if(colrdebug)
		fprint(2, "COLR colorline %#llux: nstop=%ud -> palette=%ud alpha=%#ux (flat fallback)\n",
			off, nstop, bestpalette, bestalpha);

	if(!have){
		s->rgba = 0;	/* no stops: transparent, per spec -- not an error */
		return 0;
	}

	return colrsolid(s, bestpalette, bestalpha);
}

static int
colrbitmap(TTBitmap *b, int w, int h)
{
	b->width = w;
	b->height = h;
	b->stride = w * 4;
	b->chan = 1;
	b->bit = mallocz(h * b->stride, 1);
	return b->bit != nil;
}

static void
colrstateinit(TTColrState *s)
{
	s->xx = 1;
	s->xy = 0;
	s->yx = 0;
	s->yy = 1;
	s->dx = 0;
	s->dy = 0;
}

static void
colrblend(TTColrBitmap *b, int x, int y, uchar r, uchar g, uchar bl, uchar a)
{
	uchar *p;
	ulong sa, da, oa;

	if(x < 0 || x >= b->width || y < 0 || y >= b->height)
		return;

	p = b->rgba + y * b->stride + x * 4;

	sa = a;
	da = p[3];

	oa = sa + (da * (255 - sa) + 127) / 255;
	if(oa == 0)
		return;

	p[0] = (r * sa + p[0] * da * (255 - sa) / 255) / oa;
	p[1] = (g * sa + p[1] * da * (255 - sa) / 255) / oa;
	p[2] = (bl * sa + p[2] * da * (255 - sa) / 255) / oa;
	p[3] = oa;
}

static void
colrblit(TTColrBitmap *dst, TTGlyph *g, int x, int y, ulong rgba)
{
	uchar *q;
	int i, j;
	uchar cr, cg, cb, ca;

	if(g->chan != 0 || dst->chan != 1)
		return;

	cr = rgba >> 24;
	cg = rgba >> 16;
	cb = rgba >> 8;
	ca = rgba & 0xff;

	for(j = 0; j < g->height; j++){
		if(y + j < 0 || y + j >= dst->height)
			continue;

		q = g->bit + j * g->stride;

		for(i = 0; i < g->width; i++){
			if(x + i < 0 || x + i >= dst->width)
				continue;

			if((q[i >> 3] & (1 << (7 - (i & 7)))) == 0)
				continue;

			colrblend(dst, x + i, y + j, cr, cg, cb, ca);
		}
	}
}

static void
colrbounds(TTGlyph *g)
{
	int i;
	s32int x, y;

	g->xmin = g->xmax = g->pt[0].x;
	g->ymin = g->ymax = g->pt[0].y;

	for(i = 1; i < g->npt - 2; i++){
		x = g->pt[i].x;
		y = g->pt[i].y;

		if(x < g->xmin)
			g->xmin = x;
		if(x > g->xmax)
			g->xmax = x;
		if(y < g->ymin)
			g->ymin = y;
		if(y > g->ymax)
			g->ymax = y;
	}

	g->xminpx = g->xmin >> 6;
	g->xmaxpx = (g->xmax + 63) >> 6;
	g->yminpx = g->ymin >> 6;
	g->ymaxpx = (g->ymax + 63) >> 6;
}

static void
colrscale(TTGlyph *g)
{
	int i;
	TTPoint *p;
	TTFont *f;

	f = g->font;
	for(i = 0; i < g->npt; i++){
		p = &g->pt[i];
		p->x = ttfrounddiv(p->x * f->ppem * 64, f->u->emsize);
		p->y = ttfrounddiv(p->y * f->ppem * 64, f->u->emsize);
	}
}

static void
colrtransform(TTGlyph *g, TTColrState *s)
{
	int i;
	double x, y;

	for(i = 0; i < g->npt; i++){
		x = g->pt[i].x;
		y = g->pt[i].y;

		g->pt[i].x = s->xx * x + s->xy * y + s->dx;
		g->pt[i].y = s->yx * x + s->yy * y + s->dy;
	}
}

static int
colrglyph(TTColrState *s, u16int gid)
{
	TTGlyph *src, g;
	TTPoint *pt;
	int x, y;

	src = ttfgetglyph(s->font, gid, 0);
	if(src == nil){
		if(colrdebug)
			fprint(2, "COLR colrglyph: ttfgetglyph gid=%ud failed: %r\n", gid);
		return -1;
	}

	memset(&g, 0, sizeof g);
	g = *src;

	pt = malloc(src->npt * sizeof(TTPoint));
	if(pt == nil){
		if(colrdebug)
			fprint(2, "COLR colrglyph: malloc failed\n");
		ttfputglyph(src);
		return -1;
	}

	memmove(pt, src->pt, src->npt * sizeof(TTPoint));
	g.pt = pt;
	g.bit = nil;
	ttfputglyph(src);

	/* ttfgetglyph(..., 0) returns outline coordinates in FUnits. */
	colrscale(&g);
	colrtransform(&g, s);
	colrbounds(&g);
	ttfscan(&g);

	x = g.xminpx - s->xminpx;
	/* TTGlyph bitmaps are stored top-down (row 0 is ymax). */
	y = s->ymaxpx - g.ymaxpx;

	if(colrdebug){
		int bi, bj, nset;

		nset = 0;
		if(g.bit != nil)
			for(bj = 0; bj < g.height; bj++)
				for(bi = 0; bi < g.width; bi++)
					if(g.bit[bj*g.stride + (bi>>3)] & (1 << (7 - (bi&7))))
						nset++;

		fprint(2, "COLR colrglyph gid=%ud: local %dx%d (%d/%d mask bits set)"
			" at canvas %d,%d xform=[%g %g %g %g +%g +%g]\n",
			gid, g.xmaxpx - g.xminpx, g.ymaxpx - g.yminpx,
			nset, g.width * g.height, x, y,
			s->xx, s->xy, s->yx, s->yy, s->dx, s->dy);
	}

	colrblit(s->bm, &g, x, y, s->rgba);

	free(g.bit);
	free(pt);

	return 0;
}

static TTColrState
colrchild(TTColrState *s)
{
	TTColrState c;

	c = *s;
	return c;
}

static int
colrpaint(TTColrState *s, vlong off)
{
	uchar fmt, nl;
	u16int palette, glyph;
	u32int first, n, child, paint[256];
	int i;
	vlong childoff;
	TTFontU *f;
	TTColr *c;

	f = s->f;
	c = s->c;
	
	Bseek(f->bin, off, 0);
	ttfunpack(f, "b", &fmt);

	if(colrdebug)
		fprint(2, "COLR paint %#llux format=%d\n", off, fmt);

	switch(fmt){
	case 1:		/* PaintColrLayers */
		ttfunpack(f, "b", &nl);
		ttfunpack(f, "l", &first);

		n = nl;
		if(n > nelem(paint)){
			if(colrdebug)
				fprint(2, "COLR %#llux: PaintColrLayers n=%ud too big\n",
					off, n);
			return -1;
		}

		if(colrlayers(f, c, first, n, paint) < 0){
			if(colrdebug)
				fprint(2, "COLR %#llux: colrlayers failed first=%ud n=%ud\n",
					off, first, n);
			return -1;
		}

		for(i = 0; i < n; i++){
			if(colrpaint(s, paint[i]) < 0){
				if(colrdebug)
					fprint(2, "COLR %#llux: layer %d/%ud at %#llux failed, aborting remaining layers\n",
						off, i, n, (vlong)paint[i]);
				return -1;
			}
		}
		break;

	case 2:		/* PaintSolid */
		/*
		 * F2DOT14 alpha.  Keep the value for now; rendering
		 * will convert it to the bitmap alpha representation.
		 */
		{
			u16int alpha;

			ttfunpack(f, "w", &palette);
			ttfunpack(f, "w", &alpha);

			if(colrdebug)
				fprint(2, "COLR solid palette=%ud alpha=%#ux\n",
					palette, alpha);

			return colrsolid(s, palette, alpha);
		}
		break;

	case 4:		/* PaintLinearGradient */
	case 6:		/* PaintRadialGradient */
		/*
		 * True gradient interpolation isn't implemented -- both
		 * formats start with the same Offset24 colorLineOffset
		 * (relative to this Paint table) before their geometry
		 * fields (start/end points, or center/radius pairs), which
		 * this fallback doesn't need at all: it just resolves the
		 * ColorLine to one flat color instead of walking the ramp.
		 */
		child = colroff24(f);

		if(colrcolorline(s, off + child) < 0){
			if(colrdebug)
				fprint(2, "COLR %#llux: format=%d colorline at %#llux failed\n",
					off, fmt, off + child);
			return -1;
		}
		break;

	case 10:	/* PaintGlyph */
		child = colroff24(f);
		ttfunpack(f, "w", &glyph);

		/*
		 * paintOffset is relative to this PaintGlyph table.
		 */
		childoff = off + child;

		if(colrdebug)
			fprint(2, "COLR glyph=%ud child=%#llux\n",
				glyph, childoff);

		if(colrpaint(s, childoff) < 0){
			if(colrdebug)
				fprint(2, "COLR %#llux: PaintGlyph fill at %#llux failed\n",
					off, childoff);
			return -1;
		}
		if(colrglyph(s, glyph) < 0){
			if(colrdebug)
				fprint(2, "COLR %#llux: PaintGlyph rasterize gid=%ud failed\n",
					off, glyph);
			return -1;
		}
		break;

	case 12:	/* PaintTransform */
		child = colroff24(f);
		{
			u32int transform;
			double xx, xy, yx, yy, dx, dy;
			double scale;
			TTColrState save;

			transform = colroff24(f);

			Bseek(f->bin, off + transform, 0);

			xx = colrfixed(f);
			xy = colrfixed(f);
			yx = colrfixed(f);
			yy = colrfixed(f);
			dx = colrfixed(f);
			dy = colrfixed(f);

			/*
			 * dx/dy are in FUnits, but the points they're
			 * applied to are already scaled to 26.6 pixels;
			 * xx/xy/yx/yy are unitless ratios and need no
			 * scaling.
			 */
			scale = (double)s->font->ppem * 64.0 / s->f->emsize;
			dx *= scale;
			dy *= scale;

			save = *s;
			colrcompose(s, xx, xy, yx, yy, dx, dy);

			if(colrpaint(s, off + child) < 0){
				if(colrdebug)
					fprint(2, "COLR %#llux: PaintTransform child at %#llux failed\n",
						off, off + child);
				*s = save;
				return -1;
			}

			*s = save;
		}
		break;

	case 16:	/* PaintScale */
		child = colroff24(f);
		{
			u16int sx, sy;
			TTColrState save;

			ttfunpack(f, "ww", &sx, &sy);

			save = *s;

			colrcompose(s,
				(s16int)sx / 16384.0, 0,
				0, (s16int)sy / 16384.0,
				0, 0);

			if(colrpaint(s, off + child) < 0){
				if(colrdebug)
					fprint(2, "COLR %#llux: PaintScale child at %#llux failed\n",
						off, off + child);
				*s = save;
				return -1;
			}

			*s = save;
		}
		break;

	case 18:	/* PaintScaleAroundCenter */
		child = colroff24(f);
		{
			u16int sx, sy, ucx, ucy;
			double scale, cx, cy, fsx, fsy;
			TTColrState save;

			ttfunpack(f, "wwww", &sx, &sy, &ucx, &ucy);

			/*
			 * centerX/centerY are FUnits, same as
			 * PaintTransform's dx/dy - scale to the 26.6
			 * pixel space the points are already in.
			 */
			scale = (double)s->font->ppem * 64.0 / s->f->emsize;
			cx = (s16int)ucx * scale;
			cy = (s16int)ucy * scale;

			fsx = (s16int)sx / 16384.0;
			fsy = (s16int)sy / 16384.0;

			save = *s;

			colrcompose(s,
				fsx, 0,
				0, fsy,
				cx * (1.0 - fsx),
				cy * (1.0 - fsy));

			if(colrpaint(s, off + child) < 0){
				if(colrdebug)
					fprint(2, "COLR %#llux: PaintScaleAroundCenter child at %#llux failed\n",
						off, off + child);
				*s = save;
				return -1;
			}

			*s = save;
		}
		break;

	case 32:	/* PaintComposite */
		{
			u32int srcoff, backoff;
			uchar mode;

			srcoff = colroff24(f);
			ttfunpack(f, "b", &mode);
			backoff = colroff24(f);

			/*
			 * compositeMode (Clear/Src/Dst/SrcOver/...) is
			 * ignored for now; children are just rendered
			 * backdrop-then-source with ordinary alpha-over,
			 * which is what colrblend already does.
			 */
			USED(mode);

			if(colrpaint(s, off + backoff) < 0){
				if(colrdebug)
					fprint(2, "COLR %#llux: PaintComposite backdrop at %#llux failed\n",
						off, off + backoff);
				return -1;
			}
			if(colrpaint(s, off + srcoff) < 0){
				if(colrdebug)
					fprint(2, "COLR %#llux: PaintComposite source at %#llux failed\n",
						off, off + srcoff);
				return -1;
			}
		}
		break;

	default:
		if(colrdebug)
			fprint(2, "COLR unsupported format=%d at %#llux\n",
				fmt, off);
		break;
	}

	USED(palette);
	USED(glyph);
	return 0;
}

/*
 * Render glyph gid's COLR paint graph, if it has one, into bm.
 *
 * Returns 1 and fills in bm if gid is a color glyph, 0 if gid has
 * no COLR entry (caller should fall back to normal glyf rendering),
 * -1 on error.
 *
 * The canvas is sized from COLR's clipList when present; otherwise
 * it falls back to the base glyph's own outline bounds, which will
 * clip paints that extend past that outline (gradients, transforms,
 * extra layers).
 */
int
ttfcolrglyph(TTFont *fs, u16int gid, TTBitmap *bm)
{
	TTFontU *f;
	TTColr *c;
	TTColrGlyph *cg;
	TTGlyph *base;
	TTColrBitmap cbm;
	TTColrState s;
	vlong paintoff;
	int xminpx, yminpx, xmaxpx, ymaxpx;
	int w, h;

	f = fs->u;

	if(f->colr == nil || gid >= f->numGlyphs)
		return 0;

	c = f->colr;
	cg = &c->glyph[gid];
	if(!cg->present)
		return 0;

	if(colrclip(fs, c, gid, &xminpx, &yminpx, &xmaxpx, &ymaxpx)){
		w = xmaxpx - xminpx;
		h = ymaxpx - yminpx;
		if(colrdebug)
			fprint(2, "COLR gid=%ud: bbox from clipList: %d,%d - %d,%d\n",
				gid, xminpx, yminpx, xmaxpx, ymaxpx);
	}else{
		base = ttfgetglyph(fs, gid, 0);
		if(base == nil)
			return -1;

		colrscale(base);
		colrbounds(base);

		xminpx = base->xminpx;
		yminpx = base->yminpx;
		w = base->xmaxpx - base->xminpx;
		h = base->ymaxpx - base->yminpx;

		if(colrdebug)
			fprint(2, "COLR gid=%ud: no clip box, falling back to base glyf bbox: %d,%d - %d,%d (npt=%d)\n",
				gid, base->xminpx, base->yminpx, base->xmaxpx, base->ymaxpx, base->npt);

		ttfputglyph(base);
	}

	if(w <= 0 || h <= 0)
		return -1;

	if(colrbitmap(bm, w, h) == 0)
		return -1;

	memset(&s, 0, sizeof s);
	s.font = fs;
	s.f = f;
	s.c = c;
	colrstateinit(&s);

	cbm.rgba = bm->bit;
	cbm.width = bm->width;
	cbm.height = bm->height;
	cbm.stride = bm->stride;
	cbm.chan = bm->chan;
	s.bm = &cbm;

	s.xminpx = xminpx;
	s.yminpx = yminpx;
	s.ymaxpx = ymaxpx;

	paintoff = c->base + cg->paint;

	if(colrdebug)
		fprint(2, "COLR render gid=%ud paint=%#llux %dx%d origin=%d,%d ppem=%d\n",
			gid, paintoff, w, h, xminpx, yminpx, fs->ppem);

	if(colrpaint(&s, paintoff) < 0){
		free(bm->bit);
		bm->bit = nil;
		return -1;
	}

	return 1;
}
