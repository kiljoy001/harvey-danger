/*
 * GBM variant: the panfrost drm-shim is designed to be driven through GBM
 * (PIGLIT_PLATFORM=gbm), which is what makes it present a fake render node.
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#define GET(name) do { \
	p_##name = (void *)eglGetProcAddress(#name); \
	if(p_##name == NULL){ fprintf(stderr, "missing %s\n", #name); return 1; } \
} while(0)

static void (*p_glViewport)(int, int, int, int);
static void (*p_glClearColor)(float, float, float, float);
static void (*p_glClear)(unsigned);
static void (*p_glFinish)(void);
static const unsigned char *(*p_glGetString)(unsigned);

int
main(void)
{
	PFNEGLGETPLATFORMDISPLAYEXTPROC getPlatformDisplay;
	struct gbm_device *dev;
	struct gbm_surface *gsurf;
	EGLDisplay dpy;
	EGLConfig cfg;
	EGLContext ctx;
	EGLSurface surf;
	EGLint major, minor, nconf = 0;
	EGLint attrs[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 8, EGL_NONE
	};
	EGLint ctxattrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	int fd;

	fd = open("/dev/dri/renderD128", O_RDWR);
	if(fd < 0){
		perror("open renderD128");
		return 1;
	}
	dev = gbm_create_device(fd);
	if(dev == NULL){
		fprintf(stderr, "gbm_create_device failed\n");
		return 1;
	}
	fprintf(stderr, "gbm backend %s\n", gbm_device_get_backend_name(dev));

	getPlatformDisplay = (PFNEGLGETPLATFORMDISPLAYEXTPROC)
		eglGetProcAddress("eglGetPlatformDisplayEXT");
	dpy = getPlatformDisplay(EGL_PLATFORM_GBM_KHR, dev, NULL);
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
	gsurf = gbm_surface_create(dev, 64, 64, GBM_FORMAT_XRGB8888,
	                           GBM_BO_USE_RENDERING);
	if(gsurf == NULL){
		fprintf(stderr, "gbm_surface_create failed\n");
		return 1;
	}
	ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxattrs);
	if(ctx == EGL_NO_CONTEXT){
		fprintf(stderr, "no context\n");
		return 1;
	}
	surf = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)gsurf, NULL);
	if(surf == EGL_NO_SURFACE){
		fprintf(stderr, "no surface\n");
		return 1;
	}
	if(!eglMakeCurrent(dpy, surf, surf, ctx)){
		fprintf(stderr, "makeCurrent failed\n");
		return 1;
	}
	GET(glViewport);
	GET(glClearColor);
	GET(glClear);
	GET(glFinish);
	GET(glGetString);
	fprintf(stderr, "GL_VERSION %s\n",
	        (const char *)p_glGetString(0x1F02));
	fprintf(stderr, "GL_RENDERER %s\n",
	        (const char *)p_glGetString(0x1F01));
	p_glViewport(0, 0, 64, 64);
	p_glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
	p_glClear(0x4000);
	eglSwapBuffers(dpy, surf);
	p_glFinish();
	fprintf(stderr, "cleared and finished\n");
	return 0;
}
