/*
 * tab_get — fetch a cell value by column name.
 *
 * Walks the row's ndb tuple chain looking for a tuple whose `attr`
 * matches `col`.  Returns the cell text verbatim — base64 decoding,
 * unsealing, and signature verification come in later steps once
 * the crypto column types arrive.
 */

#include "tab_internal.h"

Ndbtuple *
tab_row_tuple(Ndbtuple *head, const char *col)
{
	Ndbtuple *t;

	if(head == nil || col == nil)
		return nil;
	for(t = head; t != nil; t = t->entry){
		if(strcmp(t->attr, col) == 0)
			return t;
	}
	return nil;
}

const char *
tab_row_cell(Ndbtuple *head, const char *col)
{
	Ndbtuple *t;

	t = tab_row_tuple(head, col);
	return t != nil ? t->val : nil;
}

int
tab_tuple_is_nil(Ndbtuple *t)
{
	return t == nil || (t->ptr & TabTupleNil) != 0;
}

void
tab_tuple_mark_nil(Ndbtuple *t, int is_nil)
{
	if(t == nil)
		return;
	if(is_nil)
		t->ptr |= TabTupleNil;
	else
		t->ptr &= ~TabTupleNil;
}

const char *
tab_get(TabRow *r, const char *col)
{
	Ndbtuple *t;

	if(r == nil)
		return nil;
	t = tab_row_tuple(r->chain, col);
	return tab_tuple_is_nil(t) ? nil : t->val;
}
