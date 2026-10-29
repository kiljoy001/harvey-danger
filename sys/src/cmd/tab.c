/*
 * tab — inspect and edit libtab tables.
 *
 * A table is an ndb-shaped text file with a schema= line.  This command
 * keeps the common operations scriptable:
 *
 *	tab FILE init SCHEMA COL[:TYPE[:ALGO]]...
 *	tab FILE dump
 *	tab FILE get COL=VALUE FIELD
 *	tab FILE set COL=VALUE FIELD VALUE
 *	tab FILE hash COL=VALUE FIELD		(preimage from stdin)
 *	tab FILE sign COL=VALUE FIELD KEYFILE	(body from stdin)
 *	tab FILE verify COL=VALUE FIELD PUBFILE
 *
 * hash/sign/verify are the sensitive-data path: HASHED cells are
 * BLAKE2b or Argon2id per the schema, SIGNED cells carry an
 * EdDSA-BLAKE2b signature.  Key files hold raw Monocypher key bytes
 * (64-byte secret key, 32-byte public key), not text.
 */
#include <u.h>
#include <libc.h>
#include <libtab.h>
#include <monocypher.h>

enum {
	MaxCols = 32,
};

static void
usage(void)
{
	fprint(2, "usage: tab FILE init SCHEMA COL[:TYPE[:ALGO]]...\n"
		"       tab FILE dump\n"
		"       tab FILE get COL=VALUE FIELD\n"
		"       tab FILE set COL=VALUE FIELD VALUE\n"
		"       tab FILE hash COL=VALUE FIELD\n"
		"       tab FILE sign COL=VALUE FIELD KEYFILE\n"
		"       tab FILE verify COL=VALUE FIELD PUBFILE\n"
		"       tab FILE encrypt COL=VALUE FIELD KEYFILE\n"
		"       tab FILE decrypt COL=VALUE FIELD KEYFILE\n"
		"       tab keygen KEYFILE\n"
		"       tab keygen SECRETKEY PUBLICKEY\n");
	exits("usage");
}

static void
fail(char *fmt, ...)
{
	va_list va;

	va_start(va, fmt);
	vfprint(2, fmt, va);
	va_end(va);
	fprint(2, "\n");
	exits("tab");
}

static TabRow*
find(Tab *t, char *col, char *value)
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
open_tab(char *path)
{
	Tab *t;

	t = tab_open(path);
	if(t == nil)
		fail("tab: %s: %s", path, tab_lasterror());
	return t;
}

static void
commit(Tab *t)
{
	if(tab_commit(t) < 0)
		fail("tab: commit: %s", tab_lasterror());
}

/* Read all of stdin, dropping one trailing newline. */
static char*
readstdin(int *outlen)
{
	char *buf, *nb;
	int n, cap, got;
	char chunk[4096];

	cap = 4096;
	buf = malloc(cap + 1);
	if(buf == nil)
		fail("tab: out of memory");
	got = 0;
	for(;;){
		n = read(0, chunk, sizeof chunk);
		if(n < 0)
			fail("tab: read stdin: %r");
		if(n == 0)
			break;
		if(got + n > cap){
			cap *= 2;
			nb = realloc(buf, cap + 1);
			if(nb == nil)
				fail("tab: out of memory");
			buf = nb;
		}
		memmove(buf + got, chunk, n);
		got += n;
	}
	if(got > 0 && buf[got-1] == '\n')
		got--;
	buf[got] = 0;
	*outlen = got;
	return buf;
}

/* Read exactly n bytes from a key file. */
static uchar*
readkey(char *path, int n)
{
	uchar *buf;
	int fd, got, r;

	buf = malloc(n);
	if(buf == nil)
		fail("tab: out of memory");
	fd = open(path, OREAD);
	if(fd < 0)
		fail("tab: open %s: %r", path);
	got = 0;
	while(got < n){
		r = read(fd, buf + got, n - got);
		if(r <= 0)
			fail("tab: %s: short key file", path);
		got += r;
	}
	close(fd);
	return buf;
}

static int
cmd_init(char *path, char *schema, char **specs, int nspecs)
{
	Tab *t;
	TabColSpec cols[MaxCols];
	char *spec, *p;
	int i;

	if(nspecs < 1 || nspecs > MaxCols)
		usage();
	memset(cols, 0, sizeof cols);
	for(i = 0; i < nspecs; i++){
		spec = strdup(specs[i]);
		if(spec == nil)
			fail("tab: out of memory");
		cols[i].name = spec;
		if((p = strchr(spec, ':')) != nil){
			*p++ = 0;
			cols[i].type = p;
			if((p = strchr(p, ':')) != nil){
				*p++ = 0;
				cols[i].algo = p;
			}
		}
		for(p = (char*)cols[i].name; *p; p++)
			if(*p == '=')
				fail("tab: init: column name %q contains =", cols[i].name);
	}
	t = tab_create(path, schema, cols, nspecs);
	if(t == nil)
		fail("tab: create %s: %s", path, tab_lasterror());
	commit(t);
	tab_close(t);
	return 0;
}

static int
cmd_dump(char *path)
{
	Tab *t;
	TabIter *it;
	TabRow *r;
	char *v;
	int i;

	t = open_tab(path);
	it = tab_iter(t);
	if(it == nil)
		fail("tab: %s", tab_lasterror());
	while((r = tab_iter_next(it)) != nil){
		for(i = 0; i < tab_ncolumns(t); i++){
			v = (char*)tab_get(r, tab_colname(t, i));
			if(v == nil)
				continue;
			print("%s%s=%s", i ? " " : "", tab_colname(t, i), v);
		}
		print("\n");
	}
	tab_iter_close(it);
	tab_close(t);
	return 0;
}

/*
 * Parse the row selector: either "col=value" or "col value".  Both are
 * accepted because rc treats a bare word containing '=' as an
 * environment assignment; scripts normally use the two-word form and
 * interactive users can quote "col=value".
 */
static void
selector(int argc, char **argv, int *idx, char **colp, char **valp)
{
	char *eq;

	if(*idx >= argc)
		usage();
	if((eq = strchr(argv[*idx], '=')) != nil){
		*eq = 0;
		*colp = argv[*idx];
		*valp = eq + 1;
		(*idx)++;
	}else{
		if(*idx + 1 >= argc)
			usage();
		*colp = argv[*idx];
		*valp = argv[*idx + 1];
		*idx += 2;
	}
}

static int
cmd_get(char *path, int argc, char **argv)
{
	Tab *t;
	TabRow *r;
	char *v, *col, *value, *field;
	int i;

	i = 0;
	selector(argc, argv, &i, &col, &value);
	if(i != argc - 1)
		usage();
	field = argv[i];
	t = open_tab(path);
	r = find(t, col, value);
	if(r == nil)
		fail("tab: no row with %s=%s", col, value);
	v = (char*)tab_get(r, field);
	if(v == nil)
		fail("tab: %s=%s has no %s", col, value, field);
	print("%s\n", v);
	tab_close(t);
	return 0;
}

static int
cmd_set(char *path, int argc, char **argv)
{
	Tab *t;
	TabRow *r;
	char *col, *value, *field, *val;
	int i;

	i = 0;
	selector(argc, argv, &i, &col, &value);
	if(i != argc - 2)
		usage();
	field = argv[i];
	val = argv[i+1];
	t = open_tab(path);
	r = find(t, col, value);
	if(r == nil){
		r = tab_add_row(t, col, value);
		if(r == nil)
			fail("tab: add %s=%s: %s", col, value, tab_lasterror());
	}
	if(strcmp(field, col) != 0 && tab_set(t, r, field, val) < 0)
		fail("tab: set %s: %s", field, tab_lasterror());
	commit(t);
	tab_close(t);
	return 0;
}

static TabRow*
row_for(Tab *t, char *col, char *value)
{
	TabRow *r;

	r = find(t, col, value);
	if(r == nil)
		fail("tab: no row with %s=%s", col, value);
	return r;
}

static int
cmd_hash(char *path, int argc, char **argv)
{
	Tab *t;
	TabRow *r;
	char *buf, *col, *value, *field;
	int n, i;

	i = 0;
	selector(argc, argv, &i, &col, &value);
	if(i != argc - 1)
		usage();
	field = argv[i];
	buf = readstdin(&n);
	t = open_tab(path);
	r = row_for(t, col, value);
	if(tab_set_hashed(t, r, field, (uchar*)buf, n) < 0)
		fail("tab: hash %s: %s", field, tab_lasterror());
	commit(t);
	tab_close(t);
	free(buf);
	return 0;
}

static int
cmd_sign(char *path, int argc, char **argv)
{
	Tab *t;
	TabRow *r;
	uchar *sk, *body;
	char *buf, *col, *value, *field, *keyfile;
	int n, i;

	i = 0;
	selector(argc, argv, &i, &col, &value);
	if(i != argc - 2)
		usage();
	field = argv[i];
	keyfile = argv[i+1];
	sk = readkey(keyfile, 64);
	buf = readstdin(&n);
	body = (uchar*)buf;
	t = open_tab(path);
	r = row_for(t, col, value);
	if(tab_set_signed(t, r, field, body, n, sk) < 0)
		fail("tab: sign %s: %s", field, tab_lasterror());
	commit(t);
	tab_close(t);
	memset(sk, 0, 64);
	free(sk);
	free(buf);
	return 0;
}

static int
cmd_verify(char *path, int argc, char **argv)
{
	Tab *t;
	TabRow *r;
	uchar *pk, *body;
	char *col, *value, *field, *pubfile;
	int n, i;

	i = 0;
	selector(argc, argv, &i, &col, &value);
	if(i != argc - 2)
		usage();
	field = argv[i];
	pubfile = argv[i+1];
	pk = readkey(pubfile, 32);
	t = open_tab(path);
	r = row_for(t, col, value);
	body = tab_verify_signed(r, field, pk, &n);
	if(body == nil)
		fail("tab: verify %s: %s", field, tab_lasterror());
	if(write(1, body, n) < 0)
		fail("tab: write: %r");
	tab_close(t);
	free(body);
	free(pk);
	return 0;
}

static void
randbytes(uchar *buf, int n)
{
	int fd, got, r;

	fd = open("/dev/random", OREAD);
	if(fd < 0)
		fd = open("#c/random", OREAD);
	if(fd < 0)
		fail("tab: open random: %r");
	got = 0;
	while(got < n){
		r = read(fd, buf + got, n - got);
		if(r <= 0)
			fail("tab: read random: %r");
		got += r;
	}
	close(fd);
}

static void
writekey(char *path, uchar *key, int n, int perm)
{
	int fd;

	fd = create(path, OWRITE, perm);
	if(fd < 0)
		fail("tab: create %s: %r", path);
	if(write(fd, key, n) != n)
		fail("tab: write %s: %r", path);
	close(fd);
}

static int
cmd_keygen_raw(char *path)
{
	uchar key[32];

	randbytes(key, sizeof key);
	writekey(path, key, sizeof key, 0600);
	crypto_wipe(key, sizeof key);
	return 0;
}

static int
cmd_keygen_pair(char *skpath, char *pkpath)
{
	uchar seed[32], sk[64], pk[32];

	randbytes(seed, sizeof seed);
	crypto_eddsa_key_pair(sk, pk, seed);
	crypto_wipe(seed, sizeof seed);
	writekey(skpath, sk, sizeof sk, 0600);
	writekey(pkpath, pk, sizeof pk, 0644);
	crypto_wipe(sk, sizeof sk);
	return 0;
}

static int
cmd_encrypt(char *path, int argc, char **argv)
{
	Tab *t;
	TabRow *r;
	uchar *key, *plain;
	char *buf, *col, *value, *field, *keyfile;
	int n, i;

	i = 0;
	selector(argc, argv, &i, &col, &value);
	if(i != argc - 2)
		usage();
	field = argv[i];
	keyfile = argv[i+1];
	key = readkey(keyfile, 32);
	buf = readstdin(&n);
	plain = (uchar*)buf;
	t = open_tab(path);
	r = row_for(t, col, value);
	if(tab_set_encrypted(t, r, field, plain, n, key) < 0)
		fail("tab: encrypt %s: %s", field, tab_lasterror());
	commit(t);
	tab_close(t);
	crypto_wipe(key, 32);
	free(key);
	free(buf);
	return 0;
}

static int
cmd_decrypt(char *path, int argc, char **argv)
{
	Tab *t;
	TabRow *r;
	uchar *key, *plain;
	char *col, *value, *field, *keyfile;
	int n, i;

	i = 0;
	selector(argc, argv, &i, &col, &value);
	if(i != argc - 2)
		usage();
	field = argv[i];
	keyfile = argv[i+1];
	key = readkey(keyfile, 32);
	t = open_tab(path);
	r = row_for(t, col, value);
	plain = tab_decrypt(r, field, key, &n);
	if(plain == nil)
		fail("tab: decrypt %s: %s", field, tab_lasterror());
	if(write(1, plain, n) < 0)
		fail("tab: write: %r");
	tab_close(t);
	free(plain);
	free(key);
	return 0;
}

void
main(int argc, char **argv)
{
	if(argc < 3)
		usage();
	if(strcmp(argv[1], "keygen") == 0){
		if(argc == 3)
			exits(cmd_keygen_raw(argv[2]) == 0 ? nil : "fail");
		if(argc == 4)
			exits(cmd_keygen_pair(argv[2], argv[3]) == 0 ? nil : "fail");
		usage();
	}
	if(strcmp(argv[2], "init") == 0){
		if(argc < 5)
			usage();
		exits(cmd_init(argv[1], argv[3], argv + 4, argc - 4) == 0 ? nil : "fail");
	}
	if(strcmp(argv[2], "dump") == 0){
		if(argc != 3)
			usage();
		exits(cmd_dump(argv[1]) == 0 ? nil : "fail");
	}
	if(strcmp(argv[2], "get") == 0)
		exits(cmd_get(argv[1], argc - 3, argv + 3) == 0 ? nil : "fail");
	if(strcmp(argv[2], "set") == 0)
		exits(cmd_set(argv[1], argc - 3, argv + 3) == 0 ? nil : "fail");
	if(strcmp(argv[2], "hash") == 0)
		exits(cmd_hash(argv[1], argc - 3, argv + 3) == 0 ? nil : "fail");
	if(strcmp(argv[2], "sign") == 0)
		exits(cmd_sign(argv[1], argc - 3, argv + 3) == 0 ? nil : "fail");
	if(strcmp(argv[2], "verify") == 0)
		exits(cmd_verify(argv[1], argc - 3, argv + 3) == 0 ? nil : "fail");
	if(strcmp(argv[2], "encrypt") == 0)
		exits(cmd_encrypt(argv[1], argc - 3, argv + 3) == 0 ? nil : "fail");
	if(strcmp(argv[2], "decrypt") == 0)
		exits(cmd_decrypt(argv[1], argc - 3, argv + 3) == 0 ? nil : "fail");
	usage();
}
