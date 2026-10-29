/*
 * Tests for libtab, run against the library directly rather than through a
 * language binding. Every merged fix has a case here: the semantic-nil model,
 * cell mutation, row removal, and hash-bucket growth.
 *
 *	mk test
 */
#include <tab_internal.h>

static int failures;
static int checks;

static void
ok(int cond, char *what)
{
	checks++;
	if(cond)
		return;
	failures++;
	fprint(2, "FAIL: %s\n", what);
}

static void
okstr(const char *got, const char *want, char *what)
{
	checks++;
	if(got == nil)
		got = "(nil)";
	if(want == nil)
		want = "(nil)";
	if(strcmp((char*)got, (char*)want) == 0)
		return;
	failures++;
	fprint(2, "FAIL: %s: got %q want %q\n", what, got, want);
}

/* find returns the first row whose col equals value, or nil. */
static TabRow*
find(Tab *t, const char *col, const char *value)
{
	TabIter *it;
	TabRow *r;

	it = tab_search(t, col, value);
	if(it == nil)
		return nil;
	r = tab_iter_next(it);
	tab_iter_close(it);
	return r;
}

static Tab*
fresh(char *path, int ncols)
{
	TabColSpec cols[3];

	memset(cols, 0, sizeof cols);
	cols[0].name = "k";
	cols[1].name = "v";
	cols[2].name = "w";

	remove(path);
	return tab_create(path, "test", cols, ncols);
}

static int
write_text_file(char *path, char *text)
{
	int fd;

	remove(path);
	fd = create(path, OWRITE, 0644);
	if(fd < 0)
		return -1;
	if(write(fd, text, strlen(text)) != strlen(text)){
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

static int
count_matches(Tab *t, const char *col, const char *value)
{
	TabIter *it;
	int n;

	it = tab_search(t, col, value);
	if(it == nil)
		return -1;
	n = 0;
	while(tab_iter_next(it) != nil)
		n++;
	tab_iter_close(it);
	return n;
}

static int
files_equal(char *a, char *b)
{
	int fa, fb, same;
	long na, nb;
	char ba[8192], bb[8192];

	fa = open(a, OREAD);
	if(fa < 0)
		return 0;
	fb = open(b, OREAD);
	if(fb < 0){
		close(fa);
		return 0;
	}
	same = 1;
	for(;;){
		na = read(fa, ba, sizeof ba);
		nb = read(fb, bb, sizeof bb);
		if(na != nb || na < 0 || nb < 0){
			same = 0;
			break;
		}
		if(na == 0)
			break;
		if(memcmp(ba, bb, na) != 0){
			same = 0;
			break;
		}
	}
	close(fa);
	close(fb);
	return same;
}

static int
file_contains(char *path, char *needle)
{
	int fd;
	long n;
	char buf[16384];

	fd = open(path, OREAD);
	if(fd < 0)
		return 0;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if(n < 0)
		return 0;
	buf[n] = 0;
	return strstr(buf, needle) != nil;
}

/* A committed table must reload with the same cells. */
static void
test_roundtrip(void)
{
	Tab *t;
	TabRow *r;
	char *path = "/tmp/lt_roundtrip.tab";

	t = fresh(path, 2);
	ok(t != nil, "create table");
	if(t == nil)
		return;

	r = tab_add_row(t, "k", "alpha");
	ok(r != nil, "add row");
	ok(tab_set(t, r, "v", "one") == 0, "set cell");
	ok(tab_commit(t) == 0, "commit");
	tab_close(t);

	t = tab_open(path);
	ok(t != nil, "reopen");
	if(t == nil)
		return;
	r = find(t, "k", "alpha");
	ok(r != nil, "find row after reopen");
	if(r != nil)
		okstr(tab_get(r, "v"), "one", "cell survives commit");
	tab_close(t);
	remove(path);
}

/* tab_set replaces one cell and leaves its siblings alone. */
static void
test_set_replaces_one_cell(void)
{
	Tab *t;
	TabRow *a, *b;
	char *path = "/tmp/lt_set.tab";

	t = fresh(path, 2);
	if(t == nil){
		ok(0, "create for set test");
		return;
	}
	a = tab_add_row(t, "k", "first");
	b = tab_add_row(t, "k", "second");
	tab_set(t, a, "v", "A");
	tab_set(t, b, "v", "B");
	tab_commit(t);

	ok(tab_set(t, a, "v", "CHANGED") == 0, "set existing cell");
	tab_commit(t);
	tab_close(t);

	t = tab_open(path);
	if(t == nil){
		ok(0, "reopen after set");
		return;
	}
	a = find(t, "k", "first");
	b = find(t, "k", "second");
	okstr(tab_get(a, "v"), "CHANGED", "target cell updated");
	okstr(tab_get(b, "v"), "B", "sibling cell untouched");
	tab_close(t);
	remove(path);
}

/* Missing cells and explicit nil cells are semantic nil. */
static void
test_nil_semantics(void)
{
	Tab *t;
	TabRow *r;
	char *path = "/tmp/lt_nil.tab";

	ok(tab_cell_is_nil(nil) != 0, "null pointer is nil");
	ok(tab_cell_is_nil("nil") == 0, "literal nil is ordinary text");
	ok(tab_cell_is_nil("value") == 0, "ordinary value is not nil");

	t = fresh(path, 3);
	if(t == nil){
		ok(0, "create for nil test");
		return;
	}
	r = tab_add_row(t, "k", "row");
	tab_set(t, r, "v", "set");
	/* w is never set, so it reads as nil rather than empty. */
	ok(tab_get(r, "w") == nil, "unset cell reads as nil");
	ok(tab_set(t, r, "v", nil) == 0, "set nil cell");
	ok(tab_get(r, "v") == nil, "explicit nil cell reads as nil");
	tab_close(t);
	remove(path);
}

/* Plain text values that native ndb cannot carry must round-trip. */
static void
test_text_values_roundtrip(void)
{
	struct {
		char *key;
		char *value;
	} cases[] = {
		{"space", "Divine Comedy"},
		{"quote", "The Number \"e\""},
		{"newline", "line one\nline two"},
		{"literal-nil", "nil"},
		{"nil", "head value can be nil"},
		{"prefix", "__libtab_text64_v1:not-user-visible"},
		{"amp", "AT&T"},
		{"hash", "#not-a-comment"},
	};
	Tab *t;
	TabRow *r;
	char *path = "/tmp/lt_text.tab";
	int i, ncases;

	t = fresh(path, 2);
	if(t == nil){
		ok(0, "create for text roundtrip");
		return;
	}
	ncases = sizeof cases / sizeof cases[0];
	for(i = 0; i < ncases; i++){
		r = tab_add_row(t, "k", cases[i].key);
		ok(r != nil, "add text row");
		if(r != nil)
			ok(tab_set(t, r, "v", cases[i].value) == 0,
				"set text value");
	}
	r = tab_add_row(t, "k", "bare");
	ok(r != nil, "add bare text row");

	ok(tab_commit(t) == 0, "commit text values");
	tab_close(t);

	t = tab_open(path);
	ok(t != nil, "reopen text values");
	if(t == nil)
		return;
	for(i = 0; i < ncases; i++){
		r = find(t, "k", cases[i].key);
		ok(r != nil, "find text row after reopen");
		if(r != nil)
			okstr(tab_get(r, "v"), cases[i].value,
				"text value survives commit");
	}
	ok(count_matches(t, "v", "nil") == 1,
		"literal nil is searchable as text");
	ok(count_matches(t, "v", nil) == 1,
		"missing value is searchable as nil");
	tab_close(t);

	ok(file_contains(path, "The Number &quot;e&quot;"),
		"quote is written as readable entity");
	ok(file_contains(path, "line one&#10;line two"),
		"newline is written as readable entity");
	ok(file_contains(path, "n&#105;l"),
		"literal nil is written as readable entity");
	ok(file_contains(path, "AT&amp;T"),
		"ampersand is written as readable entity");
	ok(file_contains(path, "__libtab_text64_v1&#58;not-user-visible"),
		"legacy text64 prefix is escaped readably");
	remove(path);
}

/* Existing files that wrote raw nil still load as semantic nil. */
static void
test_legacy_raw_nil_is_nil(void)
{
	Tab *t;
	TabRow *r;
	char *path = "/tmp/lt_legacy_nil.tab";
	char *text =
		"schema=test\n"
		"\tcol=k\n"
		"\tcol=v\n"
		"\n"
		"k=row\n"
		"\tv=nil\n"
		"\n";

	if(write_text_file(path, text) < 0){
		ok(0, "write legacy nil fixture");
		return;
	}
	t = tab_open(path);
	ok(t != nil, "open legacy nil fixture");
	if(t == nil)
		return;
	r = find(t, "k", "row");
	ok(r != nil, "find legacy nil row");
	if(r != nil)
		ok(tab_get(r, "v") == nil, "legacy raw nil reads as nil");
	tab_close(t);
	remove(path);
}

/* The previous text64 escape hatch remains supported for compatibility. */
static void
test_legacy_text64_still_decodes(void)
{
	Tab *t;
	TabRow *r;
	char *path = "/tmp/lt_legacy_text64.tab";
	char *text =
		"schema=test\n"
		"\tcol=k\n"
		"\tcol=v\n"
		"\n"
		"k=row\n"
		"\tv=__libtab_text64_v1:bGlidGFiLXRleHQtdjE6VGhlIE51bWJlciAiZSI=\n"
		"\n";

	if(write_text_file(path, text) < 0){
		ok(0, "write legacy text64 fixture");
		return;
	}
	t = tab_open(path);
	ok(t != nil, "open legacy text64 fixture");
	if(t == nil)
		return;
	r = find(t, "k", "row");
	ok(r != nil, "find legacy text64 row");
	if(r != nil)
		okstr(tab_get(r, "v"), "The Number \"e\"",
			"legacy text64 decodes");
	tab_close(t);
	remove(path);
}

/* The streaming writer emits rows incrementally but must read back normally. */
static void
test_stream_writer_roundtrip(void)
{
	TabWriter *w;
	Tab *t;
	TabRow *r;
	TabColSpec cols[3];
	char *path = "/tmp/lt_writer.tab";

	memset(cols, 0, sizeof cols);
	cols[0].name = "k";
	cols[1].name = "v";
	cols[2].name = "w";

	remove(path);
	w = tab_writer_create(path, "test", cols, 3);
	ok(w != nil, "create stream writer");
	if(w == nil)
		return;

	ok(tab_writer_add_row(w, "k", "space") == 0, "writer add space row");
	ok(tab_writer_set(w, "v", "Divine Comedy") == 0, "writer set space");
	ok(tab_writer_set(w, "w", "nil") == 0, "writer set literal nil");

	ok(tab_writer_add_row(w, "k", "quote") == 0, "writer add quote row");
	ok(tab_writer_set(w, "v", "The Number \"e\"") == 0, "writer set quote");

	ok(tab_writer_add_row(w, "k", "clear") == 0, "writer add clear row");
	ok(tab_writer_set(w, "v", "gone") == 0, "writer set before clear");
	ok(tab_writer_clear(w, "v") == 0, "writer clear value");

	ok(tab_writer_add_row(w, "k", "space") == 0, "writer add duplicate row");
	ok(tab_writer_set(w, "v", "Divine Comedy") == 0, "writer duplicate value");
	ok(tab_writer_set(w, "w", "nil") == 0, "writer duplicate literal nil");

	ok(tab_writer_commit(w) == 0, "writer commit");
	tab_writer_close(w);

	t = tab_open(path);
	ok(t != nil, "open stream-written table");
	if(t == nil)
		return;

	ok(count_matches(t, "k", "space") == 1, "writer deduped duplicate row");
	r = find(t, "k", "space");
	ok(r != nil, "find stream space row");
	if(r != nil){
		okstr(tab_get(r, "v"), "Divine Comedy", "stream value with space");
		okstr(tab_get(r, "w"), "nil", "stream literal nil");
	}
	r = find(t, "k", "quote");
	ok(r != nil, "find stream quote row");
	if(r != nil)
		okstr(tab_get(r, "v"), "The Number \"e\"", "stream value with quote");
	r = find(t, "k", "clear");
	ok(r != nil, "find stream clear row");
	if(r != nil)
		ok(tab_get(r, "v") == nil, "stream clear persisted as nil");

	tab_close(t);
	remove(path);
}

/* For the same successful row sequence, streaming output must match commit. */
static void
test_stream_writer_matches_tab_commit(void)
{
	Tab *t;
	TabRow *r;
	TabWriter *w;
	TabColSpec cols[3];
	char *oldpath = "/tmp/lt_writer_old.tab";
	char *streampath = "/tmp/lt_writer_stream.tab";

	memset(cols, 0, sizeof cols);
	cols[0].name = "k";
	cols[1].name = "v";
	cols[2].name = "w";

	remove(oldpath);
	t = tab_create(oldpath, "test", cols, 3);
	ok(t != nil, "create parity table");
	if(t == nil)
		return;
	r = tab_add_row(t, "k", "space");
	tab_set(t, r, "v", "Divine Comedy");
	tab_set(t, r, "w", "nil");
	r = tab_add_row(t, "k", "quote");
	tab_set(t, r, "v", "The Number \"e\"");
	tab_set(t, r, "w", "line one\nline two");
	r = tab_add_row(t, "k", "nil");
	tab_set(t, r, "v", "head value can be nil");
	r = tab_add_row(t, "k", "clear");
	tab_set(t, r, "v", "gone");
	tab_clear(t, r, "v");
	ok(tab_commit(t) == 0, "commit parity table");
	tab_close(t);

	remove(streampath);
	w = tab_writer_create(streampath, "test", cols, 3);
	ok(w != nil, "create parity writer");
	if(w == nil){
		remove(oldpath);
		return;
	}
	tab_writer_add_row(w, "k", "space");
	tab_writer_set(w, "v", "Divine Comedy");
	tab_writer_set(w, "w", "nil");
	tab_writer_add_row(w, "k", "quote");
	tab_writer_set(w, "v", "The Number \"e\"");
	tab_writer_set(w, "w", "line one\nline two");
	tab_writer_add_row(w, "k", "nil");
	tab_writer_set(w, "v", "head value can be nil");
	tab_writer_add_row(w, "k", "clear");
	tab_writer_set(w, "v", "gone");
	tab_writer_clear(w, "v");
	ok(tab_writer_commit(w) == 0, "commit parity writer");
	tab_writer_close(w);

	ok(files_equal(oldpath, streampath),
		"stream writer output matches tab_commit output");
	ok(file_contains(oldpath, "The Number &quot;e&quot;"),
		"parity output writes quotes as entities");
	ok(file_contains(oldpath, "line one&#10;line two"),
		"parity output writes newlines as entities");
	ok(file_contains(oldpath, "n&#105;l"),
		"parity output writes literal nil as entity");
	remove(oldpath);
	remove(streampath);
}

/* tab_clear removes a cell; the row survives. */
static void
test_clear_cell(void)
{
	Tab *t;
	TabRow *r;
	char *path = "/tmp/lt_clear.tab";

	t = fresh(path, 2);
	if(t == nil){
		ok(0, "create for clear test");
		return;
	}
	r = tab_add_row(t, "k", "row");
	tab_set(t, r, "v", "present");
	tab_commit(t);

	ok(tab_clear(t, r, "v") == 0, "clear cell");
	tab_commit(t);
	tab_close(t);

	t = tab_open(path);
	if(t == nil){
		ok(0, "reopen after clear");
		return;
	}
	r = find(t, "k", "row");
	ok(r != nil, "row survives clearing a cell");
	if(r != nil)
		ok(tab_cell_is_nil(tab_get(r, "v")) != 0, "cleared cell reads nil");
	tab_close(t);
	remove(path);
}

/* Row identity is the head column, so duplicates are rejected. */
static void
test_duplicate_rejected(void)
{
	Tab *t;
	TabRow *first, *second;
	char *path = "/tmp/lt_dup.tab";

	t = fresh(path, 2);
	if(t == nil){
		ok(0, "create for duplicate test");
		return;
	}
	first = tab_add_row(t, "k", "same");
	ok(first != nil, "first row added");
	second = tab_add_row(t, "k", "same");
	ok(second == first || second == nil, "duplicate head value does not create a second row");
	tab_close(t);
	remove(path);
}

/*
 * Hash buckets must grow with the table.
 *
 * Before the growth fix a table built by tab_create kept its initial 16
 * buckets however many rows arrived, so every bucket walk became a full scan
 * with an allocation per step: 160k rows took 126s. This inserts enough rows
 * to cross several growth thresholds and asserts the time stays sane.
 */
static void
test_bucket_growth(void)
{
	Tab *t;
	char key[32];
	int i;
	vlong start, elapsed;
	char *path = "/tmp/lt_growth.tab";
	enum { Nrows = 20000 };

	t = fresh(path, 2);
	if(t == nil){
		ok(0, "create for growth test");
		return;
	}

	start = nsec();
	for(i = 0; i < Nrows; i++){
		snprint(key, sizeof key, "key%d", i);
		if(tab_add_row(t, "k", key) == nil){
			ok(0, "insert during growth test");
			tab_close(t);
			remove(path);
			return;
		}
	}
	elapsed = nsec() - start;

	/*
	 * tab_create establishes a canonical nil row that rows collapse into
	 * when emptied, so the index holds one more entry than we inserted.
	 */
	ok(t->nentries == Nrows + 1, "every row is indexed, plus the canonical nil row");
	/* With growth the bucket count tracks the row count. */
	ok(t->mask + 1 > 16, "bucket array grew past its initial size");

	/*
	 * Quadratic behaviour showed as seconds for tens of thousands of rows.
	 * Ten seconds is far above a healthy result and far below the old one.
	 */
	ok(elapsed < 10LL*1000*1000*1000, "20k inserts complete in under ten seconds");
	print("\t%d rows in %.3f s (%.1f us/row, %ud buckets)\n",
		Nrows, (double)elapsed/1e9, (double)elapsed/1000.0/Nrows, t->mask + 1);

	tab_close(t);
	remove(path);
}

/* Lookups must stay correct once the bucket array has been rehashed. */
static void
test_lookup_after_growth(void)
{
	Tab *t;
	TabRow *r;
	char key[32];
	int i;
	char *path = "/tmp/lt_lookup.tab";
	enum { Nrows = 5000 };

	t = fresh(path, 2);
	if(t == nil){
		ok(0, "create for lookup test");
		return;
	}
	for(i = 0; i < Nrows; i++){
		snprint(key, sizeof key, "key%d", i);
		tab_add_row(t, "k", key);
	}

	/* Every key must still be findable after the rehashes. */
	for(i = 0; i < Nrows; i += 97){
		snprint(key, sizeof key, "key%d", i);
		r = find(t, "k", key);
		if(r == nil){
			ok(0, "row lost after bucket growth");
			tab_close(t);
			remove(path);
			return;
		}
	}
	ok(1, "sampled rows all found after growth");

	r = find(t, "k", "absent");
	ok(r == nil, "missing key is not found");

	tab_close(t);
	remove(path);
}

/*
 * A schema with a bad column must be rejected.
 *
 * This exercises the error path free_col guards, but cannot observe the leak
 * itself: a functional test sees only that the open failed. Run this case
 * under valgrind to check the free.
 */
static void
test_rejects_bad_schema(void)
{
	Tab *t;
	char *path = "/tmp/lt_badschema.tab";
	int fd;
	char *text =
		"schema=test\n"
		"\tcol=k\n"
		"\tcol=k\n"		/* duplicate column */
		"\n";

	remove(path);
	fd = create(path, OWRITE, 0644);
	if(fd < 0){
		ok(0, "write malformed schema");
		return;
	}
	write(fd, text, strlen(text));
	close(fd);

	t = tab_open(path);
	ok(t == nil, "duplicate column is rejected");
	if(t != nil)
		tab_close(t);
	remove(path);
}

/*
 * Mutation cost of a full-table write.
 *
 * test_bucket_growth() only calls tab_add_row, so it exercises the bucket
 * array but never the rehash path -- reverting the two row-lookup fixes
 * leaves all its checks passing. A real writer sets cells too, and that is
 * where the remaining quadratic term lived:
 *
 *   tab_rowmap_rehash() took an Ndbtuple* and walked t->rows[] to find its
 *   owning TabRow, although every caller had that row already; and
 *   tab_add_row() rescanned t->rows[] after a dedup that already matched
 *   the row in its bucket.
 *
 * With bucket growth but neither lookup fix: 40k records 4.29 s, 80k
 * 19.50 s, 160k 82.96 s. With both: 0.162 / 0.328 / 0.641 s.
 *
 * Mutation testing attributes all of the measurable gain to the dedup
 * rescan. Reverting it alone fails this check; reverting the rehash rescan
 * alone does not, and instrumenting the counter explains why:
 * tab_rowmap_rehash_row() fires exactly *once* across a suite issuing
 * 240,000 tab_set calls. tab_set only rehashes when a mutation changes the
 * row's canonical hash, and filling cells on a row keyed by its head value
 * does not. That mutant is equivalent for every workload here -- it
 * modifies code that effectively never runs -- so no test catches it and
 * none should. The rehash fix stays as insurance for callers that do
 * mutate head values, not because anything here measures it.
 *
 * Asserting scaling rather than absolute time. A wall-clock threshold is
 * unreliable on a loaded machine -- an earlier version of this measurement
 * produced a 3.27x ratio on correct code -- so the bound is deliberately
 * loose. Quadratic gives ~4x for a 2x row count; anything under 3x is
 * linear enough.
 */
static void
test_set_scaling(void)
{
	Tab *t;
	TabRow *r;
	char key[32], *path = "/tmp/lt_setscale.tab";
	int pass, i, n;
	vlong start, small, big;
	double ratio;
	enum { Nsmall = 40000, Nbig = 80000 };

	small = big = 0;
	for(pass = 0; pass < 2; pass++){
		n = pass == 0 ? Nsmall : Nbig;
		t = fresh(path, 3);
		if(t == nil){
			ok(0, "create for set-scaling test");
			return;
		}
		start = nsec();
		for(i = 0; i < n; i++){
			/* Reuse the head value across groups of rows. A
			 * writer keyed on something coarser than the row --
			 * sigil's store_commit keys every paragraph of a file
			 * on the same path -- drives tab_add_row down its
			 * dedup path, which is where the second rescan lived.
			 * Unique keys never reach it. */
			snprint(key, sizeof key, "k%d", i / 8);
			r = tab_add_row(t, "k", key);
			if(r == nil){
				ok(0, "insert during set-scaling test");
				tab_close(t);
				remove(path);
				return;
			}
			/* Two mutations per row: tab_set is what routes
			 * through the row index, and the cost being guarded
			 * here is per-set, not per-insert. */
			tab_set(t, r, "c1", "x");
			tab_set(t, r, "c2", "y");
		}
		if(pass == 0)
			small = nsec() - start;
		else
			big = nsec() - start;
		tab_close(t);
		remove(path);
	}

	if(small <= 0){
		ok(0, "set-scaling timing is usable");
		return;
	}
	ratio = (double)big / (double)small;
	ok(ratio < 3.0, "2x rows costs under 3x time with cell mutation");
	print("\t%d->%d rows with 2 sets each: %.3f s -> %.3f s (%.2fx)\n",
		Nsmall, Nbig, (double)small/1e9, (double)big/1e9, ratio);
}

/*
 * Dedup must never insert a byte-identical row, however large the table.
 *
 * One duplicate is a complete failure, not a degraded index: tab_commit
 * would write two rows for one logical record and tab_open would read them
 * back as distinct.
 *
 * Re-add in reverse. Ascending order hides a broken rehash, because a
 * stranded key tends to be revisited while its bucket is still the one it
 * was stranded from -- a mutant that drops one bucket per resize yields
 * ~15 duplicates in reverse and none ascending.
 */
static void
test_dedup_after_growth(void)
{
	Tab *t;
	TabIter *it;
	char key[32], *path = "/tmp/lt_dedup.tab";
	int i, before, after;
	enum { Nrows = 8000 };

	t = fresh(path, 2);
	if(t == nil){
		ok(0, "create for dedup test");
		return;
	}
	for(i = 0; i < Nrows; i++){
		snprint(key, sizeof key, "d%d", i);
		tab_add_row(t, "k", key);
	}
	before = 0;
	it = tab_iter(t);
	while(tab_iter_next(it) != nil)
		before++;
	tab_iter_close(it);

	for(i = Nrows - 1; i >= 0; i--){
		snprint(key, sizeof key, "d%d", i);
		tab_add_row(t, "k", key);
	}
	after = 0;
	it = tab_iter(t);
	while(tab_iter_next(it) != nil)
		after++;
	tab_iter_close(it);

	ok(after == before, "re-adding every row inserts nothing");
	if(after != before)
		print("\t%d duplicates after re-add\n", after - before);

	tab_close(t);
	remove(path);
}

/*
 * ENCRYPTED column type.
 *
 * Wire format is a single self-describing blob, unlike SIGNED's two
 * segments: there is no half worth inspecting on its own.
 *
 *	encrypted:<b64( ver | nonce[24] | mac[16] | ciphertext )>
 *
 * Crypto is monocypher's XChaCha20-Poly1305 (crypto_aead_lock /
 * crypto_aead_unlock).  Not AES-256-GCM: monocypher ships no AES, and
 * vendoring a second crypto library to satisfy a README line is the
 * wrong trade.  XChaCha20-Poly1305 is reachable from every binding
 * this repository feeds -- Go's x/crypto, and Bouncy Castle on .NET.
 *
 * Row identity hashes the *ciphertext*, because it must.  The rowmap
 * is rebuilt by tab_open from on-disk text, which never holds the key;
 * hashing plaintext would make row identity unreproducible on load.
 * test_encrypted_reencrypt_makes_new_row nails that consequence down
 * so nobody later "fixes" it into a dedup bug.
 */

static void
fill_key(uint8_t key[32], uint8_t seed)
{
	int i;

	for(i = 0; i < 32; i++)
		key[i] = (uint8_t)(seed + i);
}

static Tab*
fresh_enc(char *path)
{
	TabColSpec cols[2];

	memset(cols, 0, sizeof cols);
	cols[0].name = "k";
	cols[1].name = "secret";
	cols[1].type = "ENCRYPTED";

	remove(path);
	return tab_create(path, "test", cols, 2);
}

/* Encrypt, then decrypt with the same key, in memory. */
static void
test_encrypted_roundtrip(void)
{
	Tab *t;
	TabRow *r;
	uint8_t key[32];
	unsigned char *out;
	char *path = "/tmp/lt_enc_roundtrip.tab";
	int outlen;

	fill_key(key, 7);
	t = fresh_enc(path);
	if(t == nil){
		ok(0, "encrypted: create table");
		return;
	}
	r = tab_add_row(t, "k", "a");
	ok(r != nil, "encrypted: add row");
	if(r == nil){
		tab_close(t);
		remove(path);
		return;
	}

	ok(tab_set_encrypted(t, r, "secret",
		(unsigned char*)"hello", 5, key) == 0,
		"encrypted: set");

	/* The cell on disk is tagged and carries no plaintext. */
	ok(tab_cell_has_tag(tab_get(r, "secret"), "ENCRYPTED") == 1,
		"encrypted: cell carries encrypted: tag");
	ok(strstr(tab_get(r, "secret"), "hello") == nil,
		"encrypted: plaintext absent from cell text");

	out = tab_decrypt(r, "secret", key, &outlen);
	ok(out != nil, "encrypted: decrypt succeeds");
	if(out != nil){
		ok(outlen == 5, "encrypted: plaintext length");
		ok(memcmp(out, "hello", 5) == 0, "encrypted: plaintext bytes");
		free(out);
	}
	tab_close(t);
	remove(path);
}

/* Survives commit and reopen: the ciphertext is the durable form. */
static void
test_encrypted_survives_reopen(void)
{
	Tab *t;
	TabRow *r;
	uint8_t key[32];
	unsigned char *out;
	char *path = "/tmp/lt_enc_reopen.tab";
	int outlen;

	fill_key(key, 19);
	t = fresh_enc(path);
	if(t == nil){
		ok(0, "encrypted reopen: create table");
		return;
	}
	r = tab_add_row(t, "k", "a");
	if(r == nil || tab_set_encrypted(t, r, "secret",
			(unsigned char*)"across the disk", 15, key) != 0){
		ok(0, "encrypted reopen: set");
		tab_close(t);
		remove(path);
		return;
	}
	ok(tab_commit(t) == 0, "encrypted reopen: commit");
	tab_close(t);

	t = tab_open(path);
	ok(t != nil, "encrypted reopen: reopen");
	if(t == nil){
		remove(path);
		return;
	}
	r = find(t, "k", "a");
	ok(r != nil, "encrypted reopen: row found");
	if(r != nil){
		out = tab_decrypt(r, "secret", key, &outlen);
		ok(out != nil, "encrypted reopen: decrypt after reopen");
		if(out != nil){
			ok(outlen == 15 && memcmp(out, "across the disk", 15) == 0,
				"encrypted reopen: plaintext intact");
			free(out);
		}
	}
	tab_close(t);
	remove(path);
}

/* Wrong key must fail the Poly1305 tag, not return garbage. */
static void
test_encrypted_wrong_key_fails(void)
{
	Tab *t;
	TabRow *r;
	uint8_t key[32], wrong[32];
	unsigned char *out;
	char *path = "/tmp/lt_enc_wrongkey.tab";
	int outlen;

	fill_key(key, 1);
	fill_key(wrong, 2);
	t = fresh_enc(path);
	if(t == nil){
		ok(0, "encrypted wrong key: create table");
		return;
	}
	r = tab_add_row(t, "k", "a");
	if(r == nil || tab_set_encrypted(t, r, "secret",
			(unsigned char*)"top secret", 10, key) != 0){
		ok(0, "encrypted wrong key: set");
		tab_close(t);
		remove(path);
		return;
	}
	out = tab_decrypt(r, "secret", wrong, &outlen);
	ok(out == nil, "encrypted: wrong key is rejected");
	free(out);
	tab_close(t);
	remove(path);
}

/*
 * A flipped ciphertext byte must fail authentication.
 *
 * The corruption goes through the file rather than tab_set, which
 * correctly refuses to write a typed column at all.  That also makes
 * this the realistic case: an ENCRYPTED cell is tampered with on disk,
 * by someone who never went through libtab.
 */
static void
test_encrypted_tamper_detected(void)
{
	Tab *t;
	TabRow *r;
	uint8_t key[32];
	unsigned char *out;
	char *path = "/tmp/lt_enc_tamper.tab";
	char *text, *p, *end;
	int outlen, fd, n;
	char buf[8192];

	fill_key(key, 31);
	t = fresh_enc(path);
	if(t == nil){
		ok(0, "encrypted tamper: create table");
		return;
	}
	r = tab_add_row(t, "k", "a");
	if(r == nil || tab_set_encrypted(t, r, "secret",
			(unsigned char*)"integrity", 9, key) != 0
	   || tab_commit(t) != 0){
		ok(0, "encrypted tamper: build table");
		tab_close(t);
		remove(path);
		return;
	}
	tab_close(t);

	/* Read the committed file back and flip one base64 character
	 * inside the encrypted cell, past the nonce and mac. */
	fd = open(path, OREAD);
	if(fd < 0){
		ok(0, "encrypted tamper: reopen file");
		remove(path);
		return;
	}
	n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if(n <= 0){
		ok(0, "encrypted tamper: read file");
		remove(path);
		return;
	}
	buf[n] = 0;

	text = strstr(buf, "encrypted:");
	if(text == nil){
		ok(0, "encrypted tamper: locate cell");
		remove(path);
		return;
	}
	p = text + strlen("encrypted:");
	end = p;
	while(*end != 0 && *end != '\n' && *end != ' ' && *end != '\t')
		end++;
	while(end > p && end[-1] == '=')
		end--;
	if(end - p < 4){
		ok(0, "encrypted tamper: payload too short to corrupt");
		remove(path);
		return;
	}
	/*
	 * Corrupt a character in the middle of the payload, not the last
	 * one.  With '=' padding the final base64 character carries only
	 * the high bits of the last byte and the rest are discarded, so
	 * flipping it can decode to identical ciphertext and authenticate
	 * correctly -- a property of base64, not a failure of the mac.
	 * The midpoint is past the 41-byte header, so this always lands
	 * in ciphertext for a plaintext of this size.
	 */
	p += (end - p) / 2;
	*p = (*p == 'A') ? 'B' : 'A';

	if(write_text_file(path, buf) < 0){
		ok(0, "encrypted tamper: rewrite file");
		remove(path);
		return;
	}

	t = tab_open(path);
	ok(t != nil, "encrypted tamper: reopen tampered table");
	if(t == nil){
		remove(path);
		return;
	}
	r = find(t, "k", "a");
	ok(r != nil, "encrypted tamper: row still found");
	if(r != nil){
		out = tab_decrypt(r, "secret", key, &outlen);
		ok(out == nil, "encrypted: tampered ciphertext is rejected");
		free(out);
	}
	tab_close(t);
	remove(path);
}

/* Empty plaintext is legal: mac and nonce still present, decrypt gives 0 bytes. */
static void
test_encrypted_empty_plaintext(void)
{
	Tab *t;
	TabRow *r;
	uint8_t key[32];
	unsigned char *out;
	char *path = "/tmp/lt_enc_empty.tab";
	int outlen;

	fill_key(key, 5);
	t = fresh_enc(path);
	if(t == nil){
		ok(0, "encrypted empty: create table");
		return;
	}
	r = tab_add_row(t, "k", "a");
	if(r == nil){
		ok(0, "encrypted empty: add row");
		tab_close(t);
		remove(path);
		return;
	}
	ok(tab_set_encrypted(t, r, "secret",
		(unsigned char*)"", 0, key) == 0,
		"encrypted empty: set");
	out = tab_decrypt(r, "secret", key, &outlen);
	ok(out != nil, "encrypted empty: decrypt succeeds");
	ok(out != nil && outlen == 0, "encrypted empty: zero-length plaintext");
	free(out);
	tab_close(t);
	remove(path);
}

/* Nonces are per-cell and random, so the same plaintext under the same
 * key yields different ciphertext. */
static void
test_encrypted_nonce_is_fresh(void)
{
	Tab *t;
	TabRow *r1, *r2;
	uint8_t key[32];
	char *first;
	char *path = "/tmp/lt_enc_nonce.tab";

	fill_key(key, 11);
	t = fresh_enc(path);
	if(t == nil){
		ok(0, "encrypted nonce: create table");
		return;
	}
	r1 = tab_add_row(t, "k", "a");
	r2 = tab_add_row(t, "k", "b");
	if(r1 == nil || r2 == nil){
		ok(0, "encrypted nonce: add rows");
		tab_close(t);
		remove(path);
		return;
	}
	ok(tab_set_encrypted(t, r1, "secret",
		(unsigned char*)"same", 4, key) == 0, "encrypted nonce: set 1");
	first = strdup((char*)tab_get(r1, "secret"));
	ok(tab_set_encrypted(t, r2, "secret",
		(unsigned char*)"same", 4, key) == 0, "encrypted nonce: set 2");

	ok(first != nil && strcmp(first, tab_get(r2, "secret")) != 0,
		"encrypted: identical plaintext gives different ciphertext");
	free(first);
	tab_close(t);
	remove(path);
}

/*
 * Row identity hashes ciphertext, so re-encrypting the same plaintext
 * onto a fresh row makes a *second* row rather than deduping onto the
 * first.  This is a consequence of the data model, not a defect: the
 * rowmap is rebuilt from on-disk text by tab_open, which has no key.
 * Asserted here so the behaviour is deliberate and stays that way.
 */
static void
test_encrypted_reencrypt_makes_new_row(void)
{
	Tab *t;
	TabRow *r1, *r2;
	TabIter *it;
	uint8_t key[32];
	char *path = "/tmp/lt_enc_dedup.tab";
	int n;

	fill_key(key, 23);
	t = fresh_enc(path);
	if(t == nil){
		ok(0, "encrypted dedup: create table");
		return;
	}
	r1 = tab_add_row(t, "k", "dup");
	if(r1 == nil || tab_set_encrypted(t, r1, "secret",
			(unsigned char*)"payload", 7, key) != 0){
		ok(0, "encrypted dedup: first row");
		tab_close(t);
		remove(path);
		return;
	}
	r2 = tab_add_row(t, "k", "dup");
	ok(r2 != nil, "encrypted dedup: second row admitted");
	if(r2 != nil)
		ok(tab_set_encrypted(t, r2, "secret",
			(unsigned char*)"payload", 7, key) == 0,
			"encrypted dedup: second encrypt does not collide");

	n = 0;
	it = tab_iter(t);
	while(tab_iter_next(it) != nil)
		n++;
	tab_iter_close(it);
	ok(n == 2, "encrypted: same plaintext twice yields two distinct rows");

	tab_close(t);
	remove(path);
}

/* The type is only meaningful on an ENCRYPTED column. */
static void
test_encrypted_rejects_wrong_column_type(void)
{
	Tab *t;
	TabRow *r;
	uint8_t key[32];
	char *path = "/tmp/lt_enc_coltype.tab";

	fill_key(key, 3);
	t = fresh_enc(path);
	if(t == nil){
		ok(0, "encrypted coltype: create table");
		return;
	}
	r = tab_add_row(t, "k", "a");
	if(r == nil){
		ok(0, "encrypted coltype: add row");
		tab_close(t);
		remove(path);
		return;
	}
	/* "k" is a plain column. */
	ok(tab_set_encrypted(t, r, "k",
		(unsigned char*)"x", 1, key) == -1,
		"encrypted: plain column rejected");
	ok(tab_set_encrypted(t, r, "nosuch",
		(unsigned char*)"x", 1, key) == -1,
		"encrypted: unknown column rejected");
	tab_close(t);
	remove(path);
}

/*
 * An untagged cell in an ENCRYPTED column never reaches tab_decrypt:
 * tab_open validates every typed cell's tag at load time and refuses
 * the whole table.  Asserted at that layer, because that is the layer
 * that enforces it.
 */
static void
test_encrypted_untagged_cell_rejected_at_open(void)
{
	Tab *t;
	char *path = "/tmp/lt_enc_untagged.tab";
	char *text =
		"schema=test\n"
		"\tcol=k\n"
		"\tcol=secret type=ENCRYPTED\n"
		"\n"
		"k=plain\n"
		"\tsecret=just_text\n"
		"\n";

	if(write_text_file(path, text) < 0){
		ok(0, "encrypted untagged: write file");
		return;
	}
	t = tab_open(path);
	ok(t == nil, "encrypted: untagged cell in ENCRYPTED column is rejected at open");
	if(t != nil)
		tab_close(t);
	remove(path);
}

/*
 * Truncated blobs that pass the tag check reach tab_decrypt, which must
 * reject them rather than read past the end of the buffer.
 *
 * Three cases, because they trip different guards.  "QQ==" decodes to a
 * single 0x41 byte and dies on the version check.  The other two carry a
 * valid 0x01 version and can only be caught by the header-length test:
 * one is 9 bytes, one is 40 -- exactly one short of the 41-byte header,
 * which is where an off-by-one would let a read run off the end.
 */
static void
test_encrypted_rejects_truncated_blob(void)
{
	Tab *t;
	TabRow *r;
	uint8_t key[32];
	unsigned char *out;
	char *path = "/tmp/lt_enc_trunc.tab";
	int outlen;
	int i;
	static char *rows[] = {
		"badver",	/* 0x41 -- fails the version check */
		"short",	/* valid version, 9 bytes total */
		"offbyone",	/* valid version, 40 bytes: header minus one */
	};
	char *text =
		"schema=test\n"
		"\tcol=k\n"
		"\tcol=secret type=ENCRYPTED\n"
		"\n"
		"k=badver\n"
		"\tsecret=encrypted:QQ==\n"
		"\n"
		"k=short\n"
		"\tsecret=encrypted:AQABAgMEBQYH\n"
		"\n"
		"k=offbyone\n"
		"\tsecret=encrypted:AQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA==\n"
		"\n";

	fill_key(key, 13);
	if(write_text_file(path, text) < 0){
		ok(0, "encrypted truncated: write file");
		return;
	}
	t = tab_open(path);
	ok(t != nil, "encrypted truncated: table opens");
	if(t == nil){
		remove(path);
		return;
	}
	for(i = 0; i < nelem(rows); i++){
		r = find(t, "k", rows[i]);
		ok(r != nil, "encrypted truncated: row found");
		if(r == nil)
			continue;
		out = tab_decrypt(r, "secret", key, &outlen);
		ok(out == nil, "encrypted: truncated blob is rejected");
		free(out);
	}
	tab_close(t);
	remove(path);
}

/* An ENCRYPTED column must survive the schema round-trip, and the
 * streaming writer must accept the type too. */
static void
test_encrypted_schema_accepted(void)
{
	Tab *t;
	char *path = "/tmp/lt_enc_schema.tab";

	t = fresh_enc(path);
	ok(t != nil, "encrypted schema: ENCRYPTED column accepted by tab_create");
	if(t == nil)
		return;
	ok(tab_commit(t) == 0, "encrypted schema: commit");
	tab_close(t);

	t = tab_open(path);
	ok(t != nil, "encrypted schema: ENCRYPTED column accepted by tab_open");
	if(t != nil){
		okstr(tab_coltype(t, 1), "ENCRYPTED",
			"encrypted schema: type survives round-trip");
		tab_close(t);
	}
	remove(path);
}

/*
 * Oversized cells are refused at write time.
 *
 * ndb entries are parsed a line at a time by Brdline into an 8 KiB Bio
 * buffer (ndbparse.c: `if(line[len-1] != '\n') break;`).  A longer line
 * comes back partial, and ndb treats that as a clean end of entry -- so
 * a too-long cell is written to disk correctly, then silently vanishes
 * on reload.  The row survives; the cell is simply gone, with no error
 * from commit, from open, or from tab_lasterror.
 *
 * Refusing at write turns that into a caller-visible failure.  The
 * alternative, splitting a value across continuation lines, would be an
 * on-disk format change and would put multi-cell reassembly in front of
 * every reader; the cap is the cheaper contract.
 *
 * The limit is on the *encoded* line, not the caller's string: entity
 * escaping expands '&' to "&amp;", so a value of ampersands is five
 * times longer on disk than in memory.
 */

/* Build a value of `n` repetitions of `c`, NUL-terminated. */
static char *
bigval(int n, char c)
{
	char *s;

	s = malloc(n + 1);
	if(s == nil)
		return nil;
	memset(s, c, n);
	s[n] = 0;
	return s;
}

static Tab*
fresh_lim(char *path)
{
	TabColSpec cols[2];

	memset(cols, 0, sizeof cols);
	cols[0].name = "k";
	cols[1].name = "v";

	remove(path);
	return tab_create(path, "test", cols, 2);
}

/* A plain cell just under the cap round-trips; just over is refused. */
static void
test_oversized_plain_cell_refused(void)
{
	Tab *t;
	TabRow *r;
	char *path = "/tmp/lt_lim_plain.tab";
	char *ok_val, *big_val;

	t = fresh_lim(path);
	if(t == nil){
		ok(0, "cell limit: create table");
		return;
	}
	r = tab_add_row(t, "k", "a");
	if(r == nil){
		ok(0, "cell limit: add row");
		tab_close(t);
		remove(path);
		return;
	}

	ok_val = bigval(TabMaxCell - 64, 'x');
	big_val = bigval(TabMaxCell + 1, 'x');
	if(ok_val == nil || big_val == nil){
		ok(0, "cell limit: allocate values");
		free(ok_val);
		free(big_val);
		tab_close(t);
		remove(path);
		return;
	}

	ok(tab_set(t, r, "v", ok_val) == 0,
		"cell limit: a value just under the cap is accepted");
	ok(tab_set(t, r, "v", big_val) == -1,
		"cell limit: a value over the cap is refused");

	/* The refusal must not have disturbed the accepted value. */
	okstr(tab_get(r, "v"), ok_val,
		"cell limit: refused write leaves the old value intact");

	free(ok_val);
	free(big_val);
	tab_close(t);
	remove(path);
}

/*
 * The cap applies to the encoded form.  A value of ampersands is short
 * in memory and five times longer on disk, so a check against strlen()
 * would let it through and lose the cell on reload.
 */
static void
test_cell_limit_counts_encoded_length(void)
{
	Tab *t;
	TabRow *r;
	char *path = "/tmp/lt_lim_encoded.tab";
	char *amps;

	t = fresh_lim(path);
	if(t == nil){
		ok(0, "cell limit encoded: create table");
		return;
	}
	r = tab_add_row(t, "k", "a");
	if(r == nil){
		ok(0, "cell limit encoded: add row");
		tab_close(t);
		remove(path);
		return;
	}

	/* Well under the cap as a C string; "&amp;" each on disk. */
	amps = bigval(TabMaxCell / 2, '&');
	if(amps == nil){
		ok(0, "cell limit encoded: allocate value");
		tab_close(t);
		remove(path);
		return;
	}
	ok(tab_set(t, r, "v", amps) == -1,
		"cell limit: escaping expansion counts toward the cap");
	free(amps);
	tab_close(t);
	remove(path);
}

/*
 * The real point of the cap: anything accepted must survive a reload.
 * This is the assertion that would have failed before the guard existed
 * -- the row came back without its cell and nothing reported an error.
 */
static void
test_accepted_cell_survives_reload(void)
{
	Tab *t;
	TabRow *r;
	char *path = "/tmp/lt_lim_reload.tab";
	char *val;

	t = fresh_lim(path);
	if(t == nil){
		ok(0, "cell limit reload: create table");
		return;
	}
	r = tab_add_row(t, "k", "a");
	val = bigval(TabMaxCell - 64, 'x');
	if(r == nil || val == nil){
		ok(0, "cell limit reload: setup");
		free(val);
		tab_close(t);
		remove(path);
		return;
	}
	if(tab_set(t, r, "v", val) != 0 || tab_commit(t) != 0){
		ok(0, "cell limit reload: write largest accepted value");
		free(val);
		tab_close(t);
		remove(path);
		return;
	}
	tab_close(t);

	t = tab_open(path);
	ok(t != nil, "cell limit reload: reopen");
	if(t != nil){
		r = find(t, "k", "a");
		ok(r != nil, "cell limit reload: row found");
		if(r != nil)
			okstr(tab_get(r, "v"), val,
				"cell limit: the largest accepted cell reloads intact");
		tab_close(t);
	}
	free(val);
	remove(path);
}

/* Every typed setter enforces the same cap. */
static void
test_cell_limit_applies_to_typed_setters(void)
{
	Tab *t;
	TabRow *r;
	uint8_t key[32], sk[64];
	char *path = "/tmp/lt_lim_typed.tab";
	unsigned char *big;
	TabColSpec cols[4];
	int i, n;

	for(i = 0; i < 32; i++)
		key[i] = (uint8_t)i;
	memset(sk, 7, sizeof sk);

	memset(cols, 0, sizeof cols);
	cols[0].name = "k";
	cols[1].name = "e";
	cols[1].type = "ENCRYPTED";
	cols[2].name = "s";
	cols[2].type = "SIGNED";
	cols[3].name = "h";
	cols[3].type = "HASHED";

	remove(path);
	t = tab_create(path, "test", cols, 4);
	if(t == nil){
		ok(0, "cell limit typed: create table");
		return;
	}
	r = tab_add_row(t, "k", "a");
	if(r == nil){
		ok(0, "cell limit typed: add row");
		tab_close(t);
		remove(path);
		return;
	}

	n = TabMaxCell;			/* base64 alone pushes this over */
	big = (unsigned char *)bigval(n, 'x');
	if(big == nil){
		ok(0, "cell limit typed: allocate value");
		tab_close(t);
		remove(path);
		return;
	}

	ok(tab_set_encrypted(t, r, "e", big, n, key) == -1,
		"cell limit: tab_set_encrypted refuses an oversized body");
	ok(tab_set_signed(t, r, "s", big, n, sk) == -1,
		"cell limit: tab_set_signed refuses an oversized body");

	/* HASHED is exempt by construction: the digest is fixed-size
	 * however long the preimage is. */
	ok(tab_set_hashed(t, r, "h", big, n) == 0,
		"cell limit: HASHED is unaffected, its digest is fixed-size");

	free(big);
	tab_close(t);
	remove(path);
}

/* The streaming writer shares the cap; it writes the same lines. */
static void
test_cell_limit_applies_to_writer(void)
{
	TabWriter *w;
	char *path = "/tmp/lt_lim_writer.tab";
	char *big;
	TabColSpec cols[2];

	memset(cols, 0, sizeof cols);
	cols[0].name = "k";
	cols[1].name = "v";

	remove(path);
	w = tab_writer_create(path, "test", cols, 2);
	if(w == nil){
		ok(0, "cell limit writer: create");
		return;
	}
	big = bigval(TabMaxCell + 1, 'x');
	if(big == nil){
		ok(0, "cell limit writer: allocate value");
		tab_writer_close(w);
		remove(path);
		return;
	}
	ok(tab_writer_add_row(w, "k", "a") == 0,
		"cell limit writer: add row");
	ok(tab_writer_set(w, "v", big) == -1,
		"cell limit: tab_writer_set refuses an oversized value");
	ok(tab_writer_add_row(w, "k", big) == -1,
		"cell limit: tab_writer_add_row refuses an oversized head value");

	free(big);
	tab_writer_close(w);
	remove(path);
}

/*
 * Concurrent-writer detection.
 *
 * tab_commit is atomic -- posix_write_atomic renames a temp file over the
 * target, so no reader ever sees a torn file.  The read-modify-write cycle
 * around it is not:
 *
 *	A: tab_open ------ tab_set ------ tab_commit
 *	B:      tab_open ------ tab_set ------ tab_commit
 *
 * B renames over A's file and A's changes are gone entirely.  A lock held
 * during commit would not help; both processes read before either wrote.
 * Holding one from open to commit would mean holding it across arbitrary
 * caller code -- including a human in an editor, which is the common case
 * for a format whose whole point is that you can edit it by hand.
 *
 * So this is a compare-and-swap on the file rather than a lock: record the
 * file's identity at open, re-check it at commit, and refuse if it moved.
 * That also covers the writer a lock cannot see -- someone running `vi`.
 *
 * The identity is qid.path + qid.vers + mtime + length together.  On native
 * Plan 9 qid.vers is a real version counter; under plan9port on Linux it
 * falls back to mtime+ctime (_p9dir.c), which is second-granularity, so
 * length carries the weight for same-second edits and qid.path catches the
 * inode change a rename always causes.
 */

/* Overwrite `path` with `text`, simulating another writer. */
static int
foreign_write(char *path, char *text)
{
	int fd;

	fd = create(path, OWRITE, 0644);
	if(fd < 0)
		return -1;
	if(write(fd, text, strlen(text)) != strlen(text)){
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

static char *conc_table_text =
	"schema=test\n"
	"\tcol=k\n"
	"\tcol=v\n"
	"\n"
	"k=a\n"
	"\tv=original\n"
	"\n";

/* The base case: nobody else wrote, so commit succeeds. */
static void
test_commit_succeeds_when_file_untouched(void)
{
	Tab *t;
	TabRow *r;
	char *path = "/tmp/lt_conc_clean.tab";

	if(write_text_file(path, conc_table_text) < 0){
		ok(0, "concurrency: write initial table");
		return;
	}
	t = tab_open(path);
	ok(t != nil, "concurrency: open");
	if(t == nil){
		remove(path);
		return;
	}
	r = find(t, "k", "a");
	if(r == nil || tab_set(t, r, "v", "mine") != 0){
		ok(0, "concurrency: set");
		tab_close(t);
		remove(path);
		return;
	}
	ok(tab_commit(t) == 0, "concurrency: commit succeeds on an untouched file");
	tab_close(t);
	remove(path);
}

/* Two commits from the same handle must both work: the first updates the
 * recorded identity, so the second is not fooled by its own write. */
static void
test_repeated_commits_from_same_handle(void)
{
	Tab *t;
	TabRow *r;
	char *path = "/tmp/lt_conc_repeat.tab";

	if(write_text_file(path, conc_table_text) < 0){
		ok(0, "concurrency repeat: write initial table");
		return;
	}
	t = tab_open(path);
	if(t == nil){
		ok(0, "concurrency repeat: open");
		remove(path);
		return;
	}
	r = find(t, "k", "a");
	if(r == nil){
		ok(0, "concurrency repeat: row");
		tab_close(t);
		remove(path);
		return;
	}
	tab_set(t, r, "v", "first");
	ok(tab_commit(t) == 0, "concurrency: first commit");
	tab_set(t, r, "v", "second");
	ok(tab_commit(t) == 0,
		"concurrency: a handle is not confused by its own commit");
	tab_close(t);
	remove(path);
}

/*
 * The lost update.  Another writer replaces the file between our open and
 * our commit; the commit must refuse rather than silently discard their
 * work.  Before this check existed, the commit succeeded and the other
 * writer's row vanished with no error anywhere.
 */
static void
test_commit_refuses_after_foreign_write(void)
{
	Tab *t;
	TabRow *r;
	char *path = "/tmp/lt_conc_lost.tab";
	char *theirs =
		"schema=test\n"
		"\tcol=k\n"
		"\tcol=v\n"
		"\n"
		"k=a\n"
		"\tv=theirs\n"
		"\n"
		"k=b\n"
		"\tv=added-by-them\n"
		"\n";

	if(write_text_file(path, conc_table_text) < 0){
		ok(0, "concurrency lost: write initial table");
		return;
	}
	t = tab_open(path);
	if(t == nil){
		ok(0, "concurrency lost: open");
		remove(path);
		return;
	}
	r = find(t, "k", "a");
	if(r == nil || tab_set(t, r, "v", "mine") != 0){
		ok(0, "concurrency lost: set");
		tab_close(t);
		remove(path);
		return;
	}

	/* Another writer lands between our open and our commit. */
	if(foreign_write(path, theirs) < 0){
		ok(0, "concurrency lost: foreign write");
		tab_close(t);
		remove(path);
		return;
	}

	ok(tab_commit(t) == -1,
		"concurrency: commit refuses after another writer");

	/* And their file is still on disk, untouched. */
	tab_close(t);
	t = tab_open(path);
	ok(t != nil, "concurrency lost: reopen after refusal");
	if(t != nil){
		r = find(t, "k", "b");
		ok(r != nil, "concurrency: the other writer's row survived");
		r = find(t, "k", "a");
		if(r != nil)
			okstr(tab_get(r, "v"), "theirs",
				"concurrency: the other writer's value survived");
		tab_close(t);
	}
	remove(path);
}

/*
 * Same-second detection.  Under plan9port qid.vers degrades to mtime+ctime
 * at one-second granularity, so a foreign write immediately after open can
 * carry an identical timestamp.  A length change has to catch it, which is
 * why the identity is not mtime alone.
 */
static void
test_detects_same_second_foreign_write(void)
{
	Tab *t;
	TabRow *r;
	char *path = "/tmp/lt_conc_fast.tab";
	char *shorter =
		"schema=test\n"
		"\tcol=k\n"
		"\tcol=v\n"
		"\n"
		"k=a\n"
		"\tv=s\n"
		"\n";

	if(write_text_file(path, conc_table_text) < 0){
		ok(0, "concurrency fast: write initial table");
		return;
	}
	t = tab_open(path);
	if(t == nil){
		ok(0, "concurrency fast: open");
		remove(path);
		return;
	}
	r = find(t, "k", "a");
	if(r == nil || tab_set(t, r, "v", "mine") != 0){
		ok(0, "concurrency fast: set");
		tab_close(t);
		remove(path);
		return;
	}
	/* No sleep: this races the clock deliberately. */
	if(foreign_write(path, shorter) < 0){
		ok(0, "concurrency fast: foreign write");
		tab_close(t);
		remove(path);
		return;
	}
	ok(tab_commit(t) == -1,
		"concurrency: a same-second foreign write is still detected");
	tab_close(t);
	remove(path);
}

/*
 * tab_reload discards local changes and rereads the file, so a caller that
 * hits the refusal has a way forward that is not "close and reopen".  After
 * a reload the commit is allowed again, because the handle is now based on
 * what is actually on disk.
 */
static void
test_reload_recovers_from_refusal(void)
{
	Tab *t;
	TabRow *r;
	char *path = "/tmp/lt_conc_reload.tab";
	char *theirs =
		"schema=test\n"
		"\tcol=k\n"
		"\tcol=v\n"
		"\n"
		"k=a\n"
		"\tv=theirs\n"
		"\n";

	if(write_text_file(path, conc_table_text) < 0){
		ok(0, "concurrency reload: write initial table");
		return;
	}
	t = tab_open(path);
	if(t == nil){
		ok(0, "concurrency reload: open");
		remove(path);
		return;
	}
	r = find(t, "k", "a");
	tab_set(t, r, "v", "mine");
	foreign_write(path, theirs);
	ok(tab_commit(t) == -1, "concurrency reload: commit refused");

	ok(tab_reload(t) == 0, "concurrency: reload succeeds");

	/* The reload really did take their content. */
	r = find(t, "k", "a");
	ok(r != nil, "concurrency reload: row present after reload");
	if(r != nil)
		okstr(tab_get(r, "v"), "theirs",
			"concurrency: reload picks up the other writer's value");

	/* And a commit is allowed again now that we are current. */
	r = find(t, "k", "a");
	if(r != nil && tab_set(t, r, "v", "mine-again") == 0)
		ok(tab_commit(t) == 0,
			"concurrency: commit allowed again after reload");
	else
		ok(0, "concurrency reload: set after reload");

	tab_close(t);
	remove(path);
}

/* A table created rather than opened has no file to be stale against, and
 * its first commit must not be refused. */
static void
test_created_table_commits(void)
{
	Tab *t;
	TabRow *r;
	char *path = "/tmp/lt_conc_created.tab";

	t = fresh(path, 2);
	if(t == nil){
		ok(0, "concurrency created: create");
		return;
	}
	r = tab_add_row(t, "k", "a");
	if(r == nil){
		ok(0, "concurrency created: add row");
		tab_close(t);
		remove(path);
		return;
	}
	ok(tab_commit(t) == 0,
		"concurrency: a freshly created table commits");
	ok(tab_commit(t) == 0,
		"concurrency: and commits again");
	tab_close(t);
	remove(path);
}

/*
 * tab_create over a path that already exists is a deliberate overwrite --
 * the caller asked for a new table at that path -- so it must not be
 * refused as a stale write.
 */
static void
test_create_over_existing_file_commits(void)
{
	Tab *t;
	TabRow *r;
	char *path = "/tmp/lt_conc_over.tab";

	if(write_text_file(path, conc_table_text) < 0){
		ok(0, "concurrency over: write initial table");
		return;
	}
	t = fresh(path, 2);
	if(t == nil){
		ok(0, "concurrency over: create");
		remove(path);
		return;
	}
	r = tab_add_row(t, "k", "z");
	if(r == nil){
		ok(0, "concurrency over: add row");
		tab_close(t);
		remove(path);
		return;
	}
	ok(tab_commit(t) == 0,
		"concurrency: tab_create over an existing file still commits");
	tab_close(t);
	remove(path);
}

/* A file deleted out from under us is a change too. */
static void
test_commit_refuses_after_file_removed(void)
{
	Tab *t;
	TabRow *r;
	char *path = "/tmp/lt_conc_gone.tab";

	if(write_text_file(path, conc_table_text) < 0){
		ok(0, "concurrency gone: write initial table");
		return;
	}
	t = tab_open(path);
	if(t == nil){
		ok(0, "concurrency gone: open");
		remove(path);
		return;
	}
	r = find(t, "k", "a");
	if(r == nil || tab_set(t, r, "v", "mine") != 0){
		ok(0, "concurrency gone: set");
		tab_close(t);
		remove(path);
		return;
	}
	remove(path);
	ok(tab_commit(t) == -1,
		"concurrency: commit refuses when the file has been removed");
	tab_close(t);
	remove(path);
}

void
main(void)
{
	test_roundtrip();
	test_set_replaces_one_cell();
	test_nil_semantics();
	test_text_values_roundtrip();
	test_legacy_raw_nil_is_nil();
	test_legacy_text64_still_decodes();
	test_stream_writer_roundtrip();
	test_stream_writer_matches_tab_commit();
	test_clear_cell();
	test_duplicate_rejected();
	test_bucket_growth();
	test_lookup_after_growth();
	test_set_scaling();
	test_dedup_after_growth();
	test_rejects_bad_schema();

	test_encrypted_schema_accepted();
	test_encrypted_roundtrip();
	test_encrypted_survives_reopen();
	test_encrypted_wrong_key_fails();
	test_encrypted_tamper_detected();
	test_encrypted_empty_plaintext();
	test_encrypted_nonce_is_fresh();
	test_encrypted_reencrypt_makes_new_row();
	test_encrypted_rejects_wrong_column_type();
	test_oversized_plain_cell_refused();
	test_cell_limit_counts_encoded_length();
	test_accepted_cell_survives_reload();
	test_cell_limit_applies_to_typed_setters();
	test_cell_limit_applies_to_writer();
	test_commit_succeeds_when_file_untouched();
	test_repeated_commits_from_same_handle();
	test_commit_refuses_after_foreign_write();
	test_detects_same_second_foreign_write();
	test_reload_recovers_from_refusal();
	test_created_table_commits();
	test_create_over_existing_file_commits();
	test_commit_refuses_after_file_removed();
	test_encrypted_untagged_cell_rejected_at_open();
	test_encrypted_rejects_truncated_blob();

	if(failures == 0)
		print("PASS: %d checks\n", checks);
	else
		print("FAIL: %d of %d checks failed\n", failures, checks);
	exits(failures == 0 ? nil : "failures");
}
