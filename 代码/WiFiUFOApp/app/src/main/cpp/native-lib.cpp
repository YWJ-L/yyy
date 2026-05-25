#include <android/log.h>
#include <android/native_app_glue/android_native_app_glue.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

static const char *kLogTag = "wifiufo";

static uint64_t now_ms() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000ULL + static_cast<uint64_t>(ts.tv_nsec) / 1000000ULL;
}

static int clamp_i(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static float clamp_f(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static bool resolve_ipv4(const std::string &host, sockaddr_in *out_addr) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;

    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) == 1) {
        *out_addr = addr;
        return true;
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    addrinfo *res = nullptr;
    const int rc = getaddrinfo(host.c_str(), nullptr, &hints, &res);
    if (rc != 0 || res == nullptr) {
        if (res) freeaddrinfo(res);
        return false;
    }

    auto *in = reinterpret_cast<sockaddr_in *>(res->ai_addr);
    addr.sin_addr = in->sin_addr;
    freeaddrinfo(res);
    *out_addr = addr;
    return true;
}

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
        std::string log;
        log.resize(static_cast<size_t>(len > 0 ? len : 1));
        glGetShaderInfoLog(s, len, nullptr, log.data());
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "shader compile failed: %s", log.c_str());
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint link_program(GLuint vs, GLuint fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetProgramiv(p, GL_INFO_LOG_LENGTH, &len);
        std::string log;
        log.resize(static_cast<size_t>(len > 0 ? len : 1));
        glGetProgramInfoLog(p, len, nullptr, log.data());
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "program link failed: %s", log.c_str());
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

struct GlState {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    int width = 0;
    int height = 0;
    GLuint program = 0;
    GLint a_pos = -1;
    GLint u_color = -1;
};

struct Stick {
    float x = 0.0f;
    float y = 0.0f;
    float center_x = 0.0f;
    float center_y = 0.0f;
    float default_x = 0.0f;
    float default_y = 0.0f;
    int active_id = -1;
    void reset() { x = default_x; y = default_y; active_id = -1; }
};

struct AppState {
    android_app *app = nullptr;
    GlState gl{};

    bool locked = false;
    int speed_percent = 100;

    Stick left{};
    Stick right{};

    int aux2_pulse = 0;
    int aux2_burst = 0;

    std::string host = "192.168.4.1";
    int port = 8895;
    int sock = -1;
    sockaddr_in addr{};
    bool addr_ready = false;

    uint64_t last_send_ms = 0;
};

static void gl_shutdown(GlState *gls) {
    if (gls->display != EGL_NO_DISPLAY) {
        eglMakeCurrent(gls->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (gls->context != EGL_NO_CONTEXT) eglDestroyContext(gls->display, gls->context);
        if (gls->surface != EGL_NO_SURFACE) eglDestroySurface(gls->display, gls->surface);
        eglTerminate(gls->display);
    }
    *gls = GlState{};
}

static bool gl_init(android_app *app, GlState *gls) {
    const EGLint attribs[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_BLUE_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_RED_SIZE, 8,
            EGL_NONE
    };

    EGLint num_configs = 0;
    EGLConfig config = nullptr;

    gls->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (gls->display == EGL_NO_DISPLAY) return false;
    if (eglInitialize(gls->display, nullptr, nullptr) != EGL_TRUE) return false;
    if (eglChooseConfig(gls->display, attribs, &config, 1, &num_configs) != EGL_TRUE) return false;
    if (num_configs < 1) return false;

    EGLint format = 0;
    eglGetConfigAttrib(gls->display, config, EGL_NATIVE_VISUAL_ID, &format);
    ANativeWindow_setBuffersGeometry(app->window, 0, 0, format);

    const EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    gls->surface = eglCreateWindowSurface(gls->display, config, app->window, nullptr);
    gls->context = eglCreateContext(gls->display, config, EGL_NO_CONTEXT, ctx_attribs);
    if (gls->surface == EGL_NO_SURFACE || gls->context == EGL_NO_CONTEXT) return false;
    if (eglMakeCurrent(gls->display, gls->surface, gls->surface, gls->context) != EGL_TRUE) return false;

    eglQuerySurface(gls->display, gls->surface, EGL_WIDTH, &gls->width);
    eglQuerySurface(gls->display, gls->surface, EGL_HEIGHT, &gls->height);

    const char *vs_src = "attribute vec2 aPos;void main(){gl_Position=vec4(aPos,0.0,1.0);}";
    const char *fs_src = "precision mediump float;uniform vec4 uColor;void main(){gl_FragColor=uColor;}";

    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) return false;
    gls->program = link_program(vs, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!gls->program) return false;

    gls->a_pos = glGetAttribLocation(gls->program, "aPos");
    gls->u_color = glGetUniformLocation(gls->program, "uColor");
    return true;
}

static void draw_rect(GlState *gls, float x0, float y0, float x1, float y1, float r, float g, float b, float a) {
    float vx0 = (x0 / static_cast<float>(gls->width)) * 2.0f - 1.0f;
    float vx1 = (x1 / static_cast<float>(gls->width)) * 2.0f - 1.0f;
    float vy0 = 1.0f - (y0 / static_cast<float>(gls->height)) * 2.0f;
    float vy1 = 1.0f - (y1 / static_cast<float>(gls->height)) * 2.0f;

    const GLfloat verts[] = {vx0, vy1, vx1, vy1, vx0, vy0, vx1, vy0};
    glUseProgram(gls->program);
    glUniform4f(gls->u_color, r, g, b, a);
    glEnableVertexAttribArray(gls->a_pos);
    glVertexAttribPointer(gls->a_pos, 2, GL_FLOAT, GL_FALSE, 0, verts);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(gls->a_pos);
}

static void ensure_socket(AppState *st) {
    if (st->sock >= 0) return;
    st->sock = socket(AF_INET, SOCK_DGRAM, 0);
}

static void ensure_addr(AppState *st) {
    if (st->addr_ready) return;
    sockaddr_in addr{};
    if (!resolve_ipv4(st->host, &addr)) return;
    addr.sin_port = htons(static_cast<uint16_t>(st->port));
    st->addr = addr;
    st->addr_ready = true;
}

static void send_frame(AppState *st, uint8_t roll, uint8_t pitch, uint8_t thr, uint8_t yaw, uint8_t aux2) {
    ensure_socket(st);
    ensure_addr(st);
    if (st->sock < 0 || !st->addr_ready) return;
    uint8_t frame[8] = {0x66, roll, pitch, thr, yaw, aux2, 0x80, 0x99};
    sendto(st->sock, frame, sizeof(frame), 0, reinterpret_cast<sockaddr *>(&st->addr), sizeof(st->addr));
}

static void update_and_send(AppState *st) {
    uint64_t t = now_ms();
    const uint64_t interval = 20;
    if (t - st->last_send_ms < interval) return;
    st->last_send_ms = t;
    if (st->locked) return;

    float scale = clamp_f(static_cast<float>(st->speed_percent) / 100.0f, 0.2f, 1.0f);
    int roll_i = static_cast<int>(std::lround(128.0f + clamp_f(st->right.x * scale, -1.0f, 1.0f) * 127.0f));
    int pitch_i = static_cast<int>(std::lround(128.0f + clamp_f(-st->right.y * scale, -1.0f, 1.0f) * 127.0f));
    int yaw_i = static_cast<int>(std::lround(128.0f + clamp_f(st->left.x * scale, -1.0f, 1.0f) * 127.0f));
    float thr01 = clamp_f((1.0f - st->left.y) * 0.5f, 0.0f, 1.0f);
    int thr_i = static_cast<int>(std::lround(thr01 * 255.0f));

    int aux2 = 0;
    if (st->aux2_burst > 0) {
        aux2 = st->aux2_pulse;
        st->aux2_burst -= 1;
    }

    send_frame(
            st,
            static_cast<uint8_t>(clamp_i(roll_i, 0, 255)),
            static_cast<uint8_t>(clamp_i(pitch_i, 0, 255)),
            static_cast<uint8_t>(clamp_i(thr_i, 0, 255)),
            static_cast<uint8_t>(clamp_i(yaw_i, 0, 255)),
            static_cast<uint8_t>(clamp_i(aux2, 0, 255))
    );
}

static void layout_controls(AppState *st) {
    float w = static_cast<float>(st->gl.width);
    float h = static_cast<float>(st->gl.height);
    float pad = w * 0.08f;
    float stick_r = w * 0.16f;

    st->left.center_x = pad + stick_r;
    st->left.center_y = h - pad - stick_r;
    st->left.default_x = 0.0f;
    st->left.default_y = 1.0f;
    if (st->left.active_id < 0) { st->left.x = st->left.default_x; st->left.y = st->left.default_y; }

    st->right.center_x = w - pad - stick_r;
    st->right.center_y = h - pad - stick_r;
    st->right.default_x = 0.0f;
    st->right.default_y = 0.0f;
    if (st->right.active_id < 0) { st->right.x = st->right.default_x; st->right.y = st->right.default_y; }
}

static int32_t handle_input(android_app *app, AInputEvent *event) {
    auto *st = reinterpret_cast<AppState *>(app->userData);
    if (!st) return 0;
    if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION) return 0;

    const int action = AMotionEvent_getAction(event);
    const int action_masked = action & AMOTION_EVENT_ACTION_MASK;
    const int action_index = (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;

    auto process_down = [&](int idx) {
        const int id = AMotionEvent_getPointerId(event, idx);
        const float x = AMotionEvent_getX(event, idx);
        const float y = AMotionEvent_getY(event, idx);

        float top_h = static_cast<float>(st->gl.height) * 0.12f;
        if (y <= top_h) {
            float w = static_cast<float>(st->gl.width);
            float btn_w = w / 9.0f;
            int col = static_cast<int>(x / btn_w);
            if (col == 1) { st->aux2_pulse = 16; st->aux2_burst = 3; }
            else if (col == 2) { st->aux2_pulse = 8; st->aux2_burst = 3; }
            else if (col == 7) { st->locked = !st->locked; }
            return;
        }

        if (x < static_cast<float>(st->gl.width) * 0.5f && st->left.active_id < 0) st->left.active_id = id;
        else if (st->right.active_id < 0) st->right.active_id = id;
    };

    auto process_up = [&](int idx) {
        const int id = AMotionEvent_getPointerId(event, idx);
        if (id == st->left.active_id) st->left.reset();
        if (id == st->right.active_id) st->right.reset();
    };

    auto process_move = [&]() {
        const int count = AMotionEvent_getPointerCount(event);
        float stick_r = static_cast<float>(st->gl.width) * 0.16f;
        float usable = stick_r;

        for (int i = 0; i < count; i++) {
            const int id = AMotionEvent_getPointerId(event, i);
            const float x = AMotionEvent_getX(event, i);
            const float y = AMotionEvent_getY(event, i);

            auto update_stick = [&](Stick &s) {
                float dx = (x - s.center_x) / usable;
                float dy = (y - s.center_y) / usable;
                float len = std::sqrt(dx * dx + dy * dy);
                if (len > 1.0f) { dx /= len; dy /= len; }
                s.x = clamp_f(dx, -1.0f, 1.0f);
                s.y = clamp_f(dy, -1.0f, 1.0f);
            };

            if (id == st->left.active_id) update_stick(st->left);
            if (id == st->right.active_id) update_stick(st->right);
        }
    };

    switch (action_masked) {
        case AMOTION_EVENT_ACTION_DOWN:
        case AMOTION_EVENT_ACTION_POINTER_DOWN: process_down(action_index); return 1;
        case AMOTION_EVENT_ACTION_UP:
        case AMOTION_EVENT_ACTION_POINTER_UP:
        case AMOTION_EVENT_ACTION_CANCEL: process_up(action_index); return 1;
        case AMOTION_EVENT_ACTION_MOVE: process_move(); return 1;
        default: return 0;
    }
}

static void handle_cmd(android_app *app, int32_t cmd) {
    auto *st = reinterpret_cast<AppState *>(app->userData);
    if (!st) return;
    if (cmd == APP_CMD_INIT_WINDOW) {
        if (app->window != nullptr) {
            if (st->gl.display != EGL_NO_DISPLAY) gl_shutdown(&st->gl);
            if (gl_init(app, &st->gl)) layout_controls(st);
        }
    } else if (cmd == APP_CMD_TERM_WINDOW) {
        gl_shutdown(&st->gl);
    }
}

static void render(AppState *st) {
    if (st->gl.display == EGL_NO_DISPLAY) return;
    glViewport(0, 0, st->gl.width, st->gl.height);
    glClearColor(0.043f, 0.118f, 0.169f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    float w = static_cast<float>(st->gl.width);
    float h = static_cast<float>(st->gl.height);
    float top_h = h * 0.12f;

    draw_rect(&st->gl, 0, 0, w, top_h, 0.04f, 0.18f, 0.22f, 1.0f);
    float btn_w = w / 9.0f;
    auto draw_btn = [&](int col, float r, float g, float b) {
        float x0 = col * btn_w + btn_w * 0.20f;
        float x1 = col * btn_w + btn_w * 0.80f;
        float y0 = top_h * 0.20f;
        float y1 = top_h * 0.80f;
        draw_rect(&st->gl, x0, y0, x1, y1, r, g, b, 1.0f);
    };
    draw_btn(1, 0.12f, 0.48f, 0.48f);
    draw_btn(2, 0.12f, 0.48f, 0.48f);
    draw_btn(7, st->locked ? 0.70f : 0.12f, 0.48f, 0.48f);

    float stick_r = w * 0.16f;
    auto draw_stick = [&](const Stick &s) {
        float cx = s.center_x;
        float cy = s.center_y;
        draw_rect(&st->gl, cx - stick_r, cy - stick_r, cx + stick_r, cy + stick_r, 0.07f, 0.17f, 0.24f, 1.0f);
        float k = stick_r * 0.35f;
        float kx = cx + s.x * stick_r * 0.75f;
        float ky = cy + s.y * stick_r * 0.75f;
        draw_rect(&st->gl, kx - k, ky - k, kx + k, ky + k, 0.12f, 0.48f, 0.48f, 1.0f);
    };
    draw_stick(st->left);
    draw_stick(st->right);

    eglSwapBuffers(st->gl.display, st->gl.surface);
}

void android_main(android_app *app) {
    app_dummy();
    AppState st{};
    st.app = app;
    app->userData = &st;
    app->onAppCmd = handle_cmd;
    app->onInputEvent = handle_input;

    st.left.reset();
    st.right.reset();

    while (true) {
        int events = 0;
        android_poll_source *source = nullptr;
        while (ALooper_pollOnce(0, nullptr, &events, reinterpret_cast<void **>(&source)) >= 0) {
            if (source) source->process(app, source);
            if (app->destroyRequested) {
                if (st.sock >= 0) close(st.sock);
                gl_shutdown(&st.gl);
                return;
            }
        }
        if (st.gl.display != EGL_NO_DISPLAY) {
            layout_controls(&st);
            update_and_send(&st);
            render(&st);
        }
    }
}
