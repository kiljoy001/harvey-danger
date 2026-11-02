#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ttf.h>
#include "impl.h"

static int debug = -1;

static int
readclassdef(TTFontU *f, vlong base, u16int off, TTGDEFClass *c)
{
	u16int fmt, start, count, i;
	u16int *classes;
	u16int nrange;
	struct {
		u16int start;
		u16int end;
		u16int class;
	} *range;

	memset(c, 0, sizeof *c);
	if(off == 0)
		return 0;

	Bseek(f->bin, base + off, 0);
	ttfunpack(f, "w", &fmt);

	if(fmt == 1){
		ttfunpack(f, "ww", &start, &count);
		classes = malloc(count * sizeof(u16int));
		for(i = 0; i < count; i++)
			ttfunpack(f, "w", &classes[i]);
		c->format = 1;
		c->start = start;
		c->count = count;
		c->classes = classes;
		return 1;
	}

	if(fmt == 2){
		ttfunpack(f, "w", &nrange);
		range = malloc(nrange * sizeof(*range));
		for(i = 0; i < nrange; i++)
		ttfunpack(f, "www",
			&range[i].start, &range[i].end, &range[i].class);
		c->format = 2;
		c->nrange = nrange;
		c->range = range;
		return 1;
	}

	if(debug)
		fprint(2, "gdef: unhandled ClassDef format %d\n", fmt);
	return 0;
}

static int
classdef(TTGDEFClass *c, u16int gid)
{
	int lo, hi, mid;

	if(c->format == 1){
		if(gid < c->start || gid >= c->start + c->count)
			return 0;
		return c->classes[gid - c->start];
	}

	if(c->format == 2){
		lo = 0;
		hi = c->nrange - 1;
		while(lo <= hi){
			mid = (lo + hi) / 2;
			if(gid < c->range[mid].start)
				hi = mid - 1;
			else if(gid > c->range[mid].end)
				lo = mid + 1;
			else
				return c->range[mid].class;
		}
	}
	return 0;
}

int
ttfgdefinit(TTFontU *f)
{
	vlong gdefbase;
	u16int major, minor;
	u16int glyphclassoff, attachoff, ligcaretoff, markattachoff;

	if(debug < 0)
		debug = getenv("GDEFDEBUG") != nil;

	if(ttfgototable(f, "GDEF") < 0)
		return 0;

	gdefbase = Bseek(f->bin, 0, 1);
	ttfunpack(f, "wwww",
		&major, &minor, &glyphclassoff, &attachoff);

	ttfunpack(f, "w", &ligcaretoff);

	if(major >= 1 && minor >= 2)
		ttfunpack(f, "w", &markattachoff);
	else
		markattachoff = 0;

	USED(major);
	USED(minor);
	USED(attachoff);
	USED(ligcaretoff);
	USED(markattachoff);

	f->gdefclass = mallocz(sizeof(TTGDEFClass), 1);
	if(!readclassdef(f, gdefbase, glyphclassoff, f->gdefclass)){
		free(f->gdefclass);
		f->gdefclass = nil;
		return 0;
	}

	if(debug)
		fprint(2, "gdef: glyph class definition format %d\n",
			f->gdefclass->format);

	return 1;
}

int
ttfgdefclass(TTFontU *f, u16int gid)
{
	if(f->gdefclass == nil)
		return 0;

	return classdef(f->gdefclass, gid);
}
