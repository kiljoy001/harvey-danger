/*
 * Protocol initialisation order for the kernel IP stack.
 *
 * Plan 9 generates this table from the "ip" section of the kernel
 * configuration (mkdevc); the Harvey amd64 kernel keeps an explicit list.
 */

#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../ip/ip.h"

extern void	tcpinit(Fs*);
extern void	udpinit(Fs*);
extern void	rudpinit(Fs*);
extern void	ipifcinit(Fs*);
extern void	icmpinit(Fs*);
extern void	icmp6init(Fs*);
extern void	greinit(Fs*);
extern void	ipmuxinit(Fs*);
extern void	ilinit(Fs*);

void (*ipprotoinit[])(Fs*) = {
	tcpinit,
	udpinit,
	rudpinit,
	ipifcinit,
	icmpinit,
	icmp6init,
	greinit,
	ipmuxinit,
	ilinit,
	nil,
};
