/*
 * Make the panfrost Gallium driver submit a real *draw* chain, so the same
 * capture path that produced panfrost-clear-job.txt gives us the tiler job,
 * the shader descriptors and the fragment job for a triangle.
 *
 * Everything is GLES 3.0 via eglGetProcAddress: no GL headers needed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
static unsigned (*p_glCreateShader)(unsigned);
static void (*p_glShaderSource)(unsigned, int, const char *const *, const int *);
static void (*p_glCompileShader)(unsigned);
static void (*p_glGetShaderiv)(unsigned, unsigned, int *);
static void (*p_glGetShaderInfoLog)(unsigned, int, int *, char *);
static unsigned (*p_glCreateProgram)(void);
static void (*p_glAttachShader)(unsigned, unsigned);
static void (*p_glLinkProgram)(unsigned);
static void (*p_glGetProgramiv)(unsigned, unsigned, int *);
static void (*p_glUseProgram)(unsigned);
static void (*p_glGenBuffers)(int, unsigned *);
static void (*p_glBindBuffer)(unsigned, unsigned);
static void (*p_glBufferData)(unsigned, long, const void *, unsigned);
static void (*p_glEnableVertexAttribArray)(unsigned);
static void (*p_glVertexAttribPointer)(unsigned, int, unsigned, unsigned char, int, const void *);
static void (*p_glDrawArrays)(unsigned, int, int);
static void (*p_glReadPixels)(int, int, int, int, unsigned, unsigned, void *);
static int (*p_glGetAttribLocation)(unsigned, const char *);

static unsigned
shader(unsigned type, const char *src)
{
	unsigned s = p_glCreateShader(type);
	int ok = 0;
	char log[512];

	p_glShaderSource(s, 1, &src, NULL);
	p_glCompileShader(s);
	p_glGetShaderiv(s, 0x8B81 /* COMPILE_STATUS */, &ok);
	if(!ok){
		p_glGetShaderInfoLog(s, sizeof log, NULL, log);
		fprintf(stderr, "shader %#x failed: %s\n", type, log);
		exit(1);
	}
	return s;
}

int
main(void)
{
	PFNEGLGETPLATFORMDISPLAYEXTPROC getPlatformDisplay;
	EGLDisplay dpy;
	EGLConfig cfg;
	EGLContext ctx;
	EGLint major, minor, nconf = 0;
	EGLint attrs[] = {
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 8, EGL_NONE
	};
	EGLint ctxattrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
	static const char *vs_src =
		"#version 300 es\n"
		"in vec4 position;\n"
		"void main(){ gl_Position = position; }\n";
	static const char *fs_src =
		"#version 300 es\n"
		"precision mediump float;\n"
		"out vec4 colour;\n"
		"void main(){ colour = vec4(1.0, 0.0, 0.0, 1.0); }\n";
	float verts[3][4] = {
		{ -1.0f, -1.0f, 0.0f, 1.0f },
		{  3.0f, -1.0f, 0.0f, 1.0f },
		{ -1.0f,  3.0f, 0.0f, 1.0f },
	};
	unsigned tex, fbo, prog, vbo;
	int loc, ok = 0;
	unsigned char px[4];

	getPlatformDisplay = (PFNEGLGETPLATFORMDISPLAYEXTPROC)
		eglGetProcAddress("eglGetPlatformDisplayEXT");
	if(getPlatformDisplay == NULL)
		return 1;
	dpy = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
	if(dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &major, &minor))
		return 1;
	if(!eglChooseConfig(dpy, attrs, &cfg, 1, &nconf) || nconf < 1)
		return 1;
	ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxattrs);
	if(ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
		return 1;

	GET(glGenTextures); GET(glBindTexture); GET(glTexImage2D);
	GET(glGenFramebuffers); GET(glBindFramebuffer); GET(glFramebufferTexture2D);
	GET(glCheckFramebufferStatus); GET(glViewport); GET(glClearColor);
	GET(glClear); GET(glFinish); GET(glGetString);
	GET(glCreateShader); GET(glShaderSource); GET(glCompileShader);
	GET(glGetShaderiv); GET(glGetShaderInfoLog);
	GET(glCreateProgram); GET(glAttachShader); GET(glLinkProgram);
	GET(glGetProgramiv); GET(glUseProgram);
	GET(glGenBuffers); GET(glBindBuffer); GET(glBufferData);
	GET(glEnableVertexAttribArray); GET(glVertexAttribPointer);
	GET(glDrawArrays); GET(glReadPixels); GET(glGetAttribLocation);

	fprintf(stderr, "GL_VERSION %s\n", (const char *)p_glGetString(0x1F02));
	fprintf(stderr, "GL_RENDERER %s\n", (const char *)p_glGetString(0x1F01));

	p_glGenTextures(1, &tex);
	p_glBindTexture(0x0DE1, tex);
	p_glTexImage2D(0x0DE1, 0, 0x1908, 64, 64, 0, 0x1908, 0x1401, NULL);
	p_glGenFramebuffers(1, &fbo);
	p_glBindFramebuffer(0x8D40, fbo);
	p_glFramebufferTexture2D(0x8D40, 0x8CE0, 0x0DE1, tex, 0);
	if(p_glCheckFramebufferStatus(0x8D40) != 0x8CD5){
		fprintf(stderr, "fbo incomplete\n");
		return 1;
	}

	prog = p_glCreateProgram();
	p_glAttachShader(prog, shader(0x8B31 /* VERTEX */, vs_src));
	p_glAttachShader(prog, shader(0x8B30 /* FRAGMENT */, fs_src));
	p_glLinkProgram(prog);
	p_glGetProgramiv(prog, 0x8B82 /* LINK_STATUS */, &ok);
	if(!ok){
		fprintf(stderr, "link failed\n");
		return 1;
	}
	p_glUseProgram(prog);
	loc = p_glGetAttribLocation(prog, "position");
	fprintf(stderr, "position location %d\n", loc);

	p_glGenBuffers(1, &vbo);
	p_glBindBuffer(0x8892 /* ARRAY_BUFFER */, vbo);
	p_glBufferData(0x8892, sizeof verts, verts, 0x88E4 /* STATIC_DRAW */);
	p_glEnableVertexAttribArray((unsigned)loc);
	p_glVertexAttribPointer((unsigned)loc, 4, 0x1406 /* FLOAT */, 0, 16, NULL);

	p_glViewport(0, 0, 64, 64);
	p_glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
	p_glClear(0x4000);
	p_glDrawArrays(0x0004 /* TRIANGLES */, 0, 3);
	p_glFinish();

	memset(px, 0, sizeof px);
	p_glReadPixels(0, 0, 1, 1, 0x1908, 0x1401, px);
	fprintf(stderr, "DRAW pixel %02x%02x%02x%02x\n", px[0], px[1], px[2], px[3]);
	fprintf(stderr, "DRAWCAP-DONE\n");
	return 0;
}
