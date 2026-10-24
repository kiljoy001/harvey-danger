/* virtio 1.0 PCI common configuration access, including legacy I/O BARs. */
enum {
	Vconf_devfeatsel = 0,
	Vconf_devfeat = 4,
	Vconf_drvfeatsel = 8,
	Vconf_drvfeat = 12,
	Vconf_msixcfg = 16,
	Vconf_nqueues = 18,
	Vconf_status = 20,
	Vconf_cfggen = 21,
	Vconf_queuesel = 22,
	Vconf_queuesize = 24,
	Vconf_queuemsixvect = 26,
	Vconf_queueenable = 28,
	Vconf_queuenotifyoff = 30,
	Vconf_queuedesc = 32,
	Vconf_queueavail = 40,
	Vconf_queueused = 48,
	Vconf_sz = 56,

	Vio_port = 0,
	Vio_mem,
};

typedef struct Vio Vio;
struct Vio {
	int type;
	union {
		int port;
		uchar *mem;
	};
};

u8int vin8(Vio *, int);
u16int vin16(Vio *, int);
u32int vin32(Vio *, int);
u64int vin64(Vio *, int);
void vout8(Vio *, int, u8int);
void vout16(Vio *, int, u16int);
void vout32(Vio *, int, u32int);
void vout64(Vio *, int, u64int);
void virtiounmap(Vio *, usize);
struct Pcidev;
struct Pcicap;
Vio *virtiomapregs(struct Pcidev *, struct Pcicap *, int, Vio *);
