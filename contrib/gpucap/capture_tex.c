/*
 * Capture a textured draw, so the texture and sampler descriptors and the
 * sampling shader can be read out of the dump the same way the clear and
 * the plain draw were.
 *
 * One attribute (a vec4 position); the texture coordinates are derived in
 * the vertex shader, so the attribute layout stays identical to
 * capture_draw.c.
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
static void (*p_glTexParameteri)(unsigned, unsigned, int);
static void (*p_glActiveTexture)(unsigned);
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
static int (*p_glGetUniformLocation)(unsigned, const char *);
static void (*p_glUniform1i)(int, int);

static unsigned
shader(unsigned type, const char *src)
{
	unsigned s = p_glCreateShader(type);
	int ok = 0;
	char log[512];

	p_glShaderSource(s, 1, &src, NULL);
	p_glCompileShader(s);
	p_glGetShaderiv(s, 0x8B81, &ok);
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
		"out vec2 uv;\n"
		"void main(){ gl_Position = position; uv = position.xy * 0.5 + 0.5; }\n";
	static const char *fs_src =
		"#version 300 es\n"
		"precision mediump float;\n"
		"in vec2 uv;\n"
		"uniform sampler2D tex;\n"
		"out vec4 colour;\n"
		"void main(){ colour = texture(tex, uv); }\n";
	/* 2x2: red, green / blue, white -- all distinct, so a single readback
	 * identifies which corner and which channel mapping we got. */
	unsigned char texels[2][2][4] = {
		{ {255, 0, 0, 255}, {0, 255, 0, 255} },
		{ {0, 0, 255, 255}, {255, 255, 255, 255} },
	};
	float verts[3][4] = {
		{ -1.0f, -1.0f, 0.0f, 1.0f },
		{  3.0f, -1.0f, 0.0f, 1.0f },
		{ -1.0f,  3.0f, 0.0f, 1.0f },
	};
	unsigned tex, fbo, prog, vbo;
	int loc, uloc, ok = 0;
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
	GET(glTexParameteri); GET(glActiveTexture);
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
	GET(glGetUniformLocation); GET(glUniform1i);

	fprintf(stderr, "GL_VERSION %s\n", (const char *)p_glGetString(0x1F02));
	fprintf(stderr, "GL_RENDERER %s\n", (const char *)p_glGetString(0x1F01));

	/* Source texture: 2x2 RGBA, nearest, clamped. */
	p_glGenTextures(1, &tex);
	p_glActiveTexture(0x84C0 /* TEXTURE0 */);
	p_glBindTexture(0x0DE1, tex);
	p_glTexParameteri(0x0DE1, 0x2801 /* MIN_FILTER */, 0x2600 /* NEAREST */);
	p_glTexParameteri(0x0DE1, 0x2800 /* MAG_FILTER */, 0x2600);
	p_glTexParameteri(0x0DE1, 0x2802 /* WRAP_S */, 0x812F /* CLAMP */);
	p_glTexParameteri(0x0DE1, 0x2803 /* WRAP_T */, 0x812F);
	p_glTexImage2D(0x0DE1, 0, 0x1908, 2, 2, 0, 0x1908, 0x1401, texels);

	/* Render target: 64x64 RGBA8. */
	p_glGenTextures(1, &fbo);
	p_glBindTexture(0x0DE1, fbo);
	p_glTexImage2D(0x0DE1, 0, 0x1908, 64, 64, 0, 0x1908, 0x1401, NULL);
	{
		unsigned fb;
		p_glGenFramebuffers(1, &fb);
		p_glBindFramebuffer(0x8D40, fb);
		p_glFramebufferTexture2D(0x8D40, 0x8CE0, 0x0DE1, fbo, 0);
		if(p_glCheckFramebufferStatus(0x8D40) != 0x8CD5){
			fprintf(stderr, "fbo incomplete\n");
			return 1;
		}
	}

	prog = p_glCreateProgram();
	p_glAttachShader(prog, shader(0x8B31, vs_src));
	p_glAttachShader(prog, shader(0x8B30, fs_src));
	p_glLinkProgram(prog);
	p_glGetProgramiv(prog, 0x8B82, &ok);
	if(!ok){
		fprintf(stderr, "link failed\n");
		return 1;
	}
	p_glUseProgram(prog);
	loc = p_glGetAttribLocation(prog, "position");
	uloc = p_glGetUniformLocation(prog, "tex");
	fprintf(stderr, "position %d tex %d\n", loc, uloc);
	p_glUniform1i(uloc, 0);

	p_glGenBuffers(1, &vbo);
	p_glBindBuffer(0x8892, vbo);
	p_glBufferData(0x8892, sizeof verts, verts, 0x88E4);
	p_glEnableVertexAttribArray((unsigned)loc);
	p_glVertexAttribPointer((unsigned)loc, 4, 0x1406, 0, 16, NULL);

	/* The render-target texture was bound to unit 0 above; put the source
	 * texture back or the draw samples its own target. */
	p_glActiveTexture(0x84C0);
	p_glBindTexture(0x0DE1, tex);

	p_glViewport(0, 0, 64, 64);
	p_glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	p_glClear(0x4000);
	p_glDrawArrays(0x0004, 0, 3);
	p_glFinish();

	memset(px, 0, sizeof px);
	p_glReadPixels(0, 0, 1, 1, 0x1908, 0x1401, px);
	fprintf(stderr, "TEX pixel %02x%02x%02x%02x\n", px[0], px[1], px[2], px[3]);
	fprintf(stderr, "TEXCAP-DONE\n");
	return 0;
}
