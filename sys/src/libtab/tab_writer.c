/*
 * Streaming writer for bulk table creation.
 *
 * The in-memory API keeps every row live until tab_commit serializes the
 * whole table.  TabWriter instead streams finalized rows through Bio to the
 * platform's publish target and retains only a 128-bit row-content hash set for
 * dedup plus the current row.  On plan9port/POSIX this target is a temp file
 * renamed at commit; on native Plan 9 it is the final path, matching
 * tab_commit's truncate-and-write semantics there.
 */

#include <tab_internal.h>
#include <monocypher.h>

#ifdef USE_POSIX
extern int rename(const char *oldpath, const char *newpath);
#endif

enum {
	HashInitSlots = 1024,
	HashLen = 16,
};

struct TabWriter {
	char *path;
	char *tmp;
	int fd;
	Biobuf bio;
	int bio_active;
	int direct;
	TabSchema schema;
	Ndbtuple *row;
	uchar *hashes;	/* power-of-two slots, HashLen bytes each */
	uchar *used;
	uint32_t mask;
	int nhashes;
	int committed;
	int failed;
};

static int
schema_type_ok(const char *type)
{
	if(type == nil)
		return 1;
	return strcmp(type, "HASHED") == 0 || strcmp(type, "SIGNED") == 0
	    || strcmp(type, "ENCRYPTED") == 0;
}

static TabCol *
find_col(TabWriter *w, const char *name)
{
	int i;

	for(i = 0; i < w->schema.ncols; i++)
		if(strcmp(w->schema.cols[i].name, name) == 0)
			return &w->schema.cols[i];
	return nil;
}

static void
free_schema(TabSchema *s)
{
	int i, j;

	if(s == nil)
		return;
	for(i = 0; i < s->ncols; i++){
		free(s->cols[i].name);
		free(s->cols[i].type);
		for(j = 0; j < s->cols[i].nattrs; j++){
			free(s->cols[i].attrs[j].key);
			free(s->cols[i].attrs[j].val);
		}
		free(s->cols[i].attrs);
	}
	free(s->cols);
	free(s->name);
	memset(s, 0, sizeof *s);
}

static int
materialise_schema(TabWriter *w, const char *schema_name,
	const TabColSpec *cols, int ncols)
{
	int i, na, k;
	TabCol *out;

	w->schema.name = strdup((char *)schema_name);
	if(w->schema.name == nil){
		tab_seterror("tab_writer_create: out of memory for schema name");
		return -1;
	}
	if(ncols <= 0){
		tab_seterror("tab_writer_create: schema %q declares no columns",
			schema_name);
		return -1;
	}
	w->schema.cols = mallocz(ncols * sizeof *w->schema.cols, 1);
	if(w->schema.cols == nil){
		tab_seterror("tab_writer_create: out of memory for columns");
		return -1;
	}
	for(i = 0; i < ncols; i++){
		if(cols[i].name == nil || cols[i].name[0] == '\0'){
			tab_seterror("tab_writer_create: empty column name");
			return -1;
		}
		if(!schema_type_ok(cols[i].type)){
			tab_seterror("tab_writer_create: column %q has unsupported type %q",
				cols[i].name, cols[i].type);
			return -1;
		}
		for(k = 0; k < i; k++){
			if(strcmp(cols[k].name, cols[i].name) == 0){
				tab_seterror("tab_writer_create: duplicate column %q",
					cols[i].name);
				return -1;
			}
		}

		out = &w->schema.cols[i];
		w->schema.ncols++;
		out->name = strdup((char *)cols[i].name);
		if(out->name == nil){
			tab_seterror("tab_writer_create: out of memory for col name");
			return -1;
		}
		if(cols[i].type != nil){
			out->type = strdup((char *)cols[i].type);
			if(out->type == nil){
				tab_seterror("tab_writer_create: out of memory for type");
				return -1;
			}
		}

		na = 0;
		if(cols[i].algo != nil) na++;
		if(cols[i].signer != nil) na++;
		if(na > 0){
			out->attrs = mallocz(na * sizeof *out->attrs, 1);
			if(out->attrs == nil){
				tab_seterror("tab_writer_create: out of memory for attrs");
				return -1;
			}
			k = 0;
			if(cols[i].algo != nil){
				out->attrs[k].key = strdup("algo");
				out->attrs[k].val = strdup((char *)cols[i].algo);
				if(out->attrs[k].key == nil ||
				   out->attrs[k].val == nil){
					tab_seterror("tab_writer_create: out of memory");
					return -1;
				}
				k++;
			}
			if(cols[i].signer != nil){
				out->attrs[k].key = strdup("signer");
				out->attrs[k].val = strdup((char *)cols[i].signer);
				if(out->attrs[k].key == nil ||
				   out->attrs[k].val == nil){
					tab_seterror("tab_writer_create: out of memory");
					return -1;
				}
				k++;
			}
			out->nattrs = k;
		}
	}
	return 0;
}

static char *
make_writer_path(const char *path, int *direct)
{
#ifdef USE_POSIX
	char *tmp;
	int n;

	*direct = 0;
	n = strlen((char *)path) + 32;
	tmp = malloc(n);
	if(tmp == nil){
		tab_seterror("tab_writer_create: out of memory for temp path");
		return nil;
	}
	snprint(tmp, n, "%s.tmp.%d", path, (int)getpid());
	return tmp;
#else
	*direct = 1;
	return strdup((char *)path);
#endif
}

static int
writer_emit(void *arg, const char *p, int n)
{
	TabWriter *w;
	long m;

	w = arg;
	if(w == nil || w->fd < 0 || !w->bio_active){
		tab_seterror("tab_writer: writer is closed");
		return -1;
	}
	m = Bwrite(&w->bio, (void *)p, n);
	if(m != n){
		w->failed = 1;
		tab_seterror("tab_writer: write %s: %r", w->tmp);
		return -1;
	}
	return 0;
}

static uint32_t
hash_slot(const uchar h[HashLen])
{
	uint32_t x;

	x = ((uint32_t)h[0])
	  | ((uint32_t)h[1] << 8)
	  | ((uint32_t)h[2] << 16)
	  | ((uint32_t)h[3] << 24);
	x ^= ((uint32_t)h[4])
	   | ((uint32_t)h[5] << 8)
	   | ((uint32_t)h[6] << 16)
	   | ((uint32_t)h[7] << 24);
	x ^= ((uint32_t)h[8])
	   | ((uint32_t)h[9] << 8)
	   | ((uint32_t)h[10] << 16)
	   | ((uint32_t)h[11] << 24);
	x ^= ((uint32_t)h[12])
	   | ((uint32_t)h[13] << 8)
	   | ((uint32_t)h[14] << 16)
	   | ((uint32_t)h[15] << 24);
	return x;
}

static void
hash_insert_into(uchar *hashes, uchar *used, uint32_t mask, const uchar h[HashLen])
{
	uint32_t i;

	i = hash_slot(h) & mask;
	while(used[i])
		i = (i + 1) & mask;
	used[i] = 1;
	memcpy(hashes + (i * HashLen), h, HashLen);
}

static int
ensure_hash_slots(TabWriter *w)
{
	uchar *nhashes, *nused;
	uint32_t oldcap, newcap, i;

	if(w->hashes == nil){
		newcap = HashInitSlots;
		nhashes = mallocz(newcap * HashLen, 1);
		nused = mallocz(newcap, 1);
		if(nhashes == nil || nused == nil){
			free(nhashes);
			free(nused);
			tab_seterror("tab_writer: out of memory for row hashes");
			return -1;
		}
		w->hashes = nhashes;
		w->used = nused;
		w->mask = newcap - 1;
		return 0;
	}
	oldcap = w->mask + 1;
	if((uint32_t)(w->nhashes + 1) * 4 < oldcap * 3)
		return 0;

	newcap = oldcap * 2;
	nhashes = mallocz(newcap * HashLen, 1);
	nused = mallocz(newcap, 1);
	if(nhashes == nil || nused == nil){
		free(nhashes);
		free(nused);
		tab_seterror("tab_writer: out of memory growing row hashes");
		return -1;
	}
	for(i = 0; i < oldcap; i++){
		if(w->used[i])
			hash_insert_into(nhashes, nused, newcap - 1,
				w->hashes + (i * HashLen));
	}
	free(w->hashes);
	free(w->used);
	w->hashes = nhashes;
	w->used = nused;
	w->mask = newcap - 1;
	return 0;
}

static int
hash_seen_or_insert(TabWriter *w, const uchar h[HashLen], int *seen)
{
	uint32_t i;

	if(ensure_hash_slots(w) < 0)
		return -1;
	i = hash_slot(h) & w->mask;
	while(w->used[i]){
		if(memcmp(w->hashes + (i * HashLen), h, HashLen) == 0){
			*seen = 1;
			return 0;
		}
		i = (i + 1) & w->mask;
	}
	w->used[i] = 1;
	memcpy(w->hashes + (i * HashLen), h, HashLen);
	w->nhashes++;
	*seen = 0;
	return 0;
}

static void
hash_row(TabWriter *w, Ndbtuple *chain, uchar out[HashLen])
{
	crypto_blake2b_ctx ctx;
	Ndbtuple *tp;
	uchar nilbyte, tab;
	int i;

	nilbyte = 0;
	tab = '\t';
	crypto_blake2b_init(&ctx, HashLen);
	for(i = 0; i < w->schema.ncols; i++){
		tp = tab_row_tuple(chain, w->schema.cols[i].name);
		if(tab_tuple_is_nil(tp))
			crypto_blake2b_update(&ctx, &nilbyte, 1);
		else
			crypto_blake2b_update(&ctx, (uchar *)tp->val,
				(size_t)strlen(tp->val));
		if(i + 1 < w->schema.ncols)
			crypto_blake2b_update(&ctx, &tab, 1);
	}
	crypto_blake2b_final(&ctx, out);
}

static int
flush_row(TabWriter *w)
{
	uchar h[HashLen];
	int seen;

	if(w->row == nil)
		return 0;
	hash_row(w, w->row, h);
	if(hash_seen_or_insert(w, h, &seen) < 0)
		return -1;
	if(!seen){
		if(tab_emit_row(w->row, writer_emit, w) < 0)
			return -1;
		if(writer_emit(w, "\n", 1) < 0)
			return -1;
	}
	ndbfree(w->row);
	w->row = nil;
	return 0;
}

TabWriter *
tab_writer_create(const char *path, const char *schema_name,
	const TabColSpec *cols, int ncols)
{
	TabWriter *w;

	tab_clearerror();
	if(path == nil || path[0] == '\0' || schema_name == nil ||
	   schema_name[0] == '\0' || cols == nil){
		tab_seterror("tab_writer_create: nil/empty argument");
		return nil;
	}
	w = mallocz(sizeof *w, 1);
	if(w == nil){
		tab_seterror("tab_writer_create: out of memory for writer");
		return nil;
	}
	w->fd = -1;
	w->path = strdup((char *)path);
	w->tmp = make_writer_path(path, &w->direct);
	if(w->path == nil || w->tmp == nil){
		tab_writer_close(w);
		return nil;
	}
	if(materialise_schema(w, schema_name, cols, ncols) < 0){
		tab_writer_close(w);
		return nil;
	}
	w->fd = create(w->tmp, OWRITE, 0644);
	if(w->fd < 0){
		tab_seterror("tab_writer_create: create %s: %r", w->tmp);
		tab_writer_close(w);
		return nil;
	}
	if(Binit(&w->bio, w->fd, OWRITE) < 0){
		tab_seterror("tab_writer_create: Binit %s: %r", w->tmp);
		tab_writer_close(w);
		return nil;
	}
	w->bio_active = 1;
	if(tab_emit_schema(&w->schema, writer_emit, w) < 0 ||
	   writer_emit(w, "\n", 1) < 0){
		tab_writer_close(w);
		return nil;
	}
	return w;
}

int
tab_writer_add_row(TabWriter *w, const char *head_attr, const char *head_val)
{
	Ndbtuple *chain;
	TabCol *schema_col;

	tab_clearerror();
	if(w == nil || head_attr == nil || head_val == nil){
		tab_seterror("tab_writer_add_row: nil argument");
		return -1;
	}
	if(w->failed || w->committed || w->fd < 0){
		tab_seterror("tab_writer_add_row: writer is not writable");
		return -1;
	}
	if(head_attr[0] == '\0' || head_val[0] == '\0'){
		tab_seterror("tab_writer_add_row: empty row name");
		return -1;
	}
	if(flush_row(w) < 0)
		return -1;
	schema_col = find_col(w, head_attr);
	if(schema_col == nil){
		tab_seterror("tab_writer_add_row: head column %q not in schema",
			head_attr);
		return -1;
	}
	if(schema_col->type != nil){
		tab_seterror("tab_writer_add_row: head column %q is typed %q",
			head_attr, schema_col->type);
		return -1;
	}
	if(tab_check_cell_size(head_attr, head_val, "tab_writer_add_row") < 0)
		return -1;
	chain = ndbnew((char *)head_attr, (char *)head_val);
	if(chain == nil){
		tab_seterror("tab_writer_add_row: ndbnew failed");
		return -1;
	}
	w->row = chain;
	return 0;
}

int
tab_writer_set(TabWriter *w, const char *col, const char *value)
{
	Ndbtuple *tup, *last, *newtup;
	const char *store;
	TabCol *schema_col;
	int store_nil;

	tab_clearerror();
	if(w == nil || col == nil){
		tab_seterror("tab_writer_set: nil argument");
		return -1;
	}
	if(w->failed || w->committed || w->fd < 0 || w->row == nil){
		tab_seterror("tab_writer_set: no current writable row");
		return -1;
	}
	store_nil = value == nil;
	store = store_nil ? "nil" : value;
	schema_col = find_col(w, col);
	if(schema_col == nil){
		tab_seterror("tab_writer_set: column %q not in schema", col);
		return -1;
	}
	if(schema_col->type != nil){
		tab_seterror("tab_writer_set: column %q is typed %q",
			col, schema_col->type);
		return -1;
	}
	if(w->schema.ncols > 0 && strcmp(col, w->schema.cols[0].name) == 0 &&
	   (store_nil || store[0] == '\0')){
		tab_seterror("tab_writer_set: empty row name");
		return -1;
	}
	if(!store_nil && tab_check_cell_size(col, store, "tab_writer_set") < 0)
		return -1;

	tup = nil;
	last = nil;
	for(newtup = w->row; newtup != nil; newtup = newtup->entry){
		if(strcmp(newtup->attr, col) == 0){
			tup = newtup;
			break;
		}
		last = newtup;
	}
	if(tup == nil){
		tup = ndbnew((char *)col, (char *)store);
		if(tup == nil){
			tab_seterror("tab_writer_set: ndbnew failed");
			return -1;
		}
		tab_tuple_mark_nil(tup, store_nil);
		last->entry = tup;
		return 0;
	}
	ndbsetval(tup, (char *)store, strlen(store));
	tab_tuple_mark_nil(tup, store_nil);
	return 0;
}

int
tab_writer_clear(TabWriter *w, const char *col)
{
	Ndbtuple *victim, **link;
	TabCol *schema_col;

	tab_clearerror();
	if(w == nil || col == nil){
		tab_seterror("tab_writer_clear: nil argument");
		return -1;
	}
	if(w->failed || w->committed || w->fd < 0 || w->row == nil){
		tab_seterror("tab_writer_clear: no current writable row");
		return -1;
	}
	schema_col = find_col(w, col);
	if(schema_col == nil){
		tab_seterror("tab_writer_clear: column %q not in schema", col);
		return -1;
	}
	if(w->schema.ncols > 0 && strcmp(col, w->schema.cols[0].name) == 0){
		tab_seterror("tab_writer_clear: cannot clear row identity column %q",
			col);
		return -1;
	}
	link = &w->row;
	while(*link != nil && strcmp((*link)->attr, col) != 0)
		link = &(*link)->entry;
	if(*link == nil)
		return 0;
	victim = *link;
	*link = victim->entry;
	victim->entry = nil;
	ndbfree(victim);
	return 0;
}

int
tab_writer_commit(TabWriter *w)
{
	tab_clearerror();
	if(w == nil){
		tab_seterror("tab_writer_commit: nil writer");
		return -1;
	}
	if(w->committed)
		return 0;
	if(w->failed || w->fd < 0){
		tab_seterror("tab_writer_commit: writer is failed or closed");
		return -1;
	}
	if(flush_row(w) < 0)
		return -1;
	if(w->bio_active){
		if(Bterm(&w->bio) < 0){
			w->bio_active = 0;
			w->failed = 1;
			tab_seterror("tab_writer_commit: flush %s: %r", w->tmp);
			close(w->fd);
			w->fd = -1;
			return -1;
		}
		w->bio_active = 0;
	}
#ifdef USE_POSIX
	if(fsync(w->fd) < 0){
		w->failed = 1;
		tab_seterror("tab_writer_commit: fsync %s: %r", w->tmp);
		return -1;
	}
#endif
	if(close(w->fd) < 0){
		w->fd = -1;
		w->failed = 1;
		tab_seterror("tab_writer_commit: close %s: %r", w->tmp);
		return -1;
	}
	w->fd = -1;
#ifdef USE_POSIX
	if(rename(w->tmp, w->path) < 0){
		w->failed = 1;
		tab_seterror("tab_writer_commit: rename %s -> %s: %r",
			w->tmp, w->path);
		remove(w->tmp);
		return -1;
	}
#endif
	w->committed = 1;
	return 0;
}

void
tab_writer_close(TabWriter *w)
{
	if(w == nil)
		return;
	if(w->bio_active){
		Bterm(&w->bio);
		w->bio_active = 0;
	}
	if(w->fd >= 0){
		close(w->fd);
		w->fd = -1;
	}
	if(!w->committed && !w->direct && w->tmp != nil)
		remove(w->tmp);
	if(w->row != nil)
		ndbfree(w->row);
	free_schema(&w->schema);
	free(w->hashes);
	free(w->used);
	free(w->path);
	free(w->tmp);
	free(w);
}
