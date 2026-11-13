/*
 * This file is part of the UCB release of Plan 9. It is subject to the license
 * terms in the LICENSE file found in the top-level directory of this
 * distribution and at http://akaros.cs.berkeley.edu/files/Plan9License. No
 * part of the UCB release of Plan 9, including this file, may be copied,
 * modified, propagated, or distributed except according to the terms contained
 * in the LICENSE file.
 */

#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"

/* Segment type from portdat.h */
char *segtypes[SG_TYPE] = {
	[SG_BAD0] = "Bad0",
	[SG_TEXT] = "Text",
	[SG_DATA] = "Data",
	[SG_BSS] = "Bss",
	[SG_STACK] = "Stack",
	[SG_SHARED] = "Shared",
	[SG_PHYSICAL] = "Phys",
	[SG_LOAD] = "Load"};

u64
segppn(Segment *s, u64 pa)
{
	u64 pgsz;

	pgsz = sys->pgsz[s->pgszi];
	pa &= ~(pgsz - 1);
	return pa;
}

/*
 * size is the number of pages, and the page size is pgsz.  Segment.size
 * and Segment.top use the same unit, so a 4 KiB mmap segment really is
 * four kilobytes long.  Most callers still use BIGPGSZ (the historical
 * Plan 9 segment unit); SG_MMAP uses PGSZ so a small mapping does not
 * reserve 2 MiB of address space or fault in a 2 MiB page.
 */
Segment *
newsegsz(int type, usize base, u64 size, u32 pgsz)
{
	Segment *s;
	int mapsize, ptepertab;

	if(pgsz == 0 || getpgszi(pgsz) < 0)
		panic("newsegsz: bad page size %d", pgsz);

	/*
	 * Keep one PTE table to roughly one page of pointers: 512 entries
	 * is the old 1 GiB/BIGPGSZ table and becomes 2 MiB when the segment
	 * uses 4 KiB pages.
	 */
	ptepertab = PTEMAPMEM / BIGPGSZ;
	if(size > SEGMAXPTE * ptepertab)
		error(Enovmem);

	s = smalloc(sizeof(Segment));
	s->ptepertab = ptepertab;
	s->ptemapmem = ptepertab * pgsz;
	s->r.ref = 1;
	s->type = type;
	s->base = base;
	s->top = base + (size * pgsz);
	s->size = size;
	s->pgszi = getpgszi(pgsz);
	s->sema.prev = &s->sema;
	s->sema.next = &s->sema;
	s->color = NOCOLOR;

	mapsize = HOWMANY(size, s->ptepertab);
	if(mapsize > SEGMAXPTE)
		error(Enovmem);
	if(mapsize > nelem(s->ssegmap)){
		s->map = smalloc(mapsize * sizeof(Pte *));
		s->mapsize = mapsize;
	} else {
		s->map = s->ssegmap;
		s->mapsize = nelem(s->ssegmap);
	}

	return s;
}

Segment *
newseg(int type, usize base, u64 size)
{
	return newsegsz(type, base, size, BIGPGSZ);
}

void
putseg(Segment *s)
{
	Pte **pp, **emap;
	Image *i;
	extern void freezseg(Segment *);

	if(s == 0)
		return;

	i = s->image;
	if(i != 0){
		lock(&i->r.l);
		lock(&s->r.l);
		if(i->s == s && s->r.ref == 1)
			i->s = 0;
		unlock(&i->r.l);
	} else
		lock(&s->r.l);

	s->r.ref--;
	if(s->r.ref != 0){
		unlock(&s->r.l);
		return;
	}
	unlock(&s->r.l);

	qlock(&s->lk);
	if(i)
		putimage(i);

	emap = &s->map[s->mapsize];
	for(pp = s->map; pp < emap; pp++)
		if(*pp)
			freepte(s, *pp);

	qunlock(&s->lk);
	if(s->map != s->ssegmap)
		free(s->map);
	if(s->profile != 0)
		free(s->profile);
	if(s->type & SG_ZIO)
		freezseg(s);
	free(s);
}

void
relocateseg(Segment *s, usize offset)
{
	Page **pg, *x;
	Pte *pte, **p, **endpte;

	endpte = &s->map[s->mapsize];
	for(p = s->map; p < endpte; p++){
		if(*p == 0)
			continue;
		pte = *p;
		for(pg = pte->first; pg <= pte->last; pg++){
			if((x = *pg) != nil)
				x->va += offset;
		}
	}
}

Segment *
dupseg(Segment **seg, int segno, int share)
{
	Proc *up = externup();
	int i, size;
	Pte *pte;
	Segment *n, *s;

	SET(n);
	s = seg[segno];

	qlock(&s->lk);
	if(waserror()){
		qunlock(&s->lk);
		nexterror();
	}
	switch(s->type & SG_TYPE){
	case SG_TEXT: /* New segment shares pte set */
	case SG_SHARED:
	case SG_PHYSICAL:
		goto sameseg;

	case SG_MMAP:
		if(share)
			goto sameseg;
		n = newsegsz(s->type, s->base, s->size, sys->pgsz[s->pgszi]);
		if(s->image != nil){
			incref(&s->image->r);
			n->image = s->image;
			n->ldseg = s->ldseg;
		}
		n->pgszi = s->pgszi;
		n->color = s->color;
		n->ptepertab = s->ptepertab;
		n->ptemapmem = s->ptemapmem;
		break;

	case SG_STACK:
		n = newsegsz(s->type, s->base, s->size, sys->pgsz[s->pgszi]);
		break;

	case SG_BSS: /* Just copy on write */
		if(share)
			goto sameseg;
		n = newsegsz(s->type, s->base, s->size, sys->pgsz[s->pgszi]);
		break;

	case SG_LOAD:
		if((s->type & SG_EXEC) != 0 && (s->type & SG_WRITE) == 0)
			goto sameseg;
	case SG_DATA: /* Copy on write plus demand load info */
		if((s->type & SG_EXEC) != 0){
			poperror();
			qunlock(&s->lk);
			return data2txt(s);
		}

		if(share)
			goto sameseg;
		n = newsegsz(s->type, s->base, s->size, sys->pgsz[s->pgszi]);

		incref(&s->image->r);
		n->image = s->image;
		n->ldseg = s->ldseg;
		n->pgszi = s->pgszi;
		n->color = s->color;
		n->ptepertab = s->ptepertab;
		break;
	}
	size = s->mapsize;
	for(i = 0; i < size; i++)
		if((pte = s->map[i]) != nil)
			n->map[i] = ptecpy(n, pte);

	n->flushme = s->flushme;
	if(s->r.ref > 1)
		procflushseg(s);
	poperror();
	qunlock(&s->lk);
	return n;

sameseg:
	incref(&s->r);
	poperror();
	qunlock(&s->lk);
	return s;
}

QLock segtablock;

/*
 * Exited processes point at this empty table so that a pager pass can still
 * take a valid seglock without racing a table teardown.
 */
static Segtab deadsegtab;

Segtab *
segtabempty(void)
{
	return &deadsegtab;
}

/* Every process owns one segment table; it is freed when the process exits. */
Segtab *
newsegtab(void)
{
	Segtab *t;

	t = malloc(sizeof *t);
	if(t == nil)
		return nil;
	memset(t, 0, sizeof *t);
	t->r.ref = 1;
	return t;
}

void
segtabput(Segtab *t)
{
	int i;

	if(t == nil)
		return;
	if(decref(&t->r) != 0)
		return;
	for(i = 0; i < NSEG; i++)
		if(t->seg[i] != nil)
			putseg(t->seg[i]);
	free(t);
}

/*
 * RFMEM address-space groups.  Every process has its own segment table, so
 * dupseg() can keep giving each one a private stack; the group is only used
 * to propagate a mapping made later to the other threads.
 */
QLock memgrplock;

/* memgrplock is held */
int
memgrpjoin(Proc *p, Proc *leader)
{
	Memgrp *g;

	g = leader->memgrp;
	if(g == nil){
		g = malloc(sizeof *g);
		if(g == nil)
			return -1;
		g->mem = leader;
		leader->memnext = nil;
		leader->memgrp = g;
	}
	p->memgrp = g;
	p->memnext = g->mem;
	g->mem = p;
	return 0;
}

void
memgrpleave(Proc *p)
{
	Memgrp *g;
	Proc **l, *m;

	qlock(&memgrplock);
	if((g = p->memgrp) != nil){
		for(l = &g->mem; (m = *l) != nil; l = &m->memnext)
			if(m == p){
				*l = p->memnext;
				break;
			}
		p->memgrp = nil;
		p->memnext = nil;
		if(g->mem == nil)
			free(g);
	}
	qunlock(&memgrplock);
}

/*
 * Install s at slot sno in p and every member of its group.  The caller
 * holds memgrplock and p->seglock; each member's seglock is taken in turn.
 */
void
memgrpset(Proc *p, int sno, Segment *s)
{
	Memgrp *g;
	Proc *m;

	if((g = p->memgrp) == nil)
		return;
	for(m = g->mem; m != nil; m = m->memnext){
		if(m == p)
			continue;
		qlock(m->seglock);
		if(m->seg[sno] == nil){
			m->seg[sno] = s;
			incref(&s->r);
		}else
			print("memgrpset: slot %d busy in pid %d\n", sno, m->pid);
		qunlock(m->seglock);
	}
}

/*
 * Remove the segment in slot sno from p and every member of its group.
 * The caller holds memgrplock and p->seglock.
 */
void
memgrpclr(Proc *p, int sno)
{
	Memgrp *g;
	Proc *m;
	Segment *s;
	int dropped;

	if((s = p->seg[sno]) == nil)
		return;
	p->seg[sno] = nil;
	if((g = p->memgrp) != nil){
		for(m = g->mem; m != nil; m = m->memnext){
			if(m == p)
				continue;
			dropped = 0;
			qlock(m->seglock);
			if(m->seg[sno] == s){
				m->seg[sno] = nil;
				dropped = 1;
			}
			qunlock(m->seglock);
			if(dropped)
				putseg(s);
		}
	}
	putseg(s);
}

void
segpage(Segment *s, Page *p)
{
	Pte **pte;
	usize soff;
	u64 pgsz;
	Page **pg;

	if(s->pgszi < 0)
		s->pgszi = p->pgszi;
	if(s->color == NOCOLOR)
		s->color = p->color;
	if(s->pgszi != p->pgszi){
		iprint("s %p s->pgszi %d p %p p->pgszi %p\n", s, s->pgszi, p, p->pgszi);
		panic("segpage: s->pgszi != p->pgszi");
	}

	if(p->va < s->base || p->va >= s->top)
		panic("segpage: p->va < s->base || p->va >= s->top");

	soff = p->va - s->base;
	pte = &s->map[soff / s->ptemapmem];
	if(*pte == 0)
		*pte = ptealloc(s);
	pgsz = sys->pgsz[s->pgszi];
	pg = &(*pte)->pages[(soff & (s->ptemapmem - 1)) / pgsz];
	*pg = p;
	if(pg < (*pte)->first)
		(*pte)->first = pg;
	if(pg > (*pte)->last)
		(*pte)->last = pg;
}

/*
 *  called with s->lk locked
 */
void
mfreeseg(Segment *s, usize start, int pages)
{
	int i, j, size;
	usize soff;
	u64 pgsz;
	Page *pg;
	Page *list;

	pgsz = sys->pgsz[s->pgszi];
	soff = start - s->base;
	j = (soff & (s->ptemapmem - 1)) / pgsz;

	size = s->mapsize;
	list = nil;
	for(i = soff / s->ptemapmem; i < size; i++){
		if(pages <= 0)
			break;
		if(s->map[i] == 0){
			pages -= s->ptepertab - j;
			j = 0;
			continue;
		}
		while(j < s->ptepertab){
			pg = s->map[i]->pages[j];
			/*
			 * We want to zero s->map[i]->page[j] and putpage(pg),
			 * but we have to make sure other processors flush the
			 * entry from their TLBs before the page is freed.
			 * We construct a list of the pages to be freed, zero
			 * the entries, then (below) call procflushseg, and call
			 * putpage on the whole list.
			 *
			 * Swapped-out pages don't appear in TLBs, so it's okay
			 * to putswap those pages before procflushseg.
			 */
			if(pg){
				if(onswap(pg))
					putswap(pg);
				else {
					pg->next = list;
					list = pg;
				}
				s->map[i]->pages[j] = 0;
			}
			if(--pages == 0)
				goto out;
			j++;
		}
		j = 0;
	}
out:
	/* flush this seg in all other processes */
	if(s->r.ref > 1)
		procflushseg(s);

	/* free the pages */
	for(pg = list; pg != nil; pg = list){
		list = list->next;
		putpage(pg);
	}
}

Segment *
isoverlap(Proc *p, usize va, usize len)
{
	int i;
	Segment *ns;
	usize newtop;

	newtop = va + len;
	for(i = 0; i < NSEG; i++){
		ns = p->seg[i];
		if(ns == 0)
			continue;
		if((newtop > ns->base && newtop <= ns->top) ||
		   (va >= ns->base && va < ns->top))
			return ns;
	}
	return nil;
}

void
segclock(usize pc)
{
	Proc *up = externup();
	Segment *s;
	int sno;

	for(sno = 0; sno < NSEG; sno++){
		s = up->seg[sno];
		if(s == nil)
			continue;
		if((s->type & SG_EXEC) == 0 || s->profile == 0)
			continue;
		s->profile[0] += TK2MS(1);
		if(pc >= s->base && pc < s->top){
			pc -= s->base;
			s->profile[pc >> LRESPROF] += TK2MS(1);
		}
	}
}

static void
prepageseg(int i)
{
	Proc *up = externup();
	Segment *s;
	usize addr, pgsz;

	s = up->seg[i];
	if(s == nil)
		return;
	DBG("prepage: base %#p top %#p\n", s->base, s->top);
	pgsz = sys->pgsz[s->pgszi];
	for(addr = s->base; addr < s->top; addr += pgsz)
		fault(addr, -1, (s->type & SG_WRITE) ? FT_WRITE : FT_READ);
}

/*
 * BUG: should depend only in segment attributes, not in
 * the slot used in up->seg.
 */
void
nixprepage(int i)
{
	if(i >= 0)
		prepageseg(i);
	else
		for(i = 0; i < NSEG; i++)
			prepageseg(i);
}
