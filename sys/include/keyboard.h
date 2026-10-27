/*
 * This file is part of the UCB release of Plan 9. It is subject to the license
 * terms in the LICENSE file found in the top-level directory of this
 * distribution and at http://akaros.cs.berkeley.edu/files/Plan9License. No
 * part of the UCB release of Plan 9, including this file, may be copied,
 * modified, propagated, or distributed except according to the terms contained
 * in the LICENSE file.
 */


typedef struct 	Keyboardctl Keyboardctl;
typedef struct	Channel	Channel;

struct	Keyboardctl
{
	Channel	*c;	/* chan(Rune)[20] */

	char		*file;
	int		consfd;		/* to cons file */
	int		ctlfd;		/* to ctl file */
	int		pid;		/* of slave proc */
};


extern	Keyboardctl*	initkeyboard(char*);
extern	int		ctlkeyboard(Keyboardctl*, char*);
extern	void		closekeyboard(Keyboardctl*);

enum {
	KF=	0xF000,	/* Rune: beginning of private Unicode space */
	Spec=	0xF800,
	/* KF|1, KF|2, ..., KF|0xC is F1, F2, ..., F12 */
	Khome=	KF|0x0D,
	Kup=	KF|0x0E,
	Kpgup=	KF|0x0F,
	Kprint=	KF|0x10,
	Kleft=	KF|0x11,
	Kright=	KF|0x12,
	Kdown=	Spec|0x00,
	Kview=	Spec|0x00,
	Kpgdown=	KF|0x13,
	Kins=	KF|0x14,
	Kend=	KF|0x18,

	Kalt=		KF|0x15,
	Kshift=	KF|0x16,
	Kctl=		KF|0x17,
	/* 9front keyboard runes, for sys/src/cmd/rio */
	Kscroll=	KF|0x19,
	Kscrolloneup=	KF|0x20,
	Kscrollonedown=	KF|0x21,
	Kcaps=		Spec|0x64,
	Knum=		Spec|0x65,
	Kaltgr=		Spec|0x67,
	Kmod4=		Spec|0x68,
	Ksbwd=		KF|0x22,
	Ksfwd=		KF|0x23,
	Kpause=		KF|0x24,
	Kvoldn=		KF|0x25,
	Kvolup=		KF|0x26,
	Kmute=		KF|0x27,
	Kbrtdn=		KF|0x28,
	Kbrtup=		KF|0x29,
	Ksoh=		0x01,
	Kstx=		0x02,
	Ketx=		0x03,
	Kenq=		0x05,
	Kack=		0x06,
	Knack=		0x15,
	Ketb=		0x17,
	Kbreak=		Spec|0x61,
	Kmiddle=	Spec|0x66,
	Kmouse=		Spec|0x100,

	Kbs=	0x08,
	Kdel=	0x7f,
	Kesc=	0x1b,
	Keof=	0x04,
};
