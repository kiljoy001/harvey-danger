#include <u.h>
#include <libc.h>
#include <ctype.h>
#include <draw.h>
#include <thread.h>
#include <cursor.h>
#include <mouse.h>
#include <keyboard.h>
#include <frame.h>
#include <fcall.h>
#include <plumb.h>
#include "dat.h"
#include "fns.h"

enum {
    SpanColor,
    SpanUnderline,
    SpanURL,
};

/*
 * Binary search: index of first span where pos+len > lo.
 * That is, the first span that could overlap position lo.
 * Returns nspans if none found.
 */
uint
spanfirst(Window *w, uint lo)
{
    uint a, b, m;

    a = 0;
    b = w->nspans;
    while(a < b){
        m = (a + b) / 2;
        if(w->spans[m].pos + w->spans[m].len <= lo)
            a = m + 1;
        else
            b = m;
    }
    return a;
}

int
wcolinuse(Window *w, Image *col)
{
    Span s; 
	if (cols[TEXT] == col || cols[BACK] == col)
	  return 1;
	for(int i = 0; i < w->nspans; i++){
	  s = w->spans[i];
	  if (s.bg == col || s.fg == col)
		return 1;
	}
	return 0;
		
}

void
freespan(Window *w)
{
    Span s; 
	for(int i = 0; i < w->nspans; i++){
	  s = w->spans[i];
	  if (s.bg != cols[BACK]) {
		freeimage(s.bg);
	  }
	  if (s.fg != cols[TEXT]) {
		freeimage(s.fg);
	  }
	}
	free(w->spans);
}

/*
 * Insert span s into w->spans[], keeping array sorted by pos.
 * Caller fills s.pos, s.len, s.type, and the union field.
 */
void
waddspan(Window *w, Span s)
{
    uint i;
    if(s.len == 0)
        return;
    if(w->nspans >= w->maxspans){
        w->maxspans += 32;
        w->spans = erealloc(w->spans, w->maxspans * sizeof(Span));
    }

    /* find insertion point via binary search */
    i = spanfirst(w, s.pos);

    /* walk forward past spans at same pos (stable insert) */
    while(i < w->nspans && w->spans[i].pos <= s.pos)
        i++;

    /* truncate previous span of same type if it overlaps into s */
    if(i > 0){
        Span *prev = &w->spans[i-1];
        if(prev->type == s.type && prev->pos + prev->len > s.pos)
            prev->len = s.pos - prev->pos;
    }

    /* delete or trim spans that s fully or partially covers */
    uint j = i;
    while(j < w->nspans && w->spans[j].pos < s.pos + s.len){
        Span *next = &w->spans[j];
        uint next_end = next->pos + next->len;
        uint s_end = s.pos + s.len;
        if(next_end <= s_end){
            /* fully covered — mark for removal */
            j++;
        } else {
            /* partially covered — trim its front edge */
            next->len = next_end - s_end;
            next->pos = s_end;
            break;
        }
    }

    /* collapse deleted spans */
    if(j > i)
        memmove(&w->spans[i], &w->spans[j], (w->nspans - j) * sizeof(Span));
    w->nspans -= (j - i);

    /* insert new span */
    memmove(&w->spans[i+1], &w->spans[i], (w->nspans - i) * sizeof(Span));
    w->spans[i] = s;
    w->nspans++;
}

/* Convenience wrapper — keeps existing call sites working */
void
waddcolorspan(Window *w, uint pos, uint len)
{
	Span span;
	span.pos = pos;
	span.len = len;
	span.type = SpanColor;
	span.fg = w->cols[TEXT];
	span.bg = w->cols[BACK];
    waddspan(w, span);
}

/*
 * Adjust all spans after a rune buffer edit.
 * delta > 0: insertion of delta runes at pos.
 * delta < 0: deletion of -delta runes starting at pos.
 *
 * This is the single choke point — winsert and wdelete
 * each call this once; no span type needs its own edit handler.
 */
void
spansadjust(Window *w, uint pos, int delta)
{
    uint i, end;
    Span *s;

    if(delta == 0 || w->nspans == 0)
        return;

    if(delta > 0){
        /* insertion: spans after pos shift right;
         * spans containing pos extend */
        for(i = 0; i < w->nspans; i++){
            s = &w->spans[i];
			/* print("%d:%d %d ", i, s->pos, s->len); */
			/* print("%d %d,", pos, delta); */
            if(s->pos > pos){
                s->pos += delta;
			} else if(pos >= s->pos && pos < s->pos + s->len){
                s->len += delta;
            }
        }
        return;
    }

    /* deletion: delta is negative, work with positive end */
    end = pos + (uint)(-delta);

    i = 0;
    while(i < w->nspans){
        s = &w->spans[i];

        if(s->pos >= end){
            /* entirely after deletion window: shift left */
            s->pos += delta;   /* delta is negative */
            i++;
        } else if(s->pos + s->len <= pos){
            /* entirely before deletion window: untouched */
            i++;
        } else if(s->pos >= pos && s->pos + s->len <= end){
            /* entirely inside deletion window: remove */
            if(s->type == SpanURL)
                free(s->url);
            memmove(&w->spans[i], &w->spans[i+1], (w->nspans - i - 1) * sizeof(Span));
            w->nspans--;
            /* don't increment i — recheck this slot */
        } else if(s->pos < pos && s->pos + s->len > end){
            /* deletion is interior to span: shrink */
            s->len += delta;
            i++;
        } else if(s->pos < pos){
            /* deletion clips the tail */
            s->len = pos - s->pos;
            i++;
        } else {
            /* deletion clips the head */
            s->len -= (end - s->pos);
            s->pos = pos;
            i++;
        }
		/* print("%d:%d %d,", i, s->pos, s->len); */
    }
}

/*
 * Draw all color spans over the visible frame.
 * Other span types (underline, URL hover, etc.) add
 * their own pass here as the type set grows.
 */
void
wcolordraw(Window *w, uint q0, uint q1)
{
    uint i, start, end, framepos, framelen;
    Point pt;
    Span *s;
	Image *fg, *bg;

    if(w->i == nil || w->spans == nil)
        return;
    if(q0 == q1)
        return;
    if(q0 > w->nr || q1 > w->nr)
        return;
    
    /* Normalize */
    if(q0 > q1){
        uint t = q0;
        q0 = q1;
        q1 = t;
    }
    
    /* Find spans overlapping [q0, q1) */
    start = spanfirst(w, q0);   /* binary search to first span at or after q0 */

	int flag = 0;
    for(i = start; i < w->nspans; i++){
        s = &w->spans[i];
        
        /* Stop when past q1 */
        if(s->pos >= q1)
            break;
        
        if(s->type != SpanColor)
            continue;
        
        /* Clip span to [q0, q1) range */
        framepos = s->pos;
        framelen = s->len;
        
        if(s->pos < q0){
            uint diff = q0 - s->pos;
            if(diff >= s->len)
                continue;
            framepos = q0;
            framelen = s->len - diff;
        }
        if(framepos + framelen > q1)
            framelen = q1 - framepos;
        
        /* Convert to frame coordinates (subtract window origin) */
        if(framepos < w->org){
            uint diff = w->org - framepos;
            if(diff >= framelen)
                continue;
            framepos = w->org;
            framelen -= diff;
        }
        if(framepos >= w->org + w->nchars)
            continue;
        if(framepos + framelen > w->org + w->nchars)
            framelen = (w->org + w->nchars) - framepos;
        
        if(framelen == 0)
            continue;

		if (!flag)
		  flag = (framepos + framelen) < q1;
        /* Get screen position and redraw */
        pt = frptofchar(w, framepos - w->org);
		/* print("%d:%d %d, ", i, s->pos, s->len); */
		/* print("%p %p, ", (s->fg), display); */
		/* print("%P %P, ", pt, frptofchar(w, framepos - w->org +1)); */
		/* print("%d %d, ", framepos - w->org, framepos - w->org + framelen); */
		/* frsetcol(w, s->fg, s->bg); */
        /* frdrawsel(w, pt, framepos - w->org, framepos - w->org + framelen, 0); */
        frdrawsel0(w, pt, framepos - w->org, framepos - w->org + framelen, (s->bg), (s->fg));
    }
}
