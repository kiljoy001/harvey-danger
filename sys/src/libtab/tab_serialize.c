/*
 * libtab — serialise an in-memory Tab back to ndb-shaped text.
 *
 * Emits, in this order:
 *
 *	schema=<name>
 *	\tcol=<col0_name>  [type=<col0_type>]  [<k>=<v>]...
 *	\tcol=<col1_name>  [type=<col1_type>]  [<k>=<v>]...
 *	                                          ← blank line
 *	<head_attr>=<head_val>                   ← first row, head on left margin
 *	\t<attr>=<val>
 *	\t<attr>=<val>
 *	                                          ← blank line
 *	<head_attr>=<head_val>                   ← next row
 *	...
 *
 * Rows are emitted in source order (the t->rows[] order, which is the
 * order tab_open observed them — minus any dedup'd duplicates).  Each
 * row's head is its first tuple in chain order; remaining tuples
 * indent with one '\t'.  Values that are empty strings emit as
 * `attr=` with nothing after.  Plain-text row values use native ndb
 * quoting when that is enough, and HTML-style entities when native ndb
 * cannot safely represent the bytes.
 *
 * Comments on disk are lost on round-trip — libndb's parser doesn't
 * preserve them.  Schemas with no rows still emit the schema= entry
 * plus the trailing blank line.
 */

#include <tab_internal.h>

/* Grow a buffer in-place; appends `n` bytes from `p` to `*buf` (which
 * has current length `*len` and capacity `*cap`).  Reallocates as
 * needed.  Returns 0 on success, -1 on OOM. */
static int
buf_append(char **buf, int *len, int *cap, const char *p, int n)
{
	int need = *len + n;
	if(need > *cap){
		int newcap = *cap > 0 ? *cap : 256;
		while(newcap < need)
			newcap *= 2;
		char *nb = realloc(*buf, newcap);
		if(nb == nil){
			tab_seterror("tab_serialize: out of memory (need %d)", need);
			return -1;
		}
		*buf = nb;
		*cap = newcap;
	}
	memcpy(*buf + *len, p, n);
	*len += n;
	return 0;
}

static int
buf_append_c(char **buf, int *len, int *cap, char c)
{
	return buf_append(buf, len, cap, &c, 1);
}

struct Sbuf {
	char **buf;
	int *len;
	int *cap;
};

static int
emit_to_sbuf(void *arg, const char *p, int n)
{
	struct Sbuf *s;

	s = arg;
	return buf_append(s->buf, s->len, s->cap, p, n);
}

/* Build the full serialised text into a freshly-allocated NUL-
 * terminated buffer.  Returns the buffer (caller frees) and writes
 * its length-without-the-terminator into *outlen.  Returns nil on
 * OOM. */
char *
tab_serialize(Tab *t, int *outlen)
{
	char *buf = nil;
	int len = 0, cap = 0;
	int i;
	struct Sbuf sbuf;

	tab_clearerror();
	if(t == nil){
		tab_seterror("tab_serialize: nil Tab");
		return nil;
	}

	sbuf.buf = &buf;
	sbuf.len = &len;
	sbuf.cap = &cap;

	if(tab_emit_schema(&t->schema, emit_to_sbuf, &sbuf) < 0)
		goto fail;
	if(buf_append_c(&buf, &len, &cap, '\n') < 0)
		goto fail;

	for(i = 0; i < t->nrows; i++){
		if(tab_row_is_nil(t, t->rows[i]))
			continue;
		if(tab_emit_row(t->rows[i]->chain, emit_to_sbuf, &sbuf) < 0)
			goto fail;
		if(buf_append_c(&buf, &len, &cap, '\n') < 0)
			goto fail;
	}

	/* NUL-terminate for callers that want to treat it as a C string;
	 * outlen does not include the terminator. */
	if(buf_append_c(&buf, &len, &cap, 0) < 0)
		goto fail;
	*outlen = len - 1;
	return buf;

fail:
	free(buf);
	return nil;
}
