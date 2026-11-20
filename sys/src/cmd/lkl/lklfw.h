/* Firmware files embedded in lklboot by util/arm64/build_lkl.py
 * (from util/arm64/fw/) and staged into the guest /lib/firmware
 * by lkl9p, so Linux request_firmware works without a rootfs. */
typedef struct Lklfw Lklfw;
struct Lklfw {
	char	*path;	/* guest path, /lib/firmware/... */
	uchar	*start;
	uchar	*end;
};
extern Lklfw lklfwfiles[];
extern int nlklfwfiles;
