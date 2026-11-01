/*
 * keyroot - hardware-rooted secret store.
 *
 * Client-side ABI for the 9P service described in doc/auth/keyroot.md.
 * The server (keyroot) and its backends use the full blob format; clients
 * only need these constants and the four calls below.
 */
#ifndef _KEYROOT_H_
#define _KEYROOT_H_

enum
{
	KrootMagiclen	= 6,		/* "KROOT1" */
	KrootHdrlen	= 60,		/* fixed header, includes nonce and tag */
	KrootNoncelen	= 12,
	KrootTaglen	= 16,
	KrootMaxlabel	= 64,
	KrootMaxplain	= 64*1024,
	KrootMaxblob	= KrootHdrlen + KrootMaxlabel + KrootMaxplain,
	KrootKeylen	= 32,		/* default derive output */
	KrootKeymin	= 16,
	KrootKeymax	= 64,
};

enum
{
	KrootMock	= 1,
	KrootOptee	= 2,
	KrootTpm	= 3,
};

enum
{
	KrootAesGcm	= 1,
	KrootTpmSeal	= 2,
};

/* Fixed header offsets.  Integers are big-endian; see the blob layout. */
enum
{
	KrootOmagic	= 0,
	KrootOversion	= 6,
	KrootObackend	= 7,
	KrootOalg	= 8,
	KrootOlabellen	= 9,
	KrootOplainlen	= 12,
	KrootObloblen	= 16,
	KrootOflags	= 20,
	KrootOnonce	= 32,
	KrootOtag	= 44,
	KrootOlabel	= 60,
};

/*
 * keyroot_setmtpt changes the mount point used by the calls below.
 * The default is /mnt/keyroot.  Passing nil restores the default.
 */
void	keyroot_setmtpt(char *mtpt);

/*
 * keyroot_seal seals nplain bytes under label into blob.  On entry *nblob
 * is the capacity of blob; on return it is the blob length.  Returns 0 or
 * -1 with errstr set.
 */
int	keyroot_seal(char *label, void *plain, int nplain,
		     void *blob, int *nblob);

/*
 * keyroot_unseal opens a blob.  On entry *nplain is the capacity of
 * plain; on return it is the plaintext length.  label is filled with the
 * NUL-terminated label carried in the blob, up to nlabel bytes.  Returns
 * 0 or -1 with errstr set.
 */
int	keyroot_unseal(void *blob, int nblob, void *plain, int *nplain,
		       char *label, int nlabel);

/*
 * keyroot_derive writes nkey deterministic key bytes for label.  nkey
 * must be between KrootKeymin and KrootKeymax.  Returns 0 or -1.
 */
int	keyroot_derive(char *label, void *key, int nkey);

/*
 * keyroot_status reads the key=value status text into buf.  Returns the
 * number of bytes read, or -1 with errstr set.
 */
int	keyroot_status(char *buf, int nbuf);

#endif
