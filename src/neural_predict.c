/*
 * neural_predict.c  -  Neural network window prediction (GPU OpenGL 3.3 + CPU fallback + RL Self-Correction)
 *
 * Network architecture:
 *   Input(NN_INPUT_DIM = 16) -> Dense(NN_HIDDEN1 = 32, ReLU)
 *                            -> Dense(NN_HIDDEN2 = 16, ReLU)
 *                            -> Dense(NN_OUTPUT_DIM = 64, Linear / Softmax)
 *
 * GPU Inference on Intel HD3000:
 *   Uses off-screen GLX Pbuffer + Core Profile 3.3 fragment shaders rendering to
 *   R32F float textures.
 *
 * Reinforcement Learning & Online Self-Correction:
 *   When the user focuses a window, feedback is received:
 *     - If correct (y_pred == y_actual): Positive reward (+1.0), reinforces pathway.
 *     - If incorrect (y_pred != y_actual): Penalty (-1.0), computes cross-entropy error
 *       gradient dz3 = p - 1(y_actual), backpropagates through dense layers, updates
 *       weights W1, b1, W2, b2, W3, b3 online via SGD, syncs to GPU textures, and saves to disk.
 */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define GL_GLEXT_PROTOTYPES

#include "zrn_perf.h"
#include <math.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <GL/glext.h>

/* Global NN state for UI / CLI / TUI / logging */
NNState g_nn_state = {0};
char    g_predicted_comm[64] = "";

/* NN weights in host memory */
static int    s_loaded = 0;
static float  s_W1[NN_HIDDEN1 * NN_INPUT_DIM];
static float  s_b1[NN_HIDDEN1];
static float  s_W2[NN_HIDDEN2 * NN_HIDDEN1];
static float  s_b2[NN_HIDDEN2];
static float  s_W3[NN_OUTPUT_DIM * NN_HIDDEN2];
static float  s_b3[NN_OUTPUT_DIM];
static char   s_app_names[NN_OUTPUT_DIM][64];
static int    s_n_apps = 0;

/* Online RL state */
static float  s_last_input[NN_INPUT_DIM];
static float  s_last_z1[NN_HIDDEN1];
static float  s_last_h1[NN_HIDDEN1];
static float  s_last_z2[NN_HIDDEN2];
static float  s_last_h2[NN_HIDDEN2];
static float  s_last_probs[NN_OUTPUT_DIM];
static int    s_last_predicted_idx = -1;
static int    s_has_last_pred = 0;

/* OpenGL 3.3 GPU acceleration state */
static int        s_gpu_available = 0;
static Display   *s_gl_dpy        = NULL;
static GLXContext s_gl_ctx        = NULL;
static GLXPbuffer s_gl_pbuf       = 0;
static GLuint     s_program       = 0;
static GLuint     s_vao           = 0;

/* GPU Textures */
static GLuint     s_tex_input     = 0;
static GLuint     s_tex_w1        = 0;
static GLuint     s_tex_b1        = 0;
static GLuint     s_tex_h1        = 0;
static GLuint     s_fbo_h1        = 0;

static GLuint     s_tex_w2        = 0;
static GLuint     s_tex_b2        = 0;
static GLuint     s_tex_h2        = 0;
static GLuint     s_fbo_h2        = 0;

static GLuint     s_tex_w3        = 0;
static GLuint     s_tex_b3        = 0;
static GLuint     s_tex_out       = 0;
static GLuint     s_fbo_out       = 0;

/* Uniform locations */
static GLint      s_loc_input     = -1;
static GLint      s_loc_weights   = -1;
static GLint      s_loc_bias      = -1;
static GLint      s_loc_in_dim    = -1;
static GLint      s_loc_out_dim   = -1;
static GLint      s_loc_use_relu  = -1;

/* -------------------------------------------------------------------------- */
/* OpenGL 3.3 Shader Sources                                                  */
/* -------------------------------------------------------------------------- */
static const char *s_gpu_vert_src =
    "#version 330 core\n"
    "void main() {\n"
    "    vec2 pos[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));\n"
    "    gl_Position = vec4(pos[gl_VertexID], 0.0, 1.0);\n"
    "}\n";

static const char *s_gpu_frag_src =
    "#version 330 core\n"
    "uniform sampler2D u_input;\n"
    "uniform sampler2D u_weights;\n"
    "uniform sampler2D u_bias;\n"
    "uniform int u_in_dim;\n"
    "uniform int u_out_dim;\n"
    "uniform int u_use_relu;\n"
    "out vec4 frag_color;\n"
    "void main() {\n"
    "    int out_idx = int(gl_FragCoord.x);\n"
    "    if (out_idx >= u_out_dim) { frag_color = vec4(0.0); return; }\n"
    "    float sum = texelFetch(u_bias, ivec2(out_idx, 0), 0).r;\n"
    "    for (int i = 0; i < u_in_dim; i++) {\n"
    "        float x = texelFetch(u_input, ivec2(i, 0), 0).r;\n"
    "        float w = texelFetch(u_weights, ivec2(i, out_idx), 0).r;\n"
    "        sum += w * x;\n"
    "    }\n"
    "    if (u_use_relu == 1) sum = max(0.0, sum);\n"
    "    frag_color = vec4(sum, 0.0, 0.0, 1.0);\n"
    "}\n";

/* -------------------------------------------------------------------------- */
/* Texture & FBO helpers                                                      */
/* -------------------------------------------------------------------------- */
static GLuint create_float_tex(int width, int height, const float *data)
{
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, width, height, 0, GL_RED, GL_FLOAT, data);
    return tex;
}

static GLuint create_fbo_with_tex(int width, int height, GLuint *out_tex)
{
    GLuint tex = create_float_tex(width, height, NULL);
    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    *out_tex = tex;
    return fbo;
}

static void upload_weights_to_gpu(void)
{
    if (!s_gpu_available || !s_gl_ctx || !s_loaded) return;

    glXMakeCurrent(s_gl_dpy, s_gl_pbuf, s_gl_ctx);

    glBindTexture(GL_TEXTURE_2D, s_tex_w1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, NN_INPUT_DIM, NN_HIDDEN1, GL_RED, GL_FLOAT, s_W1);

    glBindTexture(GL_TEXTURE_2D, s_tex_b1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, NN_HIDDEN1, 1, GL_RED, GL_FLOAT, s_b1);

    glBindTexture(GL_TEXTURE_2D, s_tex_w2);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, NN_HIDDEN1, NN_HIDDEN2, GL_RED, GL_FLOAT, s_W2);

    glBindTexture(GL_TEXTURE_2D, s_tex_b2);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, NN_HIDDEN2, 1, GL_RED, GL_FLOAT, s_b2);

    glBindTexture(GL_TEXTURE_2D, s_tex_w3);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, NN_HIDDEN2, NN_OUTPUT_DIM, GL_RED, GL_FLOAT, s_W3);

    glBindTexture(GL_TEXTURE_2D, s_tex_b3);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, NN_OUTPUT_DIM, 1, GL_RED, GL_FLOAT, s_b3);

    glBindTexture(GL_TEXTURE_2D, 0);
    glXMakeCurrent(s_gl_dpy, None, NULL);
}

/* -------------------------------------------------------------------------- */
/* Simple hash to convert a comm name to a float feature                      */
/* -------------------------------------------------------------------------- */
static float hash_comm(const char *comm)
{
    unsigned long h = 5381;
    for (const char *p = comm; *p; p++)
        h = ((h << 5) + h) + (unsigned char)*p;
    return (float)(h % 10000) / 10000.0f;
}

/* -------------------------------------------------------------------------- */
/* Feature recording                                                          */
/* -------------------------------------------------------------------------- */
void nn_record_features(const SwitchEvent *ev, int switch_rate_1min)
{
    (void)ev;
    (void)switch_rate_1min;
}

/* -------------------------------------------------------------------------- */
/* GPU OpenGL 3.3 Initialization & Shutdown                                   */
/* -------------------------------------------------------------------------- */
int nn_gpu_init(Display *dpy)
{
    if (!dpy) return -1;

    int glx_major, glx_minor;
    if (!glXQueryVersion(dpy, &glx_major, &glx_minor))
        return -1;

    int fb_attribs[] = {
        GLX_RENDER_TYPE,   GLX_RGBA_BIT,
        GLX_DRAWABLE_TYPE, GLX_PBUFFER_BIT,
        GLX_RED_SIZE,   8,
        GLX_GREEN_SIZE, 8,
        GLX_BLUE_SIZE,  8,
        GLX_ALPHA_SIZE, 8,
        None
    };

    int nconfigs = 0;
    GLXFBConfig *configs = glXChooseFBConfig(dpy, DefaultScreen(dpy),
                                             fb_attribs, &nconfigs);
    if (!configs || nconfigs == 0) return -1;

    typedef GLXContext (*PFNGLXCCAARBPROC)(Display *, GLXFBConfig,
                                           GLXContext, Bool, const int *);
    PFNGLXCCAARBPROC createCtx = (PFNGLXCCAARBPROC)
        glXGetProcAddress((const GLubyte *)"glXCreateContextAttribsARB");
    if (!createCtx) { XFree(configs); return -1; }

    int ctx_attribs[] = {
        GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
        GLX_CONTEXT_MINOR_VERSION_ARB, 3,
        GLX_CONTEXT_PROFILE_MASK_ARB,  GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
        None
    };

    s_gl_dpy = dpy;
    s_gl_ctx = createCtx(dpy, configs[0], NULL, True, ctx_attribs);
    if (!s_gl_ctx) { XFree(configs); return -1; }

    int pbuf_attribs[] = {
        GLX_PBUFFER_WIDTH,  128,
        GLX_PBUFFER_HEIGHT, 128,
        None
    };
    s_gl_pbuf = glXCreatePbuffer(dpy, configs[0], pbuf_attribs);
    XFree(configs);
    if (!s_gl_pbuf) {
        glXDestroyContext(dpy, s_gl_ctx);
        s_gl_ctx = NULL;
        return -1;
    }

    if (!glXMakeCurrent(dpy, s_gl_pbuf, s_gl_ctx)) {
        glXDestroyPbuffer(dpy, s_gl_pbuf);
        glXDestroyContext(dpy, s_gl_ctx);
        s_gl_pbuf = 0;
        s_gl_ctx = NULL;
        return -1;
    }

    /* Compile shaders */
    GLint ok;
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &s_gpu_vert_src, NULL);
    glCompileShader(vs);
    glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) { glDeleteShader(vs); goto fail_gl; }

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &s_gpu_frag_src, NULL);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) { glDeleteShader(vs); glDeleteShader(fs); goto fail_gl; }

    s_program = glCreateProgram();
    glAttachShader(s_program, vs);
    glAttachShader(s_program, fs);
    glLinkProgram(s_program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    glGetProgramiv(s_program, GL_LINK_STATUS, &ok);
    if (!ok) { glDeleteProgram(s_program); s_program = 0; goto fail_gl; }

    s_loc_input    = glGetUniformLocation(s_program, "u_input");
    s_loc_weights  = glGetUniformLocation(s_program, "u_weights");
    s_loc_bias     = glGetUniformLocation(s_program, "u_bias");
    s_loc_in_dim   = glGetUniformLocation(s_program, "u_in_dim");
    s_loc_out_dim  = glGetUniformLocation(s_program, "u_out_dim");
    s_loc_use_relu = glGetUniformLocation(s_program, "u_use_relu");

    glGenVertexArrays(1, &s_vao);

    /* Allocate static textures and FBOs */
    s_tex_input = create_float_tex(NN_INPUT_DIM, 1, NULL);
    s_tex_w1    = create_float_tex(NN_INPUT_DIM, NN_HIDDEN1, NULL);
    s_tex_b1    = create_float_tex(NN_HIDDEN1, 1, NULL);
    s_fbo_h1    = create_fbo_with_tex(NN_HIDDEN1, 1, &s_tex_h1);

    s_tex_w2    = create_float_tex(NN_HIDDEN1, NN_HIDDEN2, NULL);
    s_tex_b2    = create_float_tex(NN_HIDDEN2, 1, NULL);
    s_fbo_h2    = create_fbo_with_tex(NN_HIDDEN2, 1, &s_tex_h2);

    s_tex_w3    = create_float_tex(NN_HIDDEN2, NN_OUTPUT_DIM, NULL);
    s_tex_b3    = create_float_tex(NN_OUTPUT_DIM, 1, NULL);
    s_fbo_out   = create_fbo_with_tex(NN_OUTPUT_DIM, 1, &s_tex_out);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glXMakeCurrent(dpy, None, NULL);

    s_gpu_available = 1;
    if (!g_tui_mode && !g_gui_mode) {
        printf("[neural_predict] GPU Inference Engine: OpenGL 3.3 Core READY on Intel HD3000\n");
        fflush(stdout);
    }

    if (s_loaded) upload_weights_to_gpu();
    return 0;

fail_gl:
    glXMakeCurrent(dpy, None, NULL);
    if (s_gl_pbuf) { glXDestroyPbuffer(dpy, s_gl_pbuf); s_gl_pbuf = 0; }
    if (s_gl_ctx)  { glXDestroyContext(dpy, s_gl_ctx);   s_gl_ctx  = NULL; }
    return -1;
}

void nn_gpu_shutdown(void)
{
    if (!s_gpu_available || !s_gl_ctx || !s_gl_dpy) return;

    glXMakeCurrent(s_gl_dpy, s_gl_pbuf, s_gl_ctx);

    if (s_fbo_h1)  glDeleteFramebuffers(1, &s_fbo_h1);
    if (s_fbo_h2)  glDeleteFramebuffers(1, &s_fbo_h2);
    if (s_fbo_out) glDeleteFramebuffers(1, &s_fbo_out);

    if (s_tex_input) glDeleteTextures(1, &s_tex_input);
    if (s_tex_w1)    glDeleteTextures(1, &s_tex_w1);
    if (s_tex_b1)    glDeleteTextures(1, &s_tex_b1);
    if (s_tex_h1)    glDeleteTextures(1, &s_tex_h1);
    if (s_tex_w2)    glDeleteTextures(1, &s_tex_w2);
    if (s_tex_b2)    glDeleteTextures(1, &s_tex_b2);
    if (s_tex_h2)    glDeleteTextures(1, &s_tex_h2);
    if (s_tex_w3)    glDeleteTextures(1, &s_tex_w3);
    if (s_tex_b3)    glDeleteTextures(1, &s_tex_b3);
    if (s_tex_out)   glDeleteTextures(1, &s_tex_out);

    if (s_vao)       glDeleteVertexArrays(1, &s_vao);
    if (s_program)   glDeleteProgram(s_program);

    glXMakeCurrent(s_gl_dpy, None, NULL);

    glXDestroyPbuffer(s_gl_dpy, s_gl_pbuf);
    glXDestroyContext(s_gl_dpy, s_gl_ctx);

    s_gl_pbuf = 0;
    s_gl_ctx  = NULL;
    s_gpu_available = 0;
}

/* -------------------------------------------------------------------------- */
/* Save model weights to disk (atomic)                                         */
/* -------------------------------------------------------------------------- */
int nn_save_weights(const char *path)
{
    if (!s_loaded || s_n_apps == 0) return -1;

    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    FILE *f = fopen(tmp_path, "wb");
    if (!f) return -1;

    fwrite(s_W1, sizeof(float), NN_HIDDEN1 * NN_INPUT_DIM, f);
    fwrite(s_b1, sizeof(float), NN_HIDDEN1, f);
    fwrite(s_W2, sizeof(float), NN_HIDDEN2 * NN_HIDDEN1, f);
    fwrite(s_b2, sizeof(float), NN_HIDDEN2, f);
    fwrite(s_W3, sizeof(float), NN_OUTPUT_DIM * NN_HIDDEN2, f);
    fwrite(s_b3, sizeof(float), NN_OUTPUT_DIM, f);

    fwrite(&s_n_apps, sizeof(int), 1, f);
    for (int i = 0; i < s_n_apps; i++) {
        fwrite(s_app_names[i], 1, 64, f);
    }
    fclose(f);

    rename(tmp_path, path);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Load model weights from disk                                                */
/* -------------------------------------------------------------------------- */
int nn_load_weights(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    size_t r = 0;
    r += fread(s_W1, sizeof(float), NN_HIDDEN1 * NN_INPUT_DIM, f);
    r += fread(s_b1, sizeof(float), NN_HIDDEN1, f);
    r += fread(s_W2, sizeof(float), NN_HIDDEN2 * NN_HIDDEN1, f);
    r += fread(s_b2, sizeof(float), NN_HIDDEN2, f);
    r += fread(s_W3, sizeof(float), NN_OUTPUT_DIM * NN_HIDDEN2, f);
    r += fread(s_b3, sizeof(float), NN_OUTPUT_DIM, f);

    /* Read app name table */
    if (fread(&s_n_apps, sizeof(int), 1, f) == 1) {
        if (s_n_apps > NN_OUTPUT_DIM) s_n_apps = NN_OUTPUT_DIM;
        for (int i = 0; i < s_n_apps; i++) {
            if (fread(s_app_names[i], 1, 64, f) != 64) {
                fclose(f);
                return -1;
            }
            s_app_names[i][63] = '\0';
        }
    }

    fclose(f);

    size_t expected = (size_t)(NN_HIDDEN1 * NN_INPUT_DIM + NN_HIDDEN1
                     + NN_HIDDEN2 * NN_HIDDEN1 + NN_HIDDEN2
                     + NN_OUTPUT_DIM * NN_HIDDEN2 + NN_OUTPUT_DIM);
    if (r != expected) return -1;

    s_loaded = 1;
    upload_weights_to_gpu();
    return 0;
}

/* -------------------------------------------------------------------------- */
/* GPU Forward Pass execution                                                 */
/* -------------------------------------------------------------------------- */
static int nn_forward_gpu(const float input[NN_INPUT_DIM], float out[NN_OUTPUT_DIM])
{
    if (!s_gpu_available || !s_gl_ctx) return -1;

    if (!glXMakeCurrent(s_gl_dpy, s_gl_pbuf, s_gl_ctx))
        return -1;

    glUseProgram(s_program);
    glBindVertexArray(s_vao);

    /* Upload input vector */
    glBindTexture(GL_TEXTURE_2D, s_tex_input);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, NN_INPUT_DIM, 1, GL_RED, GL_FLOAT, input);

    /* Layer 1: Input(16) -> Hidden1(32, ReLU) */
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo_h1);
    glViewport(0, 0, NN_HIDDEN1, 1);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, s_tex_input); glUniform1i(s_loc_input, 0);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, s_tex_w1);    glUniform1i(s_loc_weights, 1);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, s_tex_b1);    glUniform1i(s_loc_bias, 2);
    glUniform1i(s_loc_in_dim, NN_INPUT_DIM);
    glUniform1i(s_loc_out_dim, NN_HIDDEN1);
    glUniform1i(s_loc_use_relu, 1);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    /* Layer 2: Hidden1(32) -> Hidden2(16, ReLU) */
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo_h2);
    glViewport(0, 0, NN_HIDDEN2, 1);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, s_tex_h1);    glUniform1i(s_loc_input, 0);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, s_tex_w2);    glUniform1i(s_loc_weights, 1);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, s_tex_b2);    glUniform1i(s_loc_bias, 2);
    glUniform1i(s_loc_in_dim, NN_HIDDEN1);
    glUniform1i(s_loc_out_dim, NN_HIDDEN2);
    glUniform1i(s_loc_use_relu, 1);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    /* Layer 3: Hidden2(16) -> Output(64, Linear) */
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo_out);
    glViewport(0, 0, NN_OUTPUT_DIM, 1);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, s_tex_h2);    glUniform1i(s_loc_input, 0);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, s_tex_w3);    glUniform1i(s_loc_weights, 1);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, s_tex_b3);    glUniform1i(s_loc_bias, 2);
    glUniform1i(s_loc_in_dim, NN_HIDDEN2);
    glUniform1i(s_loc_out_dim, NN_OUTPUT_DIM);
    glUniform1i(s_loc_use_relu, 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    /* Read back predictions */
    glReadPixels(0, 0, NN_OUTPUT_DIM, 1, GL_RED, GL_FLOAT, out);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindVertexArray(0);
    glUseProgram(0);
    glXMakeCurrent(s_gl_dpy, None, NULL);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* CPU Forward Pass fallback                                                  */
/* -------------------------------------------------------------------------- */
static void nn_forward_cpu(const float input[NN_INPUT_DIM], float out[NN_OUTPUT_DIM])
{
    /* Layer 1: h1 = ReLU(W1 * input + b1) */
    float h1[NN_HIDDEN1];
    for (int i = 0; i < NN_HIDDEN1; i++) {
        float sum = s_b1[i];
        for (int j = 0; j < NN_INPUT_DIM; j++)
            sum += s_W1[i * NN_INPUT_DIM + j] * input[j];
        h1[i] = sum > 0 ? sum : 0;
    }

    /* Layer 2: h2 = ReLU(W2 * h1 + b2) */
    float h2[NN_HIDDEN2];
    for (int i = 0; i < NN_HIDDEN2; i++) {
        float sum = s_b2[i];
        for (int j = 0; j < NN_HIDDEN1; j++)
            sum += s_W2[i * NN_HIDDEN1 + j] * h1[j];
        h2[i] = sum > 0 ? sum : 0;
    }

    /* Layer 3: out = W3 * h2 + b3 */
    for (int i = 0; i < s_n_apps; i++) {
        float sum = s_b3[i];
        for (int j = 0; j < NN_HIDDEN2; j++)
            sum += s_W3[i * NN_HIDDEN2 + j] * h2[j];
        out[i] = sum;
    }
}

/* -------------------------------------------------------------------------- */
/* Predict Next Window (GPU with CPU fallback & Probabilities calculation)    */
/* -------------------------------------------------------------------------- */
int nn_predict_next(const char *current_comm, char *predicted_out, size_t n)
{
    if (!s_loaded || !g_experimental || s_n_apps == 0) return 0;

    /* Build input feature vector */
    float input[NN_INPUT_DIM];
    memset(input, 0, sizeof(input));

    /* Feature 0: hashed current comm */
    input[0] = hash_comm(current_comm);

    /* Feature 1: time of day normalised [0,1] */
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    input[1] = (float)(tm->tm_hour * 60 + tm->tm_min) / 1440.0f;

    /* Feature 2: switch rate in last minute, normalised */
    int total = g_switch_count;
    time_t cutoff = now - 60;
    int rate = 0;
    for (int i = 0; i < total; i++) {
        int idx = (g_switch_head - 1 - i + SWITCH_HISTORY_MAX) % SWITCH_HISTORY_MAX;
        if (g_switch_history[idx].timestamp < cutoff) break;
        rate++;
    }
    input[2] = (float)rate / 60.0f;

    /* Features 3..15: recent switch history (hashed comms) */
    for (int i = 0; i < 13 && i < total; i++) {
        int idx = (g_switch_head - 1 - i + SWITCH_HISTORY_MAX) % SWITCH_HISTORY_MAX;
        input[3 + i] = hash_comm(g_switch_history[idx].to_comm);
    }

    /* Compute activations for backpropagation cache */
    for (int i = 0; i < NN_HIDDEN1; i++) {
        float sum = s_b1[i];
        for (int j = 0; j < NN_INPUT_DIM; j++)
            sum += s_W1[i * NN_INPUT_DIM + j] * input[j];
        s_last_z1[i] = sum;
        s_last_h1[i] = sum > 0.0f ? sum : 0.0f;
    }

    for (int i = 0; i < NN_HIDDEN2; i++) {
        float sum = s_b2[i];
        for (int j = 0; j < NN_HIDDEN1; j++)
            sum += s_W2[i * NN_HIDDEN1 + j] * s_last_h1[j];
        s_last_z2[i] = sum;
        s_last_h2[i] = sum > 0.0f ? sum : 0.0f;
    }

    float out[NN_OUTPUT_DIM];
    memset(out, 0, sizeof(out));

    /* Execute forward pass on GPU (fallback to CPU if needed) */
    if (nn_forward_gpu(input, out) != 0) {
        nn_forward_cpu(input, out);
    }

    /* Compute Softmax probabilities */
    float max_val = -1e30f;
    for (int i = 0; i < s_n_apps; i++) {
        if (out[i] > max_val) max_val = out[i];
    }
    float sum_exp = 0.0f;
    for (int i = 0; i < s_n_apps; i++) {
        s_last_probs[i] = expf(out[i] - max_val);
        sum_exp += s_last_probs[i];
    }
    if (sum_exp > 1e-12f) {
        for (int i = 0; i < s_n_apps; i++) {
            s_last_probs[i] /= sum_exp;
        }
    }

    /* Sort candidate apps by probability */
    typedef struct { int idx; float prob; } Candidate;
    Candidate cands[NN_OUTPUT_DIM];
    for (int i = 0; i < s_n_apps; i++) {
        cands[i].idx = i;
        cands[i].prob = s_last_probs[i];
    }
    for (int i = 0; i < s_n_apps - 1; i++) {
        for (int j = i + 1; j < s_n_apps; j++) {
            if (cands[j].prob > cands[i].prob) {
                Candidate tmp = cands[i];
                cands[i] = cands[j];
                cands[j] = tmp;
            }
        }
    }

    /* Update global top predictions for UI/TUI/GUI */
    g_nn_state.count = s_n_apps < 5 ? s_n_apps : 5;
    for (int i = 0; i < g_nn_state.count; i++) {
        strncpy(g_nn_state.top[i].comm, s_app_names[cands[i].idx], 63);
        g_nn_state.top[i].comm[63] = '\0';
        g_nn_state.top[i].prob = cands[i].prob;
    }

    /* Pick top predicted application (skip same comm if alternatives exist) */
    int best = cands[0].idx;
    if (strcmp(s_app_names[best], current_comm) == 0 && s_n_apps > 1) {
        best = cands[1].idx;
    }

    strncpy(predicted_out, s_app_names[best], n - 1);
    predicted_out[n - 1] = '\0';

    strncpy(g_nn_state.last_predicted, s_app_names[best], sizeof(g_nn_state.last_predicted) - 1);
    g_nn_state.last_predicted[sizeof(g_nn_state.last_predicted) - 1] = '\0';
    g_nn_state.last_conf = s_last_probs[best];

    strncpy(g_predicted_comm, s_app_names[best], sizeof(g_predicted_comm) - 1);
    g_predicted_comm[sizeof(g_predicted_comm) - 1] = '\0';

    s_last_predicted_idx = best;
    memcpy(s_last_input, input, sizeof(input));
    s_has_last_pred = 1;

    /* Log prediction & probabilities once */
    if (!g_tui_mode && !g_gui_mode) {
        char prob_buf[512] = "";
        int ppos = 0;
        for (int i = 0; i < g_nn_state.count && i < 3; i++) {
            ppos += snprintf(prob_buf + ppos, sizeof(prob_buf) - ppos,
                             "%d. %s (%.1f%%)  ",
                             i + 1, g_nn_state.top[i].comm, g_nn_state.top[i].prob * 100.0f);
        }
        printf("[neural_predict] Next Target: %s | Top-3: %s\n", predicted_out, prob_buf);
        fflush(stdout);
    }

    return 1;
}

/* -------------------------------------------------------------------------- */
/* Reinforcement Learning Feedback & Online Self-Correction                   */
/* -------------------------------------------------------------------------- */
void nn_feedback(const char *actual_comm)
{
    if (!s_loaded || !g_experimental || !s_has_last_pred || s_n_apps == 0) return;
    s_has_last_pred = 0;

    int target_idx = -1;
    for (int i = 0; i < s_n_apps; i++) {
        if (strcmp(s_app_names[i], actual_comm) == 0) {
            target_idx = i;
            break;
        }
    }

    /* Register newly observed application if space permits */
    if (target_idx < 0 && s_n_apps < NN_OUTPUT_DIM) {
        target_idx = s_n_apps;
        strncpy(s_app_names[target_idx], actual_comm, 63);
        s_app_names[target_idx][63] = '\0';
        s_n_apps++;
    }

    if (target_idx < 0) return;

    int correct = (target_idx == s_last_predicted_idx);
    float reward = correct ? 1.0f : -1.0f;

    g_nn_state.total_predictions++;
    if (correct) g_nn_state.total_correct++;

    g_nn_state.last_reward = correct ? 1 : -1;
    strncpy(g_nn_state.last_actual, actual_comm, sizeof(g_nn_state.last_actual) - 1);
    g_nn_state.last_actual[sizeof(g_nn_state.last_actual) - 1] = '\0';

    /* Cross-Entropy Loss */
    float p_target = s_last_probs[target_idx];
    float loss = -logf(p_target > 1e-12f ? p_target : 1e-12f);
    g_nn_state.last_loss = loss;

    /* Online SGD Gradient Backpropagation */
    const float lr = 0.02f;

    /* dz3 = probs - 1(y == target) */
    float dz3[NN_OUTPUT_DIM];
    memset(dz3, 0, sizeof(dz3));
    for (int i = 0; i < s_n_apps; i++) {
        dz3[i] = s_last_probs[i] - (i == target_idx ? 1.0f : 0.0f);
    }

    /* Layer 2 backprop: dh2 = W3^T * dz3 */
    float dh2[NN_HIDDEN2];
    for (int j = 0; j < NN_HIDDEN2; j++) {
        float sum = 0.0f;
        for (int i = 0; i < s_n_apps; i++) {
            sum += dz3[i] * s_W3[i * NN_HIDDEN2 + j];
        }
        dh2[j] = sum;
    }

    /* dz2 = dh2 * (z2 > 0) */
    float dz2[NN_HIDDEN2];
    for (int j = 0; j < NN_HIDDEN2; j++) {
        dz2[j] = (s_last_z2[j] > 0.0f) ? dh2[j] : 0.0f;
    }

    /* Layer 1 backprop: dh1 = W2^T * dz2 */
    float dh1[NN_HIDDEN1];
    for (int j = 0; j < NN_HIDDEN1; j++) {
        float sum = 0.0f;
        for (int i = 0; i < NN_HIDDEN2; i++) {
            sum += dz2[i] * s_W2[i * NN_HIDDEN1 + j];
        }
        dh1[j] = sum;
    }

    /* dz1 = dh1 * (z1 > 0) */
    float dz1[NN_HIDDEN1];
    for (int j = 0; j < NN_HIDDEN1; j++) {
        dz1[j] = (s_last_z1[j] > 0.0f) ? dh1[j] : 0.0f;
    }

    /* Update Layer 3: W3, b3 */
    for (int i = 0; i < s_n_apps; i++) {
        s_b3[i] -= lr * dz3[i];
        for (int j = 0; j < NN_HIDDEN2; j++) {
            s_W3[i * NN_HIDDEN2 + j] -= lr * dz3[i] * s_last_h2[j];
        }
    }

    /* Update Layer 2: W2, b2 */
    for (int i = 0; i < NN_HIDDEN2; i++) {
        s_b2[i] -= lr * dz2[i];
        for (int j = 0; j < NN_HIDDEN1; j++) {
            s_W2[i * NN_HIDDEN1 + j] -= lr * dz2[i] * s_last_h1[j];
        }
    }

    /* Update Layer 1: W1, b1 */
    for (int i = 0; i < NN_HIDDEN1; i++) {
        s_b1[i] -= lr * dz1[i];
        for (int j = 0; j < NN_INPUT_DIM; j++) {
            s_W1[i * NN_INPUT_DIM + j] -= lr * dz1[i] * s_last_input[j];
        }
    }

    /* Sync updated weights to GPU texture memory */
    upload_weights_to_gpu();

    g_nn_state.online_updates++;
    if (g_nn_state.online_updates % 5 == 0) {
        nn_save_weights(MODEL_WEIGHTS_PATH);
    }

    float acc = (float)g_nn_state.total_correct / (float)g_nn_state.total_predictions * 100.0f;
    if (!g_tui_mode && !g_gui_mode) {
        printf("[neural_predict] RL %s: Target='%s' | Pred='%s' (conf=%.1f%%) | Reward=%+.1f | Step Loss=%.4f | Online Acc=%.1f%% (%d/%d)\n",
               correct ? "Reward" : "Correction",
               actual_comm, g_nn_state.last_predicted,
               g_nn_state.last_conf * 100.0f,
               reward, loss, acc,
               g_nn_state.total_correct, g_nn_state.total_predictions);
        fflush(stdout);
    }
}
