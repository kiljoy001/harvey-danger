/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef O9_XHCI_H
#define O9_XHCI_H

/* No OS headers: the same engine runs in Prism and the QEMU test harness. */
typedef unsigned char Xbyte;
typedef unsigned int Xword;
typedef unsigned long long Xaddr;
typedef struct Xio Xio;
typedef struct Xring Xring;
typedef struct Xdevice Xdevice;
typedef struct Xhci Xhci;

enum {
	Xslots = 16, Xports = 255, Xeps = 32,
	Xringlen = 32, Xevents = 256, Xmaxdata = 65536,
	Xarena = 4*1024*1024,
	Xok = 0, Xioerr = -1, Xinvalid = -2, Xtimeout = -3,
	Xstall = -4, Xunsupported = -5, Xgone = -6, Xnomem = -7,
};

/* Callbacks return 0 on success. DMA writes must be visible before MMIO
 * writes; reads must synchronize device writes before returning. Physical
 * addresses are supplied by a trusted backend, never by a USB client.
 * All calls on a controller MUST be serialized by its service. */
struct Xio {
	void *arg;
	int (*read32)(void *, Xword, Xword *);
	int (*write32)(void *, Xword, Xword);
	int (*readmem)(void *, Xaddr, void *, Xword);
	int (*writemem)(void *, Xaddr, const void *, Xword);
	void (*delay)(void *, Xword);
	Xaddr dma;
	Xword size, mmiosize;
};
struct Xring {
	Xaddr base;
	Xword next, cycle;
};
struct Xdevice {
	Xword slot, port, speed, maxpkt, configured;
	Xword enabled, halted, maxep;
	Xword packet[Xeps], type[Xeps];
	Xaddr context, input, buffer;
	Xring ep[Xeps];
};
struct Xhci {
	Xio io;
	Xword op, runtime, doorbell, slots, ports, ctxsize, used;
	Xword eventnext, eventcycle, running, failed, completion;
	Xword protocol[Xports+1], slottype[Xports+1];
	Xaddr dcbaa, events, erst;
	Xring command;
	Xdevice device[Xslots+1];
};

int xhci_init(Xhci *, const Xio *);
int xhci_stop(Xhci *);
int xhci_port(Xhci *, unsigned, Xword *);
int xhci_attach(Xhci *, unsigned); /* returns slot, after Address Device */
int xhci_detach(Xhci *, unsigned);
int xhci_control(Xhci *, unsigned, const Xbyte[8], void *, unsigned);
int xhci_configure(Xhci *, unsigned, const Xbyte *, unsigned);
int xhci_transfer(Xhci *, unsigned, unsigned, void *, unsigned);
int xhci_clearhalt(Xhci *, unsigned, unsigned);
int xhci_noop(Xhci *);
/* Parser validates the entire configuration before any controller change. */
int xhci_validate_config(const Xbyte *, unsigned, unsigned);

#endif
