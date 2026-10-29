/*
 * tab_get — fetch a cell value by column name.
 *
 * Walks the row's ndb tuple chain looking for a tuple whose `attr`
 * matches `col`.  Returns the cell text verbatim — base64 decoding,
 * unsealing, and signature verification come in later steps once
 * the crypto column types arrive.
 */

#include <tab_internal.h>

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

/*
 * Refuse a cell whose emitted line would not survive a reload.
 *
 * ndbparse reads one line at a time through Brdline into an 8 KiB Bio
 * buffer and, on a longer line, takes the truncated result as a clean
 * end of entry.  The effect is silent: commit writes the file correctly,
 * open reports no error, and the row comes back missing the cell.
 * Refusing at write time is what turns that into a visible failure.
 *
 * The measurement is of the encoded value, not the caller's string.
 * tab_text_wire_encode expands '&' to "&amp;", so a check against
 * strlen() would pass a value that is five times longer on disk.  This
 * calls the same encoder the emitter does rather than estimating a
 * worst-case multiplier.
 */
int
tab_check_cell_size(const char *col, const char *val, const char *who)
{
	char *wire;
	int len, framing;

	if(val == nil)
		return 0;

	if(tab_text_needs_encoding(val)){
		wire = tab_text_wire_encode(val);
		if(wire == nil)
			return -1;	/* encoder set errstr */
		len = strlen(wire);
		if(tab_text_needs_quote(wire))
			len += 2;
		free(wire);
	}else{
		len = strlen(val);
		if(tab_text_needs_quote(val))
			len += 2;
	}

	/* Leading tab, "attr", '=', trailing newline. */
	framing = 1 + (col != nil ? (int)strlen(col) : 0) + 1 + 1;

	if(len + framing > TabMaxCell){
		tab_seterror("%s: cell %q is %d bytes encoded, over the %d-byte "
			"limit; store a value this large as a file and keep its "
			"path or digest in the cell",
			who, col != nil ? col : "(value)",
			len + framing, TabMaxCell);
		return -1;
	}
	return 0;
}
