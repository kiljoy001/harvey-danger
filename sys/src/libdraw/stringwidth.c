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

int
_stringnwidth(Font *f, char *s, Rune *r, int len)
{
	int wid, twid, n, max, try, nr;
	enum { Max = 64 };
	ushort cbuf[Max];
	Rune rune, **rptr;
	char *subfontname, **sptr;
	Subfont *sf;

	if(s == nil){
		s = "";
		sptr = nil;
	}else
		sptr = &s;
	if(r == nil){
		r = (Rune*)L"";
		rptr = nil;
	}else
		rptr = &r;
	subfontname = nil;
	sf = nil;
	twid = 0;
	try = 0;
	while(len > 0 && (*s || *r)){
		char *sp0;
		Rune *rp0;

		max = Max;
		if(len < max)
			max = len;
		sp0 = s;
		rp0 = r;
		if(subfontname){
		/*   fprint(2, "_stringnwidth: before _getsubfont f=%p subfont=%s\n", */
		/* f, subfontname); */
			freesubfont(sf);
			if((sf=_getsubfont(f->display, subfontname)) == nil){
				if(f->display == nil || f->display->defaultfont == nil || f->display->defaultfont == f)
					break;
				f = f->display->defaultfont;
			}
		/* 	fprint(2, "_stringnwidth: after _getsubfont f=%p subfont=%s\n", */
		/* f, subfontname); */
		}
		if((n = _cachecharsn(f, sptr, rptr, cbuf, max, &wid, &subfontname, nil, nil, len)) <= 0){
			if(n == 0){
				if(++try > 10)
					break;
				continue;
			}
			if(*r)
				r++;
			else
				s += chartorune(&rune, s);
			len--;
			continue;
		}
		try = 0;

		if(rptr != nil)
			nr = r - rp0;
		else
			nr = utfnlen(sp0, s - sp0);

		if(nr <= 0)
			break;

		agefont(f);
		twid += wid;
		len -= nr;
	}
	freesubfont(sf);
	return twid;
}

int
stringnwidth(Font *f, char *s, int len)
{
	return _stringnwidth(f, s, nil, len);
}

int
stringwidth(Font *f, char *s)
{
	return _stringnwidth(f, s, nil, 1<<24);
}

Point
stringsize(Font *f, char *s)
{
	return Pt(_stringnwidth(f, s, nil, 1<<24), f->height);
}

int
runestringnwidth(Font *f, Rune *r, int len)
{
	return _stringnwidth(f, nil, r, len);
}

int
runestringwidth(Font *f, Rune *r)
{
	return _stringnwidth(f, nil, r, 1<<24);
}

Point
runestringsize(Font *f, Rune *r)
{
	return Pt(_stringnwidth(f, nil, r, 1<<24), f->height);
}
