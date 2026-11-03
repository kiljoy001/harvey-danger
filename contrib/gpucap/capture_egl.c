/*
 * Make the panfrost Gallium driver build and submit a real job chain while
 * running against Mesa's DRM shim, so the shim can print the chain bytes.
 * All GL entry points come from eglGetProcAddress so no GLES vendor library
 * is needed.
 */
#include <stdio.h>
#include <stdlib.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#define GET(name) do { \
	p_##name = (void *)eglGetProcAddress(#name); \
	if(p_##name == NULL){ fprintf(stderr, "missing %s\n", #name); return 1; } \
} while(0)

static void (*p_glGenTextures)(int, unsigned *);
static void (*p_glBindTexture)(unsigned, unsigned);
static void (*p_glTexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
static void (*p_glGenFramebuffers)(int, unsigned *);
static void (*p_glBindFramebuffer)(unsigned, unsigned);
static void (*p_glFramebufferTexture2D)(unsigned, unsigned, unsigned, unsigned, int);
static unsigned (*p_glCheckFramebufferStatus)(unsigned);
static void (*p_glViewport)(int, int, int, int);
static void (*p_glClearColor)(float, float, float, float);
static void (*p_glClear)(unsigned);
static void (*p_glFinish)(void);
static const unsigned char *(*p_glGetString)(unsigned);

int
main(void)
{
	PFNEGLGETPLATFORMDISPLAYEXTPROC getPlatformDisplay;
	EGLDisplay dpy;
	EGLConfig cfg;
	EGLContext ctx;
	EGLint major, minor;
	EGLint nconf = 0;
	EGLint attrs[] = {
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 8,
		EGL_NONE
	};
	EGLint ctxattrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	unsigned tex, fbo;
	const char *ver;

	getPlatformDisplay = (PFNEGLGETPLATFORMDISPLAYEXTPROC)
		eglGetProcAddress("eglGetPlatformDisplayEXT");
	if(getPlatformDisplay == NULL){
		fprintf(stderr, "no eglGetPlatformDisplayEXT\n");
		return 1;
	}
	dpy = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
	if(dpy == EGL_NO_DISPLAY){
		fprintf(stderr, "no display\n");
		return 1;
	}
	if(!eglInitialize(dpy, &major, &minor)){
		fprintf(stderr, "eglInitialize failed\n");
		return 1;
	}
	fprintf(stderr, "EGL %d.%d vendor %s\n", major, minor,
	        eglQueryString(dpy, EGL_VENDOR));
	if(!eglChooseConfig(dpy, attrs, &cfg, 1, &nconf) || nconf < 1){
		fprintf(stderr, "no config\n");
		return 1;
	}
	ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxattrs);
	if(ctx == EGL_NO_CONTEXT){
		fprintf(stderr, "no context\n");
		return 1;
	}
	if(!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)){
		fprintf(stderr, "makeCurrent failed\n");
		return 1;
	}
	GET(glGenTextures);
	GET(glBindTexture);
	GET(glTexImage2D);
	GET(glGenFramebuffers);
	GET(glBindFramebuffer);
	GET(glFramebufferTexture2D);
	GET(glCheckFramebufferStatus);
	GET(glViewport);
	GET(glClearColor);
	GET(glClear);
	GET(glFinish);
	GET(glGetString);
	ver = (const char *)p_glGetString(0x1F02 /* GL_VERSION */);
	fprintf(stderr, "GL_VERSION %s\n", ver ? ver : "(null)");
	fprintf(stderr, "GL_RENDERER %s\n",
	        (const char *)p_glGetString(0x1F01 /* GL_RENDERER */));

	p_glGenTextures(1, &tex);
	p_glBindTexture(0x0DE1 /* GL_TEXTURE_2D */, tex);
	p_glTexImage2D(0x0DE1, 0, 0x1908 /* GL_RGBA */, 64, 64, 0, 0x1908,
	               0x1401 /* UNSIGNED_BYTE */, NULL);
	p_glGenFramebuffers(1, &fbo);
	p_glBindFramebuffer(0x8D40 /* FRAMEBUFFER */, fbo);
	p_glFramebufferTexture2D(0x8D40, 0x8CE0 /* COLOR_ATTACHMENT0 */,
	                         0x0DE1, tex, 0);
	if(p_glCheckFramebufferStatus(0x8D40) != 0x8CD5 /* COMPLETE */)
		fprintf(stderr, "fbo incomplete\n");
	p_glViewport(0, 0, 64, 64);
	p_glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
	p_glClear(0x4000 /* COLOR_BUFFER_BIT */);
	p_glFinish();
	fprintf(stderr, "cleared and finished\n");
	return 0;
}
