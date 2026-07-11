#include <X11/Xlib.h>
#include <X11/X.h>
#include <X11/keysym.h>

#define THUMB_MAX 200

#define TITLE 1
#define CLASS 1
#define GEOM 1
#define IDX 1

#define SWITCHMOD Mod1Mask

static const KeySym release_keys[] = { XK_Alt_L, XK_Alt_R };

#define MODKEY ControlMask
static const Key keys[] = {
    { 0,                    XK_h,      nav_left   },
    { 0,                    XK_Left,   nav_left   },
    { 0,                    XK_l,      nav_right  },
    { 0,                    XK_Right,  nav_right  },
    { 0,                    XK_k,      nav_up     },
    { 0,                    XK_Up,     nav_up     },
    { 0,                    XK_j,      nav_down   },
    { 0,                    XK_Down,   nav_down   },
    { 0,                    XK_Return, nav_select },
    { 0,                    XK_space,  nav_select },
    { 0,                    XK_Escape, nav_quit   },
    { 0,                    XK_q,      nav_quit   },
    { MODKEY,               XK_c,      nav_quit   },
    { SWITCHMOD,            XK_Tab,    nav_next   },
    { SWITCHMOD|ShiftMask,  XK_Tab,    nav_prev   },
};
