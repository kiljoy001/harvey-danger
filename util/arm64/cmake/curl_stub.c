/* Lean CMake: cURL entry points used by CMake, all failing cleanly. */
#include <stddef.h>
typedef void CURL;
typedef int CURLcode;
typedef int CURLoption;
typedef int CURLINFO;
typedef struct curl_slist { char *data; struct curl_slist *next; } curl_slist;
CURL *curl_easy_init(void) { return NULL; }
void curl_easy_cleanup(CURL *c) { (void)c; }
int curl_easy_setopt(CURL *c, CURLoption o, ...) { (void)c; (void)o; return 2; }
int curl_easy_perform(CURL *c) { (void)c; return 2; }
const char *curl_easy_strerror(int e) { (void)e; return "curl disabled in lean CMake"; }
int curl_easy_getinfo(CURL *c, CURLINFO i, ...) { (void)c; (void)i; return 2; }
struct curl_slist *curl_slist_append(struct curl_slist *l, const char *s) { (void)l; (void)s; return NULL; }
void curl_slist_free_all(struct curl_slist *l) { (void)l; }
int curl_global_init(long f) { (void)f; return 0; }
void curl_global_cleanup(void) {}
const void *curl_version_info(int a) { (void)a; return NULL; }
