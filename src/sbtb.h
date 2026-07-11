#pragma once
#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <X11/extensions/Xinerama.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/Xft/Xft.h>
#include <pthread.h>
#include <GL/gl.h>
#include <GL/glx.h>

#include <stdint.h>

typedef XftDraw *Draw;
typedef XftColor *Color;

typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;

typedef unsigned short int uint16;
typedef unsigned int uint32;
typedef unsigned long long int uint64;

typedef struct Client Client;

struct Client {
    Display *d;
    int scr;
    Colormap cmap;
    Window w, root;
    Drawable buf;
    Pixmap pix;
    Atom wmdeletewin, wmname, netwmname, wmnameutf8, net_active_window;
    Draw draw;
    Visual *vis;
    XSetWindowAttributes attrs;
    int width, height;

    XVisualInfo *vi;
    GLXContext glc;
    XftFont *font;
    XftColor xftcolor;
    int running;
};

typedef struct {
  Color *col;
  size_t collen;
  Font font;
  GC gc;
} DC;

typedef struct {
    Window win;
    GLuint tex;
    int w, h;
    int src_w, src_h;
    unsigned char *pixels;
    int ready;
    int uploaded;
    GLuint label_tex;
    int label_w, label_h;
    char title[256];
    char class[128];
} WinItem;

typedef struct {
    unsigned int mod;
    KeySym keysym;
    void (*func)(Client *c);
} Key;

void initx(Client *c);
void run(Client *c);
void cleanup(Client *c);
void resize(Client *c, int w, int h);
GLuint make_text_texture(Client *c, const char *text, XGlyphInfo *extents_out);
void draw_quad(GLuint tex, int x, int y, int w, int h);
static int grid_cols(Client *c);
static int grid_rows(Client *c);
static int items_page(Client *c);

static Window *get_clients(Display *d, Window root, int *count);
static void get_title(Display *d, Window w, char *buf, size_t bufsz);
static void get_class(Display *d, Window w, char *buf, size_t bufsz);
static unsigned char *capture_thumb(Client *c, Window w, int *out_w, int *out_h, int *src_w, int *src_h);
void load_clients_async(Client *c);
void draw_windows(Client *c);
void upload_pending_textures(Client *c);
static void draw_highlight(int x, int y, int w, int h);

void nav_left(Client *c);
void nav_right(Client *c);
void nav_up(Client *c);
void nav_down(Client *c);
void nav_next(Client *c);
void nav_prev(Client *c);
void nav_select(Client *c);
void nav_quit(Client *c);
void handle_key(Client *c, XKeyEvent *e);

void draw_rect(int x, int y, int w, int h, float r, float g, float b, float a);
void draw_text(Client *c, GLuint *ut, const char *msg, int *w, int *h);
void draw_hud(Client *c);

static void *loader_main(void *arg);
