/* Original bionic/Mali ABI, tiny offscreen render; no framebuffer/window. */
typedef void *EGLDisplay;
typedef void *EGLConfig;
typedef void *EGLSurface;
typedef void *EGLContext;
extern int printf(const char *, ...);
extern int fflush(void *);
extern unsigned alarm(unsigned);
extern EGLDisplay eglGetDisplay(void *);
extern unsigned eglInitialize(EGLDisplay, int *, int *);
extern unsigned eglTerminate(EGLDisplay);
extern unsigned eglBindAPI(unsigned);
extern unsigned eglChooseConfig(EGLDisplay, const int *, EGLConfig *, int, int *);
extern EGLSurface eglCreatePbufferSurface(EGLDisplay, EGLConfig, const int *);
extern EGLContext eglCreateContext(EGLDisplay, EGLConfig, EGLContext, const int *);
extern unsigned eglMakeCurrent(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
extern unsigned eglDestroySurface(EGLDisplay, EGLSurface);
extern unsigned eglDestroyContext(EGLDisplay, EGLContext);
extern int eglGetError(void);
extern const char *eglQueryString(EGLDisplay, int);
extern const unsigned char *glGetString(unsigned);
extern void glClearColor(float, float, float, float);
extern void glClear(unsigned);
extern void glReadPixels(int, int, int, int, unsigned, unsigned, void *);
extern void glFinish(void);
extern unsigned glGetError(void);
extern unsigned glCreateShader(unsigned);
extern void glShaderSource(unsigned, int, const char *const *, const int *);
extern void glCompileShader(unsigned);
extern void glGetShaderiv(unsigned, unsigned, int *);
extern void glGetShaderInfoLog(unsigned, int, int *, char *);
extern unsigned glCreateProgram(void);
extern void glAttachShader(unsigned, unsigned);
extern void glBindAttribLocation(unsigned, unsigned, const char *);
extern void glLinkProgram(unsigned);
extern void glGetProgramiv(unsigned, unsigned, int *);
extern void glUseProgram(unsigned);
extern void glGenBuffers(int, unsigned *);
extern void glBindBuffer(unsigned, unsigned);
extern void glBufferData(unsigned, long, const void *, unsigned);
extern void glVertexAttribPointer(unsigned, int, unsigned, unsigned char, int, const void *);
extern void glEnableVertexAttribArray(unsigned);
extern void glViewport(int, int, int, int);
extern void glDrawArrays(unsigned, int, int);
extern void glDeleteBuffers(int, const unsigned *);
extern void glDeleteProgram(unsigned);
extern void glDeleteShader(unsigned);

static void step(const char *name) { printf("GPU_STEP %s\n", name); fflush(0); }
static unsigned shader(unsigned type, const char *source) {
    unsigned id = glCreateShader(type);
    if (!id) return 0;
    glShaderSource(id, 1, &source, 0);
    glCompileShader(id);
    int compiled = 0;
    glGetShaderiv(id, 0x8b81, &compiled);
    if (!compiled) {
        char log[1024] = {0};
        glGetShaderInfoLog(id, sizeof log-1, 0, log);
        printf("GPU_SHADER_COMPILE_FAILED %s\n", log);
        glDeleteShader(id);
        return 0;
    }
    return id;
}
int main(void) {
    alarm(15);
    int result = 1, major = 0, minor = 0, count = 0;
    EGLDisplay display = 0;
    EGLSurface surface = 0;
    EGLContext context = 0;
    unsigned vertex = 0, fragment = 0, program = 0, buffer = 0;
    step("get_display");
    display = eglGetDisplay(0);
    if (!display) goto done;
    step("initialize");
    if (!eglInitialize(display, &major, &minor)) goto done;
    printf("GPU_EGL version=%d.%d vendor=%s\n", major, minor, eglQueryString(display, 0x3053));
    if (!eglBindAPI(0x30a0)) goto done;
    const int attrs[] = {0x3033, 1, 0x3040, 4, 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3038};
    EGLConfig config = 0;
    step("choose_pbuffer_config");
    if (!eglChooseConfig(display, attrs, &config, 1, &count) || count != 1) goto done;
    const int pbuffer[] = {0x3057, 4, 0x3056, 4, 0x3038};
    surface = eglCreatePbufferSurface(display, config, pbuffer);
    if (!surface) goto done;
    const int ctx[] = {0x3098, 2, 0x3038};
    context = eglCreateContext(display, config, 0, ctx);
    if (!context || !eglMakeCurrent(display, surface, surface, context)) goto done;
    printf("GPU_GL vendor=%s renderer=%s version=%s\n", glGetString(0x1f00), glGetString(0x1f01), glGetString(0x1f02));
    step("clear_and_readback");
    glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
    glClear(0x4000);
    glFinish();
    unsigned char pixel[4] = {0};
    glReadPixels(0, 0, 1, 1, 0x1908, 0x1401, pixel);
    unsigned error = glGetError();
    printf("GPU_PIXEL rgba=%u,%u,%u,%u gl_error=0x%x\n", pixel[0], pixel[1], pixel[2], pixel[3], error);
    if (error || pixel[0]<63 || pixel[0]>65 || pixel[1]<127 || pixel[1]>129 ||
        pixel[2]<190 || pixel[2]>192 || pixel[3]!=255) goto done;
    step("compile_and_draw_shader");
    vertex = shader(0x8b31, "attribute vec2 p; void main(){gl_Position=vec4(p,0.0,1.0);}");
    fragment = shader(0x8b30, "precision mediump float; void main(){gl_FragColor=vec4(gl_FragCoord.x/4.0,gl_FragCoord.y/4.0,0.75,1.0);}");
    if (!vertex || !fragment) goto done;
    program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glBindAttribLocation(program, 0, "p");
    glLinkProgram(program);
    int linked = 0;
    glGetProgramiv(program, 0x8b82, &linked);
    if (!linked) goto done;
    glUseProgram(program);
    const float triangle[] = {-1,-1, 3,-1, -1,3};
    glGenBuffers(1, &buffer);
    glBindBuffer(0x8892, buffer);
    glBufferData(0x8892, sizeof triangle, triangle, 0x88e4);
    glVertexAttribPointer(0, 2, 0x1406, 0, 0, 0);
    glEnableVertexAttribArray(0);
    glViewport(0, 0, 4, 4);
    glDrawArrays(4, 0, 3);
    glFinish();
    unsigned char pixels[64] = {0};
    glReadPixels(0, 0, 4, 4, 0x1908, 0x1401, pixels);
    unsigned mismatches = 0;
    for (int y=0; y<4; y++) for (int x=0; x<4; x++) {
        unsigned char *p = pixels+4*(4*y+x);
        int red = (2*x+1)*255/8, green = (2*y+1)*255/8;
        if (p[0]<red-1 || p[0]>red+1 || p[1]<green-1 || p[1]>green+1 ||
            p[2]<190 || p[2]>192 || p[3]!=255) mismatches++;
    }
    error = glGetError();
    printf("GPU_SHADER_CHECK pixels=16 mismatches=%u gl_error=0x%x\n", mismatches, error);
    if (!mismatches && !error) result = 0;
done:
    printf("GPU_RESULT success=%d egl_error=0x%x\n", !result, eglGetError());
    fflush(0);
    if (display) {
        if (buffer) glDeleteBuffers(1, &buffer);
        if (program) glDeleteProgram(program);
        if (fragment) glDeleteShader(fragment);
        if (vertex) glDeleteShader(vertex);
        eglMakeCurrent(display, 0, 0, 0);
        if (context) eglDestroyContext(display, context);
        if (surface) eglDestroySurface(display, surface);
        eglTerminate(display);
    }
    alarm(0);
    return result;
}
