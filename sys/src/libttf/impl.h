enum {
	ONCURVE = 1,
	TOUCHX = 2,
	TOUCHY = 4,
	TOUCH = 6,
};

int ttfgototable(TTFontU *, char *);
int ttfrounddiv(int, int);
int ttfvrounddiv(vlong, int);
int ttfhint(TTGlyph *);
int ttfparsecmap(TTFontU *);
int ttfrunfpgm(TTFont *);
int ttfruncvt(TTFont *);
void ttfunpack(TTFontU *, char *, ...);
void ttfscan(TTGlyph *);
void ttffreebitmap(TTBitmap *);
int ttfgsubinit(TTFontU *f); /* parse GSUB, build f->ligatures[] */
int ttfcolrinit(TTFontU *f); /* parse COLR */
int readcoverage(TTFontU *f, u16int **gp);
int ttfgposinit(TTFontU *f);
int ttfgdefinit(TTFontU *f);
u16int *ttffeaturelookups(TTFontU *f, vlong tablebase, u16int scriptoff,
	u16int featoff, char **tags, int ntags, int *nout);
