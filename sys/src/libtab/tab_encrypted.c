/*
 * libtab — ENCRYPTED column type.
 *
 * Wire format of an ENCRYPTED cell:
 *
 *	encrypted: <base64-url-safe( ver | nonce[24] | mac[16] | ct )>
 *
 * One base64 segment inside the `encrypted:` tag, unlike SIGNED's two.
 * SIGNED splits body from signature because the body is worth reading
 * on its own with `base64 -d`; here every byte is opaque, so a second
 * segment would buy nothing and cost a parse.
 *
 * The header travels with the ciphertext so decrypt is self-contained,
 * the same property argon2id cells have in tab_hashed.c: a reader needs
 * the cell and the key, nothing else.  `ver` leaves room to change
 * primitives later without guessing at what an old cell contains.
 *
 * Crypto: monocypher XChaCha20-Poly1305 (crypto_aead_lock /
 * crypto_aead_unlock).  Explicitly *not* AES-256-GCM: monocypher ships
 * no AES, and vendoring a second crypto library would undo the point of
 * a single bundled dependency.  The 24-byte nonce matters here --
 * cells are rewritten by callers we do not control, and a random
 * 96-bit nonce would eventually repeat.  At 192 bits, random per-cell
 * nonces need no counter and no state.
 *
 * Key bytes flow in from the caller, as with SIGNED.  Libtab does not
 * open key files, resolve principals, or derive keys from passwords.
 * A schema column may carry `key=<name>` as a label for consumers to
 * read via tab_col_attr() — libtab never interprets it.
 *
 * Row identity hashes the ciphertext, because it can hash nothing else:
 * tab_open rebuilds the rowmap from on-disk text and never holds a key.
 * One visible consequence, asserted in the tests: encrypting the same
 * plaintext twice yields two distinct rows.  That is the data model
 * working, not a dedup bug.
 *
 * Mutations are in-memory until tab_commit() or close-time auto-commit.
 */

#include <tab_internal.h>
#include <monocypher.h>

/* In tab_rowmap.c. */
int tab_rowmap_rehash(Tab *t, Ndbtuple *chain);

/* From the kernel via getrandom(2), same as the argon2id salt in
 * tab_hashed.c.  Declared here rather than in tab_internal.h to keep
 * the type modules independent of each other. */
extern long getrandom(void *, unsigned long, unsigned int);

enum {
	EncVer1		= 0x01,
	EncNonceLen	= 24,
	EncMacLen	= 16,
	EncHdrLen	= 1 + EncNonceLen + EncMacLen,
};

/* Find a tuple by attr in a chain.  Mirrors the helpers in tab_hashed.c
 * and tab_signed.c; keeping the type modules independent is worth the
 * duplication. */
static Ndbtuple *
find_tuple(Ndbtuple *chain, const char *col)
{
	Ndbtuple *t;
	for(t = chain; t != nil; t = t->entry){
		if(strcmp(t->attr, col) == 0)
			return t;
	}
	return nil;
}

static TabCol *
find_col(Tab *t, const char *name)
{
	int i;
	for(i = 0; i < t->schema.ncols; i++){
		if(strcmp(t->schema.cols[i].name, name) == 0)
			return &t->schema.cols[i];
	}
	return nil;
}

/* Mutate a cell's text, then re-hash the row.  On collision (mutation
 * would duplicate an existing row) the original cell is restored.
 * Same shape as tab_signed.c's mutate_cell. */
static int
mutate_cell(Tab *t, Ndbtuple *chain, const char *col, const char *newval)
{
	Ndbtuple *tup;
	char *saved_inline, *saved_heap;
	ulong saved_ptr;
	int rh;

	tup = find_tuple(chain, col);
	if(tup == nil){
		/* No existing cell — append a fresh tuple. */
		Ndbtuple *last, *newtup;
		int rh2;
		last = chain;
		while(last->entry != nil) last = last->entry;
		newtup = ndbnew((char *)col, (char *)newval);
		if(newtup == nil){
			tab_seterror("tab_set_encrypted: ndbnew failed");
			return -1;
		}
		last->entry = newtup;
		rh2 = tab_rowmap_rehash(t, chain);
		if(rh2 == 0){
			t->dirty = 1;
			return 0;
		}
		last->entry = nil;
		ndbfree(newtup);
		return -1;
	}

	saved_inline = nil;
	saved_heap = nil;
	saved_ptr = tup->ptr;
	if(tup->val == tup->valbuf){
		saved_inline = strdup(tup->valbuf);
		if(saved_inline == nil){
			tab_seterror("tab_set_encrypted: out of memory snapshotting");
			return -1;
		}
	}else{
		saved_heap = tup->val;
		tup->val = tup->valbuf;
	}

	ndbsetval(tup, (char *)newval, strlen(newval));
	tab_tuple_mark_nil(tup, 0);

	rh = tab_rowmap_rehash(t, chain);
	if(rh == 0){
		t->dirty = 1;
		free(saved_inline);
		free(saved_heap);
		return 0;
	}

	if(tup->val != tup->valbuf)
		free(tup->val);
	if(saved_heap != nil){
		tup->val = saved_heap;
	}else{
		tup->val = tup->valbuf;
		ndbsetval(tup, saved_inline, strlen(saved_inline));
		free(saved_inline);
	}
	tup->ptr = saved_ptr;
	return -1;
}

/* Validate that `col` exists and is ENCRYPTED-typed. */
static TabCol *
validate_encrypted_col(Tab *t, const char *col, const char *who)
{
	TabCol *c = find_col(t, col);
	if(c == nil){
		tab_seterror("%s: column %q not in schema", who, col);
		return nil;
	}
	if(c->type == nil || strcmp(c->type, "ENCRYPTED") != 0){
		tab_seterror("%s: column %q is not ENCRYPTED (type=%q)",
			who, col, c->type != nil ? c->type : "(plain)");
		return nil;
	}
	return c;
}

/* ---- Public: encrypt + decrypt ---- */

int
tab_set_encrypted(Tab *t, TabRow *r, const char *col,
	const unsigned char *plain, int n,
	const unsigned char key[32])
{
	uint8_t nonce[EncNonceLen], mac[EncMacLen];
	uint8_t *wire;
	char *cell;
	int wlen, rc;

	tab_clearerror();
	if(t == nil || r == nil || col == nil || key == nil
	   || (plain == nil && n != 0)){
		tab_seterror("tab_set_encrypted: nil argument");
		return -1;
	}
	if(n < 0){
		tab_seterror("tab_set_encrypted: negative plaintext length");
		return -1;
	}
	if(validate_encrypted_col(t, col, "tab_set_encrypted") == nil)
		return -1;

	/* A repeated (key, nonce) pair with XChaCha20 leaks the xor of two
	 * plaintexts, so a short read here has to be fatal rather than
	 * quietly leaving the buffer as it was. */
	if(getrandom(nonce, sizeof nonce, 0) != (long)sizeof nonce){
		tab_seterror("tab_set_encrypted: cannot read %d random bytes "
			"for nonce", EncNonceLen);
		return -1;
	}

	wlen = EncHdrLen + n;
	wire = malloc(wlen);
	if(wire == nil){
		tab_seterror("tab_set_encrypted: out of memory for cell");
		memset(nonce, 0, sizeof nonce);
		return -1;
	}

	crypto_aead_lock(wire + EncHdrLen, mac, key, nonce,
		nil, 0, plain, (size_t)n);

	wire[0] = EncVer1;
	memcpy(wire + 1, nonce, EncNonceLen);
	memcpy(wire + 1 + EncNonceLen, mac, EncMacLen);

	cell = tab_cell_encode("ENCRYPTED", wire, wlen);
	memset(wire, 0, wlen);
	free(wire);
	memset(nonce, 0, sizeof nonce);
	memset(mac, 0, sizeof mac);
	if(cell == nil)
		return -1;
	if(tab_check_cell_size(col, cell, "tab_set_encrypted") < 0){
		free(cell);
		return -1;
	}
	rc = mutate_cell(t, r->chain, col, cell);
	free(cell);
	return rc;
}

unsigned char *
tab_decrypt(TabRow *r, const char *col,
	const unsigned char key[32], int *outlen)
{
	const char *cellv;
	uint8_t *wire, *plain;
	int wlen, ctlen;

	tab_clearerror();
	if(r == nil || col == nil || key == nil || outlen == nil){
		tab_seterror("tab_decrypt: nil argument");
		return nil;
	}
	cellv = tab_row_cell(r->chain, col);
	if(cellv == nil || *cellv == 0){
		tab_seterror("tab_decrypt: cell %q is empty", col);
		return nil;
	}

	wire = tab_cell_decode(cellv, "ENCRYPTED", &wlen);
	if(wire == nil)
		return nil;
	if(wlen < EncHdrLen){
		tab_seterror("tab_decrypt: cell is %d bytes, too short for "
			"a %d-byte header", wlen, EncHdrLen);
		free(wire);
		return nil;
	}
	if(wire[0] != EncVer1){
		tab_seterror("tab_decrypt: unknown ENCRYPTED version 0x%02x",
			wire[0]);
		free(wire);
		return nil;
	}
	ctlen = wlen - EncHdrLen;

	/* Zero-length plaintext is legal; malloc(0) is not portable. */
	plain = malloc(ctlen > 0 ? ctlen : 1);
	if(plain == nil){
		tab_seterror("tab_decrypt: out of memory for plaintext");
		free(wire);
		return nil;
	}

	if(crypto_aead_unlock(plain, wire + 1 + EncNonceLen, key,
			wire + 1, nil, 0,
			wire + EncHdrLen, (size_t)ctlen) != 0){
		/* Wrong key or tampered cell — indistinguishable by design. */
		tab_seterror("tab_decrypt: authentication failed for cell %q", col);
		memset(plain, 0, ctlen > 0 ? ctlen : 1);
		free(plain);
		free(wire);
		return nil;
	}
	free(wire);
	*outlen = ctlen;
	return plain;
}
