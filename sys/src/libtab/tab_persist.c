/*
 * libtab — persistence.
 *
 * Two write paths, both serialising via tab_serialize() first:
 *
 *   POSIX path (dial == nil):
 *     Write to <path>.tmp.<pid>, fsync, rename(2) over the original.
 *     Atomicity guaranteed by the kernel's rename semantics.
 *
 *   9P path  (dial != nil):
 *     Dial the named fileserver via lib9pclient, fsopen the target
 *     path for OWRITE|OTRUNC, fswrite the bytes, fsclose, fsunmount.
 *     Atomicity, durability, concurrent-writer serialisation are the
 *     fileserver's problem — libtab just emits bytes.  This is the
 *     intended path for cross-namespace and admin-inspectable tables
 *     once the system has a 9P fileserver hosting the .tab tree.
 *
 * Persistence trigger model:
 *
 *   - Any successful tab_set_* sets t->dirty = 1.
 *   - tab_commit(t) flushes and clears the flag.
 *   - tab_close(t) auto-flushes if dirty.  Errors at this stage
 *     surface via tab_lasterror() but cannot fail the close.
 *   - Consumers that need a discard semantic call tab_discard()
 *     before close (added when a consumer asks for it).
 *
 * Concurrent writers:
 *
 *   The rename above makes a commit atomic, so no reader ever sees a
 *   torn file.  The read-modify-write cycle around it is not atomic:
 *   two handles that open the same table, mutate, and commit will have
 *   the second silently discard the first.  A lock does not fix that --
 *   both read before either wrote -- and a lock held from open to commit
 *   would span arbitrary caller code, including a human in an editor,
 *   which is the case this format exists to support.
 *
 *   So tab_commit does a compare-and-swap instead: tab_open records the
 *   file's identity, tab_commit re-checks it and refuses if it moved.
 *   That covers the writer a lock cannot see -- someone running `vi`
 *   or `ed` on the table -- at the cost of making the caller decide how
 *   to merge.  tab_reload() gives them a way to start over from disk.
 *
 *   The 9P path does none of this: serialising concurrent writers is
 *   the fileserver's job, and a 9P server is a serialisation point by
 *   construction.
 */

#include <tab_internal.h>
#ifdef USE_POSIX
#include <9pclient.h>

/* POSIX rename(2) — not declared in plan9port's libc.h, so we
 * forward-declare here.  Signature is the standard one. */
extern int rename(const char *oldpath, const char *newpath);
#endif

/* Write `bytes` of length `n` to `path` via POSIX, atomically.  The
 * temp-file is `path.tmp.<pid>` in the same directory; rename(2)
 * moves it over the original in one step. */
static int
posix_write_atomic(const char *path, const char *bytes, int n)
{
#ifdef USE_POSIX
	char tmp[1024];
	int fd;
	long written;

	snprint(tmp, sizeof tmp, "%s.tmp.%d", path, (int)getpid());
	fd = create(tmp, OWRITE, 0644);
	if(fd < 0){
		tab_seterror("tab_commit: create %s: %r", tmp);
		return -1;
	}
	written = write(fd, (void *)bytes, n);
	if(written != n){
		tab_seterror("tab_commit: short write to %s: %ld of %d (%r)",
			tmp, written, n);
		close(fd);
		remove(tmp);
		return -1;
	}
	/* fsync via the underlying fd.  plan9port's libc maps this to
	 * the POSIX fsync(2) on Unix. */
	if(fsync(fd) < 0){
		tab_seterror("tab_commit: fsync %s: %r", tmp);
		close(fd);
		remove(tmp);
		return -1;
	}
	close(fd);
	if(rename(tmp, path) < 0){
		tab_seterror("tab_commit: rename %s -> %s: %r", tmp, path);
		remove(tmp);
		return -1;
	}
	return 0;
#else
	int fd;
	long written;

	fd = open((char *)path, OWRITE|OTRUNC);
	if(fd < 0)
		fd = create((char *)path, OWRITE, 0644);
	if(fd < 0){
		tab_seterror("tab_commit: open/create %s: %r", path);
		return -1;
	}
	written = write(fd, (void *)bytes, n);
	if(written != n){
		tab_seterror("tab_commit: short write to %s: %ld of %d (%r)",
			path, written, n);
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
#endif
}

/* Write `bytes` of length `n` to `path` via the named 9P fileserver.
 * The dial string is anything `nsmount` / `dial` accepts — a
 * service name (mounted under $NAMESPACE/<name>) or a network
 * address. */
static int
ninep_write(const char *dial, const char *path, const char *bytes, int n)
{
#ifdef USE_POSIX
	CFsys *fs;
	CFid *fid;
	long written;

	fs = nsmount((char *)dial, nil);
	if(fs == nil){
		tab_seterror("tab_commit: nsmount %s: %r", dial);
		return -1;
	}
	fid = fsopen(fs, (char *)path, OWRITE | OTRUNC);
	if(fid == nil){
		/* Try create instead — most 9P servers return an error on
		 * open of a non-existent file. */
		fid = fscreate(fs, (char *)path, OWRITE, 0644);
	}
	if(fid == nil){
		tab_seterror("tab_commit: fsopen/fscreate %s on %s: %r",
			path, dial);
		fsunmount(fs);
		return -1;
	}
	written = fswrite(fid, (void *)bytes, n);
	if(written != n){
		tab_seterror("tab_commit: short fswrite to %s: %ld of %d (%r)",
			path, written, n);
		fsclose(fid);
		fsunmount(fs);
		return -1;
	}
	fsclose(fid);
	fsunmount(fs);
	return 0;
#else
	USED(dial);
	USED(path);
	USED(bytes);
	USED(n);
	tab_seterror("tab_commit: 9P dial persistence requires plan9port lib9pclient");
	return -1;
#endif
}

/* BLAKE2b-256 of the file, for the same-second/same-size case the
 * Dir fields cannot see (Harvey's ramfs does not advance qid.vers). */
static int
file_hash(const char *path, uchar out[32])
{
	crypto_blake2b_ctx ctx;
	uchar buf[4096];
	int fd, n;

	fd = open((char*)path, OREAD);
	if(fd < 0)
		return -1;
	crypto_blake2b_init(&ctx, 32);
	while((n = read(fd, buf, sizeof buf)) > 0)
		crypto_blake2b_update(&ctx, buf, n);
	close(fd);
	if(n < 0)
		return -1;
	crypto_blake2b_final(&ctx, out);
	return 0;
}

/*
 * File identity for the commit-time compare-and-swap.
 *
 * qid.path is the inode, which any rename changes -- and every commit is
 * a rename, so this catches a foreign commit even when the clock does not
 * move.  qid.vers is a real version counter on native Plan 9; under
 * plan9port on Linux it degrades to mtime+ctime (see _p9dir.c), which is
 * second-granularity, so length is carried too and catches a same-second
 * edit that changes the file's size.
 *
 * None of these four is sufficient alone.  Together they miss only a
 * same-second, same-inode, same-length rewrite -- which a rename cannot
 * produce, so in practice only an in-place editor could, and it would have
 * to preserve the size exactly.
 */
void
tab_fileid_record(Tab *t)
{
	Dir *d;

	if(t == nil || t->path == nil)
		return;
	d = dirstat(t->path);
	if(d == nil){
		/* No file yet: a created table, or one whose file has not
		 * been written.  Nothing to be stale against. */
		t->seen = 0;
		return;
	}
	t->id_qpath = d->qid.path;
	t->id_qvers = d->qid.vers;
	t->id_mtime = d->mtime;
	t->id_length = d->length;
	t->seen = 1;
	free(d);

	t->id_hashok = 0;
	if(file_hash(t->path, t->id_blake) == 0)
		t->id_hashok = 1;
}

int
tab_fileid_check(Tab *t)
{
	Dir *d;
	int changed;

	if(t == nil || t->path == nil)
		return 0;
	/* Never opened from a file: the first commit is the caller's own
	 * deliberate write, not an overwrite of someone else's work. */
	if(!t->seen)
		return 0;

	d = dirstat(t->path);
	if(d == nil){
		tab_seterror("tab_commit: %s has been removed since it was "
			"opened; reload or write to a new path", t->path);
		return -1;
	}
	changed = d->qid.path != t->id_qpath
	       || d->qid.vers != t->id_qvers
	       || d->mtime    != t->id_mtime
	       || d->length   != t->id_length;
	free(d);

	/* Metadata alone misses a same-second rewrite on file systems that
	 * do not advance qid.vers.  Compare content when we recorded it. */
	if(!changed && t->id_hashok){
		uchar h[32];
		if(file_hash(t->path, h) < 0){
			tab_seterror("tab_commit: cannot reread %s: %r",
				t->path);
			return -1;
		}
		if(memcmp(h, t->id_blake, sizeof h) != 0)
			changed = 1;
	}
	if(changed){
		tab_seterror("tab_commit: %s changed since it was opened; "
			"another writer got there first -- tab_reload() to "
			"take their version, or commit to a different path",
			t->path);
		return -1;
	}
	return 0;
}

int
tab_commit(Tab *t)
{
	char *buf;
	int len, rc;

	tab_clearerror();
	if(t == nil){
		tab_seterror("tab_commit: nil Tab");
		return -1;
	}

	/* Compare-and-swap: refuse rather than discard another writer's
	 * work.  Only the POSIX path -- on 9P, serialising writers is the
	 * fileserver's job. */
	if(t->dial == nil && tab_fileid_check(t) < 0)
		return -1;

	buf = tab_serialize(t, &len);
	if(buf == nil)
		return -1;

	if(t->dial != nil){
		const char *rp = t->remote_path != nil ? t->remote_path
		                                       : t->path;
		rc = ninep_write(t->dial, rp, buf, len);
	}else{
		rc = posix_write_atomic(t->path, buf, len);
	}
	free(buf);
	if(rc == 0){
		t->dirty = 0;
		/* Our own write is now the file's identity, so the next
		 * commit from this handle is not fooled by it. */
		if(t->dial == nil)
			tab_fileid_record(t);
	}
	return rc;
}
