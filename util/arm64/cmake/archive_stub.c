/* Lean CMake: libarchive entry points, all failing cleanly. */
#include <stddef.h>
struct archive;
struct archive_entry;
struct archive *archive_write_new(void) { return NULL; }
int archive_write_set_format_pax_restricted(struct archive *a) { (void)a; return -1; }
int archive_write_set_format_gnutar(struct archive *a) { (void)a; return -1; }
int archive_write_set_format_zip(struct archive *a) { (void)a; return -1; }
int archive_write_set_format_7zip(struct archive *a) { (void)a; return -1; }
int archive_write_set_format_raw(struct archive *a) { (void)a; return -1; }
int archive_write_add_filter_gzip(struct archive *a) { (void)a; return -1; }
int archive_write_add_filter_bzip2(struct archive *a) { (void)a; return -1; }
int archive_write_add_filter_xz(struct archive *a) { (void)a; return -1; }
int archive_write_add_filter_zstd(struct archive *a) { (void)a; return -1; }
int archive_write_open_filename(struct archive *a, const char *n) { (void)a; (void)n; return -1; }
int archive_write_header(struct archive *a, struct archive_entry *e) { (void)a; (void)e; return -1; }
long long archive_write_data(struct archive *a, const void *b, size_t n) { (void)a; (void)b; (void)n; return -1; }
int archive_write_close(struct archive *a) { (void)a; return -1; }
int archive_write_free(struct archive *a) { (void)a; return -1; }
struct archive_entry *archive_entry_new(void) { return NULL; }
void archive_entry_free(struct archive_entry *e) { (void)e; }
void archive_entry_set_pathname(struct archive_entry *e, const char *p) { (void)e; (void)p; }
void archive_entry_set_filetype(struct archive_entry *e, unsigned int t) { (void)e; (void)t; }
void archive_entry_set_perm(struct archive_entry *e, int p) { (void)e; (void)p; }
void archive_entry_set_size(struct archive_entry *e, long long s) { (void)e; (void)s; }
void archive_entry_set_mtime(struct archive_entry *e, long long s, long n) { (void)e; (void)s; (void)n; }
struct archive *archive_read_new(void) { return NULL; }
int archive_read_support_filter_all(struct archive *a) { (void)a; return -1; }
int archive_read_support_format_all(struct archive *a) { (void)a; return -1; }
int archive_read_open_filename(struct archive *a, const char *n, size_t b) { (void)a; (void)n; (void)b; return -1; }
int archive_read_next_header(struct archive *a, struct archive_entry **e) { (void)a; (void)e; return -1; }
long long archive_read_data(struct archive *a, void *b, size_t n) { (void)a; (void)b; (void)n; return -1; }
int archive_read_free(struct archive *a) { (void)a; return -1; }
const char *archive_error_string(struct archive *a) { (void)a; return "libarchive disabled in lean CMake"; }

/* Additional entry points used by cmArchiveWrite/ctest. */
void archive_entry_acl_clear(struct archive_entry *e) { (void)e; }
int archive_entry_copy_pathname_w(struct archive_entry *e, const void *p) { (void)e; (void)p; return -1; }
int archive_entry_copy_sourcepath_w(struct archive_entry *e, const void *p) { (void)e; (void)p; return -1; }
int archive_entry_filetype(struct archive_entry *e) { (void)e; return 0; }
long long archive_entry_gid(struct archive_entry *e) { (void)e; return 0; }
const char *archive_entry_gname(struct archive_entry *e) { (void)e; return NULL; }
const char *archive_entry_hardlink(struct archive_entry *e) { (void)e; return NULL; }
long long archive_entry_mtime(struct archive_entry *e) { (void)e; return 0; }
long long archive_entry_nlink(struct archive_entry *e) { (void)e; return 0; }
const void *archive_entry_pathname_w(struct archive_entry *e) { (void)e; return NULL; }
int archive_entry_perm(struct archive_entry *e) { (void)e; return 0; }
long long archive_entry_rdevmajor(struct archive_entry *e) { (void)e; return 0; }
long long archive_entry_rdevminor(struct archive_entry *e) { (void)e; return 0; }
void archive_entry_set_atime(struct archive_entry *e, long long s, long n) { (void)e; (void)s; (void)n; }
void archive_entry_set_birthtime(struct archive_entry *e, long long s, long n) { (void)e; (void)s; (void)n; }
void archive_entry_set_ctime(struct archive_entry *e, long long s, long n) { (void)e; (void)s; (void)n; }
void archive_entry_set_fflags(struct archive_entry *e, unsigned long f, unsigned long m) { (void)e; (void)f; (void)m; }
void archive_entry_set_gid(struct archive_entry *e, long long g) { (void)e; (void)g; }
void archive_entry_set_gname(struct archive_entry *e, const char *g) { (void)e; (void)g; }
void archive_entry_set_uid(struct archive_entry *e, long long u) { (void)e; (void)u; }
void archive_entry_set_uname(struct archive_entry *e, const char *u) { (void)e; (void)u; }
long long archive_entry_size(struct archive_entry *e) { (void)e; return 0; }
void archive_entry_sparse_clear(struct archive_entry *e) { (void)e; }
const char *archive_entry_strmode(struct archive_entry *e) { (void)e; return "----------"; }
const char *archive_entry_symlink(struct archive_entry *e) { (void)e; return NULL; }
long long archive_entry_uid(struct archive_entry *e) { (void)e; return 0; }
const char *archive_entry_uname(struct archive_entry *e) { (void)e; return NULL; }
void archive_entry_xattr_clear(struct archive_entry *e) { (void)e; }
int archive_match_excluded(void *m, void *e) { (void)m; (void)e; return 0; }
void archive_match_free(void *m) { (void)m; }
int archive_match_include_pattern(void *m, const char *p) { (void)m; (void)p; return -1; }
void *archive_match_new(void) { return NULL; }
int archive_match_path_unmatched_inclusions_next(void *m, const char **p) { (void)m; (void)p; return -1; }
int archive_read_close(struct archive *a) { (void)a; return -1; }
int archive_read_data_block(struct archive *a, const void **b, size_t *s, long long *o) { (void)a; (void)b; (void)s; (void)o; return -1; }
int archive_read_disk_entry_from_file(void *d, void *e, int fd, const void *st) { (void)d; (void)e; (void)fd; (void)st; return -1; }
void *archive_read_disk_new(void) { return NULL; }
int archive_read_disk_set_standard_lookup(void *d) { (void)d; return -1; }
int archive_read_open_filename_w(struct archive *a, const void *n, size_t b) { (void)a; (void)n; (void)b; return -1; }
int archive_write_add_filter_compress(struct archive *a) { (void)a; return -1; }
int archive_write_add_filter_lzma(struct archive *a) { (void)a; return -1; }
int archive_write_add_filter_none(struct archive *a) { (void)a; return -1; }
int archive_write_data_block(struct archive *a, const void *b, size_t n, long long o) { (void)a; (void)b; (void)n; (void)o; return -1; }
void *archive_write_disk_new(void) { return NULL; }
int archive_write_disk_set_options(void *a, int o) { (void)a; (void)o; return -1; }
int archive_write_finish_entry(struct archive *a) { (void)a; return -1; }
int archive_write_open(struct archive *a, void *c, void *op, void *wr, void *cl) { (void)a; (void)c; (void)op; (void)wr; (void)cl; return -1; }
int archive_write_set_bytes_in_last_block(struct archive *a, int b) { (void)a; (void)b; return -1; }
int archive_write_set_filter_option(struct archive *a, const char *m, const char *o, const char *v) { (void)a; (void)m; (void)o; (void)v; return -1; }
int archive_write_set_format_by_name(struct archive *a, const char *n) { (void)a; (void)n; return -1; }
