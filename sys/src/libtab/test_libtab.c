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

	if(failures == 0)
		print("PASS: %d checks\n", checks);
	else
		print("FAIL: %d of %d checks failed\n", failures, checks);
	exits(failures == 0 ? nil : "failures");
}
