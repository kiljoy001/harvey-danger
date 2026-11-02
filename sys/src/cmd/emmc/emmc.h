/* emmc.h: interface between the eMMC driver core and the 9P server. */
int emmcinit(void);
char *emmcmodename(void);
u64 emmccapacity(void);
int emmcflush(void);
u64 emmcwear(void);
int emmcread(u32 lba, int n, void *buf);
int emmcwrite(u32 lba, int n, void *buf);
