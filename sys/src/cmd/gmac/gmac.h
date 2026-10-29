/* gmac.h: interface between the GMAC driver core and the 9P service. */
int gmacinit(void);
int gmactx(void *frame, int len);
int gmacrx(void *buf, int max);
uchar *gmacaddr(void);
int gmaclink(void);
