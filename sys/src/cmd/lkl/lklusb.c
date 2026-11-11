/*
 * lklusb -- the USB family service.
 *
 * LKL owns the DWC3 controller, the USB core, hubs and the class drivers.
 * This file is the Harvey side: it waits for the Linux input device to show
 * up and translates input_event records into Plan 9 /dev/kbd characters.
 *
 * Block devices (/dev/sd*) and a proper /srv/input 9P service come after
 * the keyboard path is proven; the first milestone is a USB keyboard
 * typing into Rio through the LKL-owned controller.
 */
#include <u.h>
#include <libc.h>
#include <lkl/asm/unistd.h>

extern long	lkl_syscall(long, long*);
extern int	lkl_printf(const char*, ...);

typedef struct Uevent Uevent;
struct Uevent {
	long	sec;
	long	usec;
	ushort	type;
	ushort	code;
	int	value;
};

enum {
	EvSyn	= 0,
	EvKey	= 1,
	EvRel	= 2,

	KeyEsc		= 1,
	Key1		= 2,
	Key0		= 11,
	KeyMinus	= 12,
	KeyEqual	= 13,
	KeyBackspace	= 14,
	KeyTab		= 15,
	KeyQ		= 16,
	KeyW		= 17,
	KeyE		= 18,
	KeyR		= 19,
	KeyT		= 20,
	KeyY		= 21,
	KeyU		= 22,
	KeyI		= 23,
	KeyO		= 24,
	KeyP		= 25,
	KeyLeftBrace	= 26,
	KeyRightBrace	= 27,
	KeyEnter	= 28,
	KeyLeftCtrl	= 29,
	KeyA		= 30,
	KeyS		= 31,
	KeyD		= 32,
	KeyF		= 33,
	KeyG		= 34,
	KeyH		= 35,
	KeyJ		= 36,
	KeyK		= 37,
	KeyL		= 38,
	KeySemicolon	= 39,
	KeyApostrophe	= 40,
	KeyGrave	= 41,
	KeyLeftShift	= 42,
	KeyBackslash	= 43,
	KeyZ		= 44,
	KeyX		= 45,
	KeyC		= 46,
	KeyV		= 47,
	KeyB		= 48,
	KeyN		= 49,
	KeyM		= 50,
	KeyComma	= 51,
	KeyDot		= 52,
	KeySlash	= 53,
	KeyRightShift	= 54,
	KeyLeftAlt	= 56,
	KeySpace	= 57,
	KeyRightCtrl	= 97,
	KeyRightAlt	= 100,

	AtFdcwd		= -100,
	O_Rdonly	= 0,
};

static int
lklsleep(long ms)
{
	long p[6];
	struct { long long sec, nsec; } ts;

	ts.sec = ms / 1000;
	ts.nsec = (ms % 1000) * 1000000LL;
	memset(p, 0, sizeof p);
	p[0] = (long)&ts;
	return lkl_syscall(__lkl__NR_nanosleep, p);
}

static long
lklopen(char *path)
{
	long p[6];

	memset(p, 0, sizeof p);
	p[0] = AtFdcwd;
	p[1] = (long)path;
	p[2] = O_Rdonly;
	return lkl_syscall(__lkl__NR_openat, p);
}

static long
lklread(long fd, void *buf, long len)
{
	long p[6];

	memset(p, 0, sizeof p);
	p[0] = fd;
	p[1] = (long)buf;
	p[2] = len;
	return lkl_syscall(__lkl__NR_read, p);
}

static long
lklclose(long fd)
{
	long p[6];

	memset(p, 0, sizeof p);
	p[0] = fd;
	return lkl_syscall(__lkl__NR_close, p);
}

/*
 * One table for the US layout: keycode -> unshifted, shifted.  Zero means
 * the key is not translated yet (function keys, arrows, media keys).
 */
static struct {
	int	code;
	char	plain;
	char	shift;
} keymap[] = {
	{ Key1, '1', '!' }, { Key0, '0', ')' },
	{ KeyMinus, '-', '_' }, { KeyEqual, '=', '+' },
	{ KeyQ, 'q', 'Q' }, { KeyW, 'w', 'W' }, { KeyE, 'e', 'E' },
	{ KeyR, 'r', 'R' }, { KeyT, 't', 'T' }, { KeyY, 'y', 'Y' },
	{ KeyU, 'u', 'U' }, { KeyI, 'i', 'I' }, { KeyO, 'o', 'O' },
	{ KeyP, 'p', 'P' }, { KeyLeftBrace, '[', '{' },
	{ KeyRightBrace, ']', '}' },
	{ KeyA, 'a', 'A' }, { KeyS, 's', 'S' }, { KeyD, 'd', 'D' },
	{ KeyF, 'f', 'F' }, { KeyG, 'g', 'G' }, { KeyH, 'h', 'H' },
	{ KeyJ, 'j', 'J' }, { KeyK, 'k', 'K' }, { KeyL, 'l', 'L' },
	{ KeySemicolon, ';', ':' }, { KeyApostrophe, '\'', '"' },
	{ KeyGrave, '`', '~' }, { KeyBackslash, '\\', '|' },
	{ KeyZ, 'z', 'Z' }, { KeyX, 'x', 'X' }, { KeyC, 'c', 'C' },
	{ KeyV, 'v', 'V' }, { KeyB, 'b', 'B' }, { KeyN, 'n', 'N' },
	{ KeyM, 'm', 'M' }, { KeyComma, ',', '<' }, { KeyDot, '.', '>' },
	{ KeySlash, '/', '?' }, { KeySpace, ' ', ' ' },
};

static int
translate(int code, int shift)
{
	int i;
	char c;

	if(code >= Key1 && code <= Key0){
		/* 2..11 map to 1..0; shift gives the US symbols. */
		static char plain[] = "1234567890";
		static char shifted[] = "!@#$%^&*()";
		c = shift ? shifted[code - Key1] : plain[code - Key1];
		return c;
	}
	for(i = 0; i < nelem(keymap); i++)
		if(keymap[i].code == code)
			return shift ? keymap[i].shift : keymap[i].plain;
	if(code == KeyEnter)
		return '\n';
	if(code == KeyTab)
		return '\t';
	if(code == KeyBackspace)
		return 0x7f;
	if(code == KeyEsc)
		return 0x1b;
	return 0;
}

void
usbservice(void)
{
	long fd, n;
	char path[32];
	Uevent ev;
	int i, shift, kbd, logged;
	char c;

	fd = -1;
	for(i = 0; i < 200 && fd < 0; i++){
		snprint(path, sizeof path, "/dev/input/event0");
		fd = lklopen(path);
		if(fd < 0)
			lklsleep(100);
	}
	if(fd < 0){
		lkl_printf("lklusb: no /dev/input/event0 after 20s\n");
		return;
	}
	lkl_printf("lklusb: input device up (fd %ld)\n", fd);

	kbd = open("/dev/kbd", OWRITE);
	if(kbd < 0)
		lkl_printf("lklusb: /dev/kbd: %r (logging only)\n");

	shift = 0;
	logged = 0;
	for(;;){
		n = lklread(fd, &ev, sizeof ev);
		if(n < (long)sizeof ev){
			lkl_printf("lklusb: read %ld, reopening\n", n);
			lklclose(fd);
			lklsleep(500);
			fd = lklopen(path);
			if(fd < 0)
				lklsleep(1000);
			continue;
		}
		if(ev.type != EvKey)
			continue;
		if(ev.code == KeyLeftShift || ev.code == KeyRightShift){
			shift = ev.value != 0;
			continue;
		}
		if(ev.value == 0)	/* release */
			continue;
		c = translate(ev.code, shift);
		if(c == 0)
			continue;
		if(kbd >= 0){
			if(write(kbd, &c, 1) != 1)
				lkl_printf("lklusb: kbd write: %r\n");
		}
		if(logged < 200){
			lkl_printf("lklusb: key code %d -> %d\n", ev.code, c);
			logged++;
		}
	}
}
