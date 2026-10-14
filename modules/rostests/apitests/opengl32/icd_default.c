/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     The OpenGL driver the system installs for display drivers without one
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <windows.h>
#include <wingdi.h>
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wine/test.h"

#ifndef APIENTRY
#define APIENTRY WINAPI
#endif

#define GL_FRAGMENT_SHADER                0x8B30
#define GL_VERTEX_SHADER                  0x8B31
#define GL_COMPILE_STATUS                 0x8B81
#define GL_LINK_STATUS                    0x8B82
#define GL_SHADING_LANGUAGE_VERSION       0x8B8C
#define GL_FRAMEBUFFER_EXT                0x8D40
#define GL_RENDERBUFFER_EXT               0x8D41
#define GL_COLOR_ATTACHMENT0_EXT          0x8CE0
#define GL_FRAMEBUFFER_COMPLETE_EXT       0x8CD5
#define GL_READ_FRAMEBUFFER_EXT           0x8CA8
#define GL_DRAW_FRAMEBUFFER_EXT           0x8CA9

typedef char GLchar_;
typedef GLuint (APIENTRY *PFN_CREATESHADER)(GLenum);
typedef void (APIENTRY *PFN_SHADERSOURCE)(GLuint, GLsizei, const GLchar_ **, const GLint *);
typedef void (APIENTRY *PFN_COMPILESHADER)(GLuint);
typedef void (APIENTRY *PFN_GETSHADERIV)(GLuint, GLenum, GLint *);
typedef void (APIENTRY *PFN_GETINFOLOG)(GLuint, GLsizei, GLsizei *, GLchar_ *);
typedef GLuint (APIENTRY *PFN_CREATEPROGRAM)(void);
typedef void (APIENTRY *PFN_ATTACHSHADER)(GLuint, GLuint);
typedef void (APIENTRY *PFN_LINKPROGRAM)(GLuint);
typedef void (APIENTRY *PFN_USEPROGRAM)(GLuint);
typedef void (APIENTRY *PFN_GETPROGRAMIV)(GLuint, GLenum, GLint *);
typedef GLint (APIENTRY *PFN_GETUNIFORMLOCATION)(GLuint, const GLchar_ *);
typedef void (APIENTRY *PFN_UNIFORM4F)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (APIENTRY *PFN_GENFRAMEBUFFERS)(GLsizei, GLuint *);
typedef void (APIENTRY *PFN_GENRENDERBUFFERS)(GLsizei, GLuint *);
typedef void (APIENTRY *PFN_BINDFRAMEBUFFER)(GLenum, GLuint);
typedef void (APIENTRY *PFN_BINDRENDERBUFFER)(GLenum, GLuint);
typedef void (APIENTRY *PFN_RENDERBUFFERSTORAGE)(GLenum, GLenum, GLsizei, GLsizei);
typedef void (APIENTRY *PFN_FRAMEBUFFERRENDERBUFFER)(GLenum, GLenum, GLenum, GLuint);
typedef GLenum (APIENTRY *PFN_CHECKFRAMEBUFFERSTATUS)(GLenum);
typedef void (APIENTRY *PFN_BLITFRAMEBUFFER)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
typedef const char *(WINAPI *PFN_WGLGETEXTENSIONSSTRING)(HDC);

static PFN_CREATESHADER pglCreateShader;
static PFN_SHADERSOURCE pglShaderSource;
static PFN_COMPILESHADER pglCompileShader;
static PFN_GETSHADERIV pglGetShaderiv;
static PFN_GETINFOLOG pglGetShaderInfoLog;
static PFN_CREATEPROGRAM pglCreateProgram;
static PFN_ATTACHSHADER pglAttachShader;
static PFN_LINKPROGRAM pglLinkProgram;
static PFN_USEPROGRAM pglUseProgram;
static PFN_GETPROGRAMIV pglGetProgramiv;
static PFN_GETUNIFORMLOCATION pglGetUniformLocation;
static PFN_UNIFORM4F pglUniform4f;
static PFN_GENFRAMEBUFFERS pglGenFramebuffersEXT;
static PFN_GENRENDERBUFFERS pglGenRenderbuffersEXT;
static PFN_BINDFRAMEBUFFER pglBindFramebufferEXT;
static PFN_BINDRENDERBUFFER pglBindRenderbufferEXT;
static PFN_RENDERBUFFERSTORAGE pglRenderbufferStorageEXT;
static PFN_FRAMEBUFFERRENDERBUFFER pglFramebufferRenderbufferEXT;
static PFN_CHECKFRAMEBUFFERSTATUS pglCheckFramebufferStatusEXT;
static PFN_BLITFRAMEBUFFER pglBlitFramebufferEXT;

#define WINDOW_SIZE 64

static BOOL load_entry_points(void)
{
    BOOL ok_all = TRUE;

#define LOAD(name) do { \
        p##name = (void *)wglGetProcAddress(#name); \
        ok(p##name != NULL, "wglGetProcAddress(\"" #name "\") returned NULL\n"); \
        if (!p##name) ok_all = FALSE; \
    } while (0)
    LOAD(glCreateShader);
    LOAD(glShaderSource);
    LOAD(glCompileShader);
    LOAD(glGetShaderiv);
    LOAD(glGetShaderInfoLog);
    LOAD(glCreateProgram);
    LOAD(glAttachShader);
    LOAD(glLinkProgram);
    LOAD(glUseProgram);
    LOAD(glGetProgramiv);
    LOAD(glGetUniformLocation);
    LOAD(glUniform4f);
    LOAD(glGenFramebuffersEXT);
    LOAD(glGenRenderbuffersEXT);
    LOAD(glBindFramebufferEXT);
    LOAD(glBindRenderbufferEXT);
    LOAD(glRenderbufferStorageEXT);
    LOAD(glFramebufferRenderbufferEXT);
    LOAD(glCheckFramebufferStatusEXT);
    LOAD(glBlitFramebufferEXT);
#undef LOAD
    return ok_all;
}

static void check_version(void)
{
    const char *version = (const char *)glGetString(GL_VERSION);
    const char *glsl = (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
    const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
    int major = 0, minor = 0;

    ok(version != NULL, "GL_VERSION is NULL\n");
    if (version)
    {
        ok(sscanf(version, "%d.%d", &major, &minor) == 2, "GL_VERSION '%s' does not start with major.minor\n", version);
        ok(major >= 2, "GL_VERSION '%s' is below 2.0\n", version);
    }
    ok(glsl != NULL, "GL_SHADING_LANGUAGE_VERSION is NULL (error 0x%x)\n", glGetError());
    if (glsl)
    {
        ok(sscanf(glsl, "%d.%d", &major, &minor) == 2 && (major > 1 || (major == 1 && minor >= 20)),
           "GL_SHADING_LANGUAGE_VERSION '%s' is below 1.20\n", glsl);
    }
    ok(extensions && strstr(extensions, "GL_EXT_framebuffer_object"), "GL_EXT_framebuffer_object is missing\n");
    ok(extensions && strstr(extensions, "GL_ARB_fragment_shader"), "GL_ARB_fragment_shader is missing\n");
    ok(extensions && strstr(extensions, "GL_ARB_vertex_shader"), "GL_ARB_vertex_shader is missing\n");
    trace("vendor '%s' renderer '%s' version '%s' glsl '%s'\n", glGetString(GL_VENDOR),
          glGetString(GL_RENDERER), version, glsl);
}

static GLuint compile(GLenum type, const char *source)
{
    GLuint shader = pglCreateShader(type);
    GLint status = 0;
    char log[512] = "";

    pglShaderSource(shader, 1, &source, NULL);
    pglCompileShader(shader);
    pglGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status)
        pglGetShaderInfoLog(shader, sizeof(log), NULL, log);
    ok(status == GL_TRUE, "shader 0x%x did not compile: %s\n", type, log);
    return shader;
}

/* Fills the viewport with a colour that only a running fragment shader can produce. */
static void test_shader(void)
{
    static const char *vs =
        "#version 120\n"
        "void main() { gl_Position = gl_Vertex; }\n";
    static const char *fs =
        "#version 120\n"
        "uniform vec4 color;\n"
        "void main() { gl_FragColor = color * 0.5 + vec4(0.0, 0.25, 0.0, 0.5); }\n";
    GLuint program, v, f;
    GLint status = 0, location;
    BYTE pixel[4];

    v = compile(GL_VERTEX_SHADER, vs);
    f = compile(GL_FRAGMENT_SHADER, fs);
    program = pglCreateProgram();
    pglAttachShader(program, v);
    pglAttachShader(program, f);
    pglLinkProgram(program);
    pglGetProgramiv(program, GL_LINK_STATUS, &status);
    ok(status == GL_TRUE, "program did not link\n");
    if (!status)
        return;

    pglUseProgram(program);
    location = pglGetUniformLocation(program, "color");
    ok(location >= 0, "uniform 'color' not found\n");
    pglUniform4f(location, 1.0f, 0.5f, 0.0f, 1.0f);

    glViewport(0, 0, WINDOW_SIZE, WINDOW_SIZE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS);
    glVertex2f(-1.0f, -1.0f);
    glVertex2f(1.0f, -1.0f);
    glVertex2f(1.0f, 1.0f);
    glVertex2f(-1.0f, 1.0f);
    glEnd();
    glFinish();
    ok(glGetError() == GL_NO_ERROR, "GL error after the shader draw\n");

    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(WINDOW_SIZE / 2, WINDOW_SIZE / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    /* (1.0, 0.5, 0.0) * 0.5 + (0, 0.25, 0) = (0.5, 0.5, 0.0) */
    ok(abs(pixel[0] - 128) <= 2 && abs(pixel[1] - 128) <= 2 && pixel[2] <= 2,
       "shader output is (%u, %u, %u), expected about (128, 128, 0)\n", pixel[0], pixel[1], pixel[2]);

    pglUseProgram(0);
}

static void test_framebuffer_object(void)
{
    GLuint fbo = 0, rbo = 0;
    GLenum status;
    BYTE pixel[4];

    pglGenFramebuffersEXT(1, &fbo);
    pglGenRenderbuffersEXT(1, &rbo);
    pglBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rbo);
    pglRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, 8, 8);
    pglBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    pglFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, rbo);
    status = pglCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
    ok(status == GL_FRAMEBUFFER_COMPLETE_EXT, "framebuffer status is 0x%x\n", status);

    if (status == GL_FRAMEBUFFER_COMPLETE_EXT)
    {
        glViewport(0, 0, 8, 8);
        glClearColor(1.0f, 0.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glReadPixels(3, 3, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        ok(pixel[0] == 255 && pixel[1] == 0 && pixel[2] == 255,
           "framebuffer object pixel is (%u, %u, %u), expected (255, 0, 255)\n", pixel[0], pixel[1], pixel[2]);
    }

    pglBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
}

/* A scaling blit between two framebuffer objects. */
static void test_blit(void)
{
    GLuint fbo[2] = {0}, rbo[2] = {0};
    BYTE pixel[4];
    int i;

    pglGenFramebuffersEXT(2, fbo);
    pglGenRenderbuffersEXT(2, rbo);
    for (i = 0; i < 2; i++)
    {
        pglBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rbo[i]);
        pglRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, 8 << i, 8 << i);
        pglBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo[i]);
        pglFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, rbo[i]);
        if (pglCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) != GL_FRAMEBUFFER_COMPLETE_EXT)
        {
            ok(0, "framebuffer %d is not complete\n", i);
            pglBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
            return;
        }
    }

    pglBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo[0]);
    glViewport(0, 0, 8, 8);
    glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    pglBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, fbo[0]);
    pglBindFramebufferEXT(GL_DRAW_FRAMEBUFFER_EXT, fbo[1]);
    glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    pglBlitFramebufferEXT(0, 0, 8, 8, 0, 0, 16, 16, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    ok(glGetError() == GL_NO_ERROR, "GL error after the blit\n");

    pglBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo[1]);
    glReadPixels(9, 5, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    ok(pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 255, "blit result is (%u, %u, %u), expected (0, 0, 255)\n",
       pixel[0], pixel[1], pixel[2]);

    pglBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
}

static void test_present(HWND hwnd, HDC hdc)
{
    COLORREF color;
    POINT center;
    BOOL ret;

    glViewport(0, 0, WINDOW_SIZE, WINDOW_SIZE);
    glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ret = SwapBuffers(hdc);
    ok(ret, "SwapBuffers failed, error %lu\n", GetLastError());

    /* GetPixel reads the screen: another window on top would give its colour. */
    center.x = center.y = WINDOW_SIZE / 2;
    ClientToScreen(hwnd, &center);
    color = GetPixel(hdc, WINDOW_SIZE / 2, WINDOW_SIZE / 2);
    if (color == CLR_INVALID || WindowFromPoint(center) != hwnd)
        skip("the window is not on screen\n");
    else
        ok(color == RGB(0, 255, 0), "the window pixel is 0x%06lx after SwapBuffers, expected 0x00ff00\n", color);
}

static void test_wgl_extensions(HDC hdc)
{
    PFN_WGLGETEXTENSIONSSTRING get = (void *)wglGetProcAddress("wglGetExtensionsStringARB");
    const char *ext;

    ok(get != NULL, "wglGetExtensionsStringARB is not available\n");
    if (!get)
        return;
    ext = get(hdc);
    ok(ext != NULL, "wglGetExtensionsStringARB returned NULL\n");
    if (ext)
        ok(strstr(ext, "WGL_ARB_pixel_format") != NULL, "WGL_ARB_pixel_format is missing from '%s'\n", ext);
}

/* ChoosePixelFormat returns a closest match when the application does not care about a feature. */
static void test_choose_pixel_format(HDC hdc)
{
    PIXELFORMATDESCRIPTOR pfd = {0}, got;
    int format;

    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER_DONTCARE | PFD_STEREO_DONTCARE;
    pfd.iPixelType = PFD_TYPE_RGBA;
    format = ChoosePixelFormat(hdc, &pfd);
    ok(format > 0, "ChoosePixelFormat with PFD_DOUBLEBUFFER_DONTCARE | PFD_STEREO_DONTCARE returned %d\n", format);

    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL;
    pfd.iPixelType = 32;
    memset(&got, 0, sizeof(got));
    format = ChoosePixelFormat(hdc, &pfd);
    ok(format > 0, "ChoosePixelFormat with an unknown pixel type returned %d\n", format);
    if (format > 0 && DescribePixelFormat(hdc, format, sizeof(got), &got))
        ok(got.iPixelType == PFD_TYPE_RGBA, "an unknown pixel type chose pixel type %d, expected RGBA\n", got.iPixelType);
}

/* After a generic format is chosen the DC belongs to the built-in implementation, but the formats of
 * the driver stay in the list: querying them must not crash. */
static void test_query_after_generic_format(HWND hwnd)
{
    PIXELFORMATDESCRIPTOR pfd;
    LAYERPLANEDESCRIPTOR plane;
    HDC hdc = GetDC(hwnd);
    int i, count, generic = 0;

    count = DescribePixelFormat(hdc, 0, 0, NULL);
    for (i = 1; i <= count && !generic; i++)
    {
        if (DescribePixelFormat(hdc, i, sizeof(pfd), &pfd) &&
            (pfd.dwFlags & PFD_GENERIC_FORMAT) && (pfd.dwFlags & PFD_DRAW_TO_WINDOW) &&
            (pfd.dwFlags & PFD_SUPPORT_OPENGL) && pfd.iPixelType == PFD_TYPE_RGBA)
            generic = i;
    }
    ok(generic > 0, "no generic window format among %d\n", count);
    if (generic > 0)
    {
        BOOL ret;

        ok(SetPixelFormat(hdc, generic, &pfd), "SetPixelFormat(%d) failed, error %lu\n", generic, GetLastError());

        ok(DescribePixelFormat(hdc, 1, sizeof(pfd), &pfd) != 0, "DescribePixelFormat(1) failed after SetPixelFormat\n");
        memset(&plane, 0, sizeof(plane));
        ret = wglDescribeLayerPlane(hdc, 1, 0, sizeof(plane), &plane);
        trace("wglDescribeLayerPlane(1) returned %d\n", ret);
    }
    ReleaseDC(hwnd, hdc);
}

START_TEST(icd_default)
{
    PIXELFORMATDESCRIPTOR pfd = {0}, got;
    WNDCLASSA wc = {0};
    HWND hwnd;
    HDC hdc;
    HGLRC rc;
    int format;
    MSG msg;

    /* a window class of its own: a static control is transparent to WindowFromPoint */
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "icd_default";
    RegisterClassA(&wc);
    hwnd = CreateWindowExA(WS_EX_TOPMOST, "icd_default", "icd_default", WS_POPUP | WS_VISIBLE, 0, 0, WINDOW_SIZE, WINDOW_SIZE,
                           NULL, NULL, GetModuleHandleA(NULL), NULL);
    ok(hwnd != NULL, "CreateWindowEx failed, error %lu\n", GetLastError());
    if (!hwnd)
        return;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
        DispatchMessageA(&msg);

    hdc = GetDC(hwnd);
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.iLayerType = PFD_MAIN_PLANE;

    format = ChoosePixelFormat(hdc, &pfd);
    ok(format > 0, "ChoosePixelFormat failed, error %lu\n", GetLastError());
    ok(DescribePixelFormat(hdc, format, sizeof(got), &got) != 0, "DescribePixelFormat failed\n");
    ok(!(got.dwFlags & PFD_GENERIC_FORMAT), "the chosen format %d is the generic software format (flags 0x%lx): no driver\n",
       format, got.dwFlags);
    ok(SetPixelFormat(hdc, format, &got), "SetPixelFormat failed, error %lu\n", GetLastError());

    test_choose_pixel_format(hdc);
    {
        HWND other = CreateWindowExA(0, "icd_default", "icd_default 2", WS_POPUP, 200, 0, WINDOW_SIZE, WINDOW_SIZE,
                                     NULL, NULL, GetModuleHandleA(NULL), NULL);
        ok(other != NULL, "CreateWindowEx failed, error %lu\n", GetLastError());
        if (other)
        {
            test_query_after_generic_format(other);
            DestroyWindow(other);
        }
    }

    rc = wglCreateContext(hdc);
    ok(rc != NULL, "wglCreateContext failed, error %lu\n", GetLastError());
    if (rc && wglMakeCurrent(hdc, rc))
    {
        check_version();
        test_wgl_extensions(hdc);
        if (load_entry_points())
        {
            test_shader();
            test_framebuffer_object();
            test_blit();
        }
        test_present(hwnd, hdc);
        wglMakeCurrent(NULL, NULL);
    }
    else
        ok(0, "wglMakeCurrent failed, error %lu\n", GetLastError());

    if (rc)
        wglDeleteContext(rc);
    ReleaseDC(hwnd, hdc);
    DestroyWindow(hwnd);
    UnregisterClassA("icd_default", wc.hInstance);
}
