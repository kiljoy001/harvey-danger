#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ttf.h>
#include "impl.h"

static int
tagmatch(u16int hi, u16int lo, char *tag)
{
	return hi == (u16int)((tag[0]<<8)|tag[1])
		&& lo == (u16int)((tag[2]<<8)|tag[3]);
}

/*
 * Resolve the set of lookup indices activated by any of the given
 * feature tags (4-char strings, e.g. "abvm"), via:
 *   ScriptList -> each Script's DefaultLangSys -> FeatureIndices
 *   -> FeatureList -> matching Feature's LookupListIndices
 *
 * Non-default LangSys (language-specific feature overrides) are not
 * consulted -- same "good enough, not spec-complete" tradeoff as the
 * rest of this shaper (anchor device tables, GSUB/GPOS header format
 * 2, etc). Every script in ScriptList is scanned and unioned, since
 * callers here have no script/language context to narrow by; a
 * lookup activated under the requested tag for ANY script's default
 * langsys is included.
 *
 * tablebase is the GSUB or GPOS table start (Bseek(f->bin,0,1) right
 * after ttfgototable), scriptoff/featoff the ScriptList/FeatureList
 * offsets read from that table's header.
 *
 * Returns a malloc'd, deduplicated array of lookup indices; *nout is
 * the count (0 if nothing matched, e.g. no ScriptList or no feature
 * with that tag). Caller frees.
 */
u16int *
ttffeaturelookups(TTFontU *f, vlong tablebase, u16int scriptoff,
	u16int featoff, char **tags, int ntags, int *nout)
{
	vlong scriptlistbase, featlistbase, scriptbase, langsysbase, featbase;
	u16int nscript, soff, dlsoff, taghi, taglo;
	u16int lookuporder, reqfeat, nfeatidx, *featidx;
	u16int nfeat, ftaghi, ftaglo, foff;
	u16int fparamoff, nlookup, *lookupidx;
	u16int *lut;
	int cap, n, i, j, k, t, dup, match;

	cap = 32;
	n = 0;
	lut = malloc(cap * sizeof(u16int));

	scriptlistbase = tablebase + scriptoff;
	Bseek(f->bin, scriptlistbase, 0);
	ttfunpack(f, "w", &nscript);

	for(i = 0; i < nscript; i++){
		Bseek(f->bin, scriptlistbase + 2 + i * 6, 0);
		ttfunpack(f, "ww", &taghi, &taglo);	/* script tag, unused: scan all scripts */
		ttfunpack(f, "w", &soff);

		scriptbase = scriptlistbase + soff;
		Bseek(f->bin, scriptbase, 0);
		ttfunpack(f, "w", &dlsoff);	/* DefaultLangSys offset, 0 = none */
		if(dlsoff == 0)
			continue;

		langsysbase = scriptbase + dlsoff;
		Bseek(f->bin, langsysbase, 0);
		ttfunpack(f, "ww", &lookuporder, &reqfeat);	/* both unused here */
		ttfunpack(f, "w", &nfeatidx);
		featidx = malloc(nfeatidx * sizeof(u16int));
		for(j = 0; j < nfeatidx; j++)
			ttfunpack(f, "w", &featidx[j]);

		featlistbase = tablebase + featoff;
		Bseek(f->bin, featlistbase, 0);
		ttfunpack(f, "w", &nfeat);

		for(j = 0; j < nfeatidx; j++){
			if(featidx[j] >= nfeat)
				continue;

			Bseek(f->bin, featlistbase + 2 + featidx[j] * 6, 0);
			ttfunpack(f, "ww", &ftaghi, &ftaglo);
			ttfunpack(f, "w", &foff);	/* relative to featlistbase */

			match = 0;
			for(t = 0; t < ntags; t++)
				if(tagmatch(ftaghi, ftaglo, tags[t])){
					match = 1;
					break;
				}
			if(!match)
				continue;

			featbase = featlistbase + foff;
			Bseek(f->bin, featbase, 0);
			ttfunpack(f, "w", &fparamoff);	/* featureParams offset, unused */
			ttfunpack(f, "w", &nlookup);
			lookupidx = malloc(nlookup * sizeof(u16int));
			for(k = 0; k < nlookup; k++)
				ttfunpack(f, "w", &lookupidx[k]);

			for(k = 0; k < nlookup; k++){
				dup = 0;
				for(t = 0; t < n; t++)
					if(lut[t] == lookupidx[k]){
						dup = 1;
						break;
					}
				if(dup)
					continue;
				if(n >= cap){
					cap *= 2;
					lut = realloc(lut, cap * sizeof(u16int));
				}
				lut[n++] = lookupidx[k];
			}
			free(lookupidx);
		}
		free(featidx);
	}

	*nout = n;
	return lut;
}
