#define _DEFAULT_SOURCE
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <png.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "sbtb.h"
#include "config.h"

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

static GLuint default_icon_tex = 0;
static int default_icon_w = 0;
static int default_icon_h = 0;
static unsigned char *default_icon_pixels = NULL;

#define MAX_BLUR_RECTS 128
static XRectangle blur_rects[MAX_BLUR_RECTS];
static int blur_rect_count = 0;
static unsigned long *blur_sent_vals = NULL;
static int blur_sent_n = -1;

#define ANIM_MS 180
#define ANIM_DURATION (ANIM_MS / 1000.0)
static float slide = 0.0f;
static float slide_from = 0.0f;
static float anim_target = 0.0f;
static double anim_start = 0.0;
static int animating = 0;

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

static int has_compositor(Display *d, int scr) {
    char name[64];
    snprintf(name, sizeof(name), "_NET_WM_CM_S%d", scr);
    Atom a = XInternAtom(d, name, False);
    return XGetSelectionOwner(d, a) != None;
}

static int exe_dir(char *buf, size_t n) {
    char link[4096];
    ssize_t r = readlink("/proc/self/exe", link, sizeof(link) - 1);
    if (r <= 0)
        return -1;
    link[r] = '\0';
    char *slash = strrchr(link, '/');
    if (!slash)
        return -1;
    snprintf(buf, n, "%.*s", (int)(slash - link), link);
    return 0;
}

static const char *find_default_icon_path(void) {
    const char *env = getenv("SBTB_ICON");
    if (env && *env) {
        struct stat st;
        if (stat(env, &st) == 0 && S_ISREG(st.st_mode))
            return env;
    }

    char buf[4096];
    if (exe_dir(buf, sizeof(buf)) == 0) {
        static char p1[8200];
        snprintf(p1, sizeof(p1), "%s/dicon.png", buf);
        struct stat st;
        if (stat(p1, &st) == 0 && S_ISREG(st.st_mode))
            return p1;
    }

    struct stat st;
    if (stat("src/dicon.png", &st) == 0 && S_ISREG(st.st_mode))
        return "src/dicon.png";
    if (stat("dicon.png", &st) == 0 && S_ISREG(st.st_mode))
        return "dicon.png";

    return NULL;
}

static unsigned char *load_png_file(const char *path, int *out_w, int *out_h) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;

    unsigned char sig[8];
    if (fread(sig, 1, 8, f) != 8 || !png_check_sig(sig, 8)) {
        fclose(f);
        return NULL;
    }

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop info = png_create_info_struct(png);
    if (!png || !info) {
        if (png) png_destroy_read_struct(&png, NULL, NULL);
        fclose(f);
        return NULL;
    }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(f);
        return NULL;
    }

    png_init_io(png, f);
    png_set_sig_bytes(png, 8);
    png_read_info(png, info);

    png_uint_32 w = png_get_image_width(png, info);
    png_uint_32 h = png_get_image_height(png, info);
    png_byte ct = png_get_color_type(png, info);
    png_byte bd = png_get_bit_depth(png, info);

    if (ct == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png);
    if (bd == 16)
        png_set_strip_16(png);
    if (ct == PNG_COLOR_TYPE_RGB || ct == PNG_COLOR_TYPE_GRAY)
        png_set_filler(png, 0xff, PNG_FILLER_AFTER);
    if (ct == PNG_COLOR_TYPE_GRAY || ct == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);

    png_read_update_info(png, info);

    unsigned char *buf = malloc((size_t)w * h * 4);
    png_bytep *rows = malloc(sizeof(png_bytep) * h);
    for (png_uint_32 y = 0; y < h; y++)
        rows[y] = buf + (size_t)y * w * 4;
    png_read_image(png, rows);
    free(rows);
    png_read_end(png, NULL);
    png_destroy_read_struct(&png, &info, NULL);
    fclose(f);

    *out_w = (int)w;
    *out_h = (int)h;
    return buf;
}

static int load_default_icon(void) {
    const char *path = find_default_icon_path();
    if (!path) {
        fprintf(stderr, "sbtb: warning: no dicon.png found, icons disabled\n");
        return 0;
    }
    default_icon_pixels = load_png_file(path, &default_icon_w, &default_icon_h);
    if (!default_icon_pixels) {
        fprintf(stderr, "sbtb: warning: failed to load %s\n", path);
        return 0;
    }

    glGenTextures(1, &default_icon_tex);
    glBindTexture(GL_TEXTURE_2D, default_icon_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, default_icon_w, default_icon_h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, default_icon_pixels);
    return 1;
}

static int get_wm_icon(Display *d, Window w, unsigned char **out, int *out_w, int *out_h) {
    Atom prop = XInternAtom(d, "_NET_WM_ICON", False);
    Atom type = None;
    int fmt = 0;
    unsigned long n = 0, rem = 0;
    unsigned char *data = NULL;

    if (XGetWindowProperty(d, w, prop, 0, 0xffffff, False, XA_CARDINAL,
        &type, &fmt, &n, &rem, &data) != Success || fmt != 32 || n < 2) {
        if (data) XFree(data);
        return 0;
    }

    unsigned long *l = (unsigned long *)data;
    unsigned long pos = 0;
    long best_w = -1, best_h = -1, best_area = -1;
    unsigned long best_off = 0;

    while (pos + 2 <= n) {
        long iw = (long)(l[pos] & 0xffffffffUL);
        long ih = (long)(l[pos + 1] & 0xffffffffUL);
        pos += 2;
        if (iw < 1 || ih < 1 || iw > 2048 || ih > 2048)
            break;
        unsigned long npix = (unsigned long)iw * ih;
        if (pos + npix > n)
            break;
        long area = (long)npix;
        if (area > best_area) {
            best_area = area;
            best_w = iw;
            best_h = ih;
            best_off = pos;
        }
        pos += npix;
    }

    if (best_w < 0) {
        XFree(data);
        return 0;
    }

    unsigned char *buf = malloc((size_t)best_w * best_h * 4);
    for (long y = 0; y < best_h; y++) {
        for (long x = 0; x < best_w; x++) {
            unsigned long v = l[best_off + (unsigned long)(y * best_w + x)] & 0xffffffffUL;
            int i = (int)(y * best_w + x) * 4;
            buf[i + 0] = (v >> 16) & 0xff;
            buf[i + 1] = (v >> 8) & 0xff;
            buf[i + 2] = v & 0xff;
            buf[i + 3] = (v >> 24) & 0xff;
        }
    }

    XFree(data);
    *out = buf;
    *out_w = (int)best_w;
    *out_h = (int)best_h;
    return 1;
}

static void set_swap_interval(Display *d, GLXDrawable w) {
    PFNGLXSWAPINTERVALEXTPROC swap =
        (PFNGLXSWAPINTERVALEXTPROC)glXGetProcAddressARB((const GLubyte *)"glXSwapIntervalEXT");
    if (swap)
        swap(d, w, 1);
}

static void blur_prop_set(Client *c, unsigned long *vals, int n) {
    Atom a1 = XInternAtom(c->d, "_KDE_NET_WM_BLUR_BEHIND_REGION", False);
    Atom a2 = XInternAtom(c->d, "_NET_WM_BLUR_BEHIND_REGION", False);
    XChangeProperty(c->d, c->w, a1, XA_CARDINAL, 32, PropModeReplace, (const unsigned char *)vals, n);
    XChangeProperty(c->d, c->w, a2, XA_CARDINAL, 32, PropModeReplace, (const unsigned char *)vals, n);
}

static void blur_prop_clear(Client *c) {
    Atom a1 = XInternAtom(c->d, "_KDE_NET_WM_BLUR_BEHIND_REGION", False);
    Atom a2 = XInternAtom(c->d, "_NET_WM_BLUR_BEHIND_REGION", False);
    XDeleteProperty(c->d, c->w, a1);
    XDeleteProperty(c->d, c->w, a2);
}

static unsigned long *blur_snapshot(unsigned long *vals, int n) {
    unsigned long *copy = malloc((size_t)n * sizeof(unsigned long));
    if (!copy)
        return NULL;
    memcpy(copy, vals, (size_t)n * sizeof(unsigned long));
    return copy;
}

static void update_blur_region(Client *c) {
    if (!c->compositing) {
        if (blur_sent_n >= 0) {
            blur_prop_clear(c);
            free(blur_sent_vals);
            blur_sent_vals = NULL;
            blur_sent_n = -1;
        }
        return;
    }

    int n;
    unsigned long vals[MAX_BLUR_RECTS * 4 + 1];
    if (blur_rect_count == 0) {
        n = 1;
        vals[0] = 0;
    } else {
        n = blur_rect_count * 4;
        for (int i = 0; i < blur_rect_count; i++) {
            vals[i * 4 + 0] = (unsigned long)blur_rects[i].x;
            vals[i * 4 + 1] = (unsigned long)blur_rects[i].y;
            vals[i * 4 + 2] = (unsigned long)blur_rects[i].width;
            vals[i * 4 + 3] = (unsigned long)blur_rects[i].height;
        }
    }

    if (blur_sent_n == n && blur_sent_vals &&
        memcmp(blur_sent_vals, vals, (size_t)n * sizeof(unsigned long)) == 0)
        return;

    blur_prop_set(c, vals, n);
    unsigned long *copy = blur_snapshot(vals, n);
    if (copy) {
        free(blur_sent_vals);
        blur_sent_vals = copy;
        blur_sent_n = n;
    }
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

static GLXFBConfig fbconfig_for_visual(Display *d, int scr, VisualID vid) {
    int n = 0;
    GLXFBConfig *cfs = glXGetFBConfigs(d, scr, &n);
    if (!cfs)
        return NULL;
    GLXFBConfig best = NULL;
    for (int i = 0; i < n; i++) {
        int vi = 0;
        glXGetFBConfigAttrib(d, cfs[i], GLX_VISUAL_ID, &vi);
        if ((VisualID)vi == vid) {
            best = cfs[i];
            break;
        }
    }
    XFree(cfs);
    return best;
}

void initx(Client *c) {
    c->d = XOpenDisplay(NULL);
    if (!c->d)
        die("cannot open display");

    int ev, err;
    if (!XCompositeQueryExtension(c->d, &ev, &err))
        die("XComposite extension unavailable (needed for client thumbnails)");

    XSetErrorHandler(xerror);

    c->scr = DefaultScreen(c->d);
    c->root = RootWindow(c->d, c->scr);
    c->compositing = has_compositor(c->d, c->scr);
    c->argb = 0;
    c->fbconfig = NULL;

    int mw = DisplayWidth(c->d, c->scr);
    int mh = DisplayHeight(c->d, c->scr);

    c->running = 1;

    XkbSetDetectableAutoRepeat(c->d, True, NULL);

    c->argb = 0;
    c->fbconfig = NULL;

    {
        XVisualInfo vis;

        /* Prefer a true 32-bit ARGB visual. Some drivers report a 24-bit
           visual with GLX_ALPHA_SIZE 8, which has no real alpha buffer and
           shows as an opaque black background under a compositor. */
        if (XMatchVisualInfo(c->d, c->scr, 32, TrueColor, &vis) ||
            XMatchVisualInfo(c->d, c->scr, 32, DirectColor, &vis)) {
            c->vi = malloc(sizeof(XVisualInfo));
            if (c->vi)
                *c->vi = vis;
            c->fbconfig = fbconfig_for_visual(c->d, c->scr, XVisualIDFromVisual(vis.visual));
            if (c->fbconfig && c->vi)
                c->argb = 1;
        }
    }
    if (!c->argb || !c->vi) {
        static int legacy[] = {
            GLX_RGBA,
            GLX_DOUBLEBUFFER,
            GLX_RED_SIZE, 8,
            GLX_GREEN_SIZE, 8,
            GLX_BLUE_SIZE, 8,
            None
        };
        if (c->vi)
            free(c->vi);
        c->vi = glXChooseVisual(c->d, c->scr, legacy);
        if (!c->vi)
            die("no suitable GLX visual");
        c->argb = 0;
    }

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

    c->width = mw;
    c->height = mh;

    c->vis = c->vi->visual;
    c->cmap = XCreateColormap(c->d, c->root, c->vis, AllocNone);

    c->attrs.override_redirect = True;
    c->attrs.colormap = c->cmap;
    c->attrs.background_pixmap = None;
    c->attrs.background_pixel = 0;
    c->attrs.border_pixel = 0;
    c->attrs.bit_gravity = StaticGravity;
    c->attrs.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask | StructureNotifyMask;

    c->w = XCreateWindow(c->d, c->root, mx + (mw - c->width) / 2, my + (mh - c->height) / 2, c->width, c->height, 0, c->vi->depth,
                      InputOutput, c->vis,
                      CWOverrideRedirect | CWColormap | CWBackPixmap | CWBorderPixel | CWBitGravity | CWEventMask | CWBackPixel, &c->attrs);

    XStoreName(c->d, c->w, "sbtb");

    {
        static const char class_name[] = "sbtb\0sbtb";
        Atom wmclass = XInternAtom(c->d, "WM_CLASS", False);
        XChangeProperty(c->d, c->w, wmclass, XA_STRING, 8, PropModeReplace,
                        (const unsigned char *)class_name, sizeof(class_name));
    }

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

    if (c->fbconfig) {
        c->glc = glXCreateNewContext(c->d, c->fbconfig, GLX_RGBA_TYPE, NULL, True);
        if (!c->glc)
            die("cannot create GLX context");
    } else {
        c->glc = glXCreateContext(c->d, c->vi, NULL, GL_TRUE);
        if (!c->glc)
            die("cannot create GLX context");
    }

    /* Let pointer events pass through; we only interact via the keyboard grab */
    XShapeCombineRectangles(c->d, c->w, ShapeInput, 0, 0, NULL, 0, ShapeSet, Unsorted);

    XMapWindow(c->d, c->w);

    XEvent xe;
    do {
        XNextEvent(c->d, &xe);
    } while (xe.type != MapNotify || xe.xmap.window != c->w);

    XSetInputFocus(c->d, c->w, RevertToParent, CurrentTime);

    int grabbed = 0;
    for (int i = 0; i < 200; i++) {
        if (XGrabKeyboard(c->d, c->w, True, GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess) {
            grabbed = 1;
            break;
        }
        struct timespec ts = { 0, 5000000 }; /* 5ms */
        nanosleep(&ts, NULL);
    }
    if (!grabbed)
        fprintf(stderr, "sbtb: failed to grab keyboard after retries\n");

    /* WM may have reasserted focus elsewhere while we were retrying */
    XSetInputFocus(c->d, c->w, RevertToParent, CurrentTime);

    if (c->fbconfig)
        glXMakeContextCurrent(c->d, c->w, c->w, c->glc);
    else
        glXMakeCurrent(c->d, c->w, c->glc);

    set_swap_interval(c->d, c->w);

    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if (c->compositing)
        glClearColor(0, 0, 0, 0);
    else
        glClearColor(0, 0, 0, 1);

    c->font = XftFontOpenName(c->d, c->scr, "monospace-15");
    if (!c->font)
        die("cannot load font");

    XRenderColor rc = { 0xffff, 0xffff, 0xffff, 0xffff };
    XftColorAllocValue(c->d, c->vis, c->cmap, &rc, &c->xftcolor);

    load_default_icon();

    resize(c, c->width, c->height);
}

void nav_left(Client *c) {
    (void)c;
    if (selected > 0) {
        selected--;
        dirty = 1;
    }
}

void nav_right(Client *c) {
    (void)c;
    if (selected + 1 < item_count) {
        selected++;
        dirty = 1;
    }
}

void nav_up(Client *c) {
    (void)c;
    nav_prev(c);
}

void nav_down(Client *c) {
    (void)c;
    nav_next(c);
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
    if (!c->compositing)
        return NULL;

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

        WinItem *it = &items[item_count];
        it->win = wins[i];
        it->pixels = NULL;
        it->w = 0;
        it->h = 0;
        it->src_w = 0;
        it->src_h = 0;
        it->tex = 0;
        it->icon_pixels = NULL;
        it->icon_w = 0;
        it->icon_h = 0;
        it->use_default_icon = 0;
        it->label_tex = 0;
        it->ready = 0;
        it->uploaded = 0;

        if (lc.compositing) {
            int w, h, sw, sh;
            unsigned char *px = capture_thumb(&lc, wins[i], &w, &h, &sw, &sh);
            if (px) {
                it->pixels = px;
                it->w = w;
                it->h = h;
                it->src_w = sw;
                it->src_h = sh;
            } else {
                fprintf(stderr, "sbtb: capture failed for 0x%08x, falling back to icon\n", (unsigned int)wins[i]);
            }
        }

        unsigned char *ip = NULL;
        int iw, ih;
        if (get_wm_icon(ld, wins[i], &ip, &iw, &ih)) {
            it->icon_pixels = ip;
            it->icon_w = iw;
            it->icon_h = ih;
        } else {
            it->use_default_icon = 1;
        }

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
            if (it->pixels) {
                glGenTextures(1, &it->tex);
                glBindTexture(GL_TEXTURE_2D, it->tex);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, it->w, it->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, it->pixels);
                free(it->pixels);
                it->pixels = NULL;
            }

            if (it->icon_pixels) {
                glGenTextures(1, &it->icon_tex);
                glBindTexture(GL_TEXTURE_2D, it->icon_tex);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, it->icon_w, it->icon_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, it->icon_pixels);
                free(it->icon_pixels);
                it->icon_pixels = NULL;
            } else if (it->use_default_icon) {
                it->icon_tex = default_icon_tex;
                it->icon_w = default_icon_w;
                it->icon_h = default_icon_h;
            }

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

static int carousel_target(Client *c) {
    int cx = c->width / 2;
    int content_w = item_count * CELL_W;
    int margin = CELL_PAD_X / 2;
    int offset = cx - (selected * CELL_W + CELL_W / 2);

    if (content_w <= c->width) {
        if (offset < margin)
            offset = margin;
        if (offset > c->width - margin - content_w)
            offset = c->width - margin - content_w;
    } else {
        if (offset > margin)
            offset = margin;
        int max_off = c->width - margin - content_w;
        if (offset < max_off)
            offset = max_off;
    }
    return offset;
}

void draw_windows(Client *c) {
    blur_rect_count = 0;

    int cy = c->height / 2;
    int cx = c->width / 2;
    int half = c->width / 2;
    if (half < 1) half = 1;
    int offset = (int)slide;

    for (int i = 0; i < item_count; i++) {
        if (!items[i].uploaded)
            continue;

        GLuint tex = 0;
        int iw = 0, ih = 0;
        if (c->compositing && items[i].tex) {
            tex = items[i].tex;
            iw = items[i].w;
            ih = items[i].h;
        } else {
            tex = items[i].icon_tex;
            iw = items[i].icon_w;
            ih = items[i].icon_h;
        }
        if (!tex || iw < 1 || ih < 1)
            continue;

        int ix = offset + i * CELL_W;
        int iy = cy - CELL_H / 2;

        int xc = ix + CELL_W / 2;
        int dx = xc > cx ? xc - cx : cx - xc;
        float s = 1.0f - 0.30f * ((float)dx / half);
        if (s < 0.70f)
            s = 0.70f;

        float fit = (float)THUMB_MAX / iw;
        float fit_h = (float)THUMB_MAX / ih;
        if (fit_h < fit)
            fit = fit_h;
        if (fit > 1)
            fit = 1;

        int sw = (int)(iw * fit * s);
        int sh = (int)(ih * fit * s);
        int lw = (int)(items[i].label_w * s);
        int lh = (int)(items[i].label_h * s);
        if (lw > sw) {
            float scale = (float)sw / lw;
            lw = sw;
            lh = (int)(lh * scale);
        }

        int tx = ix + (CELL_W - sw) / 2;
        int total_h = sh + 4 + lh;
        int ty = iy + (CELL_H - total_h) / 2;
        int lx = ix + (CELL_W - lw) / 2;
        int ly = ty + sh / 1.1;

        glColor4f(1, 1, 1, 1);
        draw_quad(tex, tx, ty, sw, sh);

        if (blur_rect_count + 2 <= MAX_BLUR_RECTS) {
            blur_rects[blur_rect_count].x = tx;
            blur_rects[blur_rect_count].y = ty;
            blur_rects[blur_rect_count].width = sw;
            blur_rects[blur_rect_count].height = sh;
            blur_rect_count++;

            blur_rects[blur_rect_count].x = lx;
            blur_rects[blur_rect_count].y = ly;
            blur_rects[blur_rect_count].width = lw;
            blur_rects[blur_rect_count].height = lh;
            blur_rect_count++;
        }

        if (i == selected)
            draw_highlight(tx, ty, sw, sh);
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

void run(Client *c) {
    int selected_init = 0;
    double last_comp_check = 0;

    while (c->running) {
        upload_pending_textures(c);

        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        double now = ts.tv_sec + ts.tv_nsec / 1e9;

        if (now - last_comp_check > 0.25) {
            last_comp_check = now;
            int comp = has_compositor(c->d, c->scr);
            if (comp != c->compositing) {
                c->compositing = comp;
                if (comp)
                    glClearColor(0, 0, 0, 0);
                else
                    glClearColor(0, 0, 0, 1);
                dirty = 1;
            }
        }

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

        int target = carousel_target(c);

        if (loader_done) {
            struct timespec ts2;
            clock_gettime(CLOCK_MONOTONIC, &ts2);
            double now2 = ts2.tv_sec + ts2.tv_nsec / 1e9;

            if (!animating && target != (int)slide) {
                animating = 1;
                slide_from = slide;
                anim_start = now2;
                anim_target = (float)target;
            } else if (animating && target != (int)anim_target) {
                slide_from = slide;
                anim_start = now2;
                anim_target = (float)target;
            }

            if (animating) {
                double t = (now2 - anim_start) / ANIM_DURATION;
                if (t >= 1.0) {
                    slide = anim_target;
                    animating = 0;
                } else {
                    double e = 1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t);
                    slide = slide_from + (anim_target - slide_from) * (float)e;
                }
            } else {
                slide = (float)target;
            }
        } else {
            slide = (float)target;
        }

        if (loader_done && !dirty && !animating) {
            struct timeval tv = { 0, 50000 };
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(ConnectionNumber(c->d), &fds);
            select(ConnectionNumber(c->d) + 1, &fds, NULL, NULL, &tv);
            continue;
        }

        if (dirty || animating) {
            glClear(GL_COLOR_BUFFER_BIT);
            if (item_count > 0)
                draw_windows(c);

            glXSwapBuffers(c->d, c->w);
            update_blur_region(c);

            dirty = 0;
        }

        if (animating) {
            struct timeval tv = { 0, 16000 };
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(ConnectionNumber(c->d), &fds);
            select(ConnectionNumber(c->d) + 1, &fds, NULL, NULL, &tv);
        }
    }
}

void cleanup(Client *c) {
    XUngrabKeyboard(c->d, CurrentTime);

    for (int i = 0; i < item_count; i++) {
        if (items[i].tex) glDeleteTextures(1, &items[i].tex);
        if (items[i].label_tex) glDeleteTextures(1, &items[i].label_tex);
        if (items[i].icon_tex && items[i].icon_tex != default_icon_tex)
            glDeleteTextures(1, &items[i].icon_tex);
        free(items[i].pixels);
        free(items[i].icon_pixels);
    }
    free(items);

    if (default_icon_tex)
        glDeleteTextures(1, &default_icon_tex);
    free(default_icon_pixels);
    free(blur_sent_vals);

    XftColorFree(c->d, c->vis, c->cmap, &c->xftcolor);
    XftFontClose(c->d, c->font);
    if (c->fbconfig)
        glXMakeContextCurrent(c->d, None, None, NULL);
    else
        glXMakeCurrent(c->d, None, NULL);
    glXDestroyContext(c->d, c->glc);
    XDestroyWindow(c->d, c->w);
    XFreeColormap(c->d, c->cmap);
    if (c->vi)
        XFree(c->vi);
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
