#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ttf.h>
#include "impl.h"

#define LIGPRIO_NONE	0xFF

static int
ligcmp(void *va, void *vb)
{
	TTLigature *a, *b;

	a = va;
	b = vb;
	if(a->first != b->first)
		return a->first - b->first;
	if(a->ncomp != b->ncomp)
		return b->ncomp - a->ncomp;	/* longest match first, always */
	return a->prio - b->prio;	/* only tie-break equal-length entries by feature priority */
}

/* read a coverage table at the current Bio position into *gp (malloc'd),
 * return glyph count, gp[i] = glyph id at coverage index i */
int
readcoverage(TTFontU *f, u16int **gp)
{
	u16int fmt, n, i, s, e, ci;
	u16int *g;
	int ng, cap;

	ttfunpack(f, "w", &fmt);
	if(fmt == 1){
		ttfunpack(f, "w", &n);
		g = malloc(n * sizeof(u16int));
		for(i = 0; i < n; i++)
			ttfunpack(f, "w", &g[i]);
		*gp = g;
		return n;
	}
	if(fmt == 2){
		ttfunpack(f, "w", &n);
		cap = 64;
		g = malloc(cap * sizeof(u16int));
		ng = 0;
		for(i = 0; i < n; i++){
			ttfunpack(f, "www", &s, &e, &ci);
			USED(ci);
			while(s <= e){
				if(ng >= cap){
					cap *= 2;
					g = realloc(g, cap * sizeof(u16int));
				}
				g[ng++] = s++;
			}
		}
		*gp = g;
		return ng;
	}
	werrstr("bad coverage format %d", fmt);
	return -1;
}

int
ttfgsubinit(TTFontU *f)
{
	vlong gsubbase, lookupbase, lbase, subbase, ligsetbase;
	u16int major, minor, scriptoff, featoff, lookupoff, nlookup, *lookupoffs;
	u16int type, flag, nsub, *suboffs, substfmt, covoff, nligset, *ligsetoffs;
	u16int ncov, *covglyphs, nligs, *ligoffs, ligout, ncomp;
	int li, si, ci, gi, j, nliga, cap, debug;
	u16int markfilterjunk;
	TTLigature *liga;
	int *prio;
	int nlut;

	if(f->liga != nil || f->nliga != 0)
		return 1;	/* already parsed via a shared TTFontU */

	if(ttfgototable(f, "GSUB") < 0){
		f->liga = nil;
		f->nliga = 0;
		return 0;	/* no GSUB table: not an error */
	}
	gsubbase = Bseek(f->bin, 0, 1);
	debug = getenv("GSUBDEBUG") != nil;

	ttfunpack(f, "wwwww", &major, &minor, &scriptoff, &featoff, &lookupoff);
	USED(major); USED(minor);

	lookupbase = gsubbase + lookupoff;
	Bseek(f->bin, lookupbase, 0);
	ttfunpack(f, "w", &nlookup);
	lookupoffs = malloc(nlookup * sizeof(u16int));
	for(li = 0; li < nlookup; li++)
		ttfunpack(f, "w", &lookupoffs[li]);

	/*
	 * Priority by feature, lowest wins on a (first,ncomp) tie.
	 * rphf must outrank blwf/vatu: a font can define both a reph
	 * ligature (rphf: Ra+virama -> reph) and an unrelated below-
	 * base/reordered Ra form (blwf/vatu) keyed off the same
	 * first+component shape, and ttfligate's linear scan picks
	 * whichever sorts first. Scanned in this order so a lookup
	 * reachable via multiple tags keeps the highest-priority one.
	 */
	prio = malloc(nlookup * sizeof(int));
	for(li = 0; li < nlookup; li++)
		prio[li] = 11;	/* fallback/untagged: lowest priority */
	{
		struct { char *tag[1]; int pri; } passes[] = {
			{{"rphf"}, 0},
			{{"akhn"}, 1},
			{{"nukt"}, 2},
			{{"rkrf"}, 3},
			{{"pref"}, 4},
			{{"blwf"}, 5},
			{{"half"}, 6},
			{{"pstf"}, 7},
			{{"vatu"}, 8},
			{{"cjct"}, 9},
			{{"liga"}, 10},
			{{"rlig"}, 10},
			{{"clig"}, 10},
		};
		u16int *lut;
		int n, k, p;

		nlut = 0;
		for(p = 0; p < nelem(passes); p++){
			lut = ttffeaturelookups(f, gsubbase, scriptoff, featoff,
				passes[p].tag, 1, &n);
			for(k = 0; k < n; k++){
				if(lut[k] < nlookup && passes[p].pri < prio[lut[k]]){
					prio[lut[k]] = passes[p].pri;
					nlut++;
				}
			}
			free(lut);
		}
	}

	if(debug)
		fprint(2, "gsub: %d lookups tagged by ligature-producing features\n", nlut);

	cap = 64;
	nliga = 0;
	liga = malloc(cap * sizeof(TTLigature));

	for(li = 0; li < nlookup; li++){
		/*
		 * If any feature tag resolved at all, only scan lookups
		 * reached by one of them. If none resolved, fall back to
		 * the old blanket "every type 4 lookup" behavior rather
		 * than silently producing zero ligatures.
		 */
		if(nlut > 0 && prio[li] == 11)
			continue;

		lbase = lookupbase + lookupoffs[li];
		Bseek(f->bin, lbase, 0);
		ttfunpack(f, "www", &type, &flag, &nsub);
		if(debug)
		  fprint(2, "lookup %d: type=%d flag=%#x nsub=%d prio=%d\n",
			li, type, flag, nsub, prio[li]);
		if(type != 4)
			continue;	/* only LigatureSubst */

		suboffs = malloc(nsub * sizeof(u16int));
		for(si = 0; si < nsub; si++)
			ttfunpack(f, "w", &suboffs[si]);
		if(flag & 0x0010)	/* UseMarkFilteringSet: trailing u16, unused here */
			ttfunpack(f, "w", &markfilterjunk);

		for(si = 0; si < nsub; si++){
			subbase = lbase + suboffs[si];
			Bseek(f->bin, subbase, 0);
			ttfunpack(f, "ww", &substfmt, &covoff);
			if(substfmt != 1)
				continue;	/* only format defined for type 4 */
			ttfunpack(f, "w", &nligset);
			ligsetoffs = malloc(nligset * sizeof(u16int));
			for(ci = 0; ci < nligset; ci++)
				ttfunpack(f, "w", &ligsetoffs[ci]);

			Bseek(f->bin, subbase + covoff, 0);
			ncov = readcoverage(f, &covglyphs);
			if(ncov != nligset){
				if(ncov > nligset)
					ncov = nligset;
			}

			for(ci = 0; ci < ncov; ci++){
				ligsetbase = subbase + ligsetoffs[ci];
				Bseek(f->bin, ligsetbase, 0);
				ttfunpack(f, "w", &nligs);
				ligoffs = malloc(nligs * sizeof(u16int));
				for(gi = 0; gi < nligs; gi++)
					ttfunpack(f, "w", &ligoffs[gi]);

				for(gi = 0; gi < nligs; gi++){
					Bseek(f->bin, ligsetbase + ligoffs[gi], 0);
					ttfunpack(f, "ww", &ligout, &ncomp);
					ncomp--;	/* stored count includes first glyph */

					if(nliga >= cap){
						cap *= 2;
						liga = realloc(liga, cap * sizeof(TTLigature));
					}
					liga[nliga].first = covglyphs[ci];
					liga[nliga].out = ligout;
					liga[nliga].ncomp = ncomp;
					liga[nliga].prio = prio[li];
					liga[nliga].comp = malloc(ncomp * sizeof(u16int));
					for(j = 0; j < ncomp; j++)
						ttfunpack(f, "w", &liga[nliga].comp[j]);
					if(debug){
						fprint(2, "liga[%d]: first=%d out=%d ncomp=%d prio=%d comp=[",
							nliga, liga[nliga].first, liga[nliga].out,
							ncomp, liga[nliga].prio);
						for(j = 0; j < ncomp; j++)
							fprint(2, "%d ", liga[nliga].comp[j]);
						fprint(2, "]\n");
					}
					nliga++;
				}
				free(ligoffs);
			}
			free(ligsetoffs);
			free(covglyphs);
		}
		free(suboffs);
	}
	free(prio);
	free(lookupoffs);

	qsort(liga, nliga, sizeof(TTLigature), ligcmp);
	f->liga = liga;
	f->nliga = nliga;
	return 1;
}

static int
isindicvirama(Rune r)
{
	switch(r){
	case 0x094D:	/* DEVANAGARI SIGN VIRAMA */
		return 1;
	}
	return 0;
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

static int
isprebasematra(Rune r)
{
	switch(r){
	case 0x093F:	/* DEVANAGARI VOWEL SIGN I -- the only pre-base matra */
		return 1;
	}
	return 0;
}

/*
 * Devanagari consonant, virama-joinable. Reused from ttfindic's
 * isindicbase, duplicated locally so this file's two reorder passes
 * (matra, reph) don't need to share state beyond the gid[]/nrunes[]
 * arrays already threaded through ttfshape.
 */
static int
isconsonant(Rune r)
{
	return (r >= 0x0915 && r <= 0x0939) || (r >= 0x0958 && r <= 0x095F);
}

/*
 * Reorder a pre-base dependent vowel sign (093F) to precede the
 * consonant cluster it attaches to, per the Devanagari rendering
 * rule: 093F is stored AFTER its base in text but drawn BEFORE it.
 *
 * str/n is the original input rune string (before variation-selector
 * skipping and cmap), gid/ng the post-ligation glyph array, nrunes[k]
 * the number of runes output glyph k absorbed (see ttfligate).
 *
 * Algorithm:
 *   1. Scan str[] for 093F at rune index i.
 *   2. Walk backward consuming (consonant, virama) pairs to find the
 *      start of the syllable's consonant cluster -- everything the
 *      matra visually sits in front of.
 *   3. Convert both the cluster-start rune index and the matra's own
 *      rune index into glyph indices via nrunes[]'s running sum.
 *   4. Remove the matra's glyph from its post-ligation position and
 *      reinsert it immediately before the cluster's first glyph.
 *
 * Only one 093F per call site is handled per pass; a second 093F
 * later in the same string is found on the next iteration of the
 * scan loop below, against the already-reordered gid[]/nrunes[] --
 * intentional, since two matras never target overlapping clusters.
 */
static int
ttfmatra(TTFont *f, Rune *str, int n, TTShapeInfo *gi, int ng)
{
	int i, j, k, runeidx, clusterrune, matraglyph, clustglyph, sum;
	TTShapeInfo mg;

	USED(f);
	USED(runeidx);

	for(i = 0; i < n; i++){
		if(!isprebasematra(str[i]))
			continue;
		if(i == 0)
			continue;

		if(!isconsonant(str[i-1]))
			continue;

		clusterrune = i - 1;
		while(clusterrune >= 2
		   && isindicvirama(str[clusterrune-1])
		   && isconsonant(str[clusterrune-2]))
			clusterrune -= 2;

		sum = 0;
		clustglyph = -1;
		matraglyph = -1;
		for(k = 0; k < ng; k++){
			if(clustglyph < 0 && sum <= clusterrune
			   && clusterrune < sum + gi[k].nrunes)
				clustglyph = k;
			if(matraglyph < 0 && sum <= i && i < sum + gi[k].nrunes)
				matraglyph = k;
			sum += gi[k].nrunes;
		}
		if(clustglyph < 0 || matraglyph < 0 || matraglyph <= clustglyph)
			continue;

		/* Single struct move: rotate the matra glyph into place. */
		mg = gi[matraglyph];
		for(j = matraglyph; j > clustglyph; j--)
			gi[j] = gi[j-1];
		gi[clustglyph] = mg;
	}
	return ng;
}

/*
 * Reorder initial Devanagari Ra + virama after the first
 * consonant of the syllable.
 *
 * At this point ttfligate() has already applied the font's
 * ligature substitutions, including rphf where represented
 * as a ligature substitution.
 */
static int
ttfindic(TTFont *f, Rune *str, int n, TTShapeInfo *gi, int ng)
{
	TTShapeInfo g;
	int i, ri, basei;

	if(n < 3 || ng < 2)
		return ng;

	if(str[0] != 0x0930 || !isindicvirama(str[1]))
		return ng;

	ri = -1;
	for(i = 0; i < ng; i++){
		if(gi[i].ligprio == 0){
			ri = i;
			break;
		}
	}
	if(ri < 0)
		return ng;

	basei = -1;
	for(i = ri + 1; i < ng; i++){
		if(ttfgdefclass(f->u, gi[i].gid) != 3){
			basei = i;
			break;
		}
	}
	if(basei < 0)
		return ng;

	g = gi[ri];
	memmove(gi + ri, gi + ri + 1,
		(basei - ri) * sizeof(gi[0]));
	gi[basei] = g;

	return ng;
}

/* shape gid[0..n) in place, return new length.
 * If nrunes != nil, nrunes[0..return value) is filled in: nrunes[k]
 * is the number of pre-ligation input glyphs (== input runes, since
 * cmap output is 1:1 with runes before this point) that collapsed
 * into output glyph k. Callers that need to map a rune-string index
 * back onto a post-ligation glyph index (matra/reph reordering) sum
 * nrunes left to right until they've accounted for enough runes. */
static int
ttfligate(TTFontU *f, TTShapeInfo *gi, int n)
{
	TTLigature *m, *match;
	int i, j, out, debug;

	if(f->nliga == 0){
		for(i = 0; i < n; i++){
			gi[i].nrunes = 1;
			gi[i].ligprio = LIGPRIO_NONE;
		}
		return n;
	}

	debug = getenv("GSUBDEBUG") != nil;
	for(i = 0, out = 0; i < n; ){
		match = nil;
		for(m = f->liga; m < f->liga + f->nliga; m++){
			if(m->first != gi[i].gid)
				continue;
			if(i + m->ncomp >= n)
				continue;
			for(j = 0; j < m->ncomp && gi[i+1+j].gid == m->comp[j]; j++)
				;
			if(j == m->ncomp){
				match = m;
				break;
			}
		}
		if(match != nil){
			if(debug){
				fprint(2, "match at i=%d: first=%d ncomp=%d -> out=%d\n",
					i, match->first, match->ncomp, match->out);
				for(j = 0; j < match->ncomp; j++)
					if(gi[i+1+j].syllable != gi[i].syllable)
						fprint(2, "ttfligate: WARNING lig at i=%d "
							"spans syllable boundary "
							"(comp %d: syllable %d vs %d)\n",
							i, j, gi[i+1+j].syllable,
							gi[i].syllable);
			}
			gi[out].gid = match->out;
			gi[out].nrunes = match->ncomp + 1;
			gi[out].ligprio = match->prio;
			gi[out].syllable = gi[i].syllable;
			out++;
			i += match->ncomp + 1;
		}else{
			if(debug)
				fprint(2, "no match at i=%d: gid=%d, passthrough\n",
					i, gi[i].gid);
			gi[out] = gi[i];
			gi[out].nrunes = 1;
			gi[out].ligprio = LIGPRIO_NONE;
			out++;
			i++;
		}
	}
	return out;
}

static int
isvarsel(Rune r)
{
	return (r >= 0xFE00 && r <= 0xFE0F)
		|| (r >= 0xE0100 && r <= 0xE01EF);
}
 
/* convert str[0..n) to glyph ids in gid[], skipping variation selectors,
 * then run GSUB ligature substitution. gid[] must hold at least n entries.
 * returns the final glyph count (<= n). */
int
ttfshape(TTFont *f, Rune *str, int n, TTShapeInfo *gi)
{
	int i, ng, synum;
	TTShapeInfo localbuf[64];
	TTShapeInfo *g;

	/* Caller may pass a small stack buffer; if n exceeds it, spill. */
	g = gi;
	if(g == nil)
		g = n <= nelem(localbuf) ? localbuf : malloc(n * sizeof(*g));

	ng = 0;
	synum = 1;
	for(i = 0; i < n; i++){
		if(isvarsel(str[i]))
			continue;
		if(isindicbase(str[i]) && !(i > 0 && str[i-1] == 0x094D)){
			if(ng > 0)
			  synum++;
			if(synum == 16)
			  synum = 1;
		}
		g[ng].gid = ttffindchar(f, str[i]);
		g[ng].nrunes = 1;
		g[ng].ligprio = LIGPRIO_NONE;
		g[ng].syllable = synum;
		ng++;
	}

	ng = ttfligate(f->u, g, ng);
	ng = ttfmatra(f, str, n, g, ng);
	ng = ttfindic(f, str, n, g, ng);

	if(g != gi && g != localbuf)
		free(g);
	return ng;
}
