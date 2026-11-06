"""Exercise existing ANGLE binaries with two independent hardware devices.

No engine build, window, profile or preference changes. This tests pbuffer
rendering and context switching, not Xbox SwapChainPanel presentation.
"""
import argparse
import ctypes as c
import os

p = argparse.ArgumentParser()
p.add_argument("runtime")
p.add_argument("--frames", type=int, default=240)
p.add_argument("--fbo", action="store_true")
p.add_argument("--width", type=int, default=256)
p.add_argument("--height", type=int, default=128)
p.add_argument("--texture-pass", action="store_true")
p.add_argument("--debug-device", action="store_true")
p.add_argument("--partial", action="store_true")
args = p.parse_args()
width, height = args.width, args.height
directory = os.add_dll_directory(os.path.abspath(args.runtime))
egl = c.WinDLL(os.path.join(args.runtime, "libEGL.dll"))
gles = c.WinDLL(os.path.join(args.runtime, "libGLESv2.dll"))
P, I, U = c.c_void_p, c.c_int, c.c_uint


def function(dll, name, result, *arguments):
    f = getattr(dll, name)
    f.restype, f.argtypes = result, arguments
    return f


get_proc = function(egl, "eglGetProcAddress", P, c.c_char_p)


def extension(name, result, *arguments):
    address = get_proc(name.encode())
    if not address:
        raise RuntimeError("Missing extension " + name)
    return c.WINFUNCTYPE(result, *arguments)(address)


error = function(egl, "eglGetError", I)
initialize = function(egl, "eglInitialize", U, P, P, P)
choose = function(egl, "eglChooseConfig", U, P, P, P, I, P)
create_context = function(egl, "eglCreateContext", P, P, P, P, P)
create_surface = function(egl, "eglCreatePbufferSurface", P, P, P, P)
make_current = function(egl, "eglMakeCurrent", U, P, P, P, P)
current_display = function(egl, "eglGetCurrentDisplay", P)
current_context = function(egl, "eglGetCurrentContext", P)
destroy_surface = function(egl, "eglDestroySurface", U, P, P)
destroy_context = function(egl, "eglDestroyContext", U, P, P)
terminate = function(egl, "eglTerminate", U, P)
create_device = extension("eglCreateDeviceANGLE", P, I, P, P)
release_device = extension("eglReleaseDeviceANGLE", U, P)
platform_display = extension("eglGetPlatformDisplayEXT", P, U, P, P)
clear_color = function(gles, "glClearColor", None, *([c.c_float] * 4))
clear = function(gles, "glClear", None, U)
read = function(gles, "glReadPixels", None, I, I, I, I, U, U, P)
gl_error = function(gles, "glGetError", U)
viewport = function(gles, "glViewport", None, I, I, I, I)
draw = function(gles, "glDrawArrays", None, U, I, I)
use = function(gles, "glUseProgram", None, U)
uniform_color = function(gles, "glUniform4f", None, I, *([c.c_float] * 4))
get_string = function(gles, "glGetString", c.c_char_p, U)
query_surface = function(egl, "eglQuerySurface", U, P, P, I, P)
get_float = function(gles, "glGetFloatv", None, U, P)
create_d3d = function(c.WinDLL("d3d11.dll"), "D3D11CreateDevice", c.c_long,
                      P, I, P, U, P, U, U, P, P, P)


def check(value, operation):
    if not value:
        raise RuntimeError(f"{operation} EGL=0x{error():04x}")
    return value


def com_release(pointer):
    table = c.cast(pointer, c.POINTER(c.POINTER(P))).contents
    c.WINFUNCTYPE(U, P)(table[2])(pointer)


def shader_program(sample=False):
    shaders = []
    for kind, source in [(0x8B31, b'''#version 300 es
void main() {
  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}'''), (0x8B30, b'''#version 300 es
precision highp float;
uniform vec4 color;
out vec4 result;
void main() { result = color; }''')]:
        if sample and kind == 0x8B30:
            source = b'''#version 300 es
precision highp float;
uniform sampler2D image;
out vec4 result;
void main() { result = texelFetch(image, ivec2(gl_FragCoord.xy), 0); }'''
        shader = function(gles, "glCreateShader", U, U)(kind)
        text = c.c_char_p(source)
        function(gles, "glShaderSource", None, U, I, P, P)(shader, 1, c.byref(text), None)
        function(gles, "glCompileShader", None, U)(shader)
        status = I()
        function(gles, "glGetShaderiv", None, U, U, P)(shader, 0x8B81, c.byref(status))
        assert status.value, "shader compilation failed"
        shaders.append(shader)
    program = function(gles, "glCreateProgram", U)()
    for shader in shaders:
        function(gles, "glAttachShader", None, U, U)(program, shader)
    function(gles, "glLinkProgram", None, U)(program)
    status = I()
    function(gles, "glGetProgramiv", None, U, U, P)(program, 0x8B82, c.byref(status))
    assert status.value, "program link failed"
    for shader in shaders:
        function(gles, "glDeleteShader", None, U)(shader)
    return program, function(gles, "glGetUniformLocation", I, U, c.c_char_p)(program, b"color")


devices, wrappers, displays, contexts, surfaces, programs = [], [], [], [], [], []
targets, copy_programs = [], []


def color_target():
    texture, framebuffer = U(), U()
    function(gles, "glGenTextures", None, I, P)(1, c.byref(texture))
    function(gles, "glBindTexture", None, U, U)(0x0DE1, texture)
    function(gles, "glTexParameteri", None, U, U, I)(0x0DE1, 0x2801, 0x2600)
    function(gles, "glTexParameteri", None, U, U, I)(0x0DE1, 0x2800, 0x2600)
    function(gles, "glTexStorage2D", None, U, I, U, I, I)(0x0DE1, 1, 0x8058, width, height)
    function(gles, "glGenFramebuffers", None, I, P)(1, c.byref(framebuffer))
    function(gles, "glBindFramebuffer", None, U, U)(0x8D40, framebuffer)
    function(gles, "glFramebufferTexture2D", None, U, U, U, U, I)(
        0x8D40, 0x8CE0, 0x0DE1, texture, 0)
    assert function(gles, "glCheckFramebufferStatus", U, U)(0x8D40) == 0x8CD5
    return texture, framebuffer


try:
    for index in range(2):
        device = P()
        hr = create_d3d(None, 1, None, 0x22 if args.debug_device else 0x20,
                        None, 0, 7, c.byref(device), None, None)
        if hr < 0:
            raise RuntimeError(f"D3D11CreateDevice 0x{hr & 0xffffffff:08x}")
        devices.append(device)
        wrappers.append(check(create_device(0x33A1, device, None), "create device"))
        displays.append(check(platform_display(0x313F, wrappers[-1], None), "display"))
        display = displays[-1]
        check(initialize(display, None, None), "initialize")
        attrs = (I * 13)(0x3033, 1, 0x3040, 0x40, 0x3024, 8,
                          0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3038)
        config, count = P(), I()
        check(choose(display, attrs, c.byref(config), 1, c.byref(count)), "config")
        check(count.value, "no config")
        contexts.append(check(create_context(display, config, None,
                              (I * 3)(0x3098, 3, 0x3038)), "context"))
        surfaces.append(check(create_surface(display, config,
                              (I * 5)(0x3057, width, 0x3056, height, 0x3038)), "surface"))
        check(make_current(display, surfaces[-1], surfaces[-1], contexts[-1]), "bind")
        if args.fbo:
            texture, framebuffer = U(), U()
            function(gles, "glGenTextures", None, I, P)(1, c.byref(texture))
            function(gles, "glBindTexture", None, U, U)(0x0DE1, texture)
            function(gles, "glTexStorage2D", None, U, I, U, I, I)(
                0x0DE1, 1, 0x8058, width, height)
            function(gles, "glGenFramebuffers", None, I, P)(1, c.byref(framebuffer))
            function(gles, "glBindFramebuffer", None, U, U)(0x8D40, framebuffer)
            function(gles, "glFramebufferTexture2D", None, U, U, U, U, I)(
                0x8D40, 0x8CE0, 0x0DE1, texture, 0)
            depth = U()
            function(gles, "glGenRenderbuffers", None, I, P)(1, c.byref(depth))
            function(gles, "glBindRenderbuffer", None, U, U)(0x8D41, depth)
            function(gles, "glRenderbufferStorage", None, U, U, I, I)(
                0x8D41, 0x88F0, width, height)
            function(gles, "glFramebufferRenderbuffer", None, U, U, U, U)(
                0x8D40, 0x821A, 0x8D41, depth)
            assert function(gles, "glCheckFramebufferStatus", U, U)(0x8D40) == 0x8CD5
        clear_color(1, 0, 0, 1)
        color = (c.c_float * 4)()
        get_float(0x0C22, color)
        dims = [I(), I()]
        query_surface(display, surfaces[-1], 0x3057, c.byref(dims[0]))
        query_surface(display, surfaces[-1], 0x3056, c.byref(dims[1]))
        print(f"surface={dims[0].value}x{dims[1].value} clear={list(color)}")
        clear(0x4000)
        pixel = (c.c_ubyte * 4)()
        read(0, 0, 1, 1, 0x1908, 0x1401, pixel)
        print(f"initial_pixel={tuple(pixel)} gl_error={gl_error():#x}", flush=True)
        print(f"device={index} display={display:#x} context={contexts[-1]:#x} "
              f"renderer={get_string(0x1F01).decode()}", flush=True)
        programs.append(shader_program())
        if args.texture_pass:
            targets.append([color_target(), color_target()])
            copy_programs.append(shader_program(True)[0])

    for frame in range(args.frames):
        for index in range(2):
            d, s, ctx = displays[index], surfaces[index], contexts[index]
            check(make_current(d, s, s, ctx), "switch")
            assert current_display() == d and current_context() == ctx
            if args.texture_pass:
                function(gles, "glBindTexture", None, U, U)(0x0DE1, 0)
                function(gles, "glBindFramebuffer", None, U, U)(0x8D40, targets[index][0][1])
            expected = (frame % 256, 255 * index, 255 * (1 - index), 255)
            clear_color(0, 0, 0, 0)
            clear(0x4000)
            viewport(0, 0, width, height)
            use(programs[index][0])
            uniform_color(programs[index][1], *(v / 255 for v in expected))
            draw(4, 0, 3)
            if args.texture_pass:
                use(copy_programs[index])
                for source, destination in [(0, 1), (1, 0)]:
                    function(gles, "glBindFramebuffer", None, U, U)(
                        0x8D40, targets[index][destination][1])
                    function(gles, "glBindTexture", None, U, U)(0x0DE1, targets[index][source][0])
                    draw(4, 0, 3)
            pixels = (c.c_ubyte * (width * height * 4))()
            expected_image = bytearray(bytes(expected) * (width * height))
            if args.partial:
                rect_width, rect_height = min(width, 37), min(height, 19)
                replacement = (255, frame % 256, index * 255, 255)
                use(programs[index][0])
                uniform_color(programs[index][1], *(v / 255 for v in replacement))
                function(gles, "glEnable", None, U)(0x0C11)
                function(gles, "glScissor", None, I, I, I, I)(0, 0, rect_width, rect_height)
                draw(4, 0, 3)
                function(gles, "glDisable", None, U)(0x0C11)
                for row in range(rect_height):
                    start = row * width * 4
                    expected_image[start:start + rect_width * 4] = bytes(replacement) * rect_width
            read(0, 0, width, height, 0x1908, 0x1401, pixels)
            assert gl_error() == 0
            assert bytes(pixels) == expected_image, (
                frame, index, tuple(pixels[:4]), tuple(pixels[-4:]),
                sorted(set(tuple(pixels[n:n + 4]) for n in range(0, len(pixels), 4)))[:16])
    print(f"PASS: {args.frames * 2} full-frame readbacks, two hardware devices", flush=True)
finally:
    if displays:
        make_current(displays[-1], None, None, None)
    for d, s in zip(displays, surfaces):
        destroy_surface(d, s)
    for d, ctx in zip(displays, contexts):
        destroy_context(d, ctx)
    for d in displays:
        terminate(d)
    for wrapper in wrappers:
        release_device(wrapper)
    for device in devices:
        com_release(device)
