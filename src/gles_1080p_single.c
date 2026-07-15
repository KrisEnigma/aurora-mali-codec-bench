// gles_1080p_single.c
//
// OpenGL ES 3.1 equivalent of vk_1080p_single.c: same staged CDF 9/7
// lifting transform, same 1080p resolution, ONE forward chain (40
// dispatches), no loops. Uses a headless EGL context (1x1 Pbuffer surface,
// broadly supported without requiring the surfaceless extension) since
// there's no on-screen window needed for compute-only work.
//
// GLSL source is compiled at runtime (standard for GLES), no SPIR-V step
// needed here, unlike the Vulkan path.
//
// Usage: ./gles_1080p_single /path/to/libEGL.so /path/to/libGLESv2.so

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <time.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>

static void die(const char *msg) { fprintf(stderr, "FATAL: %s\n", msg); exit(1); }

// EGL function pointers
static PFNEGLGETDISPLAYPROC my_eglGetDisplay;
static PFNEGLINITIALIZEPROC my_eglInitialize;
static PFNEGLBINDAPIPROC my_eglBindAPI;
static PFNEGLCHOOSECONFIGPROC my_eglChooseConfig;
static PFNEGLCREATECONTEXTPROC my_eglCreateContext;
static PFNEGLCREATEPBUFFERSURFACEPROC my_eglCreatePbufferSurface;
static PFNEGLMAKECURRENTPROC my_eglMakeCurrent;
static PFNEGLGETERRORPROC my_eglGetError;

// GLES function pointers (subset we need)
static PFNGLGENBUFFERSPROC my_glGenBuffers;
static PFNGLBINDBUFFERPROC my_glBindBuffer;
static PFNGLBUFFERDATAPROC my_glBufferData;
static PFNGLBUFFERSUBDATAPROC my_glBufferSubData;
static PFNGLBINDBUFFERBASEPROC my_glBindBufferBase;
static PFNGLCREATESHADERPROC my_glCreateShader;
static PFNGLSHADERSOURCEPROC my_glShaderSource;
static PFNGLCOMPILESHADERPROC my_glCompileShader;
static PFNGLGETSHADERIVPROC my_glGetShaderiv;
static PFNGLGETSHADERINFOLOGPROC my_glGetShaderInfoLog;
static PFNGLCREATEPROGRAMPROC my_glCreateProgram;
static PFNGLATTACHSHADERPROC my_glAttachShader;
static PFNGLLINKPROGRAMPROC my_glLinkProgram;
static PFNGLGETPROGRAMIVPROC my_glGetProgramiv;
static PFNGLGETPROGRAMINFOLOGPROC my_glGetProgramInfoLog;
static PFNGLUSEPROGRAMPROC my_glUseProgram;
static PFNGLDISPATCHCOMPUTEPROC my_glDispatchCompute;
static PFNGLMEMORYBARRIERPROC my_glMemoryBarrier;
static void (*my_glFinish)(void);
static GLenum (*my_glGetError)(void);
static const GLubyte *(*my_glGetString)(GLenum);
static void *(*my_glMapBufferRange)(GLenum, GLintptr, GLsizeiptr, GLbitfield);
static GLboolean (*my_glUnmapBuffer)(GLenum);

#define N_LEVELS 4
#define N_STAGES 5
#define W 1920
#define H 1080

typedef struct { uint32_t direction, mode, stage, fullWidth, activeWidth, activeHeight; } PC;

static const char *shader_src =
"#version 310 es\n"
"layout(local_size_x = 256, local_size_y = 1) in;\n"
"layout(std430, binding = 0) readonly buffer Src { float srcData[]; };\n"
"layout(std430, binding = 1) buffer Dst { float dstData[]; };\n"
"layout(std140, binding = 0) uniform PC { uint direction; uint mode; uint stage; uint fullWidth; uint activeWidth; uint activeHeight; };\n"
"const float ALPHA = -1.586134342; const float BETA = -0.05298011854;\n"
"const float GAMMA = 0.8829110762; const float DELTA = 0.4435068522; const float KAPPA = 1.230174105;\n"
"#define HALO 4\n"
"shared float tile[256 + 2 * HALO];\n"
"int clampIdx(int idx, int hi) { return clamp(idx, 0, hi); }\n"
"int toGlobal(int linePos, int lineIndex) { if (direction == 0u) return lineIndex * int(fullWidth) + linePos; else return linePos * int(fullWidth) + lineIndex; }\n"
"void main() {\n"
"  uint lineLen = (direction == 0u) ? activeWidth : activeHeight;\n"
"  uint numLines = (direction == 0u) ? activeHeight : activeWidth;\n"
"  uint lineIndex = gl_GlobalInvocationID.y;\n"
"  if (lineIndex >= numLines) return;\n"
"  int tid = int(gl_LocalInvocationID.x);\n"
"  int localBase = int(gl_WorkGroupID.x) * 256;\n"
"  int gp = localBase + tid - HALO;\n"
"  tile[tid] = srcData[toGlobal(clampIdx(gp, int(lineLen) - 1), int(lineIndex))];\n"
"  if (tid < 2 * HALO) {\n"
"    int gp2 = localBase + 256 + tid - HALO;\n"
"    tile[256 + tid] = srcData[toGlobal(clampIdx(gp2, int(lineLen) - 1), int(lineIndex))];\n"
"  }\n"
"  barrier();\n"
"  int i = tid + HALO;\n"
"  int globalHome = localBase + tid;\n"
"  if (globalHome >= int(lineLen)) return;\n"
"  bool isOdd = (globalHome & 1) == 1;\n"
"  float v = tile[i];\n"
"  if (mode == 0u) {\n"
"    if (stage == 0u && isOdd)  v += ALPHA * (tile[i-1] + tile[i+1]);\n"
"    if (stage == 1u && !isOdd) v += BETA  * (tile[i-1] + tile[i+1]);\n"
"    if (stage == 2u && isOdd)  v += GAMMA * (tile[i-1] + tile[i+1]);\n"
"    if (stage == 3u && !isOdd) v += DELTA * (tile[i-1] + tile[i+1]);\n"
"    if (stage == 4u) v = isOdd ? v * KAPPA : v * (1.0 / KAPPA);\n"
"  } else {\n"
"    if (stage == 0u) v = isOdd ? v * (1.0 / KAPPA) : v * KAPPA;\n"
"    if (stage == 1u && !isOdd) v -= DELTA * (tile[i-1] + tile[i+1]);\n"
"    if (stage == 2u && isOdd)  v -= GAMMA * (tile[i-1] + tile[i+1]);\n"
"    if (stage == 3u && !isOdd) v -= BETA  * (tile[i-1] + tile[i+1]);\n"
"    if (stage == 4u && isOdd)  v -= ALPHA * (tile[i-1] + tile[i+1]);\n"
"  }\n"
"  dstData[toGlobal(globalHome, int(lineIndex))] = v;\n"
"}\n";

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s /path/to/libEGL.so /path/to/libGLESv2.so\n", argv[0]); return 1; }

    void *libEGL = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
    if (!libEGL) die(dlerror());
    void *libGLES = dlopen(argv[2], RTLD_NOW | RTLD_GLOBAL);
    if (!libGLES) die(dlerror());

    #define GETE(name) my_##name = (__typeof__(my_##name))dlsym(libEGL, #name); if (!my_##name) die("missing " #name)
    GETE(eglGetDisplay); GETE(eglInitialize); GETE(eglBindAPI); GETE(eglChooseConfig);
    GETE(eglCreateContext); GETE(eglCreatePbufferSurface); GETE(eglMakeCurrent); GETE(eglGetError);
    #undef GETE

    #define GETG(name) my_##name = (__typeof__(my_##name))dlsym(libGLES, #name); if (!my_##name) die("missing " #name)
    GETG(glGenBuffers); GETG(glBindBuffer); GETG(glBufferData); GETG(glBufferSubData);
    GETG(glBindBufferBase); GETG(glCreateShader); GETG(glShaderSource); GETG(glCompileShader);
    GETG(glGetShaderiv); GETG(glGetShaderInfoLog); GETG(glCreateProgram); GETG(glAttachShader);
    GETG(glLinkProgram); GETG(glGetProgramiv); GETG(glGetProgramInfoLog); GETG(glUseProgram);
    GETG(glDispatchCompute); GETG(glMemoryBarrier); GETG(glFinish); GETG(glGetError);
    GETG(glGetString); GETG(glMapBufferRange); GETG(glUnmapBuffer);
    #undef GETG

    EGLDisplay dpy = my_eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY) die("eglGetDisplay failed");
    EGLint major, minor;
    if (!my_eglInitialize(dpy, &major, &minor)) die("eglInitialize failed");
    printf("EGL version: %d.%d\n", major, minor);

    my_eglBindAPI(EGL_OPENGL_ES_API);

    EGLint configAttribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig config;
    EGLint numConfigs;
    if (!my_eglChooseConfig(dpy, configAttribs, &config, 1, &numConfigs) || numConfigs == 0)
        die("eglChooseConfig failed / no matching config");

    EGLint pbufferAttribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    EGLSurface surface = my_eglCreatePbufferSurface(dpy, config, pbufferAttribs);
    if (surface == EGL_NO_SURFACE) die("eglCreatePbufferSurface failed");

    EGLint contextAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext ctx = my_eglCreateContext(dpy, config, EGL_NO_CONTEXT, contextAttribs);
    if (ctx == EGL_NO_CONTEXT) die("eglCreateContext failed");

    if (!my_eglMakeCurrent(dpy, surface, surface, ctx)) die("eglMakeCurrent failed");

    printf("GL_VERSION: %s\n", (const char*)my_glGetString(GL_VERSION));
    printf("GL_RENDERER: %s\n", (const char*)my_glGetString(GL_RENDERER));

    // Buffers
    GLuint bufA, bufB, ubo;
    my_glGenBuffers(1, &bufA);
    my_glGenBuffers(1, &bufB);
    my_glGenBuffers(1, &ubo);

    size_t bufSize = (size_t)W * H * sizeof(float);
    float *initData = malloc(bufSize);
    for (size_t i = 0; i < (size_t)W * H; i++) initData[i] = 0.5f;

    my_glBindBuffer(GL_SHADER_STORAGE_BUFFER, bufA);
    my_glBufferData(GL_SHADER_STORAGE_BUFFER, bufSize, initData, GL_DYNAMIC_COPY);
    my_glBindBuffer(GL_SHADER_STORAGE_BUFFER, bufB);
    my_glBufferData(GL_SHADER_STORAGE_BUFFER, bufSize, initData, GL_DYNAMIC_COPY);

    my_glBindBuffer(GL_UNIFORM_BUFFER, ubo);
    my_glBufferData(GL_UNIFORM_BUFFER, sizeof(PC), NULL, GL_DYNAMIC_DRAW);
    my_glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo);

    // Shader compile
    GLuint shader = my_glCreateShader(GL_COMPUTE_SHADER);
    my_glShaderSource(shader, 1, &shader_src, NULL);
    my_glCompileShader(shader);
    GLint compiled = 0;
    my_glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log[4096];
        my_glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        fprintf(stderr, "SHADER COMPILE FAILED:\n%s\n", log);
        return 1;
    }
    GLuint program = my_glCreateProgram();
    my_glAttachShader(program, shader);
    my_glLinkProgram(program);
    GLint linked = 0;
    my_glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[4096];
        my_glGetProgramInfoLog(program, sizeof(log), NULL, log);
        fprintf(stderr, "PROGRAM LINK FAILED:\n%s\n", log);
        return 1;
    }
    my_glUseProgram(program);

    const uint32_t DISPATCHES = N_LEVELS * 2 * N_STAGES; // 40, forward only

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    int pingpong = 0; // 0 = read A write B, 1 = read B write A
    for (uint32_t step = 0; step < DISPATCHES; step++) {
        uint32_t level = step / (2 * N_STAGES);
        uint32_t rem = step % (2 * N_STAGES);
        uint32_t axis = rem / N_STAGES;
        uint32_t stg = rem % N_STAGES;
        uint32_t activeW = W >> level, activeH = H >> level;

        PC pc = { axis, 0, stg, W, activeW, activeH };
        my_glBindBuffer(GL_UNIFORM_BUFFER, ubo);
        my_glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(pc), &pc);

        if (pingpong == 0) {
            my_glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, bufA);
            my_glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, bufB);
        } else {
            my_glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, bufB);
            my_glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, bufA);
        }

        uint32_t lineLen = (axis == 0) ? activeW : activeH;
        uint32_t numLines = (axis == 0) ? activeH : activeW;
        uint32_t gx = (lineLen + 255) / 256;
        my_glDispatchCompute(gx, numLines, 1);
        my_glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_UNIFORM_BARRIER_BIT);
        pingpong ^= 1;
    }

    my_glFinish();
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    printf("GLES 1080p single forward pass (wall clock, includes CPU dispatch overhead): %.3f ms\n", ms);

    GLenum err = my_glGetError();
    if (err != GL_NO_ERROR) printf("NOTE: glGetError reported 0x%x after the run\n", err);

    // Quick correctness spot-check: final result is in whichever buffer
    // pingpong ended pointing at as the LAST write target (40 dispatches,
    // even count, so it ends back in bufB — same parity as the Vulkan test).
    my_glBindBuffer(GL_SHADER_STORAGE_BUFFER, bufB);
    float *mapped = (float*)my_glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, 8 * sizeof(float), GL_MAP_READ_BIT);
    if (mapped) {
        printf("First 8 output values: ");
        for (int i = 0; i < 8; i++) printf("%.6f ", mapped[i]);
        printf("\n(compare against the Vulkan test's smoke-test reference values for a rough sanity check)\n");
        my_glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
    }

    return 0;
}
