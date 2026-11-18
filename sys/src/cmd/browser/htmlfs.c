/*
 * htmlfs.c - stage 1 of the browser pipeline.
 *
 *	S rc		source HTML (write replaces, bumps gen)
 *	S tree		flat node table:  B  |  T<text>
 *	S links		one L <href> per link
 *	S gen		generation counter
 *
 * The table is line oriented on purpose: it is cheap to read in chunks,
 * greppable, and the next stage only needs block breaks and text runs.
 * This is a deliberately small HTML tokenizer, not a conforming HTML5
 * tree builder; the point of the first cut is the pipeline, not the parser.
 */

#include <u.h>
#include <libc.h>
#include <ctype.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "ringfs.h"

typedef struct Buf Buf;
struct Buf {
	char	*p;
	int	n;
	int	cap;
};

static QLock	srclk;
static char	*src;
static int	srclen;
static ulong	srctag;
static Rendez	genr;
static Buf	genlog;		/* append-only generation stream */

static void
bufgrow(Buf *b, int need)
{
	int ncap;
	char *np;

	if(b->n + need <= b->cap)
		return;
	ncap = b->cap ? b->cap : 256;
	while(ncap < b->n + need)
		ncap *= 2;
	np = realloc(b->p, ncap);
	if(np == nil)
		sysfatal("htmlfs: out of memory");
	b->p = np;
	b->cap = ncap;
}

static void
bufadd(Buf *b, void *p, int n)
{
	if(n <= 0)
		return;
	bufgrow(b, n);
	memmove(b->p + b->n, p, n);
	b->n += n;
}

static void
bufputs(Buf *b, char *s)
{
	bufadd(b, s, strlen(s));
}

static void
bufputc(Buf *b, int c)
{
	char ch = c;

	bufadd(b, &ch, 1);
}

static int
isblock(char *tag)
{
	static char *blocks[] = {
		"p", "div", "br", "li", "ul", "ol", "h1", "h2", "h3", "h4",
		"h5", "h6", "table", "tr", "td", "th", "section", "article",
		"header", "footer", "pre", "blockquote", "hr", "form", "title",
		"head", "body", "html", "dt", "dd", "dl", "figure",
	};
	int i;

	for(i = 0; i < nelem(blocks); i++)
		if(strcmp(tag, blocks[i]) == 0)
			return 1;
	return 0;
}

static int
tagof(char *p, int n, char *out, int outlen)
{
	int i;

	if(n <= 0)
		return 0;
	if(n >= outlen)
		n = outlen - 1;
	for(i = 0; i < n; i++)
		out[i] = tolower((uchar)p[i]);
	out[i] = 0;
	return 1;
}

static char*
findci(char *s, int n, char *pat)
{
	int plen, i;
	char *e;

	plen = strlen(pat);
	e = s + n;
	for(; s + plen <= e; s++){
		for(i = 0; i < plen; i++)
			if(tolower((uchar)s[i]) != tolower((uchar)pat[i]))
				break;
		if(i == plen)
			return s + plen;
	}
	return nil;
}

static int
entity(char *p, int n, int *adv)
{
	static struct {
		char	*name;
		int	c;
	} tab[] = {
		{ "amp;", '&' }, { "lt;", '<' }, { "gt;", '>' },
		{ "quot;", '"' }, { "apos;", '\'' }, { "nbsp;", ' ' },
	};
	int i, v, base, digits, d, len;

	if(n < 3 || p[0] != '&')
		return -1;
	for(i = 0; i < nelem(tab); i++){
		len = strlen(tab[i].name);
		if(n >= len + 1 && strncmp(p + 1, tab[i].name, len) == 0){
			*adv = len + 1;
			return tab[i].c;
		}
	}
	if(p[1] != '#')
		return -1;
	base = 10;
	i = 2;
	if(i < n && (p[i] == 'x' || p[i] == 'X')){
		base = 16;
		i++;
	}
	v = 0;
	digits = 0;
	for(; i < n && p[i] != ';'; i++){
		if(p[i] >= '0' && p[i] <= '9')
			d = p[i] - '0';
		else if(base == 16 && p[i] >= 'a' && p[i] <= 'f')
			d = p[i] - 'a' + 10;
		else if(base == 16 && p[i] >= 'A' && p[i] <= 'F')
			d = p[i] - 'A' + 10;
		else
			break;
		v = v * base + d;
		digits++;
	}
	if(digits == 0 || i >= n || p[i] != ';')
		return -1;
	*adv = i + 1;
	return v & 0xff;
}

static void
textchunk(Buf *tree, char *s, int n)
{
	Buf t;
	char *p, *e;
	int c, adv, space;

	memset(&t, 0, sizeof t);
	p = s;
	e = s + n;
	space = 0;
	while(p < e){
		if(*p == '&' && (c = entity(p, e - p, &adv)) >= 0){
			p += adv;
			if(isspace(c)){
				space = 1;
				continue;
			}
		} else {
			c = (uchar)*p++;
			if(isspace(c)){
				space = 1;
				continue;
			}
		}
		if(space && t.n > 0)
			bufputc(&t, ' ');
		space = 0;
		bufputc(&t, c);
	}
	/* keep one trailing space so "second <b>para</b>" stays two words */
	if(space && t.n > 0)
		bufputc(&t, ' ');
	if(tree != nil && t.n > 0){
		bufputc(tree, 'T');
		bufadd(tree, t.p, t.n);
		bufputc(tree, '\n');
	}
	free(t.p);
}

static void
attr(char *p, int n, char *name, Buf *links)
{
	int nlen, i, j, q;
	char *v;

	nlen = strlen(name);
	for(i = 0; i + nlen < n; i++){
		if(i > 0 && !isspace(p[i-1]) && p[i-1] != '<')
			continue;
		for(j = 0; j < nlen; j++)
			if(tolower((uchar)p[i+j]) != name[j])
				break;
		if(j != nlen)
			continue;
		j = i + nlen;
		while(j < n && isspace(p[j]))
			j++;
		if(j >= n || p[j] != '=')
			continue;
		j++;
		while(j < n && isspace(p[j]))
			j++;
		if(j >= n)
			return;
		q = 0;
		if(p[j] == '"' || p[j] == '\''){
			q = p[j];
			j++;
		}
		v = p + j;
		while(j < n && (q ? p[j] != q : !isspace(p[j]) && p[j] != '>'))
			j++;
		if(links != nil && j > (v - p)){
			bufputs(links, "L ");
			bufadd(links, v, j - (v - p));
			bufputc(links, '\n');
		}
		return;
	}
}

static void
parse(char *s, int n, Buf *tree, Buf *links)
{
	int i, j, end;
	char tag[32];
	char *e, *c;

	if(s == nil)
		return;
	i = 0;
	while(i < n){
		if(s[i] != '<'){
			j = i;
			while(j < n && s[j] != '<')
				j++;
			textchunk(tree, s + i, j - i);
			i = j;
			continue;
		}
		if(i + 3 < n && s[i+1] == '!' && s[i+2] == '-' && s[i+3] == '-'){
			e = strstr(s + i + 4, "-->");
			i = e != nil ? (e - s) + 3 : n;
			continue;
		}
		if(i + 1 < n && (s[i+1] == '!' || s[i+1] == '?')){
			e = memchr(s + i, '>', n - i);
			i = e != nil ? e - s + 1 : n;
			continue;
		}
		if(i + 1 < n && s[i+1] == '/'){
			j = i + 2;
			while(j < n && s[j] != '>')
				j++;
			if(j < n && tagof(s + i + 2, j - (i + 2), tag, sizeof tag)
			   && tree != nil && isblock(tag))
				bufputs(tree, "B\n");
			i = j < n ? j + 1 : n;
			continue;
		}
		j = i + 1;
		while(j < n && s[j] != '>' && s[j] != '/')
			j++;
		tagof(s + i + 1, j - (i + 1), tag, sizeof tag);
		e = memchr(s + j, '>', n - j);
		end = e != nil ? e - s : n;
		if(strcmp(tag, "script") == 0 || strcmp(tag, "style") == 0){
			char pat[16];

			snprint(pat, sizeof pat, "</%s", tag);
			c = findci(s + end, n - end, pat);
			i = c != nil ? c - s : n;
			while(i < n && s[i] != '>')
				i++;
			if(i < n)
				i++;
			continue;
		}
		if(links != nil)
			attr(s + i + 1, end - (i + 1), "href", links);
		if(tree != nil && isblock(tag))
			bufputs(tree, "B\n");
		i = end < n ? end + 1 : n;
	}
}

static void
htmlread(Req *r)
{
	char *aux;
	Buf t, l;

	aux = r->fid->file->aux;
	memset(&t, 0, sizeof t);
	memset(&l, 0, sizeof l);
	if(strcmp(aux, "src") == 0){
		qlock(&srclk);
		readbuf(r, src, srclen);
		qunlock(&srclk);
	} else if(strcmp(aux, "gen") == 0){
		vlong off;

		/* the log is a stream: a read past its end blocks until a write */
		off = r->ifcall.offset;
		qlock(&srclk);
		while(genlog.n <= off)
			rsleep(&genr);
		readbuf(r, genlog.p, genlog.n);
		qunlock(&srclk);
	} else if(strcmp(aux, "tree") == 0 || strcmp(aux, "links") == 0){
		qlock(&srclk);
		parse(src, srclen, &t, &l);
		qunlock(&srclk);
		if(strcmp(aux, "tree") == 0)
			readbuf(r, t.p, t.n);
		else
			readbuf(r, l.p, l.n);
		free(t.p);
		free(l.p);
	} else {
		respond(r, "bad file");
		return;
	}
	respond(r, nil);
}

static void
htmlwrite(Req *r)
{
	char *aux, *p, rec[16];
	int off, n;

	aux = r->fid->file->aux;
	if(strcmp(aux, "src") != 0){
		respond(r, "read-only");
		return;
	}
	off = r->ifcall.offset;
	n = r->ifcall.count;
	if(n <= 0){
		r->ofcall.count = 0;
		respond(r, nil);
		return;
	}
	qlock(&srclk);
	if(off + n > srclen){
		p = realloc(src, off + n);
		if(p == nil){
			qunlock(&srclk);
			respond(r, "out of memory");
			return;
		}
		if(off > srclen)
			memset(p + srclen, 0, off - srclen);
		src = p;
	}
	memmove(src + off, r->ifcall.data, n);
	if(off + n > srclen)
		srclen = off + n;
	srctag++;
	snprint(rec, sizeof rec, "%010lud\n", srctag);
	bufadd(&genlog, rec, strlen(rec));
	rwakeup(&genr);
	qunlock(&srclk);
	r->ofcall.count = n;
	respond(r, nil);
}

void
htmlfsserve(int fd)
{
	Srv s;
	File *f;

	memset(&s, 0, sizeof s);
	s.tree = alloctree("browser", "browser", DMDIR|0555, nil);
	if(s.tree == nil)
		sysfatal("htmlfs: alloctree: %r");
	f = createfile(s.tree->root, "src", "browser", 0666, "src");
	if(f == nil)
		sysfatal("htmlfs: create src: %r");
	closefile(f);
	f = createfile(s.tree->root, "tree", "browser", 0444, "tree");
	if(f == nil)
		sysfatal("htmlfs: create tree: %r");
	closefile(f);
	f = createfile(s.tree->root, "links", "browser", 0444, "links");
	if(f == nil)
		sysfatal("htmlfs: create links: %r");
	closefile(f);
	f = createfile(s.tree->root, "gen", "browser", 0444, "gen");
	if(f == nil)
		sysfatal("htmlfs: create gen: %r");
	closefile(f);
	s.read = htmlread;
	s.write = htmlwrite;
	s.infd = fd;
	s.outfd = fd;
	s.msize = 32768;
	s.threaded = 1;		/* a blocked gen read must not stall writers */
	qlock(&srclk);
	genr.l = &srclk;
	if(genlog.n == 0)
		bufputs(&genlog, "0000000000\n");
	qunlock(&srclk);
	srv(&s);
}

static void
serveconn(void *v)
{
	int fd;

	fd = (int)(vlong)v;
	htmlfsserve(fd);
	close(fd);
	threadexits(nil);
}

void
htmlfsserver(char *name)
{
	int fd;

	for(;;){
		fd = ringlisten(name);
		if(fd < 0)
			break;
		if(proccreate(serveconn, (void*)(vlong)fd, 65536) < 0){
			close(fd);
			break;
		}
	}
}
