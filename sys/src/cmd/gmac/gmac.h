/* gmac.h: interface between the GMAC driver core and the 9P service. */
int gmacinit(void);
int gmactx(void *frame, int len);
int gmacrx(void *buf, int max);
uchar *gmacaddr(void);
int gmaclink(void);
u32 gmacreg(int off);
int gmacrxidx(void);
u64 gmacrxcpa(void);
u32 gmacrxdesc(int i);
