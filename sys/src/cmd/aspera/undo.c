/**
Support file for handling history - undo, location and command.

Copyright (C) 2025  Anand Tamariya

Author: Anand Tamariya <anand@gmail.com>
Keywords: plan9, history

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.

*/

#include <u.h>
#include <libc.h>
#include <complete.h>
#include "undo.h"

enum {
  INSERT, DELETE,
  DefaultMaxStates = 100
};

static Rune*
makecopy(const Rune *data, int datalen)
{
	Rune *copy;
	
	if(data == nil || datalen <= 0)
		return nil;
	
	copy = malloc(datalen * sizeof(Rune));
	if(copy == nil)
		return nil;
	
	memmove(copy, data, datalen * sizeof(Rune));
	return copy;
}

UndoStack*
undostackcreate(int maxstates)
{
	UndoStack *stack;
	
	stack = malloc(sizeof(UndoStack));
	if(stack == nil)
		return nil;
	
	memset(stack, 0, sizeof(UndoStack));
	stack->maxstates = maxstates > 0 ? maxstates : DefaultMaxStates;
	/* Dummy first entry */
	undoaddstate(stack, INSERT, nil, 0, 0);

	return stack;
}

void
undostackfree(UndoStack *stack)
{
	UndoState *state, *next;
	
	if(stack == nil)
		return;
	
	for(state = stack->head; state != nil; state = next) {
		next = state->next;
		free(state->data);
		free(state);
	}
	
	free(stack);
}

void undopause(UndoStack *stack) {
  if (stack) stack->pause = 1;
}

void undoresume(UndoStack *stack) {
  if (stack) stack->pause = 0;
}

int
undoaddstate(UndoStack *stack, int type, const Rune *data, int datalen, int cursorpos)
{
	UndoState *state, *tofree;
	
	if(stack == nil || stack->pause)
		return -1;
	
	state = malloc(sizeof(UndoState));
	if(state == nil)
		return -1;
	
	state->data = makecopy(data, datalen);
	state->datalen = datalen;
	state->cursorpos = cursorpos;
	state->type = type;
	state->next = nil;
	state->prev = nil;
	state->boundary = 0;
	
	if(stack->current != nil) {
		state->prev = stack->current;
		stack->current->next = state;
	}
	
	stack->current = state;
	stack->count++;
	stack->dirty = 1;
	
	if(stack->head == nil)
		stack->head = state;
	
	if(stack->current->next != nil) {
		for(tofree = stack->current->next; tofree != nil; ) {
			UndoState *next = tofree->next;
			free(tofree->data);
			free(tofree);
			tofree = next;
			stack->count--;
		}
		stack->current->next = nil;
	}
	
	while(stack->count > stack->maxstates && stack->head != stack->current) {
		tofree = stack->head;
		stack->head = tofree->next;
		if(stack->head != nil)
			stack->head->prev = nil;
		
		free(tofree->data);
		free(tofree);
		stack->count--;
	}
	/* fprint(2, "stack n=%d, ", stack->count); */
	
	return 0;
}

int
undoaddinsert(UndoStack *stack, const Rune *data, int datalen, int cursorpos)
{
 return undoaddstate(stack, INSERT, data, datalen, cursorpos);
}

int
undoadddelete(UndoStack *stack, const Rune *data, int datalen, int cursorpos)
{
 return undoaddstate(stack, DELETE, data, datalen, cursorpos);
}

int
undoaddfile(UndoStack *stack, const Rune *data, int datalen)
{
 UndoState *state;

 state = stack->current;
 if(state->datalen == datalen && state->data &&
	runestrncmp(data, state->data, state->datalen) == 0)
   return 0;
 return undoaddstate(stack, INSERT, data, datalen, 0);
}

void copydata(UndoState *state, Rune **data, int *datalen, int *cursorpos, int *type) {
	if(data != nil) {
		*data = makecopy(state->data, state->datalen);
		if(*data == nil && state->datalen > 0)
			return;
	}
	if(datalen != nil)
		*datalen = state->datalen;
	if(cursorpos != nil)
		*cursorpos = state->cursorpos;
	if(type != nil)
		*type = state->type;
}

void undoboundary(UndoStack *stack) {
  stack->current->boundary = stack->boundary++;
}

int undoisboundary(UndoStack *stack) {
  return stack->current->boundary > 0;
}

int
undoundo(UndoStack *stack, Rune **data, int *datalen, int *cursorpos, int *type)
{
	if(stack == nil || stack->current == nil || stack->current->prev == nil)
		return -1;

	copydata(stack->current, data, datalen, cursorpos, type);
	stack->current = stack->current->prev;
	
	return 0;
}

int
undoredo(UndoStack *stack, Rune **data, int *datalen, int *cursorpos, int *type)
{
	if(stack == nil || stack->current == nil || stack->current->next == nil)
		return -1;
	
	stack->current = stack->current->next;
	copydata(stack->current, data, datalen, cursorpos, type);
	
	return 0;
}

int
undocanundo(UndoStack *stack)
{
	return (stack != nil && stack->current != nil && stack->current->prev != nil);
}

int
undocanredo(UndoStack *stack)
{
	return (stack != nil && stack->current != nil && stack->current->next != nil);
}

void
undoclear(UndoStack *stack)
{
	UndoState *state, *next;
	
	if(stack == nil)
		return;
	
	for(state = stack->head; state != nil; state = next) {
		next = state->next;
		free(state->data);
		free(state);
	}
	
	stack->head = nil;
	stack->current = nil;
	stack->count = 0;
}

void
undolist(UndoStack *stack, Completion *c)
{
	UndoState *state;
	char *s;
	
	c->nfile = 0;
	if(stack == nil && stack->count > 1)
		return;
	
	c->filename = malloc(stack->count * sizeof(char*));
	/* Reverse chronological order */
	for(state = stack->current; state->prev != nil; state = state->prev) {
	  s = malloc(state->datalen + 1);
	  snprint(s, state->datalen + 1, "%S", state->data);
	  /* fprint(2, "%d %S, ", state->datalen, s); */
	  c->filename[c->nfile] = s;
	  c->nfile++;
	}
	c->nmatch = c->nfile > 0;
}

void
undofileload(UndoStack *stack, char *name)
{
	int fd, chunk, i, n, nrune;
	char buf[1024], *data, *p;
	Rune runes[1024];
  
	if(stack == nil)
		return;

	fd = open(name, OREAD);
	if (fd < 0) {
	  return;
	}

	chunk = sizeof(buf);
	while((n = read(fd, buf, chunk)) > 0) {
	  p = data = buf;
	  while (n > p - buf) {
		i = 0;
		for(p = data; *p != '\n' && (n > p - buf); p += nrune) {
		  nrune = chartorune(&runes[i], p);
		  i++;
		}
		if (i) {
		  runes[i] = L'\0';
		  /* fprint(2, "%d %d %.*S, ", n, i, i, runes); */
		  undoaddstate(stack, INSERT, runes, i, 0);
		}
		data = p + 1;
	  }
	}
	
	close(fd);
}

void
undofilesave(UndoStack *stack, char *name)
{
	UndoState *state;
	int fd;
  
	if(stack == nil && stack->count > 1 && stack->dirty)
		return;

	fd = create(name, OWRITE, 0666);
	if (fd < 0) {
	  return;
	}
	seek(fd, 0, 2);

	/* First line is the head - that's also the reading order */
	for(state = stack->head->next; state != nil; state = state->next) {
	  fprint(fd, "%.*S\n", state->datalen, state->data);
	  /* fprint(2, "%.*S\n", state->datalen, state->data); */
	}
	stack->dirty = 0;
	close(fd);
}
