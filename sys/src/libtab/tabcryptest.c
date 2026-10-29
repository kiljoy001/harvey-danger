/*
 * libtab sensitive-cell tests under Harvey: HASHED (BLAKE2b and
 * Argon2id) and SIGNED (EdDSA-BLAKE2b) cells, including persistence
 * through a commit/reopen cycle.
 */
#include <u.h>
#include <libc.h>
#include <libtab.h>
#include <monocypher.h>

static int failures;
static int checks;

static void
ok(int cond, char *what)
{
	checks++;
	if(cond)
		return;
	failures++;
	fprint(2, "TABCRYPTO FAIL: %s\n", what);
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

void
main(void)
{
	Tab *t;
	TabRow *r;
	TabColSpec cols[5];
	uchar seed[32], sk[64], pk[32], badpk[32];
	uchar key[32], badkey[32];
	uchar *body;
	int n;

	remove("/tmp/lt_crypto.tab");
	memset(cols, 0, sizeof cols);
	cols[0].name = "name";
	cols[1].name = "pass";
	cols[1].type = "HASHED";
	cols[1].algo = "blake2b";
	cols[2].name = "pass2";
	cols[2].type = "HASHED";
	cols[2].algo = "argon2id";
	cols[3].name = "sig";
	cols[3].type = "SIGNED";
	cols[4].name = "secret";
	cols[4].type = "ENCRYPTED";

	t = tab_create("/tmp/lt_crypto.tab", "accounts", cols, 5);
	ok(t != nil, "tab_create");
	if(t == nil)
		exits("create");
	r = tab_add_row(t, "name", "alice");
	ok(r != nil, "tab_add_row");
	if(r == nil)
		exits("add");

	ok(tab_set_hashed(t, r, "pass", (uchar*)"hunter2", 7) == 0,
		"tab_set_hashed");
	ok(tab_verify_hash(r, "pass", (uchar*)"hunter2", 7) == 1,
		"blake2b verify");
	ok(tab_verify_hash(r, "pass", (uchar*)"hunter3", 7) == 0,
		"blake2b reject");

	ok(tab_set_hashed_argon2id(t, r, "pass2", (uchar*)"correct horse", 13) == 0,
		"tab_set_hashed_argon2id");
	ok(tab_verify_hash(r, "pass2", (uchar*)"correct horse", 13) == 1,
		"argon2id verify");
	ok(tab_verify_hash(r, "pass2", (uchar*)"wrong horse", 11) == 0,
		"argon2id reject");

	memset(seed, 0x37, sizeof seed);
	crypto_eddsa_key_pair(sk, pk, seed);
	memset(badpk, 0x11, sizeof badpk);
	ok(tab_set_signed(t, r, "sig", (uchar*)"uid=1000", 8, sk) == 0,
		"tab_set_signed");
	body = tab_verify_signed(r, "sig", pk, &n);
	ok(body != nil && n == 8 && memcmp(body, "uid=1000", 8) == 0,
		"signed verify");
	free(body);
	body = tab_verify_signed(r, "sig", badpk, &n);
	ok(body == nil, "signed reject wrong key");

	memset(key, 0x5a, sizeof key);
	memset(badkey, 0x99, sizeof badkey);
	ok(tab_set_encrypted(t, r, "secret", (uchar*)"s3cret", 6, key) == 0,
		"tab_set_encrypted");
	body = tab_decrypt(r, "secret", key, &n);
	ok(body != nil && n == 6 && memcmp(body, "s3cret", 6) == 0,
		"encrypted decrypt");
	free(body);
	body = tab_decrypt(r, "secret", badkey, &n);
	ok(body == nil, "encrypted reject wrong key");

	ok(tab_commit(t) == 0, "tab_commit");
	tab_close(t);

	/* Reopen from disk and re-verify the typed cells. */
	t = tab_open("/tmp/lt_crypto.tab");
	ok(t != nil, "tab_open");
	if(t == nil)
		exits("open");
	r = find(t, "name", "alice");
	ok(r != nil, "row survives commit");
	if(r != nil){
		ok(tab_verify_hash(r, "pass", (uchar*)"hunter2", 7) == 1,
			"blake2b verify after reopen");
		ok(tab_verify_hash(r, "pass2", (uchar*)"correct horse", 13) == 1,
			"argon2id verify after reopen");
		body = tab_verify_signed(r, "sig", pk, &n);
		ok(body != nil && n == 8 && memcmp(body, "uid=1000", 8) == 0,
			"signed verify after reopen");
		free(body);
		body = tab_decrypt(r, "secret", key, &n);
		ok(body != nil && n == 6 && memcmp(body, "s3cret", 6) == 0,
			"encrypted decrypt after reopen");
		free(body);
	}
	tab_close(t);
	remove("/tmp/lt_crypto.tab");

	if(failures == 0)
		print("TABCRYPTO PASS: %d checks\n", checks);
	else
		print("TABCRYPTO FAIL: %d of %d checks failed\n", failures, checks);
	exits(failures == 0 ? nil : "failures");
}
