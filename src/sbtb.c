#define _DEFAULT_SOURCE
#include <X11/keysym.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/types.h>
#include <unistd.h>

#include "sbtb.h"
#include "config.h"

static GLuint fis = 0;
static int fis_w, fis_h;

static GLuint tim = 0;
static int tim_w, tim_h;

static GLuint sz = 0;
static int sz_w, sz_h;

static GLuint fm = 0;
static int fm_w, fm_h;

#define CELL_PAD_X 20
#define CELL_PAD_Y 40
#define CELL_W (THUMB_MAX + CELL_PAD_X)
#define CELL_H (THUMB_MAX + CELL_PAD_Y)

static WinItem *items;
static int item_count;
static int dirty = 1;

static int selected = 0;

static pthread_t loader_thread;
static volatile int loader_done = 0;

static volatile int expect_err = 0;
static volatile int got_err = 0;

static void die(const char *msg) {
    fprintf(stderr, "%s\n", msg);
    exit(1);
}

static int xerror(Display *d, XErrorEvent *e) {
    if (expect_err) {
        got_err = 1;
        return 0;
    }
    char buf[128];
    XGetErrorText(d, e->error_code, buf, sizeof(buf));
    fprintf(stderr, "sbtb: X error: %s\n", buf);
    return 0;
}

static int lockfd = -1;
static char lockpath[256];
static Client *g_client = NULL;

static void get_lock_path(char *buf, size_t n) {
    const char *dir = getenv("XDG_RUNTIME_DIR");
    if (!dir || !*dir)
        dir = "/tmp";
    snprintf(buf, n, "%s/sbtb.lock", dir);
}

static void release_lock(void) {
    if (lockfd >= 0) {
        flock(lockfd, LOCK_UN);
        close(lockfd);
        lockfd = -1;
    }
}

static void handle_term(int sig) {
    (void)sig;
    if (g_client)
        g_client->running = 0;
}

static void ensure_single_instance(void) {
    get_lock_path(lockpath, sizeof(lockpath));

    for (;;) {
        lockfd = open(lockpath, O_CREAT | O_RDWR, 0600);
        if (lockfd < 0)
            die("cannot open lock file");

        if (flock(lockfd, LOCK_EX | LOCK_NB) == 0)
            break;

        if (errno != EWOULDBLOCK)
            die("flock failed");

        char buf[32] = {0};
        pid_t pid = -1;
        if (read(lockfd, buf, sizeof(buf) - 1) > 0)
            pid = (pid_t)atoi(buf);

        if (pid > 0) {
            kill(pid, SIGTERM);
            for (int i = 0; i < 100; i++) {
                if (kill(pid, 0) != 0)
                    break;
            }
            if (kill(pid, 0) == 0)
                kill(pid, SIGKILL);
        }

        close(lockfd);
        lockfd = -1;
    }

    if (ftruncate(lockfd, 0) != 0)
        fprintf(stderr, "sbtb: warning: ftruncate lock file failed\n");
    char pidbuf[32];
    int len = snprintf(pidbuf, sizeof(pidbuf), "%d\n", getpid());
    lseek(lockfd, 0, SEEK_SET);
    if (write(lockfd, pidbuf, len) != len)
        fprintf(stderr, "sbtb: warning: failed to record pid\n");

    atexit(release_lock);

    struct sigaction sa = {0};
    sa.sa_handler = handle_term;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
}

void resize(Client *c, int w, int h) {
    c->width = w;
    c->height = h;
    glViewport(0, 0, w, h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, w, h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

static int grid_cols(Client *c) {
    int cols = c->width / CELL_W;
    return cols < 1 ? 1 : cols;
}

static int grid_rows(Client *c) {
    int rows = c->height / CELL_H;
    return rows < 1 ? 1 : rows;
}

static int items_page(Client *c) {
    return grid_cols(c) * grid_rows(c);
}

void initx(Client *c) {
    c->d = XOpenDisplay(NULL);
    if (!c->d)
        die("cannot open display");

    int ev, err;
    if (!XCompositeQueryExtension(c->d, &ev, &err))
        die("XComposite extension unavailable, is sbcomp running?");

    XSetErrorHandler(xerror);

    int mw = DisplayWidth(c->d, c->scr);
    int mh = DisplayHeight(c->d, c->scr);

    c->scr = DefaultScreen(c->d);
    c->root = RootWindow(c->d, c->scr);
    c->width = 1100;
    c->height = 500;
    c->running = 1;

    XkbSetDetectableAutoRepeat(c->d, True, NULL);

    int glattr[] = {
        GLX_RGBA,
        GLX_DOUBLEBUFFER,
        GLX_RED_SIZE, 8,
        GLX_GREEN_SIZE, 8,
        GLX_BLUE_SIZE, 8,
        GLX_ALPHA_SIZE, 8,
        None
    };

    int mx = 0, my = 0;
    c->mon = 0;

    int rr_event_base, rr_error_base;
    if (XRRQueryExtension(c->d, &rr_event_base, &rr_error_base)) {
        int rr_major, rr_minor;
        XRRQueryVersion(c->d, &rr_major, &rr_minor);

        int nmon = 0;
        XRRMonitorInfo *info = XRRGetMonitors(c->d, c->root, True, &nmon);
        if (info) {
            Window dw;
            int di;
            unsigned int du;
            int cx, cy;
            XQueryPointer(c->d, c->root, &dw, &dw, &cx, &cy, &di, &di, &du);
            for (int i = 0; i < nmon; i++) {
                if (cx >= info[i].x && cx < info[i].x + info[i].width &&
                    cy >= info[i].y && cy < info[i].y + info[i].height) {
                    mw = info[i].width;
                    mh = info[i].height;
                    mx = info[i].x;
                    my = info[i].y;
                    c->mon = i;
                    break;
                }
            }
            XRRFreeMonitors(info);
        }
    }

    c->vi = glXChooseVisual(c->d, c->scr, glattr);
    if (!c->vi)
        die("no suitable GLX visual");

    c->vis = c->vi->visual;
    c->cmap = XCreateColormap(c->d, c->root, c->vis, AllocNone);

    c->attrs.override_redirect = True;
    c->attrs.colormap = c->cmap;
    c->attrs.background_pixmap = None;
    c->attrs.border_pixel = 0;
    c->attrs.bit_gravity = StaticGravity;
    c->attrs.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask | StructureNotifyMask;

    c->w = XCreateWindow(c->d, c->root, mx + (mw - c->width) / 2, my + (mh - c->height) / 2, c->width, c->height, 0, c->vi->depth,
                      InputOutput, c->vis,
                      CWOverrideRedirect | CWColormap | CWBackPixmap | CWBorderPixel | CWBitGravity | CWEventMask, &c->attrs);

    XStoreName(c->d, c->w, "sbtb");

    c->wmdeletewin = XInternAtom(c->d, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(c->d, c->w, &c->wmdeletewin, 1);

    c->net_active_window = XInternAtom(c->d, "_NET_ACTIVE_WINDOW", False);
    c->netwmname = XInternAtom(c->d, "_NET_WM_NAME", False);
    c->wmnameutf8 = XInternAtom(c->d, "UTF8_STRING", False);
    c->wmname = XInternAtom(c->d, "WM_NAME", False);

    XChangeProperty(c->d, c->w, c->netwmname, c->wmnameutf8, 8,
                PropModeReplace, (const unsigned char *)"sbtb", strlen("sbtb"));
    XChangeProperty(c->d, c->w, c->wmname, c->wmnameutf8, 8,
                PropModeReplace, (const unsigned char *)"sbtb", strlen("sbtb"));

    c->glc = glXCreateContext(c->d, c->vi, NULL, GL_TRUE);
    if (!c->glc)
        die("cannot create GLX context");

    XMapWindow(c->d, c->w);

    XEvent xe;
    do {
        XNextEvent(c->d, &xe);
    } while (xe.type != MapNotify || xe.xmap.window != c->w);

    XSetInputFocus(c->d, c->w, RevertToParent, CurrentTime);

    if (XGrabKeyboard(c->d, c->w, True, GrabModeAsync, GrabModeAsync, CurrentTime) != GrabSuccess)
        fprintf(stderr, "Failed to grab keyboard\n");

    glXMakeCurrent(c->d, c->w, c->glc);

    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(0, 0, 0, 1);

    c->font = XftFontOpenName(c->d, c->scr, "monospace-12");
    if (!c->font)
        die("cannot load font");

    XRenderColor rc = { 0xffff, 0xffff, 0xffff, 0xffff };
    XftColorAllocValue(c->d, c->vis, c->cmap, &rc, &c->xftcolor);

    resize(c, c->width, c->height);
}

void nav_left(Client *c) {
    int cols = grid_cols(c);
    int col = selected % cols;
    if (col > 0) {
        selected--;
        dirty = 1;
    }
}

void nav_right(Client *c) {
    int cols = grid_cols(c);
    int col = selected % cols;
    if (col < cols - 1 && selected + 1 < item_count) {
        selected++;
        dirty = 1;
    }
}

void nav_up(Client *c) {
    int cols = grid_cols(c);
    int row = selected / cols;
    if (row > 0) {
        selected -= cols;
        dirty = 1;
    }
}

void nav_down(Client *c) {
    int cols = grid_cols(c);
    if (selected + cols < item_count) {
        selected += cols;
        dirty = 1;
    }
}

void nav_next(Client *c) {
    (void)c;
    if (item_count == 0)
        return;
    selected = (selected + 1) % item_count;
    dirty = 1;
}

void nav_prev(Client *c) {
    (void)c;
    if (item_count == 0)
        return;
    selected = (selected - 1 + item_count) % item_count;
    dirty = 1;
}

void nav_quit(Client *c) {
    c->running = 0;
}

void nav_select(Client *c) {
    if (item_count == 0) {
        nav_quit(c);
        return;
    }

    Window w = items[selected].win;

    XEvent ev = {0};
    ev.xclient.type = ClientMessage;
    ev.xclient.window = w;
    ev.xclient.message_type = c->net_active_window;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = 2;
    ev.xclient.data.l[1] = CurrentTime;
    ev.xclient.data.l[2] = 0;

    XSendEvent(c->d, c->root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XSync(c->d, False);

    nav_quit(c);
}

void handle_key(Client *c, XKeyEvent *e) {
    KeySym ks = XLookupKeysym(e, 0);

    unsigned int state = e->state &
        (ShiftMask | ControlMask | Mod1Mask | Mod4Mask);

    for (size_t i = 0; i < sizeof(keys)/sizeof(keys[0]); i++) {
        if (ks == keys[i].keysym && state == keys[i].mod) {
            keys[i].func(c);
            return;
        }
    }
}

static int handle_key_release(Client *c, XKeyEvent *e) {
    KeySym ks = XLookupKeysym(e, 0);
    for (size_t i = 0; i < sizeof(release_keys)/sizeof(release_keys[0]); i++) {
        if (ks == release_keys[i]) {
            nav_select(c);
            return 1;
        }
    }
    return 0;
}

void draw_rect(int x, int y, int w, int h, float r, float g, float b, float a) {
    glDisable(GL_TEXTURE_2D);
    glColor4f(r, g, b, a);
    glBegin(GL_QUADS);
        glVertex2i(x, y);
        glVertex2i(x + w, y);
        glVertex2i(x + w, y + h);
        glVertex2i(x, y + h);
    glEnd();
    glEnable(GL_TEXTURE_2D);
}

GLuint make_text_texture(Client *c, const char *text, XGlyphInfo *extents_out) {
    XGlyphInfo ext;
    XftTextExtentsUtf8(c->d, c->font, (const FcChar8 *)text, strlen(text), &ext);

    int w = ext.width > 0 ? ext.width : 1;
    int h = ext.height > 0 ? ext.height : 1;

    Pixmap pm = XCreatePixmap(c->d, c->root, w, h, c->vi->depth);
    GC gc = XCreateGC(c->d, pm, 0, NULL);
    XSetForeground(c->d, gc, 0);
    XFillRectangle(c->d, pm, gc, 0, 0, w, h);

    XftDraw *xd = XftDrawCreate(c->d, pm, c->vis, c->cmap);
    XftDrawStringUtf8(xd, &c->xftcolor, c->font, ext.x, ext.y, (const FcChar8 *)text, strlen(text));

    XImage *img = XGetImage(c->d, pm, 0, 0, w, h, AllPlanes, ZPixmap);

    unsigned char *rgba = malloc((size_t)w * h * 4);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            unsigned long px = XGetPixel(img, x, y);
            unsigned char lum = (px >> 16) & 0xff;
            int i = (y * w + x) * 4;
            rgba[i + 0] = 255;
            rgba[i + 1] = 255;
            rgba[i + 2] = 255;
            rgba[i + 3] = lum;
        }
    }

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);

    free(rgba);
    XDestroyImage(img);
    XftDrawDestroy(xd);
    XFreeGC(c->d, gc);
    XFreePixmap(c->d, pm);

    if (extents_out)
        *extents_out = ext;

    return tex;
}

static Window *get_clients(Display *d, Window root, int *count) {
    Atom prop = XInternAtom(d, "_NET_CLIENT_LIST_STACKING", False);
    Atom type;
    int fmt;
    unsigned long n, rem;
    unsigned char *data = NULL;

    if (XGetWindowProperty(d, root, prop, 0, 4096, False, XA_WINDOW,
        &type, &fmt, &n, &rem, &data) != Success || type != XA_WINDOW || !data || n == 0) {
        if (data) XFree(data);
        *count = 0;
        return NULL;
    }

    Window *src = (Window *)data;
    Window *wins = malloc(n * sizeof(Window));
    for (unsigned long i = 0; i < n; i++)
        wins[i] = src[n - 1 - i];

    XFree(data);
    *count = (int)n;
    return wins;
}

static void get_title(Display *d, Window w, char *buf, size_t bufsz) {
    Atom netname = XInternAtom(d, "_NET_WM_NAME", False);
    Atom utf8 = XInternAtom(d, "UTF8_STRING", False);
    Atom type;
    int fmt;
    unsigned long n, rem;
    unsigned char *data = NULL;

    if (XGetWindowProperty(d, w, netname, 0, 255, False, utf8,
        &type, &fmt, &n, &rem, &data) == Success && data && n > 0) {
        snprintf(buf, bufsz, "%.*s", (int)n, (char *)data);
        XFree(data);
        return;
    }
    if (data) XFree(data);

    char *name = NULL;
    if (XFetchName(d, w, &name) && name) {
        snprintf(buf, bufsz, "%s", name);
        XFree(name);
        return;
    }

    snprintf(buf, bufsz, "(untitled)");
}

static void get_class(Display *d, Window w, char *buf, size_t bufsz) {
    XClassHint ch = {0};
    if (XGetClassHint(d, w, &ch)) {
        snprintf(buf, bufsz, "%s", ch.res_class ? ch.res_class : (ch.res_name ? ch.res_name : "?"));
        if (ch.res_name) XFree(ch.res_name);
        if (ch.res_class) XFree(ch.res_class);
    } else {
        snprintf(buf, bufsz, "?");
    }
}

static int get_win_monitor(Display *d, Window w) {
    Atom prop = XInternAtom(d, "_SBCWM_MONITOR", False);
    Atom type;
    int fmt;
    unsigned long n, rem;
    unsigned char *data = NULL;

    if (XGetWindowProperty(d, w, prop, 0, 1, False, XA_CARDINAL,
        &type, &fmt, &n, &rem, &data) != Success || type != XA_CARDINAL || !data) {
        if (data) XFree(data);
        return -1;
    }

    int mon = (int)(*(unsigned long *)data);
    XFree(data);
    return mon;
}

static unsigned char *capture_thumb(Client *c, Window w, int *out_w, int *out_h, int *src_w, int *src_h) {
    XWindowAttributes wa;
    if (!XGetWindowAttributes(c->d, w, &wa) || wa.map_state != IsViewable)
        return NULL;

    int sw = wa.width, sh = wa.height;
    if (sw < 1 || sh < 1)
        return NULL;

    expect_err = 1;
    got_err = 0;
    Pixmap pm = XCompositeNameWindowPixmap(c->d, w);
    XSync(c->d, False);
    expect_err = 0;
    if (got_err || !pm)
        return NULL;

    XImage *img = XGetImage(c->d, pm, 0, 0, sw, sh, AllPlanes, ZPixmap);
    XFreePixmap(c->d, pm);
    if (!img)
        return NULL;

    float scale_w = (float)THUMB_MAX / sw;
    float scale_h = (float)THUMB_MAX / sh;
    float scale = scale_w < scale_h ? scale_w : scale_h;
    if (scale > 1) scale = 1;

    int w2 = (int)(sw * scale);
    int h2 = (int)(sh * scale);
    if (w2 < 1) w2 = 1;
    if (h2 < 1) h2 = 1;

    unsigned char *buf = malloc((size_t)w2 * h2 * 4);
    for (int y = 0; y < h2; y++) {
        int yy = y * sh / h2;
        for (int x = 0; x < w2; x++) {
            int xx = x * sw / w2;
            unsigned long px = XGetPixel(img, xx, yy);
            int i = (y * w2 + x) * 4;
            buf[i + 0] = (px >> 16) & 0xff;
            buf[i + 1] = (px >> 8) & 0xff;
            buf[i + 2] = px & 0xff;
            buf[i + 3] = 0xff;
        }
    }
    XDestroyImage(img);

    *out_w = w2;
    *out_h = h2;
    *src_w = sw;
    *src_h = sh;
    return buf;
}

static void *loader_main(void *arg) {
    Client *c = (Client *)arg;

    Display *ld = XOpenDisplay(NULL);
    if (!ld) {
        loader_done = 1;
        return NULL;
    }

    int n;
    Window *wins = get_clients(ld, RootWindow(ld, DefaultScreen(ld)), &n);
    if (!wins) {
        XCloseDisplay(ld);
        loader_done = 1;
        return NULL;
    }

    Client lc = *c;
    lc.d = ld;

    items = malloc(n * sizeof(WinItem));
    item_count = 0;

    for (int i = 0; i < n; i++) {
        int wmon = get_win_monitor(ld, wins[i]);
        if (wmon != -1 && wmon != c->mon)
            continue;

        int w, h, sw, sh;
        unsigned char *px = capture_thumb(&lc, wins[i], &w, &h, &sw, &sh);
        if (!px) {
            fprintf(stderr, "sbtb: capture failed for 0x%08x, showing placeholder\n", (unsigned int)wins[i]);
            w = h = THUMB_MAX;
            sw = sh = 0;
            px = malloc((size_t)w * h * 4);
            for (int p = 0; p < w * h; p++) {
                px[p * 4 + 0] = 45;
                px[p * 4 + 1] = 45;
                px[p * 4 + 2] = 52;
                px[p * 4 + 3] = 255;
            }
        }

        WinItem *it = &items[item_count];
        it->win = wins[i];
        it->pixels = px;
        it->w = w;
        it->h = h;
        it->src_w = sw;
        it->src_h = sh;
        it->tex = 0;
        it->label_tex = 0;
        it->uploaded = 0;
        get_title(ld, wins[i], it->title, sizeof(it->title));
        get_class(ld, wins[i], it->class, sizeof(it->class));
        __sync_synchronize();
        it->ready = 1;
        item_count++;
    }

    free(wins);
    XCloseDisplay(ld);
    loader_done = 1;
    return NULL;
}

void load_clients_async(Client *c) {
    pthread_create(&loader_thread, NULL, loader_main, c);
}

void upload_pending_textures(Client *c) {
    for (int i = 0; i < item_count; i++) {
        WinItem *it = &items[i];
        if (it->ready && !it->uploaded) {
            glGenTextures(1, &it->tex);
            glBindTexture(GL_TEXTURE_2D, it->tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, it->w, it->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, it->pixels);

            free(it->pixels);
            it->pixels = NULL;

            XGlyphInfo ext;
            it->label_tex = make_text_texture(c, it->title, &ext);
            it->label_w = ext.width;
            it->label_h = ext.height;

            it->uploaded = 1;
            dirty = 1;
        }
    }
}

void draw_quad(GLuint tex, int x, int y, int w, int h) {
    glBindTexture(GL_TEXTURE_2D, tex);
    glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2i(x, y);
        glTexCoord2f(1, 0); glVertex2i(x + w, y);
        glTexCoord2f(1, 1); glVertex2i(x + w, y + h);
        glTexCoord2f(0, 1); glVertex2i(x, y + h);
    glEnd();
}

void draw_windows(Client *c) {
    int cols = grid_cols(c);
    int per_page = items_page(c);
    int page = selected / per_page;
    int page_start = page * per_page;
    int page_end = page_start + per_page;
    if (page_end > item_count) page_end = item_count;

    for (int i = page_start; i < page_end; i++) {
        if (!items[i].uploaded)
            continue;

        int local = i - page_start;

        int x = (local % cols) * CELL_W + CELL_PAD_X / 2;
        int y = (local / cols) * CELL_H + CELL_PAD_Y / 2;

        int lw = items[i].label_w;
        int lh = items[i].label_h;
        if (lw > items[i].w) {
            float scale = (float)items[i].w / lw;
            lw = items[i].w;
            lh = (int)(lh * scale);
        }

        glColor4f(1, 1, 1, 1);
        draw_quad(items[i].tex, x, y, items[i].w, items[i].h);

        if (i == selected)
            draw_highlight(x, y, items[i].w, items[i].h);

        glColor4f(1, 1, 1, 1);
        draw_quad(items[i].label_tex, x, y + items[i].h + 4, lw, lh);
    }
}

static void draw_highlight(int x, int y, int w, int h) {
    glDisable(GL_TEXTURE_2D);
    glColor4f(1.0f, 0.8f, 0.0f, 1.0f);
    glLineWidth(2.0f);
    glBegin(GL_LINE_LOOP);
        glVertex2i(x - 2, y - 2);
        glVertex2i(x + w + 2, y - 2);
        glVertex2i(x + w + 2, y + h + 2);
        glVertex2i(x - 2, y + h + 2);
    glEnd();
    glEnable(GL_TEXTURE_2D);
}

void draw_text(Client *c, GLuint *ut, const char *msg, int *w, int *h, size_t max_chars) {
    if (*ut) glDeleteTextures(1, ut);
    
    XGlyphInfo ext;
    size_t len = strlen(msg);
    
    if (max_chars > 0 && len > max_chars) {
        char buffer[512];
        
        if (max_chars > sizeof(buffer) - 4) {
            max_chars = sizeof(buffer) - 4;
        }
        
        strncpy(buffer, msg, max_chars);
        strcpy(buffer + max_chars, "...");
        
        *ut = make_text_texture(c, buffer, &ext);
    } else {
        *ut = make_text_texture(c, msg, &ext);
    }

    if (w) *w = ext.width;
    if (h) *h = ext.height;
}

void draw_hud(Client *c) {
    size_t max_c = 35;

    char idx[128];
    char geom[128];

    int y = c->height - 30 - 10;

    snprintf(idx, sizeof(idx), "%d/%d", selected + 1, item_count);
    snprintf(geom, sizeof(geom), "%dx%d", items[selected].src_w, items[selected].src_h);

    draw_text(c, &fis, idx, &fis_w, &fis_h, max_c);
    draw_text(c, &tim, items[selected].title, &tim_w, &tim_h, max_c);
    draw_text(c, &sz, geom, &sz_w, &sz_h, max_c);
    draw_text(c, &fm, items[selected].class, &fm_w, &fm_h, max_c);

    glColor4f(0.0f, 0.0f, 0.0f, 1.0f);

    if (IDX)
        draw_quad(fis, 10, y, fis_w, fis_h);

    if (TITLE)
        draw_quad(tim, c->width - tim_w - 10, y, tim_w, tim_h);

    if (CLASS)
        draw_quad(fm, (c->width / 2) - (sz_w / 2) + sz_w - 30, y, fm_w, fm_h);

    if (GEOM)
        draw_quad(sz, (c->width / 2) - (sz_w / 2) - 40, y, sz_w, sz_h);
}

void run(Client *c) {
    int selected_init = 0;

    while (c->running) {
        upload_pending_textures(c);

        if (loader_done && !selected_init) {
            selected = item_count > 1 ? 1 : 0;
            selected_init = 1;
            dirty = 1;
        }

        if (!loader_done) {
            struct timeval tv = { 0, 16000 };
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(ConnectionNumber(c->d), &fds);
            select(ConnectionNumber(c->d) + 1, &fds, NULL, NULL, &tv);
        }

        while (XPending(c->d)) {
            XEvent e;
            XNextEvent(c->d, &e);
            switch (e.type) {
            case ConfigureNotify:
                if (e.xconfigure.width != c->width || e.xconfigure.height != c->height) {
                    resize(c, e.xconfigure.width, e.xconfigure.height);
                    dirty = 1;
                }
                break;
            case Expose:
                dirty = 1;
                break;
            case ClientMessage:
                if ((Atom)e.xclient.data.l[0] == c->wmdeletewin)
                    c->running = 0;
                break;
            case KeyPress:
                handle_key(c, &e.xkey);
                dirty = 1;
                break;
            case KeyRelease:
                handle_key_release(c, &e.xkey);
                break;
            }
        }

        if (!c->running)
            break;

        if (loader_done && !dirty) {
            struct timeval tv = { 0, 50000 }; 
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(ConnectionNumber(c->d), &fds);
            select(ConnectionNumber(c->d) + 1, &fds, NULL, NULL, &tv);
            continue;
        }

        if (dirty) {
            glClear(GL_COLOR_BUFFER_BIT);
            if (item_count > 0)
                draw_windows(c);

            int y = c->height - 45;

            draw_rect(0, y, c->width, 25, 1, 1, 1, 1);
            if (item_count > 0)
                draw_hud(c);

            glXSwapBuffers(c->d, c->w);
            dirty = 0;
        }
    }
}

void cleanup(Client *c) {
    for (int i = 0; i < item_count; i++) {
        if (items[i].tex) glDeleteTextures(1, &items[i].tex);
        if (items[i].label_tex) glDeleteTextures(1, &items[i].label_tex);
        free(items[i].pixels);
    }
    free(items);

    XftColorFree(c->d, c->vis, c->cmap, &c->xftcolor);
    XftFontClose(c->d, c->font);
    glXMakeCurrent(c->d, None, NULL);
    glXDestroyContext(c->d, c->glc);
    XDestroyWindow(c->d, c->w);
    XFreeColormap(c->d, c->cmap);
    XCloseDisplay(c->d);
}

int main(void) {
    Client c = {0};
    g_client = &c;

    ensure_single_instance();

    initx(&c);
    load_clients_async(&c);
    run(&c);
    pthread_join(loader_thread, NULL);
    cleanup(&c);
    return 0;
}
