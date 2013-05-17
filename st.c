/* See LICENSE for licence details. */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/Xft/Xft.h>
#include <fontconfig/fontconfig.h>

#include "arg.h"

char *argv0;

#define Glyph Glyph_
#define Font Font_
#define Draw XftDraw *
#define Colour XftColor
#define Colourmap Colormap
#define Rectangle XRectangle

#if   defined(__linux)
 #include <pty.h>
#elif defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
 #include <util.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
 #include <libutil.h>
#endif


/* XEMBED messages */
#define XEMBED_FOCUS_IN  4
#define XEMBED_FOCUS_OUT 5

/* Arbitrary sizes */
#define UTF_SIZ       4
#define ESC_BUF_SIZ   (128*UTF_SIZ)
#define ESC_ARG_SIZ   16
#define STR_BUF_SIZ   ESC_BUF_SIZ
#define STR_ARG_SIZ   ESC_ARG_SIZ
#define DRAW_BUF_SIZ  20*1024
#define XK_ANY_MOD    UINT_MAX
#define XK_NO_MOD     0
#define XK_SWITCH_MOD (1<<13)

#define REDRAW_TIMEOUT (80*1000) /* 80 ms */

/* macros */
#define SERRNO strerror(errno)
#define MIN(a, b)  ((a) < (b) ? (a) : (b))
#define MAX(a, b)  ((a) < (b) ? (b) : (a))
#define LEN(a)     (sizeof(a) / sizeof(a[0]))
#define DEFAULT(a, b)     (a) = (a) ? (a) : (b)
#define BETWEEN(x, a, b)  ((a) <= (x) && (x) <= (b))
#define LIMIT(x, a, b)    (x) = (x) < (a) ? (a) : (x) > (b) ? (b) : (x)
#define ATTRCMP(a, b) ((a).mode != (b).mode || (a).fg != (b).fg || (a).bg != (b).bg)
#define IS_SET(t, flag) (((t)->mode & (flag)) != 0)
#define TIMEDIFF(t1, t2) ((t1.tv_sec-t2.tv_sec)*1000 + (t1.tv_usec-t2.tv_usec)/1000)

#define SCROLLBACK 10000

#define VT102ID "\033[?6c"

enum glyph_attribute {
	ATTR_NULL      = 0,
	ATTR_REVERSE   = 1,
	ATTR_UNDERLINE = 2,
	ATTR_BOLD      = 4,
	ATTR_GFX       = 8,
	ATTR_ITALIC    = 16,
	ATTR_BLINK     = 32,
	ATTR_WRAP      = 64,
};

enum cursor_movement {
	CURSOR_SAVE,
	CURSOR_LOAD
};

enum cursor_state {
	CURSOR_DEFAULT  = 0,
	CURSOR_WRAPNEXT = 1,
	CURSOR_ORIGIN	= 2
};

enum term_mode {
	MODE_WRAP	 = 1,
	MODE_INSERT      = 2,
	MODE_APPKEYPAD   = 4,
	MODE_ALTSCREEN   = 8,
	MODE_CRLF	 = 16,
	MODE_MOUSEBTN    = 32,
	MODE_MOUSEMOTION = 64,
	MODE_MOUSE       = 32|64,
	MODE_REVERSE     = 128,
	MODE_KBDLOCK     = 256,
	MODE_HIDE	 = 512,
	MODE_ECHO	 = 1024,
	MODE_APPCURSOR	 = 2048,
	MODE_MOUSESGR    = 4096,
	MODE_8BIT	 = 8192,
	MODE_BLINK	 = 16384,
	MODE_FBLINK	 = 32768,
};

enum escape_state {
	ESC_START      = 1,
	ESC_CSI	= 2,
	ESC_STR	= 4, /* DSC, OSC, PM, APC */
	ESC_ALTCHARSET = 8,
	ESC_STR_END    = 16, /* a final string was encountered */
	ESC_TEST       = 32, /* Enter in test mode */
};

enum window_state {
	WIN_VISIBLE = 1,
	WIN_REDRAW  = 2,
	WIN_FOCUSED = 4
};

enum selection_type {
	SEL_REGULAR = 1,
	SEL_RECTANGULAR = 2
};

enum selection_snap {
	SNAP_WORD = 1,
	SNAP_LINE = 2
};

/* bit macro */
#undef B0
enum { B0=1, B1=2, B2=4, B3=8, B4=16, B5=32, B6=64, B7=128 };

typedef unsigned char uchar;
typedef unsigned int uint;
typedef unsigned long ulong;
typedef unsigned short ushort;

typedef struct {
	char c[UTF_SIZ];     /* character code */
	uchar mode;  /* attribute flags */
	ushort fg;   /* foreground  */
	ushort bg;   /* background  */
} Glyph;

typedef Glyph *Line;

typedef struct {
	Glyph attr;	 /* current char attributes */
	int x;
	int y;
	char state;
} TCursor;

/* CSI Escape sequence structs */
/* ESC '[' [[ [<priv>] <arg> [;]] <mode>] */
typedef struct {
	char buf[ESC_BUF_SIZ]; /* raw string */
	int len;	       /* raw string length */
	char priv;
	int arg[ESC_ARG_SIZ];
	int narg;	       /* nb of args */
	char mode;
} CSIEscape;

/* STR Escape sequence structs */
/* ESC type [[ [<priv>] <arg> [;]] <mode>] ESC '\' */
typedef struct {
	char type;	     /* ESC type ... */
	char buf[STR_BUF_SIZ]; /* raw string */
	int len;	       /* raw string length */
	char *args[STR_ARG_SIZ];
	int narg;	      /* nb of args */
} STREscape;

typedef struct _Scrollback {
	Glyph *line;
	struct _Scrollback *next;
	struct _Scrollback *prev;
	int col;
} Scrollback;

typedef struct {
	Scrollback *head;
	Scrollback *tail;
	int total;
} ScrollbackList;

/* Internal representation of the screen */
typedef struct _Term {
	int row;	/* nb row */
	int col;	/* nb col */
	Line *line;	/* screen */
	Line *alt;	/* alternate screen */
	bool *dirty;	/* dirtyness of lines */
	TCursor c;	/* cursor */
	int top;	/* top    scroll limit */
	int bot;	/* bottom scroll limit */
	int mode;	/* terminal mode flags */
	int esc;	/* escape state flags */
	bool numlock;	/* lock numbers in keyboard */
	bool *tabs;
	struct _Term *next;
	int cmdfd;
	pid_t pid;
	int ybase;
	ScrollbackList *sb;
	Line *last_line;
} Term;

/* Purely graphic info */
typedef struct {
	Display *dpy;
	Colourmap cmap;
	Window win;
	Drawable buf;
	Atom xembed, wmdeletewin;
	XIM xim;
	XIC xic;
	Draw draw;
	Visual *vis;
	int scr;
	bool isfixed; /* is fixed geometry? */
	int fx, fy, fw, fh; /* fixed geometry */
	int tw, th; /* tty width and height */
	int w, h; /* window width and height */
	int ch; /* char height */
	int cw; /* char width  */
	char state; /* focus, redraw, visible */
} XWindow;

typedef struct {
	int b;
	uint mask;
	char s[ESC_BUF_SIZ];
} Mousekey;

typedef struct {
	KeySym k;
	uint mask;
	char s[ESC_BUF_SIZ];
	/* three valued logic variables: 0 indifferent, 1 on, -1 off */
	signed char appkey;		/* application keypad */
	signed char appcursor;		/* application cursor */
	signed char crlf;		/* crlf mode          */
} Key;

/* TODO: use better name for vars... */
typedef struct {
	int mode;
	int type;
	int snap;
	int bx, by;
	int ex, ey;
	struct {
		int x, y;
	} b, e;
	char *clip;
	Atom xtarget;
	bool alt;
	struct timeval tclick1;
	struct timeval tclick2;
} Selection;

typedef union {
	int i;
	unsigned int ui;
	float f;
	const void *v;
} Arg;

typedef struct {
	unsigned int mod;
	KeySym keysym;
	void (*func)(const Arg *);
	const Arg arg;
} Shortcut;

/* function definitions used in config.h */
static void clippaste(const Arg *);
static void numlock(const Arg *);
static void selpaste(const Arg *);
static void xzoom(const Arg *);

/* Config.h for applying patches and the configuration. */
#include "config.h"

/* Font structure */
typedef struct {
	int height;
	int width;
	int ascent;
	int descent;
	short lbearing;
	short rbearing;
	XftFont *match;
	FcFontSet *set;
	FcPattern *pattern;
} Font;

/* Drawing Context */
typedef struct {
	Colour col[LEN(colorname) < 256 ? 256 : LEN(colorname)];
	Font font, bfont, ifont, ibfont;
	GC gc;
} DC;

static void die(const char *, ...);
static void draw(void);
static void redraw(int);
static void drawregion(int, int, int, int);
static void execsh(void);
#ifdef NO_TABS
static void sigchld(int);
#endif
static void run(void);

static void csidump(void);
static void csihandle(Term *);
static void csiparse(void);
static void csireset(void);
static void strdump(void);
static void strhandle(void);
static void strparse(void);
static void strreset(void);

static int tattrset(Term *, int);
static void tclearregion(Term *, int, int, int, int);
static void tcursor(Term *, int);
static void tdeletechar(Term *, int);
static void tdeleteline(Term *, int);
static void tinsertblank(Term *, int);
static void tinsertblankline(Term *, int);
static void tmoveto(Term *, int, int);
static void tmoveato(Term *, int x, int y);
static void tnew(Term *, int, int);
static void tnewline(Term *, int);
static void tputtab(Term *, bool);
static void tputc(Term *, char *, int);
static void treset(Term *);
static int tresize(Term *, int, int);
static void tscrollup(Term *, int, int);
static void tscrolldown(Term *, int, int);
static void tsetattr(Term *, int*, int);
static void tsetchar(Term *, char *, Glyph *, int, int);
static void tsetscroll(Term *, int, int);
static void tswapscreen(Term *);
static void tsetdirt(Term *, int, int);
static void tsetdirtattr(Term *, int);
static void tsetmode(Term *, bool, bool, int *, int);
static void tfulldirt(Term *);
static void techo(Term *, char *, int);
static void tscrollback(Term *term, int n);

static inline bool match(uint, uint);
static void ttynew(Term *);
static void ttyread(Term *);
static void ttyresize(Term *);
static void ttywrite(Term *, const char *, size_t);

static void term_add(void);
static void term_remove(Term *);
static void term_focus(Term *);
static void term_focus_prev(Term *);
static void term_focus_next(Term *);
static Term *term_focus_idx(int);

static void xdraws(char *, Glyph, int, int, int, int);
static void xhints(void);
static void xclear(int, int, int, int);
static void xdrawcursor(void);
static void xinit(void);
static void xloadcols(void);
static int xsetcolorname(int, const char *);
static int xloadfont(Font *, FcPattern *);
static void xloadfonts(char *, int);
static int xloadfontset(Font *);
static void xsettitle(char *);
static void xresettitle(void);
static void xseturgency(int);
static void xsetsel(char*);
static void xtermclear(int, int, int, int);
static void xunloadfont(Font *f);
static void xunloadfonts(void);
static void xresize(int, int);
static void xdrawbar(void);

static void expose(XEvent *);
static void visibility(XEvent *);
static void unmap(XEvent *);
static char *kmap(KeySym, uint);
static void kpress(XEvent *);
static void cmessage(XEvent *);
static void cresize(int, int);
static void resize(XEvent *);
static void focus(XEvent *);
static void brelease(XEvent *);
static void bpress(XEvent *);
static void bmotion(XEvent *);
static void selnotify(XEvent *);
static void selclear(XEvent *);
static void selrequest(XEvent *);

static void selinit(void);
static inline bool selected(int, int);
static void selcopy(void);
static void selscroll(Term *, int, int);
static void selsnap(int, int *, int *, int);

ScrollbackList *scrollback_create(void);
Glyph *scrollback_get(Term *, int);
void scrollback_add(Term *, Glyph *);

static int utf8decode(char *, long *);
static int utf8encode(long *, char *);
static int utf8size(char *);
static int isfullutf8(char *, int);

static ssize_t xwrite(int, char *, size_t);
static void *xmalloc(size_t);
static void *xrealloc(void *, size_t);
static void *xcalloc(size_t, size_t);

static int set_message(char *fmt, ...);

static void (*handler[LASTEvent])(XEvent *) = {
	[KeyPress] = kpress,
	[ClientMessage] = cmessage,
	[ConfigureNotify] = resize,
	[VisibilityNotify] = visibility,
	[UnmapNotify] = unmap,
	[Expose] = expose,
	[FocusIn] = focus,
	[FocusOut] = focus,
	[MotionNotify] = bmotion,
	[ButtonPress] = bpress,
	[ButtonRelease] = brelease,
	[SelectionClear] = selclear,
	[SelectionNotify] = selnotify,
	[SelectionRequest] = selrequest,
};

/* Globals */
static DC dc;
static XWindow xw;
static Term *terms;
static Term *focused_term;
static bool prefix_active = false;
static bool select_mode = false;
static bool visual_mode = false;
//static bool cursor_was_hidden = false;
static struct { int x; int y; bool hidden; int ybase; } normal_cursor;
static char *status_msg = NULL;
static CSIEscape csiescseq;
static STREscape strescseq;
static pid_t pid;
static Selection sel;
static int iofd = -1;
static char **opt_cmd = NULL;
static char *opt_io = NULL;
static char *opt_title = NULL;
static char *opt_embed = NULL;
static char *opt_class = NULL;
static char *opt_font = NULL;

static char *usedfont = NULL;
static int usedfontsize = 0;

/* Font Ring Cache */
enum {
	FRC_NORMAL,
	FRC_ITALIC,
	FRC_BOLD,
	FRC_ITALICBOLD
};

typedef struct {
	XftFont *font;
	long c;
	int flags;
} Fontcache;

/*
 * Fontcache is a ring buffer, with frccur as current position and frclen as
 * the current length of used elements.
 */

static Fontcache frc[1024];
static int frccur = -1, frclen = 0;

ssize_t
xwrite(int fd, char *s, size_t len) {
	size_t aux = len;

	while(len > 0) {
		ssize_t r = write(fd, s, len);
		if(r < 0)
			return r;
		len -= r;
		s += r;
	}
	return aux;
}

void *
xmalloc(size_t len) {
	void *p = malloc(len);

	if(!p)
		die("Out of memory\n");

	return p;
}

void *
xrealloc(void *p, size_t len) {
	if((p = realloc(p, len)) == NULL)
		die("Out of memory\n");

	return p;
}

void *
xcalloc(size_t nmemb, size_t size) {
	void *p = calloc(nmemb, size);

	if(!p)
		die("Out of memory\n");

	return p;
}

int
utf8decode(char *s, long *u) {
	uchar c;
	int i, n, rtn;

	rtn = 1;
	c = *s;
	if(~c & B7) { /* 0xxxxxxx */
		*u = c;
		return rtn;
	} else if((c & (B7|B6|B5)) == (B7|B6)) { /* 110xxxxx */
		*u = c&(B4|B3|B2|B1|B0);
		n = 1;
	} else if((c & (B7|B6|B5|B4)) == (B7|B6|B5)) { /* 1110xxxx */
		*u = c&(B3|B2|B1|B0);
		n = 2;
	} else if((c & (B7|B6|B5|B4|B3)) == (B7|B6|B5|B4)) { /* 11110xxx */
		*u = c & (B2|B1|B0);
		n = 3;
	} else {
		goto invalid;
	}

	for(i = n, ++s; i > 0; --i, ++rtn, ++s) {
		c = *s;
		if((c & (B7|B6)) != B7) /* 10xxxxxx */
			goto invalid;
		*u <<= 6;
		*u |= c & (B5|B4|B3|B2|B1|B0);
	}

	if((n == 1 && *u < 0x80) ||
	   (n == 2 && *u < 0x800) ||
	   (n == 3 && *u < 0x10000) ||
	   (*u >= 0xD800 && *u <= 0xDFFF)) {
		goto invalid;
	}

	return rtn;
invalid:
	*u = 0xFFFD;

	return rtn;
}

int
utf8encode(long *u, char *s) {
	uchar *sp;
	ulong uc;
	int i, n;

	sp = (uchar *)s;
	uc = *u;
	if(uc < 0x80) {
		*sp = uc; /* 0xxxxxxx */
		return 1;
	} else if(*u < 0x800) {
		*sp = (uc >> 6) | (B7|B6); /* 110xxxxx */
		n = 1;
	} else if(uc < 0x10000) {
		*sp = (uc >> 12) | (B7|B6|B5); /* 1110xxxx */
		n = 2;
	} else if(uc <= 0x10FFFF) {
		*sp = (uc >> 18) | (B7|B6|B5|B4); /* 11110xxx */
		n = 3;
	} else {
		goto invalid;
	}

	for(i=n,++sp; i>0; --i,++sp)
		*sp = ((uc >> 6*(i-1)) & (B5|B4|B3|B2|B1|B0)) | B7; /* 10xxxxxx */

	return n+1;
invalid:
	/* U+FFFD */
	*s++ = '\xEF';
	*s++ = '\xBF';
	*s = '\xBD';

	return 3;
}

/* use this if your buffer is less than UTF_SIZ, it returns 1 if you can decode
   UTF-8 otherwise return 0 */
int
isfullutf8(char *s, int b) {
	uchar *c1, *c2, *c3;

	c1 = (uchar *)s;
	c2 = (uchar *)++s;
	c3 = (uchar *)++s;
	if(b < 1) {
		return 0;
	} else if((*c1&(B7|B6|B5)) == (B7|B6) && b == 1) {
		return 0;
	} else if((*c1&(B7|B6|B5|B4)) == (B7|B6|B5) &&
	    ((b == 1) ||
	    ((b == 2) && (*c2&(B7|B6)) == B7))) {
		return 0;
	} else if((*c1&(B7|B6|B5|B4|B3)) == (B7|B6|B5|B4) &&
	    ((b == 1) ||
	    ((b == 2) && (*c2&(B7|B6)) == B7) ||
	    ((b == 3) && (*c2&(B7|B6)) == B7 && (*c3&(B7|B6)) == B7))) {
		return 0;
	} else {
		return 1;
	}
}

int
utf8size(char *s) {
	uchar c = *s;

	if(~c&B7) {
		return 1;
	} else if((c&(B7|B6|B5)) == (B7|B6)) {
		return 2;
	} else if((c&(B7|B6|B5|B4)) == (B7|B6|B5)) {
		return 3;
	} else {
		return 4;
	}
}

void
selinit(void) {
	memset(&sel.tclick1, 0, sizeof(sel.tclick1));
	memset(&sel.tclick2, 0, sizeof(sel.tclick2));
	sel.mode = 0;
	sel.bx = -1;
	sel.clip = NULL;
	sel.xtarget = XInternAtom(xw.dpy, "UTF8_STRING", 0);
	if(sel.xtarget == None)
		sel.xtarget = XA_STRING;
}

static int
x2col(Term *term, int x) {
	x -= borderpx;
	x /= xw.cw;

	return LIMIT(x, 0, term->col-1);
}

static int
y2row(Term *term, int y) {
	y -= borderpx;
	y /= xw.ch;

	return LIMIT(y, 0, term->row-1);
}

static int
col2x(Term *term, int x) {
	x *= xw.cw;
	x += borderpx;
	return x;
}

static int
row2y(Term *term, int y) {
	y *= xw.ch;
	y += borderpx;
	return y;
}

static inline bool
selected(int x, int y) {
	int bx, ex;

	if(sel.ey == y && sel.by == y) {
		bx = MIN(sel.bx, sel.ex);
		ex = MAX(sel.bx, sel.ex);

		return BETWEEN(x, bx, ex);
	}

	if(sel.type == SEL_RECTANGULAR) {
		return ((sel.b.y <= y && y <= sel.e.y)
			&& (sel.b.x <= x && x <= sel.e.x));
	}
	return ((sel.b.y < y && y < sel.e.y)
		|| (y == sel.e.y && x <= sel.e.x))
		|| (y == sel.b.y && x >= sel.b.x
			&& (x <= sel.e.x || sel.b.y != sel.e.y));
}

void
selsnap(int mode, int *x, int *y, int direction) {
	int i;

	switch(mode) {
	case SNAP_WORD:
		/*
		 * Snap around if the word wraps around at the end or
		 * beginning of a line.
		 */
		for(;;) {
			if(direction < 0 && *x <= 0) {
				if(*y > 0 && focused_term->line[*y - 1][focused_term->col-1].mode
						& ATTR_WRAP) {
					*y -= 1;
					*x = focused_term->col-1;
				} else {
					break;
				}
			}
			if(direction > 0 && *x >= focused_term->col-1) {
				if(*y < focused_term->row-1 && focused_term->line[*y][*x].mode
						& ATTR_WRAP) {
					*y += 1;
					*x = 0;
				} else {
					break;
				}
			}

			if(strchr(worddelimiters,
					focused_term->line[*y][*x + direction].c[0])) {
				break;
			}

			*x += direction;
		}
		break;
	case SNAP_LINE:
		/*
		 * Snap around if the the previous line or the current one
		 * has set ATTR_WRAP at its end. Then the whole next or
		 * previous line will be selected.
		 */
		*x = (direction < 0) ? 0 : focused_term->col - 1;
		if(direction < 0 && *y > 0) {
			for(; *y > 0; *y += direction) {
				if(!(focused_term->line[*y-1][focused_term->col-1].mode
						& ATTR_WRAP)) {
					break;
				}
			}
		} else if(direction > 0 && *y < focused_term->row-1) {
			for(; *y < focused_term->row; *y += direction) {
				if(!(focused_term->line[*y][focused_term->col-1].mode
						& ATTR_WRAP)) {
					break;
				}
			}
		}
		break;
	default:
		/*
		 * Select the whole line when the end of line is reached.
		 */
		if(direction > 0) {
			i = focused_term->col;
			while(--i > 0 && focused_term->line[*y][i].c[0] == ' ')
				/* nothing */;
			if(i > 0 && i < *x)
				*x = focused_term->col - 1;
		}
		break;
	}
}

void
getbuttoninfo(XEvent *e) {
	int type;
	uint state = e->xbutton.state &~Button1Mask;

	sel.alt = IS_SET(focused_term, MODE_ALTSCREEN);

	sel.ex = x2col(focused_term, e->xbutton.x);
	sel.ey = y2row(focused_term, e->xbutton.y);

	if (sel.by < sel.ey
			|| (sel.by == sel.ey && sel.bx < sel.ex)) {
		selsnap(sel.snap, &sel.bx, &sel.by, -1);
		selsnap(sel.snap, &sel.ex, &sel.ey, +1);
	} else {
		selsnap(sel.snap, &sel.ex, &sel.ey, -1);
		selsnap(sel.snap, &sel.bx, &sel.by, +1);
	}

	sel.b.x = sel.by < sel.ey ? sel.bx : sel.ex;
	sel.b.y = MIN(sel.by, sel.ey);
	sel.e.x = sel.by < sel.ey ? sel.ex : sel.bx;
	sel.e.y = MAX(sel.by, sel.ey);

	sel.type = SEL_REGULAR;
	for(type = 1; type < LEN(selmasks); ++type) {
		if(match(selmasks[type], state)) {
			sel.type = type;
			break;
		}
	}
}

void
mousereport(XEvent *e) {
	int x = x2col(focused_term, e->xbutton.x), y = y2row(focused_term, e->xbutton.y),
	    button = e->xbutton.button, state = e->xbutton.state,
	    len;
	char buf[40];
	static int ob, ox, oy;

	/* from urxvt */
	if(e->xbutton.type == MotionNotify) {
		if(!IS_SET(focused_term, MODE_MOUSEMOTION) || (x == ox && y == oy))
			return;
		button = ob + 32;
		ox = x, oy = y;
	} else if(!IS_SET(focused_term, MODE_MOUSESGR)
			&& (e->xbutton.type == ButtonRelease
				|| button == AnyButton)) {
		button = 3;
	} else {
		button -= Button1;
		if(button >= 3)
			button += 64 - 3;
		if(e->xbutton.type == ButtonPress) {
			ob = button;
			ox = x, oy = y;
		}
	}

	button += (state & ShiftMask   ? 4  : 0)
		+ (state & Mod4Mask    ? 8  : 0)
		+ (state & ControlMask ? 16 : 0);

	len = 0;
	if(IS_SET(focused_term, MODE_MOUSESGR)) {
		len = snprintf(buf, sizeof(buf), "\033[<%d;%d;%d%c",
				button, x+1, y+1,
				e->xbutton.type == ButtonRelease ? 'm' : 'M');
	} else if(x < 223 && y < 223) {
		len = snprintf(buf, sizeof(buf), "\033[M%c%c%c",
				32+button, 32+x+1, 32+y+1);
	} else {
		return;
	}

	ttywrite(focused_term, buf, len);
}

void
bpress(XEvent *e) {
	struct timeval now;
	Mousekey *mk;

	if(IS_SET(focused_term, MODE_MOUSE)) {
		mousereport(e);
		return;
	}

	for(mk = mshortcuts; mk < mshortcuts + LEN(mshortcuts); mk++) {
		if(e->xbutton.button == mk->b
				&& match(mk->mask, e->xbutton.state)) {
			ttywrite(focused_term, mk->s, strlen(mk->s));
			if(IS_SET(focused_term, MODE_ECHO))
				techo(focused_term, mk->s, strlen(mk->s));
			return;
		}
	}

	if (e->xbutton.button == Button4) {
		tscrollback(focused_term, -(focused_term->row / 2));
	} else if (e->xbutton.button == Button5) {
		tscrollback(focused_term, focused_term->row / 2);
	}

	if(e->xbutton.button == Button1) {
		gettimeofday(&now, NULL);

		/* Clear previous selection, logically and visually. */
		if(sel.bx != -1) {
			sel.bx = -1;
			tsetdirt(focused_term, sel.b.y, sel.e.y);
			draw();
		}
		sel.mode = 1;
		sel.type = SEL_REGULAR;
		sel.ex = sel.bx = x2col(focused_term, e->xbutton.x);
		sel.ey = sel.by = y2row(focused_term, e->xbutton.y);

		/*
		 * If the user clicks below predefined timeouts specific
		 * snapping behaviour is exposed.
		 */
		if(TIMEDIFF(now, sel.tclick2) <= tripleclicktimeout) {
			sel.snap = SNAP_LINE;
		} else if(TIMEDIFF(now, sel.tclick1) <= doubleclicktimeout) {
			sel.snap = SNAP_WORD;
		} else {
			sel.snap = 0;
		}
		selsnap(sel.snap, &sel.bx, &sel.by, -1);
		selsnap(sel.snap, &sel.ex, &sel.ey, +1);
		sel.b.x = sel.bx;
		sel.b.y = sel.by;
		sel.e.x = sel.ex;
		sel.e.y = sel.ey;

		/*
		 * Draw selection, unless it's regular and we don't want to
		 * make clicks visible
		 */
		if(sel.snap != 0) {
			sel.mode++;
			tsetdirt(focused_term, sel.b.y, sel.e.y);
			draw();
		}
		sel.tclick2 = sel.tclick1;
		sel.tclick1 = now;
	}
}

void
selcopy(void) {
	char *str, *ptr;
	int x, y, bufsize, size, i, ex;
	Glyph *gp, *last;

	if(sel.bx == -1) {
		str = NULL;
	} else {
		bufsize = (focused_term->col+1) * (sel.e.y-sel.b.y+1) * UTF_SIZ;
		ptr = str = xmalloc(bufsize);

		/* append every set & selected glyph to the selection */
		for(y = sel.b.y; y < sel.e.y + 1; y++) {
			gp = &focused_term->line[y][0];
			last = gp + focused_term->col;

			while(--last >= gp && !(selected(last - gp, y) && \
						strcmp(last->c, " ") != 0))
				/* nothing */;

			for(x = 0; gp <= last; x++, ++gp) {
				if(!selected(x, y))
					continue;

				size = utf8size(gp->c);
				memcpy(ptr, gp->c, size);
				ptr += size;
			}

			/*
			 * Copy and pasting of line endings is inconsistent
			 * in the inconsistent terminal and GUI world.
			 * The best solution seems like to produce '\n' when
			 * something is copied from st and convert '\n' to
			 * '\r', when something to be pasted is received by
			 * st.
			 * FIXME: Fix the computer world.
			 */
			if(y < sel.e.y && !((gp-1)->mode & ATTR_WRAP))
				*ptr++ = '\n';

			/*
			 * If the last selected line expands in the selection
			 * after the visible text '\n' is appended.
			 */
			if(y == sel.e.y) {
				i = focused_term->col;
				while(--i > 0 && focused_term->line[y][i].c[0] == ' ')
					/* nothing */;
				ex = sel.e.x;
				if(sel.b.y == sel.e.y && sel.e.x < sel.b.x)
					ex = sel.b.x;
				if(i < ex)
					*ptr++ = '\n';
			}
		}
		*ptr = 0;
	}
	xsetsel(str);
}

void
selnotify(XEvent *e) {
	ulong nitems, ofs, rem;
	int format;
	uchar *data, *last, *repl;
	Atom type;

	ofs = 0;
	do {
		if(XGetWindowProperty(xw.dpy, xw.win, XA_PRIMARY, ofs, BUFSIZ/4,
					False, AnyPropertyType, &type, &format,
					&nitems, &rem, &data)) {
			fprintf(stderr, "Clipboard allocation failed\n");
			return;
		}

		/*
		 * As seen in selcopy:
		 * Line endings are inconsistent in the terminal and GUI world
		 * copy and pasting. When receiving some selection data,
		 * replace all '\n' with '\r'.
		 * FIXME: Fix the computer world.
		 */
		repl = data;
		last = data + nitems * format / 8;
		while((repl = memchr(repl, '\n', last - repl))) {
			*repl++ = '\r';
		}

		ttywrite(focused_term, (const char *)data, nitems * format / 8);
		XFree(data);
		/* number of 32-bit chunks returned */
		ofs += nitems * format / 32;
	} while(rem > 0);
}

void
selpaste(const Arg *dummy) {
	XConvertSelection(xw.dpy, XA_PRIMARY, sel.xtarget, XA_PRIMARY,
			xw.win, CurrentTime);
}

void
clippaste(const Arg *dummy) {
	Atom clipboard;

	clipboard = XInternAtom(xw.dpy, "CLIPBOARD", 0);
	XConvertSelection(xw.dpy, clipboard, sel.xtarget, XA_PRIMARY,
			xw.win, CurrentTime);
}

void
selclear(XEvent *e) {
	if(sel.bx == -1)
		return;
	sel.bx = -1;
	tsetdirt(focused_term, sel.b.y, sel.e.y);
}

void
selrequest(XEvent *e) {
	XSelectionRequestEvent *xsre;
	XSelectionEvent xev;
	Atom xa_targets, string;

	xsre = (XSelectionRequestEvent *) e;
	xev.type = SelectionNotify;
	xev.requestor = xsre->requestor;
	xev.selection = xsre->selection;
	xev.target = xsre->target;
	xev.time = xsre->time;
	/* reject */
	xev.property = None;

	xa_targets = XInternAtom(xw.dpy, "TARGETS", 0);
	if(xsre->target == xa_targets) {
		/* respond with the supported type */
		string = sel.xtarget;
		XChangeProperty(xsre->display, xsre->requestor, xsre->property,
				XA_ATOM, 32, PropModeReplace,
				(uchar *) &string, 1);
		xev.property = xsre->property;
	} else if(xsre->target == sel.xtarget && sel.clip != NULL) {
		XChangeProperty(xsre->display, xsre->requestor, xsre->property,
				xsre->target, 8, PropModeReplace,
				(uchar *) sel.clip, strlen(sel.clip));
		xev.property = xsre->property;
	}

	/* all done, send a notification to the listener */
	if(!XSendEvent(xsre->display, xsre->requestor, True, 0, (XEvent *) &xev))
		fprintf(stderr, "Error sending SelectionNotify event\n");
}

void
xsetsel(char *str) {
	/* register the selection for both the clipboard and the primary */
	Atom clipboard;

	free(sel.clip);
	sel.clip = str;

	XSetSelectionOwner(xw.dpy, XA_PRIMARY, xw.win, CurrentTime);

	clipboard = XInternAtom(xw.dpy, "CLIPBOARD", 0);
	XSetSelectionOwner(xw.dpy, clipboard, xw.win, CurrentTime);
}

void
brelease(XEvent *e) {
	if(IS_SET(focused_term, MODE_MOUSE)) {
		mousereport(e);
		return;
	}

	if(e->xbutton.button == Button2) {
		selpaste(NULL);
	} else if(e->xbutton.button == Button1) {
		if(sel.mode < 2) {
			sel.bx = -1;
		} else {
			getbuttoninfo(e);
			selcopy();
		}
		sel.mode = 0;
		focused_term->dirty[sel.ey] = 1;
	}
}

void
bmotion(XEvent *e) {
	int oldey, oldex, oldsby, oldsey;

	if(IS_SET(focused_term, MODE_MOUSE)) {
		mousereport(e);
		return;
	}

	if(!sel.mode)
		return;

	sel.mode++;
	oldey = sel.ey;
	oldex = sel.ex;
	oldsby = sel.b.y;
	oldsey = sel.e.y;
	getbuttoninfo(e);

	if(oldey != sel.ey || oldex != sel.ex) {
		tsetdirt(focused_term, MIN(sel.b.y, oldsby), MAX(sel.e.y, oldsey));
	}
}

void
die(const char *errstr, ...) {
	va_list ap;

	va_start(ap, errstr);
	vfprintf(stderr, errstr, ap);
	va_end(ap);
	exit(EXIT_FAILURE);
}

void
execsh(void) {
	char **args;
	char *envshell = getenv("SHELL");
	const struct passwd *pass = getpwuid(getuid());
	char buf[sizeof(long) * 8 + 1];

	unsetenv("COLUMNS");
	unsetenv("LINES");
	unsetenv("TERMCAP");

	if(pass) {
		setenv("LOGNAME", pass->pw_name, 1);
		setenv("USER", pass->pw_name, 1);
		setenv("SHELL", pass->pw_shell, 0);
		setenv("HOME", pass->pw_dir, 0);
	}

	snprintf(buf, sizeof(buf), "%lu", xw.win);
	setenv("WINDOWID", buf, 1);

	signal(SIGCHLD, SIG_DFL);
	signal(SIGHUP, SIG_DFL);
	signal(SIGINT, SIG_DFL);
	signal(SIGQUIT, SIG_DFL);
	signal(SIGTERM, SIG_DFL);
	signal(SIGALRM, SIG_DFL);

	DEFAULT(envshell, shell);
	setenv("TERM", termname, 1);
	args = opt_cmd ? opt_cmd : (char *[]){envshell, "-i", NULL};
	execvp(args[0], args);
	exit(EXIT_FAILURE);
}

#ifdef NO_TABS
void
sigchld(int a) {
	int stat = 0;

	if(waitpid(pid, &stat, 0) < 0)
		die("Waiting for pid %hd failed: %s\n",	pid, SERRNO);

	if(WIFEXITED(stat)) {
		exit(WEXITSTATUS(stat));
	} else {
		exit(EXIT_FAILURE);
	}
}
#endif

void
ttynew(Term *term) {
	int m, s;
	struct winsize w = {term->row, term->col, 0, 0};

	/* seems to work fine on linux, openbsd and freebsd */
	if(openpty(&m, &s, NULL, NULL, &w) < 0)
		die("openpty failed: %s\n", SERRNO);

	switch(pid = fork()) {
	case -1:
		die("fork failed\n");
		break;
	case 0:
		setsid(); /* create a new process group */
		dup2(s, STDIN_FILENO);
		dup2(s, STDOUT_FILENO);
		dup2(s, STDERR_FILENO);
		if(ioctl(s, TIOCSCTTY, NULL) < 0)
			die("ioctl TIOCSCTTY failed: %s\n", SERRNO);
		close(s);
		close(m);
		execsh();
		break;
	default:
		close(s);
		term->cmdfd = m;
		term->pid = pid;
#ifdef NO_TABS
		// NOTE: Avoid exiting on child exit because we'll have multiple tabs
		signal(SIGCHLD, sigchld);
#endif
		if(opt_io) {
			iofd = (!strcmp(opt_io, "-")) ?
				  STDOUT_FILENO :
				  open(opt_io, O_WRONLY | O_CREAT, 0666);
			if(iofd < 0) {
				fprintf(stderr, "Error opening %s:%s\n",
					opt_io, strerror(errno));
			}
		}
	}
}

void
dump(char c) {
	static int col;

	fprintf(stderr, " %02x '%c' ", c, isprint(c)?c:'.');
	if(++col % 10 == 0)
		fprintf(stderr, "\n");
}

void
ttyread(Term *term) {
	static char buf[BUFSIZ];
	static int buflen = 0;
	char *ptr;
	char s[UTF_SIZ];
	int charsize; /* size of utf8 char in bytes */
	long utf8c;
	int ret;

	/* append read bytes to unprocessed bytes */
	if((ret = read(term->cmdfd, buf+buflen, LEN(buf)-buflen)) < 0) {
#ifdef NO_TABS
		die("Couldn't read from shell: %s\n", SERRNO);
#else
		term_remove(term);
		return;
#endif
	}

	/* ignore screen output while selecting */
	if (select_mode) return;
	// TODO: Potentially buffer data here:
	// if (...) xrealloc(select_buf, (select_buf_size *= 2));

	/* scroll down if we receive bytes while examining scrollback */
	if (term->ybase < 0) {
		tscrollback(term, -term->ybase);
	}

	/* process every complete utf8 char */
	buflen += ret;
	ptr = buf;
	while(buflen >= UTF_SIZ || isfullutf8(ptr,buflen)) {
		charsize = utf8decode(ptr, &utf8c);
		utf8encode(&utf8c, s);
		tputc(term, s, charsize);
		ptr += charsize;
		buflen -= charsize;
	}

	/* keep any uncomplete utf8 char for the next call */
	memmove(buf, ptr, buflen);
}

void
ttywrite(Term *term, const char *s, size_t n) {
	if(write(term->cmdfd, s, n) == -1)
		die("write error on tty: %s\n", SERRNO);
}

void
ttyresize(Term *term) {
	struct winsize w;

	w.ws_row = term->row;
	w.ws_col = term->col;
	w.ws_xpixel = xw.tw;
	w.ws_ypixel = xw.th;
	if(ioctl(term->cmdfd, TIOCSWINSZ, &w) < 0)
		fprintf(stderr, "Couldn't set window size: %s\n", SERRNO);
}

int
tattrset(Term *term, int attr) {
	int i, j;

	for(i = 0; i < term->row-1; i++) {
		for(j = 0; j < term->col-1; j++) {
			if(term->line[i][j].mode & attr)
				return 1;
		}
	}

	return 0;
}

void
tsetdirt(Term *term, int top, int bot) {
	int i;

	LIMIT(top, 0, term->row-1);
	LIMIT(bot, 0, term->row-1);

	for(i = top; i <= bot; i++)
		term->dirty[i] = 1;
}

void
tsetdirtattr(Term *term, int attr) {
	int i, j;

	for(i = 0; i < term->row-1; i++) {
		for(j = 0; j < term->col-1; j++) {
			if(term->line[i][j].mode & attr) {
				tsetdirt(term, i, i);
				break;
			}
		}
	}
}

void
tfulldirt(Term *term) {
	tsetdirt(term, 0, term->row-1);
}

void
tcursor(Term *term, int mode) {
	static TCursor c;

	if(mode == CURSOR_SAVE) {
		c = term->c;
	} else if(mode == CURSOR_LOAD) {
		term->c = c;
		tmoveto(term, c.x, c.y);
	}
}

void
treset(Term *term) {
	uint i;

	term->c = (TCursor){{
		.mode = ATTR_NULL,
		.fg = defaultfg,
		.bg = defaultbg
	}, .x = 0, .y = 0, .state = CURSOR_DEFAULT};

	memset(term->tabs, 0, term->col * sizeof(*term->tabs));
	for(i = tabspaces; i < term->col; i += tabspaces)
		term->tabs[i] = 1;
	term->top = 0;
	term->bot = term->row - 1;
	term->mode = MODE_WRAP;

	tclearregion(term, 0, 0, term->col-1, term->row-1);
	tmoveto(term, 0, 0);
	tcursor(term, CURSOR_SAVE);
}

void
tnew(Term *term, int col, int row) {
	memset(term, 0, sizeof(Term));
	tresize(term, col, row);
	term->numlock = 1;

	treset(term);
}

void
tswapscreen(Term *term) {
	Line *tmp = term->line;

	term->line = term->alt;
	term->alt = tmp;
	term->mode ^= MODE_ALTSCREEN;
	tfulldirt(term);
}

int
set_message(char *fmt, ...) {
	int ret;

	char *text = (char *)xmalloc(100 * sizeof(char));
	memset(text, 0, 100 * sizeof(char));

	va_list list;
	va_start(list, fmt);
	ret = snprintf(text, 100 * sizeof(char), fmt, list);
	va_end(list);

	if (status_msg) free(status_msg);
	status_msg = text;

	return ret;
}

void
tscrollback(Term *term, int n) {
	// if (term->mode & MODE_APPKEYPAD) return;

	int b = term->ybase;

	term->ybase += n;
	if (term->ybase > 0) {
		term->ybase = 0;
	} else if (term->ybase < -term->sb->total) {
		term->ybase = -term->sb->total;
	}

	int i;
	if (b != 0 && term->ybase == 0) {
		for (i = 0; i < term->row; i++) {
			term->line[i] = term->last_line[i];
			term->dirty[i] = 1;
		}
	} else {
		if (b == 0) { // make sure this is the non-scrollback
			for (i = 0; i < term->row; i++) {
				memcpy(term->last_line[i], term->line[i], term->col * sizeof(Glyph));
			}
		}
		int si;
		for (i = 0; i < term->row; i++) {
			si = i + term->ybase;
			if (si < 0) {
				term->line[i] = scrollback_get(term, -(si + 1));
			} else {
				term->line[i] = term->last_line[si];
			}
			term->dirty[i] = 1;
		}
	}

	// set_message("ybase: %d\n", term->ybase);

	redraw(0);
}


void
tscrolldown(Term *term, int orig, int n) {
	int i;
	Line temp;

	LIMIT(n, 0, term->bot-orig+1);

	tclearregion(term, 0, term->bot-n+1, term->col-1, term->bot);

	for(i = term->bot; i >= orig+n; i--) {
		temp = term->line[i];
		term->line[i] = term->line[i-n];
		term->line[i-n] = temp;

		term->dirty[i] = 1;
		term->dirty[i-n] = 1;
	}

	selscroll(term, orig, n);
}

ScrollbackList *
scrollback_create(void) {
	ScrollbackList *sb = (ScrollbackList *)xmalloc(sizeof(ScrollbackList));
	sb->head = NULL;
	sb->tail = NULL;
	sb->total = 0;
	return sb;
}

// I was trying to look up to figure out what O
// this is, and then I realized it is O(ridiculous).
// TODO: Optimize using nerdier data structures.
Glyph *
scrollback_get(Term *term, int i) {
	Scrollback *sb = term->sb->head;
	for (int j = 0; j < i && sb; j++) sb = sb->next;
	if (!sb) return NULL;
	if (term->col != sb->col) {
		sb->line = xrealloc(sb->line, term->col * sizeof(Glyph));
	}
	return sb->line;
}

void
scrollback_add(Term *term, Glyph *l) {
	Scrollback *sb = (Scrollback *)xmalloc(sizeof(Scrollback));
	sb->line = l;
	sb->next = NULL;
	sb->prev = NULL;
	sb->col = term->col;

	Scrollback *h = term->sb->head;
	term->sb->head = sb;
	if (!term->sb->tail) {
		term->sb->tail = sb;
	}

	if (h) {
		sb->next = h;
		h->prev = sb;
	}

	// TODO: Halve the scrollback instead, similar to tty.js.
	if (++term->sb->total > SCROLLBACK) {
		term->sb->total--;
		Scrollback *t = term->sb->tail;
		term->sb->tail = t->prev;
		t->prev->next = NULL;
		free(t->line);
		free(t);
	}
}

void
tscrollup(Term *term, int orig, int n) {
	int i;
	Line temp;
	LIMIT(n, 0, term->bot-orig+1);

	if (orig == term->top && term->ybase == 0) {
		for(i = orig; i <= orig + n - 1; i++) {
			Glyph *l = xmalloc(term->col * sizeof(Glyph));
			memcpy(l, term->line[i], term->col * sizeof(Glyph));
			scrollback_add(term, l);
		}
	}

	tclearregion(term, 0, orig, term->col-1, orig+n-1);

	for(i = orig; i <= term->bot-n; i++) {
		 temp = term->line[i];
		 term->line[i] = term->line[i+n];
		 term->line[i+n] = temp;

		 term->dirty[i] = 1;
		 term->dirty[i+n] = 1;
	}

	selscroll(term, orig, -n);
}

void
selscroll(Term *term, int orig, int n) {
	if(sel.bx == -1)
		return;

	if(BETWEEN(sel.by, orig, term->bot) || BETWEEN(sel.ey, orig, term->bot)) {
		if((sel.by += n) > term->bot || (sel.ey += n) < term->top) {
			sel.bx = -1;
			return;
		}
		if(sel.type == SEL_RECTANGULAR) {
			if(sel.by < term->top)
				sel.by = term->top;
			if(sel.ey > term->bot)
				sel.ey = term->bot;
		} else {
			if(sel.by < term->top) {
				sel.by = term->top;
				sel.bx = 0;
			}
			if(sel.ey > term->bot) {
				sel.ey = term->bot;
				sel.ex = term->col;
			}
		}
		sel.b.y = sel.by, sel.b.x = sel.bx;
		sel.e.y = sel.ey, sel.e.x = sel.ex;
	}
}

void
tnewline(Term *term, int first_col) {
	int y = term->c.y;

	if(y == term->bot) {
		tscrollup(term, term->top, 1);
	} else {
		y++;
	}
	tmoveto(term, first_col ? 0 : term->c.x, y);
}

void
csiparse(void) {
	char *p = csiescseq.buf, *np;
	long int v;

	csiescseq.narg = 0;
	if(*p == '?') {
		csiescseq.priv = 1;
		p++;
	}

	csiescseq.buf[csiescseq.len] = '\0';
	while(p < csiescseq.buf+csiescseq.len) {
		np = NULL;
		v = strtol(p, &np, 10);
		if(np == p)
			v = 0;
		if(v == LONG_MAX || v == LONG_MIN)
			v = -1;
		csiescseq.arg[csiescseq.narg++] = v;
		p = np;
		if(*p != ';' || csiescseq.narg == ESC_ARG_SIZ)
			break;
		p++;
	}
	csiescseq.mode = *p;
}

/* for absolute user moves, when decom is set */
void
tmoveato(Term *term, int x, int y) {
	tmoveto(term, x, y + ((term->c.state & CURSOR_ORIGIN) ? term->top: 0));
}

void
tmoveto(Term *term, int x, int y) {
	int miny, maxy;

	if(term->c.state & CURSOR_ORIGIN) {
		miny = term->top;
		maxy = term->bot;
	} else {
		miny = 0;
		maxy = term->row - 1;
	}
	LIMIT(x, 0, term->col-1);
	LIMIT(y, miny, maxy);
	term->c.state &= ~CURSOR_WRAPNEXT;
	term->c.x = x;
	term->c.y = y;
}

void
tsetchar(Term *term, char *c, Glyph *attr, int x, int y) {
	static char *vt100_0[62] = { /* 0x41 - 0x7e */
		"↑", "↓", "→", "←", "█", "▚", "☃", /* A - G */
		0, 0, 0, 0, 0, 0, 0, 0, /* H - O */
		0, 0, 0, 0, 0, 0, 0, 0, /* P - W */
		0, 0, 0, 0, 0, 0, 0, " ", /* X - _ */
		"◆", "▒", "␉", "␌", "␍", "␊", "°", "±", /* ` - g */
		"␤", "␋", "┘", "┐", "┌", "└", "┼", "⎺", /* h - o */
		"⎻", "─", "⎼", "⎽", "├", "┤", "┴", "┬", /* p - w */
		"│", "≤", "≥", "π", "≠", "£", "·", /* x - ~ */
	};

	/*
	 * The table is proudly stolen from rxvt.
	 */
	if(attr->mode & ATTR_GFX) {
		if(c[0] >= 0x41 && c[0] <= 0x7e
				&& vt100_0[c[0] - 0x41]) {
			c = vt100_0[c[0] - 0x41];
		}
	}

	term->dirty[y] = 1;
	term->line[y][x] = *attr;
	memcpy(term->line[y][x].c, c, UTF_SIZ);
}

void
tclearregion(Term *term, int x1, int y1, int x2, int y2) {
	int x, y, temp;

	if(x1 > x2)
		temp = x1, x1 = x2, x2 = temp;
	if(y1 > y2)
		temp = y1, y1 = y2, y2 = temp;

	LIMIT(x1, 0, term->col-1);
	LIMIT(x2, 0, term->col-1);
	LIMIT(y1, 0, term->row-1);
	LIMIT(y2, 0, term->row-1);

	for(y = y1; y <= y2; y++) {
		term->dirty[y] = 1;
		for(x = x1; x <= x2; x++) {
			if(selected(x, y))
				selclear(NULL);
			term->line[y][x] = term->c.attr;
			memcpy(term->line[y][x].c, " ", 2);
		}
	}
}

void
tdeletechar(Term *term, int n) {
	int src = term->c.x + n;
	int dst = term->c.x;
	int size = term->col - src;

	term->dirty[term->c.y] = 1;

	if(src >= term->col) {
		tclearregion(term, term->c.x, term->c.y, term->col-1, term->c.y);
		return;
	}

	memmove(&term->line[term->c.y][dst], &term->line[term->c.y][src],
			size * sizeof(Glyph));
	tclearregion(term, term->col-n, term->c.y, term->col-1, term->c.y);
}

void
tinsertblank(Term *term, int n) {
	int src = term->c.x;
	int dst = src + n;
	int size = term->col - dst;

	term->dirty[term->c.y] = 1;

	if(dst >= term->col) {
		tclearregion(term, term->c.x, term->c.y, term->col-1, term->c.y);
		return;
	}

	memmove(&term->line[term->c.y][dst], &term->line[term->c.y][src],
			size * sizeof(Glyph));
	tclearregion(term, src, term->c.y, dst - 1, term->c.y);
}

void
tinsertblankline(Term *term, int n) {
	if(term->c.y < term->top || term->c.y > term->bot)
		return;

	tscrolldown(term, term->c.y, n);
}

void
tdeleteline(Term *term, int n) {
	if(term->c.y < term->top || term->c.y > term->bot)
		return;

	tscrollup(term, term->c.y, n);
}

void
tsetattr(Term *term, int *attr, int l) {
	int i;

	for(i = 0; i < l; i++) {
		switch(attr[i]) {
		case 0:
			term->c.attr.mode &= ~(ATTR_REVERSE | ATTR_UNDERLINE \
					| ATTR_BOLD | ATTR_ITALIC \
					| ATTR_BLINK);
			term->c.attr.fg = defaultfg;
			term->c.attr.bg = defaultbg;
			break;
		case 1:
			term->c.attr.mode |= ATTR_BOLD;
			break;
		case 3:
			term->c.attr.mode |= ATTR_ITALIC;
			break;
		case 4:
			term->c.attr.mode |= ATTR_UNDERLINE;
			break;
		case 5: /* slow blink */
		case 6: /* rapid blink */
			term->c.attr.mode |= ATTR_BLINK;
			break;
		case 7:
			term->c.attr.mode |= ATTR_REVERSE;
			break;
		case 21:
		case 22:
			term->c.attr.mode &= ~ATTR_BOLD;
			break;
		case 23:
			term->c.attr.mode &= ~ATTR_ITALIC;
			break;
		case 24:
			term->c.attr.mode &= ~ATTR_UNDERLINE;
			break;
		case 25:
		case 26:
			term->c.attr.mode &= ~ATTR_BLINK;
			break;
		case 27:
			term->c.attr.mode &= ~ATTR_REVERSE;
			break;
		case 38:
			if(i + 2 < l && attr[i + 1] == 5) {
				i += 2;
				if(BETWEEN(attr[i], 0, 255)) {
					term->c.attr.fg = attr[i];
				} else {
					fprintf(stderr,
						"erresc: bad fgcolor %d\n",
						attr[i]);
				}
			} else {
				fprintf(stderr,
					"erresc(38): gfx attr %d unknown\n",
					attr[i]);
			}
			break;
		case 39:
			term->c.attr.fg = defaultfg;
			break;
		case 48:
			if(i + 2 < l && attr[i + 1] == 5) {
				i += 2;
				if(BETWEEN(attr[i], 0, 255)) {
					term->c.attr.bg = attr[i];
				} else {
					fprintf(stderr,
						"erresc: bad bgcolor %d\n",
						attr[i]);
				}
			} else {
				fprintf(stderr,
					"erresc(48): gfx attr %d unknown\n",
					attr[i]);
			}
			break;
		case 49:
			term->c.attr.bg = defaultbg;
			break;
		default:
			if(BETWEEN(attr[i], 30, 37)) {
				term->c.attr.fg = attr[i] - 30;
			} else if(BETWEEN(attr[i], 40, 47)) {
				term->c.attr.bg = attr[i] - 40;
			} else if(BETWEEN(attr[i], 90, 97)) {
				term->c.attr.fg = attr[i] - 90 + 8;
			} else if(BETWEEN(attr[i], 100, 107)) {
				term->c.attr.bg = attr[i] - 100 + 8;
			} else {
				fprintf(stderr,
					"erresc(default): gfx attr %d unknown\n",
					attr[i]), csidump();
			}
			break;
		}
	}
}

void
tsetscroll(Term *term, int t, int b) {
	int temp;

	LIMIT(t, 0, term->row-1);
	LIMIT(b, 0, term->row-1);
	if(t > b) {
		temp = t;
		t = b;
		b = temp;
	}
	term->top = t;
	term->bot = b;
}

#define MODBIT(x, set, bit) ((set) ? ((x) |= (bit)) : ((x) &= ~(bit)))

void
tsetmode(Term *term, bool priv, bool set, int *args, int narg) {
	int *lim, mode;
	bool alt;

	for(lim = args + narg; args < lim; ++args) {
		if(priv) {
			switch(*args) {
				break;
			case 1: /* DECCKM -- Cursor key */
				MODBIT(term->mode, set, MODE_APPCURSOR);
				break;
			case 5: /* DECSCNM -- Reverse video */
				mode = term->mode;
				MODBIT(term->mode, set, MODE_REVERSE);
				if(mode != term->mode)
					redraw(REDRAW_TIMEOUT);
				break;
			case 6: /* DECOM -- Origin */
				MODBIT(term->c.state, set, CURSOR_ORIGIN);
				tmoveato(term, 0, 0);
				break;
			case 7: /* DECAWM -- Auto wrap */
				MODBIT(term->mode, set, MODE_WRAP);
				break;
			case 0:  /* Error (IGNORED) */
			case 2:  /* DECANM -- ANSI/VT52 (IGNORED) */
			case 3:  /* DECCOLM -- Column  (IGNORED) */
			case 4:  /* DECSCLM -- Scroll (IGNORED) */
			case 8:  /* DECARM -- Auto repeat (IGNORED) */
			case 18: /* DECPFF -- Printer feed (IGNORED) */
			case 19: /* DECPEX -- Printer extent (IGNORED) */
			case 42: /* DECNRCM -- National characters (IGNORED) */
			case 12: /* att610 -- Start blinking cursor (IGNORED) */
				break;
			case 25: /* DECTCEM -- Text Cursor Enable Mode */
				MODBIT(term->mode, !set, MODE_HIDE);
				break;
			case 1000: /* 1000,1002: enable xterm mouse report */
				MODBIT(term->mode, set, MODE_MOUSEBTN);
				MODBIT(term->mode, 0, MODE_MOUSEMOTION);
				break;
			case 1002:
				MODBIT(term->mode, set, MODE_MOUSEMOTION);
				MODBIT(term->mode, 0, MODE_MOUSEBTN);
				break;
			case 1006:
				MODBIT(term->mode, set, MODE_MOUSESGR);
				break;
			case 1034:
				MODBIT(term->mode, set, MODE_8BIT);
				break;
			case 1049: /* = 1047 and 1048 */
			case 47:
			case 1047:
				if (!allowaltscreen)
					break;

				alt = IS_SET(term, MODE_ALTSCREEN);
				if(alt) {
					tclearregion(term, 0, 0, term->col-1,
							term->row-1);
				}
				if(set ^ alt)		/* set is always 1 or 0 */
					tswapscreen(term);
				if(*args != 1049)
					break;
				/* FALLTRU */
			case 1048:
				tcursor(term, (set) ? CURSOR_SAVE : CURSOR_LOAD);
				break;
			default:
				fprintf(stderr,
					"erresc: unknown private set/reset mode %d\n",
					*args);
				break;
			}
		} else {
			switch(*args) {
			case 0:  /* Error (IGNORED) */
				break;
			case 2:  /* KAM -- keyboard action */
				MODBIT(term->mode, set, MODE_KBDLOCK);
				break;
			case 4:  /* IRM -- Insertion-replacement */
				MODBIT(term->mode, set, MODE_INSERT);
				break;
			case 12: /* SRM -- Send/Receive */
				MODBIT(term->mode, !set, MODE_ECHO);
				break;
			case 20: /* LNM -- Linefeed/new line */
				MODBIT(term->mode, set, MODE_CRLF);
				break;
			default:
				fprintf(stderr,
					"erresc: unknown set/reset mode %d\n",
					*args);
				break;
			}
		}
	}
}
#undef MODBIT


void
csihandle(Term *term) {
	switch(csiescseq.mode) {
	default:
	unknown:
		fprintf(stderr, "erresc: unknown csi ");
		csidump();
		/* die(""); */
		break;
	case '@': /* ICH -- Insert <n> blank char */
		DEFAULT(csiescseq.arg[0], 1);
		tinsertblank(term, csiescseq.arg[0]);
		break;
	case 'A': /* CUU -- Cursor <n> Up */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(term, term->c.x, term->c.y-csiescseq.arg[0]);
		break;
	case 'B': /* CUD -- Cursor <n> Down */
	case 'e': /* VPR --Cursor <n> Down */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(term, term->c.x, term->c.y+csiescseq.arg[0]);
		break;
	case 'c': /* DA -- Device Attributes */
		if(csiescseq.arg[0] == 0)
			ttywrite(term, VT102ID, sizeof(VT102ID) - 1);
		break;
	case 'C': /* CUF -- Cursor <n> Forward */
	case 'a': /* HPR -- Cursor <n> Forward */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(term, term->c.x+csiescseq.arg[0], term->c.y);
		break;
	case 'D': /* CUB -- Cursor <n> Backward */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(term, term->c.x-csiescseq.arg[0], term->c.y);
		break;
	case 'E': /* CNL -- Cursor <n> Down and first col */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(term, 0, term->c.y+csiescseq.arg[0]);
		break;
	case 'F': /* CPL -- Cursor <n> Up and first col */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(term, 0, term->c.y-csiescseq.arg[0]);
		break;
	case 'g': /* TBC -- Tabulation clear */
		switch(csiescseq.arg[0]) {
		case 0: /* clear current tab stop */
			term->tabs[term->c.x] = 0;
			break;
		case 3: /* clear all the tabs */
			memset(term->tabs, 0, term->col * sizeof(*term->tabs));
			break;
		default:
			goto unknown;
		}
		break;
	case 'G': /* CHA -- Move to <col> */
	case '`': /* HPA */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(term, csiescseq.arg[0]-1, term->c.y);
		break;
	case 'H': /* CUP -- Move to <row> <col> */
	case 'f': /* HVP */
		DEFAULT(csiescseq.arg[0], 1);
		DEFAULT(csiescseq.arg[1], 1);
		tmoveato(term, csiescseq.arg[1]-1, csiescseq.arg[0]-1);
		break;
	case 'I': /* CHT -- Cursor Forward Tabulation <n> tab stops */
		DEFAULT(csiescseq.arg[0], 1);
		while(csiescseq.arg[0]--)
			tputtab(term, 1);
		break;
	case 'J': /* ED -- Clear screen */
		sel.bx = -1;
		switch(csiescseq.arg[0]) {
		case 0: /* below */
			tclearregion(term, term->c.x, term->c.y, term->col-1, term->c.y);
			if(term->c.y < term->row-1) {
				tclearregion(term, 0, term->c.y+1, term->col-1,
						term->row-1);
			}
			break;
		case 1: /* above */
			if(term->c.y > 1)
				tclearregion(term, 0, 0, term->col-1, term->c.y-1);
			tclearregion(term, 0, term->c.y, term->c.x, term->c.y);
			break;
		case 2: /* all */
			tclearregion(term, 0, 0, term->col-1, term->row-1);
			break;
		default:
			goto unknown;
		}
		break;
	case 'K': /* EL -- Clear line */
		switch(csiescseq.arg[0]) {
		case 0: /* right */
			tclearregion(term, term->c.x, term->c.y, term->col-1,
					term->c.y);
			break;
		case 1: /* left */
			tclearregion(term, 0, term->c.y, term->c.x, term->c.y);
			break;
		case 2: /* all */
			tclearregion(term, 0, term->c.y, term->col-1, term->c.y);
			break;
		}
		break;
	case 'S': /* SU -- Scroll <n> line up */
		DEFAULT(csiescseq.arg[0], 1);
		tscrollup(term, term->top, csiescseq.arg[0]);
		break;
	case 'T': /* SD -- Scroll <n> line down */
		DEFAULT(csiescseq.arg[0], 1);
		tscrolldown(term, term->top, csiescseq.arg[0]);
		break;
	case 'L': /* IL -- Insert <n> blank lines */
		DEFAULT(csiescseq.arg[0], 1);
		tinsertblankline(term, csiescseq.arg[0]);
		break;
	case 'l': /* RM -- Reset Mode */
		tsetmode(term, csiescseq.priv, 0, csiescseq.arg, csiescseq.narg);
		break;
	case 'M': /* DL -- Delete <n> lines */
		DEFAULT(csiescseq.arg[0], 1);
		tdeleteline(term, csiescseq.arg[0]);
		break;
	case 'X': /* ECH -- Erase <n> char */
		DEFAULT(csiescseq.arg[0], 1);
		tclearregion(term, term->c.x, term->c.y,
				term->c.x + csiescseq.arg[0] - 1, term->c.y);
		break;
	case 'P': /* DCH -- Delete <n> char */
		DEFAULT(csiescseq.arg[0], 1);
		tdeletechar(term, csiescseq.arg[0]);
		break;
	case 'Z': /* CBT -- Cursor Backward Tabulation <n> tab stops */
		DEFAULT(csiescseq.arg[0], 1);
		while(csiescseq.arg[0]--)
			tputtab(term, 0);
		break;
	case 'd': /* VPA -- Move to <row> */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveato(term, term->c.x, csiescseq.arg[0]-1);
		break;
	case 'h': /* SM -- Set terminal mode */
		tsetmode(term, csiescseq.priv, 1, csiescseq.arg, csiescseq.narg);
		break;
	case 'm': /* SGR -- Terminal attribute (color) */
		tsetattr(term, csiescseq.arg, csiescseq.narg);
		break;
	case 'r': /* DECSTBM -- Set Scrolling Region */
		if(csiescseq.priv) {
			goto unknown;
		} else {
			DEFAULT(csiescseq.arg[0], 1);
			DEFAULT(csiescseq.arg[1], term->row);
			tsetscroll(term, csiescseq.arg[0]-1, csiescseq.arg[1]-1);
			tmoveato(term, 0, 0);
		}
		break;
	case 's': /* DECSC -- Save cursor position (ANSI.SYS) */
		tcursor(term, CURSOR_SAVE);
		break;
	case 'u': /* DECRC -- Restore cursor position (ANSI.SYS) */
		tcursor(term, CURSOR_LOAD);
		break;
	}
}

void
csidump(void) {
	int i;
	uint c;

	printf("ESC[");
	for(i = 0; i < csiescseq.len; i++) {
		c = csiescseq.buf[i] & 0xff;
		if(isprint(c)) {
			putchar(c);
		} else if(c == '\n') {
			printf("(\\n)");
		} else if(c == '\r') {
			printf("(\\r)");
		} else if(c == 0x1b) {
			printf("(\\e)");
		} else {
			printf("(%02x)", c);
		}
	}
	putchar('\n');
}

void
csireset(void) {
	memset(&csiescseq, 0, sizeof(csiescseq));
}

void
strhandle(void) {
	char *p = NULL;
	int i, j, narg;

	strparse();
	narg = strescseq.narg;

	switch(strescseq.type) {
	case ']': /* OSC -- Operating System Command */
		switch(i = atoi(strescseq.args[0])) {
		case 0:
		case 1:
		case 2:
			if(narg > 1)
				xsettitle(strescseq.args[1]);
			break;
		case 4: /* color set */
			if(narg < 3)
				break;
			p = strescseq.args[2];
			/* fall through */
		case 104: /* color reset, here p = NULL */
			j = (narg > 1) ? atoi(strescseq.args[1]) : -1;
			if (!xsetcolorname(j, p)) {
				fprintf(stderr, "erresc: invalid color %s\n", p);
			} else {
				/*
				 * TODO if defaultbg color is changed, borders
				 * are dirty
				 */
				redraw(0);
			}
			break;
		default:
			fprintf(stderr, "erresc: unknown str ");
			strdump();
			break;
		}
		break;
	case 'k': /* old title set compatibility */
		xsettitle(strescseq.args[0]);
		break;
	case 'P': /* DSC -- Device Control String */
	case '_': /* APC -- Application Program Command */
	case '^': /* PM -- Privacy Message */
	default:
		fprintf(stderr, "erresc: unknown str ");
		strdump();
		/* die(""); */
		break;
	}
}

void
strparse(void) {
	char *p = strescseq.buf;

	strescseq.narg = 0;
	strescseq.buf[strescseq.len] = '\0';
	while(p && strescseq.narg < STR_ARG_SIZ)
		strescseq.args[strescseq.narg++] = strsep(&p, ";");
}

void
strdump(void) {
	int i;
	uint c;

	printf("ESC%c", strescseq.type);
	for(i = 0; i < strescseq.len; i++) {
		c = strescseq.buf[i] & 0xff;
		if(c == '\0') {
			return;
		} else if(isprint(c)) {
			putchar(c);
		} else if(c == '\n') {
			printf("(\\n)");
		} else if(c == '\r') {
			printf("(\\r)");
		} else if(c == 0x1b) {
			printf("(\\e)");
		} else {
			printf("(%02x)", c);
		}
	}
	printf("ESC\\\n");
}

void
strreset(void) {
	memset(&strescseq, 0, sizeof(strescseq));
}

void
tputtab(Term *term, bool forward) {
	uint x = term->c.x;

	if(forward) {
		if(x == term->col)
			return;
		for(++x; x < term->col && !term->tabs[x]; ++x)
			/* nothing */ ;
	} else {
		if(x == 0)
			return;
		for(--x; x > 0 && !term->tabs[x]; --x)
			/* nothing */ ;
	}
	tmoveto(term, x, term->c.y);
}

void
techo(Term *term, char *buf, int len) {
	for(; len > 0; buf++, len--) {
		char c = *buf;

		if(c == '\033') {		/* escape */
			tputc(term, "^", 1);
			tputc(term, "[", 1);
		} else if(c < '\x20') {	/* control code */
			if(c != '\n' && c != '\r' && c != '\t') {
				c |= '\x40';
				tputc(term, "^", 1);
			}
			tputc(term, &c, 1);
		} else {
			break;
		}
	}
	if(len)
		tputc(term, buf, len);
}

void
tputc(Term *term, char *c, int len) {
	uchar ascii = *c;
	bool control = ascii < '\x20' || ascii == 0177;

	if(iofd != -1) {
		if(xwrite(iofd, c, len) < 0) {
			fprintf(stderr, "Error writing in %s:%s\n",
				opt_io, strerror(errno));
			close(iofd);
			iofd = -1;
		}
	}

	/*
	 * STR sequences must be checked before anything else
	 * because it can use some control codes as part of the sequence.
	 */
	if(term->esc & ESC_STR) {
		switch(ascii) {
		case '\033':
			term->esc = ESC_START | ESC_STR_END;
			break;
		case '\a': /* backwards compatibility to xterm */
			term->esc = 0;
			strhandle();
			break;
		default:
			if(strescseq.len + len < sizeof(strescseq.buf) - 1) {
				memmove(&strescseq.buf[strescseq.len], c, len);
				strescseq.len += len;
			} else {
			/*
			 * Here is a bug in terminals. If the user never sends
			 * some code to stop the str or esc command, then st
			 * will stop responding. But this is better than
			 * silently failing with unknown characters. At least
			 * then users will report back.
			 *
			 * In the case users ever get fixed, here is the code:
			 */
			/*
			 * term->esc = 0;
			 * strhandle();
			 */
			}
		}
		return;
	}

	/*
	 * Actions of control codes must be performed as soon they arrive
	 * because they can be embedded inside a control sequence, and
	 * they must not cause conflicts with sequences.
	 */
	if(control) {
		switch(ascii) {
		case '\t':	/* HT */
			tputtab(term, 1);
			return;
		case '\b':	/* BS */
			tmoveto(term, term->c.x-1, term->c.y);
			return;
		case '\r':	/* CR */
			tmoveto(term, 0, term->c.y);
			return;
		case '\f':	/* LF */
		case '\v':	/* VT */
		case '\n':	/* LF */
			/* go to first col if the mode is set */
			tnewline(term, IS_SET(term, MODE_CRLF));
			return;
		case '\a':	/* BEL */
			if(!(xw.state & WIN_FOCUSED))
				xseturgency(1);
			return;
		case '\033':	/* ESC */
			csireset();
			term->esc = ESC_START;
			return;
		case '\016':	/* SO */
		case '\017':	/* SI */
			/*
			 * Different charsets are hard to handle. Applications
			 * should use the right alt charset escapes for the
			 * only reason they still exist: line drawing. The
			 * rest is incompatible history st should not support.
			 */
			return;
		case '\032':	/* SUB */
		case '\030':	/* CAN */
			csireset();
			return;
		case '\005':	/* ENQ (IGNORED) */
		case '\000':	/* NUL (IGNORED) */
		case '\021':	/* XON (IGNORED) */
		case '\023':	/* XOFF (IGNORED) */
		case 0177:	/* DEL (IGNORED) */
			return;
		}
	} else if(term->esc & ESC_START) {
		if(term->esc & ESC_CSI) {
			csiescseq.buf[csiescseq.len++] = ascii;
			if(BETWEEN(ascii, 0x40, 0x7E)
					|| csiescseq.len >= \
					sizeof(csiescseq.buf)-1) {
				term->esc = 0;
				csiparse();
				csihandle(term);
			}
		} else if(term->esc & ESC_STR_END) {
			term->esc = 0;
			if(ascii == '\\')
				strhandle();
		} else if(term->esc & ESC_ALTCHARSET) {
			switch(ascii) {
			case '0': /* Line drawing set */
				term->c.attr.mode |= ATTR_GFX;
				break;
			case 'B': /* USASCII */
				term->c.attr.mode &= ~ATTR_GFX;
				break;
			case 'A': /* UK (IGNORED) */
			case '<': /* multinational charset (IGNORED) */
			case '5': /* Finnish (IGNORED) */
			case 'C': /* Finnish (IGNORED) */
			case 'K': /* German (IGNORED) */
				break;
			default:
				fprintf(stderr, "esc unhandled charset: ESC ( %c\n", ascii);
			}
			term->esc = 0;
		} else if(term->esc & ESC_TEST) {
			if(ascii == '8') { /* DEC screen alignment test. */
				char E[UTF_SIZ] = "E";
				int x, y;

				for(x = 0; x < term->col; ++x) {
					for(y = 0; y < term->row; ++y)
						tsetchar(term, E, &term->c.attr, x, y);
				}
			}
			term->esc = 0;
		} else {
			switch(ascii) {
			case '[':
				term->esc |= ESC_CSI;
				break;
			case '#':
				term->esc |= ESC_TEST;
				break;
			case 'P': /* DCS -- Device Control String */
			case '_': /* APC -- Application Program Command */
			case '^': /* PM -- Privacy Message */
			case ']': /* OSC -- Operating System Command */
			case 'k': /* old title set compatibility */
				strreset();
				strescseq.type = ascii;
				term->esc |= ESC_STR;
				break;
			case '(': /* set primary charset G0 */
				term->esc |= ESC_ALTCHARSET;
				break;
			case ')': /* set secondary charset G1 (IGNORED) */
			case '*': /* set tertiary charset G2 (IGNORED) */
			case '+': /* set quaternary charset G3 (IGNORED) */
				term->esc = 0;
				break;
			case 'D': /* IND -- Linefeed */
				if(term->c.y == term->bot) {
					tscrollup(term, term->top, 1);
				} else {
					tmoveto(term, term->c.x, term->c.y+1);
				}
				term->esc = 0;
				break;
			case 'E': /* NEL -- Next line */
				tnewline(term, 1); /* always go to first col */
				term->esc = 0;
				break;
			case 'H': /* HTS -- Horizontal tab stop */
				term->tabs[term->c.x] = 1;
				term->esc = 0;
				break;
			case 'M': /* RI -- Reverse index */
				if(term->c.y == term->top) {
					tscrolldown(term, term->top, 1);
				} else {
					tmoveto(term, term->c.x, term->c.y-1);
				}
				term->esc = 0;
				break;
			case 'Z': /* DECID -- Identify Terminal */
				ttywrite(term, VT102ID, sizeof(VT102ID) - 1);
				term->esc = 0;
				break;
			case 'c': /* RIS -- Reset to inital state */
				treset(term);
				term->esc = 0;
				xresettitle();
				break;
			case '=': /* DECPAM -- Application keypad */
				term->mode |= MODE_APPKEYPAD;
				term->esc = 0;
				break;
			case '>': /* DECPNM -- Normal keypad */
				term->mode &= ~MODE_APPKEYPAD;
				term->esc = 0;
				break;
			case '7': /* DECSC -- Save Cursor */
				tcursor(term, CURSOR_SAVE);
				term->esc = 0;
				break;
			case '8': /* DECRC -- Restore Cursor */
				tcursor(term, CURSOR_LOAD);
				term->esc = 0;
				break;
			case '\\': /* ST -- Stop */
				term->esc = 0;
				break;
			default:
				fprintf(stderr, "erresc: unknown sequence ESC 0x%02X '%c'\n",
					(uchar) ascii, isprint(ascii)? ascii:'.');
				term->esc = 0;
			}
		}
		/*
		 * All characters which form part of a sequence are not
		 * printed
		 */
		return;
	}
	/*
	 * Display control codes only if we are in graphic mode
	 */
	if(control && !(term->c.attr.mode & ATTR_GFX))
		return;
	if(sel.bx != -1 && BETWEEN(term->c.y, sel.by, sel.ey))
		sel.bx = -1;
	if(IS_SET(term, MODE_WRAP) && (term->c.state & CURSOR_WRAPNEXT)) {
		term->line[term->c.y][term->c.x].mode |= ATTR_WRAP;
		tnewline(term, 1);
	}

	if(IS_SET(term, MODE_INSERT) && term->c.x+1 < term->col) {
		memmove(&term->line[term->c.y][term->c.x+1],
			&term->line[term->c.y][term->c.x],
			(term->col - term->c.x - 1) * sizeof(Glyph));
	}

	tsetchar(term, c, &term->c.attr, term->c.x, term->c.y);
	if(term->c.x+1 < term->col) {
		tmoveto(term, term->c.x+1, term->c.y);
	} else {
		term->c.state |= CURSOR_WRAPNEXT;
	}
}

int
tresize(Term *term, int col, int row) {
	int i;
	int minrow = MIN(row, term->row);
	int mincol = MIN(col, term->col);
	int slide = term->c.y - row + 1;
	bool *bp;
	Line *orig;

	if(col < 1 || row < 1)
		return 0;

	/* free unneeded rows */
	i = 0;
	if(slide > 0) {
		/*
		 * slide screen to keep cursor where we expect it -
		 * tscrollup would work here, but we can optimize to
		 * memmove because we're freeing the earlier lines
		 */
		for(/* i = 0 */; i < slide; i++) {
			free(term->line[i]);
			free(term->alt[i]);
		}
		memmove(term->line, term->line + slide, row * sizeof(Line));
		memmove(term->alt, term->alt + slide, row * sizeof(Line));
		memmove(term->last_line, term->last_line + slide, row * sizeof(Line));
	}
	for(i += row; i < term->row; i++) {
		free(term->line[i]);
		free(term->alt[i]);
		free(term->last_line[i]);
	}

	/* resize to new height */
	term->line = xrealloc(term->line, row * sizeof(Line));
	term->alt  = xrealloc(term->alt,  row * sizeof(Line));
	term->dirty = xrealloc(term->dirty, row * sizeof(*term->dirty));
	term->tabs = xrealloc(term->tabs, col * sizeof(*term->tabs));
	term->last_line = xrealloc(term->last_line, row * sizeof(Line));

	/* resize each row to new width, zero-pad if needed */
	for(i = 0; i < minrow; i++) {
		term->dirty[i] = 1;
		term->line[i] = xrealloc(term->line[i], col * sizeof(Glyph));
		term->alt[i]  = xrealloc(term->alt[i],  col * sizeof(Glyph));
		term->last_line[i] = xrealloc(term->last_line[i], col * sizeof(Glyph));
	}

	/* allocate any new rows */
	for(/* i == minrow */; i < row; i++) {
		term->dirty[i] = 1;
		term->line[i] = xcalloc(col, sizeof(Glyph));
		term->alt [i] = xcalloc(col, sizeof(Glyph));
		term->last_line[i] = xcalloc(col, sizeof(Glyph));
	}
	if(col > term->col) {
		bp = term->tabs + term->col;

		memset(bp, 0, sizeof(*term->tabs) * (col - term->col));
		while(--bp > term->tabs && !*bp)
			/* nothing */ ;
		for(bp += tabspaces; bp < term->tabs + col; bp += tabspaces)
			*bp = 1;
	}
	/* update terminal size */
	term->col = col;
	term->row = row;
	/* reset scrolling region */
	tsetscroll(term, 0, row-1);
	/* make use of the LIMIT in tmoveto */
	tmoveto(term, term->c.x, term->c.y);
	/* Clearing both screens */
	orig = term->line;
	do {
		if(mincol < col && 0 < minrow) {
			tclearregion(term, mincol, 0, col - 1, minrow - 1);
		}
		if(0 < col && minrow < row) {
			tclearregion(term, 0, minrow, col - 1, row - 1);
		}
		tswapscreen(term);
	} while(orig != term->line);

	return (slide > 0);
}

void
xresize(int col, int row) {
	xw.tw = MAX(1, col * xw.cw);
	xw.th = MAX(1, row * xw.ch);

	XFreePixmap(xw.dpy, xw.buf);
	xw.buf = XCreatePixmap(xw.dpy, xw.win, xw.w, xw.h,
			DefaultDepth(xw.dpy, xw.scr));
	XftDrawChange(xw.draw, xw.buf);
	xclear(0, 0, xw.w, xw.h);
}

static inline ushort
sixd_to_16bit(int x) {
	return x == 0 ? 0 : 0x3737 + 0x2828 * x;
}

void
xloadcols(void) {
	int i, r, g, b;
	XRenderColor color = { .alpha = 0xffff };

	/* load colors [0-15] colors and [256-LEN(colorname)[ (config.h) */
	for(i = 0; i < LEN(colorname); i++) {
		if(!colorname[i])
			continue;
		if(!XftColorAllocName(xw.dpy, xw.vis, xw.cmap, colorname[i], &dc.col[i])) {
			die("Could not allocate color '%s'\n", colorname[i]);
		}
	}

	/* load colors [16-255] ; same colors as xterm */
	for(i = 16, r = 0; r < 6; r++) {
		for(g = 0; g < 6; g++) {
			for(b = 0; b < 6; b++) {
				color.red = sixd_to_16bit(r);
				color.green = sixd_to_16bit(g);
				color.blue = sixd_to_16bit(b);
				if(!XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &color, &dc.col[i])) {
					die("Could not allocate color %d\n", i);
				}
				i++;
			}
		}
	}

	for(r = 0; r < 24; r++, i++) {
		color.red = color.green = color.blue = 0x0808 + 0x0a0a * r;
		if(!XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &color,
					&dc.col[i])) {
			die("Could not allocate color %d\n", i);
		}
	}
}

int
xsetcolorname(int x, const char *name) {
	XRenderColor color = { .alpha = 0xffff };
	Colour colour;
	if (x < 0 || x > LEN(colorname))
		return -1;
	if(!name) {
		if(16 <= x && x < 16 + 216) {
			int r = (x - 16) / 36, g = ((x - 16) % 36) / 6, b = (x - 16) % 6;
			color.red = sixd_to_16bit(r);
			color.green = sixd_to_16bit(g);
			color.blue = sixd_to_16bit(b);
			if(!XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &color, &colour))
				return 0; /* something went wrong */
			dc.col[x] = colour;
			return 1;
		} else if (16 + 216 <= x && x < 256) {
			color.red = color.green = color.blue = 0x0808 + 0x0a0a * (x - (16 + 216));
			if(!XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &color, &colour))
				return 0; /* something went wrong */
			dc.col[x] = colour;
			return 1;
		} else {
			name = colorname[x];
		}
	}
	if(!XftColorAllocName(xw.dpy, xw.vis, xw.cmap, name, &colour))
		return 0;
	dc.col[x] = colour;
	return 1;
}

void
xtermclear(int col1, int row1, int col2, int row2) {
	XftDrawRect(xw.draw,
			&dc.col[IS_SET(focused_term, MODE_REVERSE) ? defaultfg : defaultbg],
			borderpx + col1 * xw.cw,
			borderpx + row1 * xw.ch,
			(col2-col1+1) * xw.cw,
			(row2-row1+1) * xw.ch);
}

/*
 * Absolute coordinates.
 */
void
xclear(int x1, int y1, int x2, int y2) {
	XftDrawRect(xw.draw,
			&dc.col[IS_SET(focused_term, MODE_REVERSE)? defaultfg : defaultbg],
			x1, y1, x2-x1, y2-y1);
}

void
xhints(void) {
	XClassHint class = {opt_class ? opt_class : termname, termname};
	XWMHints wm = {.flags = InputHint, .input = 1};
	XSizeHints *sizeh = NULL;

	sizeh = XAllocSizeHints();
	if(xw.isfixed == False) {
		sizeh->flags = PSize | PResizeInc | PBaseSize;
		sizeh->height = xw.h;
		sizeh->width = xw.w;
		sizeh->height_inc = xw.ch;
		sizeh->width_inc = xw.cw;
		sizeh->base_height = 2 * borderpx;
		sizeh->base_width = 2 * borderpx;
	} else {
		sizeh->flags = PMaxSize | PMinSize;
		sizeh->min_width = sizeh->max_width = xw.fw;
		sizeh->min_height = sizeh->max_height = xw.fh;
	}

	XSetWMProperties(xw.dpy, xw.win, NULL, NULL, NULL, 0, sizeh, &wm, &class);
	XFree(sizeh);
}

int
xloadfont(Font *f, FcPattern *pattern) {
	FcPattern *match;
	FcResult result;

	match = FcFontMatch(NULL, pattern, &result);
	if(!match)
		return 1;

	if(!(f->match = XftFontOpenPattern(xw.dpy, match))) {
		FcPatternDestroy(match);
		return 1;
	}

	f->set = NULL;
	f->pattern = FcPatternDuplicate(pattern);

	f->ascent = f->match->ascent;
	f->descent = f->match->descent;
	f->lbearing = 0;
	f->rbearing = f->match->max_advance_width;

	f->height = f->ascent + f->descent;
	f->width = f->lbearing + f->rbearing;

	return 0;
}

void
xloadfonts(char *fontstr, int fontsize) {
	FcPattern *pattern;
	FcResult result;
	double fontval;

	if(fontstr[0] == '-') {
		pattern = XftXlfdParse(fontstr, False, False);
	} else {
		pattern = FcNameParse((FcChar8 *)fontstr);
	}

	if(!pattern)
		die("st: can't open font %s\n", fontstr);

	if(fontsize > 0) {
		FcPatternDel(pattern, FC_PIXEL_SIZE);
		FcPatternAddDouble(pattern, FC_PIXEL_SIZE, (double)fontsize);
		usedfontsize = fontsize;
	} else {
		result = FcPatternGetDouble(pattern, FC_PIXEL_SIZE, 0, &fontval);
		if(result == FcResultMatch) {
			usedfontsize = (int)fontval;
		} else {
			/*
			 * Default font size is 12, if none given. This is to
			 * have a known usedfontsize value.
			 */
			FcPatternAddDouble(pattern, FC_PIXEL_SIZE, 12);
			usedfontsize = 12;
		}
	}

	FcConfigSubstitute(0, pattern, FcMatchPattern);
	FcDefaultSubstitute(pattern);

	if(xloadfont(&dc.font, pattern))
		die("st: can't open font %s\n", fontstr);

	/* Setting character width and height. */
	xw.cw = dc.font.width;
	xw.ch = dc.font.height;

	FcPatternDel(pattern, FC_SLANT);
	FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ITALIC);
	if(xloadfont(&dc.ifont, pattern))
		die("st: can't open font %s\n", fontstr);

	FcPatternDel(pattern, FC_WEIGHT);
	FcPatternAddInteger(pattern, FC_WEIGHT, FC_WEIGHT_BOLD);
	if(xloadfont(&dc.ibfont, pattern))
		die("st: can't open font %s\n", fontstr);

	FcPatternDel(pattern, FC_SLANT);
	FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ROMAN);
	if(xloadfont(&dc.bfont, pattern))
		die("st: can't open font %s\n", fontstr);

	FcPatternDestroy(pattern);
}

int
xloadfontset(Font *f) {
	FcResult result;

	if(!(f->set = FcFontSort(0, f->pattern, FcTrue, 0, &result)))
		return 1;
	return 0;
}

void
xunloadfont(Font *f) {
	XftFontClose(xw.dpy, f->match);
	FcPatternDestroy(f->pattern);
	if(f->set)
		FcFontSetDestroy(f->set);
}

void
xunloadfonts(void) {
	int i, ip;

	/*
	 * Free the loaded fonts in the font cache. This is done backwards
	 * from the frccur.
	 */
	for(i = 0, ip = frccur; i < frclen; i++, ip--) {
		if(ip < 0)
			ip = LEN(frc) - 1;
		XftFontClose(xw.dpy, frc[ip].font);
	}
	frccur = -1;
	frclen = 0;

	xunloadfont(&dc.font);
	xunloadfont(&dc.bfont);
	xunloadfont(&dc.ifont);
	xunloadfont(&dc.ibfont);
}

void
xzoom(const Arg *arg) {
	xunloadfonts();
	xloadfonts(usedfont, usedfontsize + arg->i);
	cresize(0, 0);
	redraw(0);
}

void
xinit(void) {
	XSetWindowAttributes attrs;
	XGCValues gcvalues;
	Cursor cursor;
	Window parent;
	int sw, sh;

	if(!(xw.dpy = XOpenDisplay(NULL)))
		die("Can't open display\n");
	xw.scr = XDefaultScreen(xw.dpy);
	xw.vis = XDefaultVisual(xw.dpy, xw.scr);

	/* font */
	if(!FcInit())
		die("Could not init fontconfig.\n");

	usedfont = (opt_font == NULL)? font : opt_font;
	xloadfonts(usedfont, 0);

	/* colors */
	xw.cmap = XDefaultColormap(xw.dpy, xw.scr);
	xloadcols();

	/* adjust fixed window geometry */
	if(xw.isfixed) {
		sw = DisplayWidth(xw.dpy, xw.scr);
		sh = DisplayHeight(xw.dpy, xw.scr);
		if(xw.fx < 0)
			xw.fx = sw + xw.fx - xw.fw - 1;
		if(xw.fy < 0)
			xw.fy = sh + xw.fy - xw.fh - 1;

		xw.h = xw.fh;
		xw.w = xw.fw;
	} else {
		/* window - default size */
		xw.h = 2 * borderpx + focused_term->row * xw.ch;
		xw.w = 2 * borderpx + focused_term->col * xw.cw;
		xw.fx = 0;
		xw.fy = 0;
	}

	/* Events */
	attrs.background_pixel = dc.col[defaultbg].pixel;
	attrs.border_pixel = dc.col[defaultbg].pixel;
	attrs.bit_gravity = NorthWestGravity;
	attrs.event_mask = FocusChangeMask | KeyPressMask
		| ExposureMask | VisibilityChangeMask | StructureNotifyMask
		| ButtonMotionMask | ButtonPressMask | ButtonReleaseMask;
	attrs.colormap = xw.cmap;

	parent = opt_embed ? strtol(opt_embed, NULL, 0) : \
			XRootWindow(xw.dpy, xw.scr);
	xw.win = XCreateWindow(xw.dpy, parent, xw.fx, xw.fy,
			xw.w, xw.h, 0, XDefaultDepth(xw.dpy, xw.scr), InputOutput,
			xw.vis,
			CWBackPixel | CWBorderPixel | CWBitGravity | CWEventMask
			| CWColormap,
			&attrs);

	memset(&gcvalues, 0, sizeof(gcvalues));
	gcvalues.graphics_exposures = False;
	dc.gc = XCreateGC(xw.dpy, parent, GCGraphicsExposures,
			&gcvalues);
	xw.buf = XCreatePixmap(xw.dpy, xw.win, xw.w, xw.h,
			DefaultDepth(xw.dpy, xw.scr));
	XSetForeground(xw.dpy, dc.gc, dc.col[defaultbg].pixel);
	XFillRectangle(xw.dpy, xw.buf, dc.gc, 0, 0, xw.w, xw.h);

	/* Xft rendering context */
	xw.draw = XftDrawCreate(xw.dpy, xw.buf, xw.vis, xw.cmap);

	/* input methods */
	if((xw.xim =  XOpenIM(xw.dpy, NULL, NULL, NULL)) == NULL) {
		XSetLocaleModifiers("@im=local");
		if((xw.xim =  XOpenIM(xw.dpy, NULL, NULL, NULL)) == NULL) {
			XSetLocaleModifiers("@im=");
			if((xw.xim = XOpenIM(xw.dpy,
					NULL, NULL, NULL)) == NULL) {
				die("XOpenIM failed. Could not open input"
					" device.\n");
			}
		}
	}
	xw.xic = XCreateIC(xw.xim, XNInputStyle, XIMPreeditNothing
					   | XIMStatusNothing, XNClientWindow, xw.win,
					   XNFocusWindow, xw.win, NULL);
	if(xw.xic == NULL)
		die("XCreateIC failed. Could not obtain input method.\n");

	/* white cursor, black outline */
	cursor = XCreateFontCursor(xw.dpy, XC_xterm);
	XDefineCursor(xw.dpy, xw.win, cursor);
	XRecolorCursor(xw.dpy, cursor,
		&(XColor){.red = 0xffff, .green = 0xffff, .blue = 0xffff},
		&(XColor){.red = 0x0000, .green = 0x0000, .blue = 0x0000});

	xw.xembed = XInternAtom(xw.dpy, "_XEMBED", False);
	xw.wmdeletewin = XInternAtom(xw.dpy, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(xw.dpy, xw.win, &xw.wmdeletewin, 1);

	xresettitle();
	XMapWindow(xw.dpy, xw.win);
	xhints();
	XSync(xw.dpy, 0);
}

void
xdraws(char *s, Glyph base, int x, int y, int charlen, int bytelen) {
	int winx = borderpx + x * xw.cw, winy = borderpx + y * xw.ch,
	    width = charlen * xw.cw, xp, i;
	int frp, frcflags;
	int u8fl, u8fblen, u8cblen, doesexist;
	char *u8c, *u8fs;
	long u8char;
	Font *font = &dc.font;
	FcResult fcres;
	FcPattern *fcpattern, *fontpattern;
	FcFontSet *fcsets[] = { NULL };
	FcCharSet *fccharset;
	Colour *fg, *bg, *temp, revfg, revbg;
	XRenderColor colfg, colbg;
	Rectangle r;

	frcflags = FRC_NORMAL;

	if(base.mode & ATTR_ITALIC) {
		if(base.fg == defaultfg)
			base.fg = defaultitalic;
		font = &dc.ifont;
		frcflags = FRC_ITALIC;
	} else if((base.mode & ATTR_ITALIC) && (base.mode & ATTR_BOLD)) {
		if(base.fg == defaultfg)
			base.fg = defaultitalic;
		font = &dc.ibfont;
		frcflags = FRC_ITALICBOLD;
	} else if(base.mode & ATTR_UNDERLINE) {
		if(base.fg == defaultfg)
			base.fg = defaultunderline;
	}
	fg = &dc.col[base.fg];
	bg = &dc.col[base.bg];

	if(base.mode & ATTR_BOLD) {
		if(BETWEEN(base.fg, 0, 7)) {
			/* basic system colors */
			fg = &dc.col[base.fg + 8];
		} else if(BETWEEN(base.fg, 16, 195)) {
			/* 256 colors */
			fg = &dc.col[base.fg + 36];
		} else if(BETWEEN(base.fg, 232, 251)) {
			/* greyscale */
			fg = &dc.col[base.fg + 4];
		}
		/*
		 * Those ranges will not be brightened:
		 *	8 - 15 – bright system colors
		 *	196 - 231 – highest 256 color cube
		 *	252 - 255 – brightest colors in greyscale
		 */
		font = &dc.bfont;
		frcflags = FRC_BOLD;
	}

	if(IS_SET(focused_term, MODE_REVERSE)) {
		if(fg == &dc.col[defaultfg]) {
			fg = &dc.col[defaultbg];
		} else {
			colfg.red = ~fg->color.red;
			colfg.green = ~fg->color.green;
			colfg.blue = ~fg->color.blue;
			colfg.alpha = fg->color.alpha;
			XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &colfg, &revfg);
			fg = &revfg;
		}

		if(bg == &dc.col[defaultbg]) {
			bg = &dc.col[defaultfg];
		} else {
			colbg.red = ~bg->color.red;
			colbg.green = ~bg->color.green;
			colbg.blue = ~bg->color.blue;
			colbg.alpha = bg->color.alpha;
			XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &colbg, &revbg);
			bg = &revbg;
		}
	}

	if(base.mode & ATTR_REVERSE) {
		temp = fg;
		fg = bg;
		bg = temp;
	}

	if(base.mode & ATTR_BLINK && focused_term->mode & MODE_BLINK)
		fg = bg;

	/* Intelligent cleaning up of the borders. */
	if(x == 0) {
		xclear(0, (y == 0)? 0 : winy, borderpx,
			winy + xw.ch + ((y >= focused_term->row-1)? xw.h : 0));
	}
	if(x + charlen >= focused_term->col) {
		xclear(winx + width, (y == 0)? 0 : winy, xw.w,
			((y >= focused_term->row-1)? xw.h : (winy + xw.ch)));
	}
	if(y == 0)
		xclear(winx, 0, winx + width, borderpx);
	if(y == focused_term->row-1)
		xclear(winx, winy + xw.ch, winx + width, xw.h);

	/* Clean up the region we want to draw to. */
	XftDrawRect(xw.draw, bg, winx, winy, width, xw.ch);

	/* Set the clip region because Xft is sometimes dirty. */
	r.x = 0;
	r.y = 0;
	r.height = xw.ch;
	r.width = width;
	XftDrawSetClipRectangles(xw.draw, winx, winy, &r, 1);

	for(xp = winx; bytelen > 0;) {
		/*
		 * Search for the range in the to be printed string of glyphs
		 * that are in the main font. Then print that range. If
		 * some glyph is found that is not in the font, do the
		 * fallback dance.
		 */
		u8fs = s;
		u8fblen = 0;
		u8fl = 0;
		for(;;) {
			u8c = s;
			u8cblen = utf8decode(s, &u8char);
			s += u8cblen;
			bytelen -= u8cblen;

			doesexist = XftCharIndex(xw.dpy, font->match, u8char);
			if(!doesexist || bytelen <= 0) {
				if(bytelen <= 0) {
					if(doesexist) {
						u8fl++;
						u8fblen += u8cblen;
					}
				}

				if(u8fl > 0) {
					XftDrawStringUtf8(xw.draw, fg,
							font->match, xp,
							winy + font->ascent,
							(FcChar8 *)u8fs,
							u8fblen);
					xp += font->width * u8fl;

				}
				break;
			}

			u8fl++;
			u8fblen += u8cblen;
		}
		if(doesexist)
			break;

		frp = frccur;
		/* Search the font cache. */
		for(i = 0; i < frclen; i++, frp--) {
			if(frp <= 0)
				frp = LEN(frc) - 1;

			if(frc[frp].c == u8char
					&& frc[frp].flags == frcflags) {
				break;
			}
		}

		/* Nothing was found. */
		if(i >= frclen) {
			if(!font->set)
				xloadfontset(font);
			fcsets[0] = font->set;

			/*
			 * Nothing was found in the cache. Now use
			 * some dozen of Fontconfig calls to get the
			 * font for one single character.
			 */
			fcpattern = FcPatternDuplicate(font->pattern);
			fccharset = FcCharSetCreate();

			FcCharSetAddChar(fccharset, u8char);
			FcPatternAddCharSet(fcpattern, FC_CHARSET,
					fccharset);
			FcPatternAddBool(fcpattern, FC_SCALABLE,
					FcTrue);

			FcConfigSubstitute(0, fcpattern,
					FcMatchPattern);
			FcDefaultSubstitute(fcpattern);

			fontpattern = FcFontSetMatch(0, fcsets,
					FcTrue, fcpattern, &fcres);

			/*
			 * Overwrite or create the new cache entry.
			 */
			frccur++;
			frclen++;
			if(frccur >= LEN(frc))
				frccur = 0;
			if(frclen > LEN(frc)) {
				frclen = LEN(frc);
				XftFontClose(xw.dpy, frc[frccur].font);
			}

			frc[frccur].font = XftFontOpenPattern(xw.dpy,
					fontpattern);
			frc[frccur].c = u8char;
			frc[frccur].flags = frcflags;

			FcPatternDestroy(fcpattern);
			FcCharSetDestroy(fccharset);

			frp = frccur;
		}

		XftDrawStringUtf8(xw.draw, fg, frc[frp].font,
				xp, winy + frc[frp].font->ascent,
				(FcChar8 *)u8c, u8cblen);

		xp += font->width;
	}

	/*
	XftDrawStringUtf8(xw.draw, fg, font->set, winx,
			winy + font->ascent, (FcChar8 *)s, bytelen);
	*/

	if(base.mode & ATTR_UNDERLINE) {
		XftDrawRect(xw.draw, fg, winx, winy + font->ascent + 1,
				width, 1);
	}

	/* Reset clip to none. */
	XftDrawSetClip(xw.draw, 0);
}

void
xdrawcursor(void) {
	static int oldx = 0, oldy = 0;
	int sl;
	Glyph g = {{' '}, ATTR_NULL, defaultbg, defaultcs};

	LIMIT(oldx, 0, focused_term->col-1);
	LIMIT(oldy, 0, focused_term->row-1);

	if (focused_term->ybase == 0 || select_mode) {
		memcpy(g.c, focused_term->line[focused_term->c.y][focused_term->c.x].c, UTF_SIZ);
	} else {
		return;
	}

	/* remove the old cursor */
	sl = utf8size(focused_term->line[oldy][oldx].c);
	xdraws(focused_term->line[oldy][oldx].c, focused_term->line[oldy][oldx], oldx,
			oldy, 1, sl);

	/* draw the new one */
	if(!(IS_SET(focused_term, MODE_HIDE))) {
		if(xw.state & WIN_FOCUSED) {
			if(IS_SET(focused_term, MODE_REVERSE)) {
				g.mode |= ATTR_REVERSE;
				g.fg = defaultcs;
				g.bg = defaultfg;
			}

			sl = utf8size(g.c);
			xdraws(g.c, g, focused_term->c.x, focused_term->c.y, 1, sl);
		} else {
			XftDrawRect(xw.draw, &dc.col[defaultcs],
					borderpx + focused_term->c.x * xw.cw,
					borderpx + focused_term->c.y * xw.ch,
					xw.cw - 1, 1);
			XftDrawRect(xw.draw, &dc.col[defaultcs],
					borderpx + focused_term->c.x * xw.cw,
					borderpx + focused_term->c.y * xw.ch,
					1, xw.ch - 1);
			XftDrawRect(xw.draw, &dc.col[defaultcs],
					borderpx + (focused_term->c.x + 1) * xw.cw - 1,
					borderpx + focused_term->c.y * xw.ch,
					1, xw.ch - 1);
			XftDrawRect(xw.draw, &dc.col[defaultcs],
					borderpx + focused_term->c.x * xw.cw,
					borderpx + (focused_term->c.y + 1) * xw.ch - 1,
					xw.cw, 1);
		}
		oldx = focused_term->c.x, oldy = focused_term->c.y;
	}
}


void
xsettitle(char *p) {
	XTextProperty prop;

	Xutf8TextListToTextProperty(xw.dpy, &p, 1, XUTF8StringStyle,
			&prop);
	XSetWMName(xw.dpy, xw.win, &prop);
}

void
xresettitle(void) {
	xsettitle(opt_title ? opt_title : "st");
}

void
redraw(int timeout) {
	struct timespec tv = {0, timeout * 1000};

	tfulldirt(focused_term);
	draw();

	if(timeout > 0) {
		nanosleep(&tv, NULL);
		XSync(xw.dpy, False); /* necessary for a good tput flash */
	}
}

void
draw(void) {
	drawregion(0, 0, focused_term->col, focused_term->row);
	XCopyArea(xw.dpy, xw.buf, xw.win, dc.gc, 0, 0, xw.w,
			xw.h, 0, 0);
	XSetForeground(xw.dpy, dc.gc,
			dc.col[IS_SET(focused_term, MODE_REVERSE)?
				defaultfg : defaultbg].pixel);
}

void
drawregion(int x1, int y1, int x2, int y2) {
	int ic, ib, x, y, ox, sl;
	Glyph base, new;
	char buf[DRAW_BUF_SIZ];
	bool ena_sel = sel.bx != -1;

	if(sel.alt ^ IS_SET(focused_term, MODE_ALTSCREEN))
		ena_sel = 0;

	if(!(xw.state & WIN_VISIBLE))
		return;

	for(y = y1; y < y2; y++) {
		if(!focused_term->dirty[y])
			continue;

		xtermclear(0, y, focused_term->col, y);
		focused_term->dirty[y] = 0;
		base = focused_term->line[y][0];
		ic = ib = ox = 0;
		for(x = x1; x < x2; x++) {
			new = focused_term->line[y][x];
			if(ena_sel && selected(x, y))
				new.mode ^= ATTR_REVERSE;
			if(ib > 0 && (ATTRCMP(base, new)
					|| ib >= DRAW_BUF_SIZ-UTF_SIZ)) {
				xdraws(buf, base, ox, y, ic, ib);
				ic = ib = 0;
			}
			if(ib == 0) {
				ox = x;
				base = new;
			}

			sl = utf8size(new.c);
			memcpy(buf+ib, new.c, sl);
			ib += sl;
			++ic;
		}
		if(ib > 0)
			xdraws(buf, base, ox, y, ic, ib);
	}

	xdrawcursor();
	xdrawbar();
}

void
xdrawbar(void) {
	// if (!bar_needs_refresh) return;
	// bar_needs_refresh = false;

	// for autohide
	if (!terms->next) return;

	int i = 0;
	int drawn = 0;
	Term *term;
	Glyph attr = {{' '}, ATTR_NULL, defaultfg, defaultbg};
	char buf[20];
	int buflen;

	for (term = terms; term; term = term->next) {
		i++;
		if (term == focused_term) {
			snprintf(buf, 20, "[%d]", i);
			attr.mode = ATTR_NULL;
			attr.fg = 15;
			attr.bg = defaultbg;
		} else {
			//if (term->has_activity) {
			//	term->has_activity = false;
			//	snprintf(buf, 20, " %d*", i);
			//} else
			snprintf(buf, 20, " %d ", i);
			attr.mode = ATTR_NULL;
			attr.fg = 6;
			attr.bg = defaultbg;
		}
		buflen = strlen(buf);
		if (drawn + buflen > term->col) {
			break;
		}
		xdraws(buf, attr, drawn, focused_term->row, buflen, buflen);
		drawn += 1;
		drawn += buflen;
	}

	// TODO: Make message appear longer - mark in event loop.
	if (status_msg) {
		int l = strlen(status_msg);
		attr.mode = ATTR_NULL;
		attr.fg = 1;
		attr.bg = defaultbg;
		drawn += 1;
		if (drawn + l > focused_term->col) {
			return;
		}
		xdraws(status_msg, attr, drawn, focused_term->row, l, l);
		free(status_msg);
		status_msg = NULL;
	}
}

void
expose(XEvent *ev) {
	XExposeEvent *e = &ev->xexpose;

	if(xw.state & WIN_REDRAW) {
		if(!e->count)
			xw.state &= ~WIN_REDRAW;
	}
	redraw(0);
}

void
visibility(XEvent *ev) {
	XVisibilityEvent *e = &ev->xvisibility;

	if(e->state == VisibilityFullyObscured) {
		xw.state &= ~WIN_VISIBLE;
	} else if(!(xw.state & WIN_VISIBLE)) {
		/* need a full redraw for next Expose, not just a buf copy */
		xw.state |= WIN_VISIBLE | WIN_REDRAW;
	}
}

void
unmap(XEvent *ev) {
	xw.state &= ~WIN_VISIBLE;
}

void
xseturgency(int add) {
	XWMHints *h = XGetWMHints(xw.dpy, xw.win);

	h->flags = add ? (h->flags | XUrgencyHint) : (h->flags & ~XUrgencyHint);
	XSetWMHints(xw.dpy, xw.win, h);
	XFree(h);
}

void
focus(XEvent *ev) {
	XFocusChangeEvent *e = &ev->xfocus;

	if(e->mode == NotifyGrab)
		return;

	if(ev->type == FocusIn) {
		XSetICFocus(xw.xic);
		xw.state |= WIN_FOCUSED;
		xseturgency(0);
	} else {
		XUnsetICFocus(xw.xic);
		xw.state &= ~WIN_FOCUSED;
	}
}

inline bool
match(uint mask, uint state) {
	state &= ~(ignoremod);

	if(mask == XK_NO_MOD && state)
		return false;
	if(mask != XK_ANY_MOD && mask != XK_NO_MOD && !state)
		return false;
	if((state & mask) != state)
		return false;
	return true;
}

void
numlock(const Arg *dummy) {
	focused_term->numlock ^= 1;
}

char*
kmap(KeySym k, uint state) {
	uint mask;
	Key *kp;
	int i;

	/* Check for mapped keys out of X11 function keys. */
	for(i = 0; i < LEN(mappedkeys); i++) {
		if(mappedkeys[i] == k)
			break;
	}
	if(i == LEN(mappedkeys)) {
		if((k & 0xFFFF) < 0xFD00)
			return NULL;
	}

	for(kp = key; kp < key + LEN(key); kp++) {
		mask = kp->mask;

		if(kp->k != k)
			continue;

		if(!match(mask, state))
			continue;

		if(kp->appkey > 0) {
			if(!IS_SET(focused_term, MODE_APPKEYPAD))
				continue;
			if(focused_term->numlock && kp->appkey == 2)
				continue;
		} else if(kp->appkey < 0 && IS_SET(focused_term, MODE_APPKEYPAD)) {
			continue;
		}

		if((kp->appcursor < 0 && IS_SET(focused_term, MODE_APPCURSOR)) ||
				(kp->appcursor > 0
				 && !IS_SET(focused_term, MODE_APPCURSOR))) {
			continue;
		}

		if((kp->crlf < 0 && IS_SET(focused_term, MODE_CRLF)) ||
				(kp->crlf > 0 && !IS_SET(focused_term, MODE_CRLF))) {
			continue;
		}

		return kp->s;
	}

	return NULL;
}

void
kpress(XEvent *ev) {
	XKeyEvent *e = &ev->xkey;
	KeySym ksym;
	char xstr[31], buf[32], *customkey, *cp = buf;
	int len, ret;
	long c;
	Status status;
	Shortcut *bp;

	if(IS_SET(focused_term, MODE_KBDLOCK))
		return;

	len = XmbLookupString(xw.xic, e, xstr, sizeof(xstr), &ksym, &status);
	e->state &= ~Mod2Mask;

#define CREATE_MEVENT \
	XEvent ev; \
	ev.xbutton.button = Button1; \
	ev.xbutton.state |= Button1Mask; \
	ev.xbutton.x = col2x(term, term->c.x); \
	ev.xbutton.y = row2y(term, term->c.y);

#define CREATE_BPRESS do { \
	CREATE_MEVENT; \
	bpress(&ev); \
} while (0)

#define CREATE_BMOTION do { \
	CREATE_MEVENT; \
	bmotion(&ev); \
} while (0)

#define CREATE_BRELEASE do { \
	CREATE_MEVENT; \
	brelease(&ev); \
} while (0)

	/* 0. prefix - C-a */
	if (select_mode) {
		Term *term = focused_term;
		if (ksym == XK_q) {
			if (visual_mode) {
				CREATE_BRELEASE;
				CREATE_BPRESS;
				// Not necessary (?):
				CREATE_BRELEASE;
				visual_mode = false;
			}
			select_mode = false;
			//tcursor(term, CURSOR_LOAD);
			//if (cursor_was_hidden) term->mode |= MODE_HIDE;

			tscrollback(term, normal_cursor.ybase - term->ybase);
			//term->ybase = normal_cursor.ybase;

			tmoveto(term, normal_cursor.x, normal_cursor.y);
			if (normal_cursor.hidden) term->mode |= MODE_HIDE;
			redraw(0);
		} else if (ksym == XK_slash) {
			// start_search = true;
		} else if (ksym == XK_h) {
			tmoveto(term, term->c.x - 1, term->c.y);
			if (visual_mode) CREATE_BMOTION;
		} else if (ksym == XK_j) {
			if (term->c.y == term->row - 1) {
				tscrollback(term, 1);
			} else {
				tmoveto(term, term->c.x, term->c.y + 1);
			}
			if (visual_mode) CREATE_BMOTION;
		} else if (ksym == XK_k) {
			if (term->c.y == 0) {
				tscrollback(term, -1);
			} else {
				tmoveto(term, term->c.x, term->c.y - 1);
			}
			if (visual_mode) CREATE_BMOTION;
		} else if (ksym == XK_l) {
			tmoveto(term, term->c.x + 1, term->c.y);
			if (visual_mode) CREATE_BMOTION;
		} else if (ksym == XK_0 || ksym == XK_asciicircum) {
			tmoveto(term, 0, term->c.y);
			if (visual_mode) CREATE_BMOTION;
		} else if (ksym == XK_w || ksym == XK_W) {
			int cx = term->c.x;
			// TODO: Make a get_real_line() function, which checks scrollback.
			// TODO: Handle scrollback when the alternate buffer is active.
			// TODO: NOTE: selsnap and other functions may need to be updated to
			// use ybase in line[].
			Glyph *l = term->line[term->c.y];
			bool saw_space = false;
			while (cx < term->col) {
				if (l[cx].c && l[cx].c[0] <= ' ') {
					saw_space = true;
				} else if (saw_space) {
					break;
				}
				cx++;
			}
			tmoveto(term, cx, term->c.y);
			if (visual_mode) CREATE_BMOTION;
		} else if (ksym == XK_e || ksym == XK_E) {
			int cx = term->c.x;
			Glyph *l = term->line[term->c.y];
			bool saw_space = false;
			while (cx < term->col) {
				if (l[cx].c && l[cx].c[0] <= ' ') {
					if (saw_space && (cx - 1 >= 0 && l[cx-1].c && l[cx-1].c[0] > ' ')) {
						cx--;
						break;
					} else {
						saw_space = true;
					}
				}
				cx++;
			}
			tmoveto(term, cx, term->c.y);
			if (visual_mode) CREATE_BMOTION;
		} else if (ksym == XK_b || ksym == XK_B) {
			int cx = term->c.x;
			Glyph *l = term->line[term->c.y];
			bool saw_space = false;
			while (cx >= 0) {
				if (l[cx].c && l[cx].c[0] <= ' ') {
					if (saw_space && (cx + 1 < term->col && l[cx+1].c && l[cx+1].c[0] > ' ')) {
						cx++;
						break;
					} else {
						saw_space = true;
					}
				}
				cx--;
			}
			tmoveto(term, cx, term->c.y);
			if (visual_mode) CREATE_BMOTION;
		} else if (ksym == XK_dollar) {
			tmoveto(term, term->col - 1, term->c.y);
			if (visual_mode) CREATE_BMOTION;
		} else if (ksym == XK_braceleft) {
			if (term->c.y == 0) {
				tscrollback(term, -(term->row / 5));
			} else {
				tmoveto(term, term->c.x, term->c.y - term->row / 5);
			}
			if (visual_mode) CREATE_BMOTION;
		} else if (ksym == XK_braceright) {
			if (term->c.y == term->row - 1) {
				tscrollback(term, term->row / 5);
			} else {
				tmoveto(term, term->c.x, term->c.y + term->row / 5);
			}
			if (visual_mode) CREATE_BMOTION;
		} else if (ksym == XK_u && match(ControlMask, e->state)) {
			tscrollback(term, -(term->row / 2));
			if (visual_mode) CREATE_BMOTION;
		} else if (ksym == XK_d && match(ControlMask, e->state)) {
			tscrollback(term, term->row / 2);
			if (visual_mode) CREATE_BMOTION;
		} else if (ksym == XK_b && match(ControlMask, e->state)) {
			tscrollback(term, -term->row);
			if (visual_mode) CREATE_BMOTION;
		} else if (ksym == XK_f && match(ControlMask, e->state)) {
			tscrollback(term, term->row);
			if (visual_mode) CREATE_BMOTION;
		} else if (ksym == XK_v) {
			visual_mode = true;
			CREATE_BPRESS;
		} else if (ksym == XK_y) {
			if (visual_mode) {
				CREATE_BRELEASE;
				CREATE_BPRESS;
				// Not necessary (?):
				CREATE_BRELEASE;
				visual_mode = false;
				select_mode = false;
				//tcursor(term, CURSOR_LOAD);
				//if (cursor_was_hidden) term->mode |= MODE_HIDE;

				tscrollback(term, normal_cursor.ybase - term->ybase);
				//term->ybase = normal_cursor.ybase;

				tmoveto(term, normal_cursor.x, normal_cursor.y);
				if (normal_cursor.hidden) term->mode |= MODE_HIDE;
				redraw(0);
			}
		}
		return;
	}
#undef CREATE_MEVENT
#undef CREATE_BPRESS
#undef CREATE_BMOTION
#undef CREATE_BRELEASE

	if (ksym == XK_a && match(ControlMask, e->state)) {
		if (!prefix_active) {
			prefix_active = true;
			return;
		}
	}
	if (prefix_active) {
		Term *term = focused_term;
		if (ksym == XK_bracketleft) {
			select_mode = true;

			//tcursor(term, CURSOR_SAVE);
			//cursor_was_hidden = term->mode & MODE_HIDE;
			//term->mode &= ~MODE_HIDE;
			normal_cursor.x = term->c.x;
			normal_cursor.y = term->c.y;
			normal_cursor.hidden = term->mode & MODE_HIDE;
			normal_cursor.ybase = term->ybase;
			term->mode &= ~MODE_HIDE;

			tmoveto(term, 0, term->row - 1);
		} else if (ksym == XK_p) {
			selpaste(NULL);
		} else if (ksym == XK_c) {
			term_add();
		} else if (ksym == XK_k) {
			term_remove(term);
		} else if (ksym >= XK_1 && ksym <= XK_9) {
			term_focus_idx(ksym - XK_0);
		} else if (ksym == XK_N) {
			term_focus_prev(term);
		} else if (ksym == XK_n) {
			term_focus_next(term);
		}
		prefix_active = false;
		return;
	}

	/* 1. shortcuts */
	for(bp = shortcuts; bp < shortcuts + LEN(shortcuts); bp++) {
		if(ksym == bp->keysym && match(bp->mod, e->state)) {
			bp->func(&(bp->arg));
			return;
		}
	}

	/* 2. custom keys from config.h */
	if((customkey = kmap(ksym, e->state))) {
		len = strlen(customkey);
		memcpy(buf, customkey, len);
	/* 3. hardcoded (overrides X lookup) */
	} else {
		if(len == 0)
			return;

		if(len == 1 && e->state & Mod1Mask) {
			if(IS_SET(focused_term, MODE_8BIT)) {
				if(*xstr < 0177) {
					c = *xstr | B7;
					ret = utf8encode(&c, cp);
					cp += ret;
					len = 0;
				}
			} else {
				*cp++ = '\033';
			}
		}

		memcpy(cp, xstr, len);
		len = cp - buf + len;
	}

	ttywrite(focused_term, buf, len);
	if(IS_SET(focused_term, MODE_ECHO))
		techo(focused_term, buf, len);
}


void
cmessage(XEvent *e) {
	/*
	 * See xembed specs
	 *  http://standards.freedesktop.org/xembed-spec/xembed-spec-latest.html
	 */
	if(e->xclient.message_type == xw.xembed && e->xclient.format == 32) {
		if(e->xclient.data.l[1] == XEMBED_FOCUS_IN) {
			xw.state |= WIN_FOCUSED;
			xseturgency(0);
		} else if(e->xclient.data.l[1] == XEMBED_FOCUS_OUT) {
			xw.state &= ~WIN_FOCUSED;
		}
	} else if(e->xclient.data.l[0] == xw.wmdeletewin) {
		/* Send SIGHUP to shell */
		Term *term;
		for (term = terms; term; term = term->next) {
			kill(term->pid, SIGHUP);
		}
		exit(EXIT_SUCCESS);
	}
}

void
cresize(int width, int height) {
	int col, row;

	if(width != 0)
		xw.w = width;
	if(height != 0)
		xw.h = height;

	col = (xw.w - 2 * borderpx) / xw.cw;
	row = (xw.h - 2 * borderpx) / xw.ch;

	if (terms->next) { // <- for autohide
		//row = MAX(row - 1, 0);
		if (--row < 0) row = 0;
	}

	Term *term;
	for (term = terms; term; term = term->next) {
		tresize(term, col, row);
	}
	xresize(col, row);
	for (term = terms; term; term = term->next) {
		ttyresize(term);
	}
}

void
resize(XEvent *e) {
	if(e->xconfigure.width == xw.w && e->xconfigure.height == xw.h)
		return;

	cresize(e->xconfigure.width, e->xconfigure.height);
}

void
term_add(void) {
	Term *term;

	//bar_needs_refresh = true;

	if (!terms) {
		terms = (Term *)xmalloc(sizeof(Term));
		memset(terms, 0, sizeof(Term));
		focused_term = terms;
		tnew(focused_term, 80, 24);
	} else {
		for (term = terms; term; term = term->next) {
			if (!term->next) break;
		}
		if (!term) die("no terminal found\n");
		term->next = (Term *)xmalloc(sizeof(Term));
		focused_term = term->next;
		tnew(focused_term, terms->col, terms->row);
	}

	focused_term->sb = scrollback_create();

	ttynew(focused_term);

	// for autohide
	// just got two tabs
	if (terms->next && !terms->next->next) {
		cresize(0, 0);
	}

	redraw(0);
}

void
term_remove(Term *target) {
	//bar_needs_refresh = true;
	if (terms == target) {
		terms = terms->next;
		if (!terms) exit(EXIT_SUCCESS);
		focused_term = terms;
	} else {
		Term *term;
		for (term = terms; term; term = term->next) {
			if (term->next == target) break;
		}
		if (!term) die("no terminal found\n");
		term->next = target->next;
		focused_term = term;
	}
	free(target);

	// for autohide
	// just fell back to one tab
	// NOTE: Why does this randomly segfault without `terms &&`?
	// This is probably a sign of a bigger problem.
	if (terms && !terms->next) {
		cresize(0, 0);
	}

	redraw(0);
}

void
term_focus(Term *target) {
	//bar_needs_refresh = true;
	focused_term = target == NULL ? terms : target;
	redraw(0);
}

void
term_focus_prev(Term *target) {
	Term *term;
	for (term = terms; term; term = term->next) {
		if (term->next == target) {
			break;
		}
	}
	term_focus(term);
}

void
term_focus_next(Term *target) {
	term_focus(target ? target->next : NULL);
}

Term *
term_focus_idx(int tab) {
	int i = 0;
	Term *term;
	for (term = terms; term; term = term->next) {
		if (++i == tab) {
			term_focus(term);
			break;
		}
	}
	return term;
}

void
run(void) {
	XEvent ev;
	fd_set rfd;
	int xfd = XConnectionNumber(xw.dpy), xev, blinkset = 0, dodraw = 0;
	struct timeval drawtimeout, *tv = NULL, now, last, lastblink;

	gettimeofday(&lastblink, NULL);
	gettimeofday(&last, NULL);

	for(xev = actionfps;;) {
		FD_ZERO(&rfd);
		Term *term;
		for (term = terms; term; term = term->next) {
			FD_SET(term->cmdfd, &rfd);
		}
		FD_SET(xfd, &rfd);

		int lastfd = 0;
		for (term = terms; term; term = term->next) {
			lastfd = MAX(term->cmdfd, lastfd);
		}
		if(select(MAX(xfd, lastfd)+1, &rfd, NULL, NULL, tv) < 0) {
			if(errno == EINTR)
				continue;
			die("select failed: %s\n", SERRNO);
		}
		for (term = terms; term; term = term->next) {
			if(FD_ISSET(term->cmdfd, &rfd)) {
				ttyread(term);
				if(blinktimeout) {
					blinkset = tattrset(term, ATTR_BLINK);
					if(!blinkset && term->mode & ATTR_BLINK)
						term->mode &= ~(MODE_BLINK);
				}
			}
		}

		if(FD_ISSET(xfd, &rfd))
			xev = actionfps;

		gettimeofday(&now, NULL);
		drawtimeout.tv_sec = 0;
		drawtimeout.tv_usec = (1000/xfps) * 1000;
		tv = &drawtimeout;

		dodraw = 0;
		if(blinktimeout && TIMEDIFF(now, lastblink) > blinktimeout) {
			for (term = terms; term; term = term->next) {
				tsetdirtattr(term, ATTR_BLINK);
				term->mode ^= MODE_BLINK;
			}
			gettimeofday(&lastblink, NULL);
			dodraw = 1;
		}
		if(TIMEDIFF(now, last) \
				> (xev? (1000/xfps) : (1000/actionfps))) {
			dodraw = 1;
			last = now;
		}

		if(dodraw) {
			while(XPending(xw.dpy)) {
				XNextEvent(xw.dpy, &ev);
				if(XFilterEvent(&ev, None))
					continue;
				if(handler[ev.type])
					(handler[ev.type])(&ev);
			}

			draw();
			XFlush(xw.dpy);

			if(xev && !FD_ISSET(xfd, &rfd))
				xev--;
			if(!FD_ISSET(focused_term->cmdfd, &rfd) && !FD_ISSET(xfd, &rfd)) {
				if(blinkset) {
					if(TIMEDIFF(now, lastblink) \
							> blinktimeout) {
						drawtimeout.tv_usec = 1;
					} else {
						drawtimeout.tv_usec = (1000 * \
							(blinktimeout - \
							TIMEDIFF(now,
								lastblink)));
					}
				} else {
					tv = NULL;
				}
			}
		}
	}
}

void
usage(void) {
	die("%s " VERSION " (c) 2010-2013 st engineers\n" \
	"usage: st [-a] [-v] [-c class] [-f font] [-g geometry] [-o file]" \
	" [-t title] [-w windowid] [-e command ...]\n", argv0);
}

int
main(int argc, char *argv[]) {
	int bitm, xr, yr;
	uint wr, hr;

	xw.fw = xw.fh = xw.fx = xw.fy = 0;
	xw.isfixed = False;

	ARGBEGIN {
	case 'a':
		allowaltscreen = false;
		break;
	case 'c':
		opt_class = EARGF(usage());
		break;
	case 'e':
		/* eat all remaining arguments */
		if(argc > 1)
			opt_cmd = &argv[1];
		goto run;
	case 'f':
		opt_font = EARGF(usage());
		break;
	case 'g':
		bitm = XParseGeometry(EARGF(usage()), &xr, &yr, &wr, &hr);
		if(bitm & XValue)
			xw.fx = xr;
		if(bitm & YValue)
			xw.fy = yr;
		if(bitm & WidthValue)
			xw.fw = (int)wr;
		if(bitm & HeightValue)
			xw.fh = (int)hr;
		if(bitm & XNegative && xw.fx == 0)
			xw.fx = -1;
		if(bitm & XNegative && xw.fy == 0)
			xw.fy = -1;

		if(xw.fh != 0 && xw.fw != 0)
			xw.isfixed = True;
		break;
	case 'o':
		opt_io = EARGF(usage());
		break;
	case 't':
		opt_title = EARGF(usage());
		break;
	case 'w':
		opt_embed = EARGF(usage());
		break;
	case 'v':
	default:
		usage();
	} ARGEND;

run:
	setlocale(LC_CTYPE, "");
	XSetLocaleModifiers("");
	// term_add();
	terms = (Term *)xmalloc(sizeof(Term));
	memset(terms, 0, sizeof(Term));
	focused_term = terms;
	tnew(focused_term, 80, 24);
	terms->sb = scrollback_create();
	xinit();
	ttynew(focused_term);
	selinit();
	if(xw.isfixed)
		cresize(xw.h, xw.w);
	run();

	return 0;
}

