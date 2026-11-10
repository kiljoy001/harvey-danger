/* Lean CMake: libuv entry points used by CMake, all failing cleanly. */
#include <stddef.h>
int uv_loop_init(void *l) { (void)l; return -1; }
int uv_loop_close(void *l) { (void)l; return -1; }
int uv_run(void *l, int m) { (void)l; (void)m; return 0; }
int uv_async_init(void *l, void *h, void (*cb)(void *)) { (void)l; (void)h; (void)cb; return -1; }
int uv_async_send(void *h) { (void)h; return -1; }
int uv_close(void *h, void (*cb)(void *)) { (void)h; (void)cb; return -1; }
int uv_is_closing(void *h) { (void)h; return 1; }
int uv_pipe_init(void *l, void *h, int i) { (void)l; (void)h; (void)i; return -1; }
int uv_pipe_open(void *h, int fd) { (void)h; (void)fd; return -1; }
int uv_signal_init(void *l, void *h) { (void)l; (void)h; return -1; }
int uv_signal_start(void *h, void (*cb)(void *, int), int s) { (void)h; (void)cb; (void)s; return -1; }
int uv_signal_stop(void *h) { (void)h; return -1; }
int uv_spawn(void *l, void *p, const void *o) { (void)l; (void)p; (void)o; return -1; }
int uv_timer_init(void *l, void *h) { (void)l; (void)h; return -1; }
int uv_timer_start(void *h, void (*cb)(void *), unsigned long long t, unsigned long long r) { (void)h; (void)cb; (void)t; (void)r; return -1; }
int uv_tty_init(void *l, void *h, int fd, int r) { (void)l; (void)h; (void)fd; (void)r; return -1; }
const char *uv_strerror(int e) { (void)e; return "libuv disabled in lean CMake"; }
int uv_cpumask_size(void) { return 0; }
void *uv_default_loop(void) { return NULL; }
void uv_disable_stdio_inheritance(void) {}
int uv_fs_get_system_error(void *r) { (void)r; return -1; }
int uv_fs_open(void *l, void *r, const char *p, int f, int m, void *cb) { (void)l; (void)r; (void)p; (void)f; (void)m; (void)cb; return -1; }
int uv_fs_stat(void *l, void *r, const char *p, void *cb) { (void)l; (void)r; (void)p; (void)cb; return -1; }
int uv_fs_link(void *l, void *r, const char *p, const char *n, void *cb) { (void)l; (void)r; (void)p; (void)n; (void)cb; return -1; }
int uv_fs_symlink(void *l, void *r, const char *p, const char *n, int f, void *cb) { (void)l; (void)r; (void)p; (void)n; (void)f; (void)cb; return -1; }
void uv_fs_req_cleanup(void *r) { (void)r; }
int uv_read_start(void *s, void *a, void *cb) { (void)s; (void)a; (void)cb; return -1; }
int uv_read_stop(void *s) { (void)s; return -1; }
int uv_gettimeofday(void *t) { (void)t; return -1; }
int uv_translate_sys_error(int e) { (void)e; return e; }
