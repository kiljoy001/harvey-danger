/*
 * Submit the same fragment-job clear from Linux userspace, to separate the
 * descriptor from gpufs' kernel-side setup.  No libdrm: the three ioctls are
 * declared directly from the panfrost UAPI.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>

#define DRM_COMMAND_BASE 0x40
#define DRM_IOCTL_BASE 'd'
#define DRM_IOW(nr, type) _IOW(DRM_IOCTL_BASE, nr, type)
#define DRM_IOWR(nr, type) _IOWR(DRM_IOCTL_BASE, nr, type)

struct drm_panfrost_submit {
	uint64_t jc;
	uint64_t in_syncs;
	uint32_t in_sync_count;
	uint32_t out_sync;
	uint64_t bo_handles;
	uint32_t bo_handle_count;
	uint32_t requirements;
};

struct drm_panfrost_wait_bo {
	uint32_t handle;
	uint32_t pad;
	int64_t timeout_ns;
};

struct drm_panfrost_create_bo {
	uint32_t size;
	uint32_t flags;
	uint32_t handle;
	uint32_t pad;
	uint64_t offset;
};

struct drm_panfrost_mmap_bo {
	uint32_t handle;
	uint32_t flags;
	uint64_t offset;
};

#define IOCTL_SUBMIT    DRM_IOW(DRM_COMMAND_BASE + 0x00, struct drm_panfrost_submit)
#define IOCTL_WAIT_BO   DRM_IOW(DRM_COMMAND_BASE + 0x01, struct drm_panfrost_wait_bo)
#define IOCTL_CREATE_BO DRM_IOWR(DRM_COMMAND_BASE + 0x02, struct drm_panfrost_create_bo)
#define IOCTL_MMAP_BO   DRM_IOWR(DRM_COMMAND_BASE + 0x03, struct drm_panfrost_mmap_bo)

static int
create_bo(int fd, uint32_t size, uint32_t *handle, uint64_t *va)
{
	struct drm_panfrost_create_bo c = { .size = size };

	if(ioctl(fd, IOCTL_CREATE_BO, &c) < 0){
		perror("CREATE_BO");
		return -1;
	}
	*handle = c.handle;
	*va = c.offset;
	return 0;
}

static void *
map_bo(int fd, uint32_t handle, uint32_t size)
{
	struct drm_panfrost_mmap_bo m = { .handle = handle };
	void *p;

	if(ioctl(fd, IOCTL_MMAP_BO, &m) < 0){
		perror("MMAP_BO");
		return MAP_FAILED;
	}
	p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, m.offset);
	if(p == MAP_FAILED)
		perror("mmap");
	return p;
}

int
main(void)
{
	int fd, i;
	uint32_t jh, rh, w, pix, *job, *rt;
	uint64_t jva, rva, slva;
	uint32_t handles[2];
	struct drm_panfrost_submit s;
	struct drm_panfrost_wait_bo wb;
	void *jmap, *rmap;
	const uint32_t W = 64, H = 64, STRIDE = 64*4, COLOR = 0xFF0000FF;

	fd = open("/dev/dri/renderD128", O_RDWR);
	if(fd < 0){
		perror("open");
		return 1;
	}
	if(create_bo(fd, 4096, &jh, &jva) < 0 || create_bo(fd, W*H*4, &rh, &rva) < 0)
		return 1;
	printf("PANCLR job va %#llx handle %u, rt va %#llx handle %u\n",
	       (unsigned long long)jva, jh, (unsigned long long)rva, rh);

	jmap = map_bo(fd, jh, 4096);
	rmap = map_bo(fd, rh, W*H*4);
	if(jmap == MAP_FAILED || rmap == MAP_FAILED)
		return 1;

	memset(jmap, 0, 4096);
	memset(rmap, 0, W*H*4);
	job = jmap;

	/* Sample locations at +0x200. */
	slva = jva + 0x200;
	for(i = 0; i < 32; i++)
		job[(0x200/4) + i] = i == 0 ? 0 : 0x8080;

	/* Framebuffer parameters. */
	job[4] = (uint32_t)slva;
	job[5] = (uint32_t)(slva >> 32);
	job[8] = (W - 1) | ((H - 1) << 16);
	job[9] = 0;
	job[10] = (W - 1) | ((H - 1) << 16);
	job[11] = (2 << 6) | (8 << 9) | (1 << 24);
	job[12] = 1 << 16;		/* z internal format D24 */

	/* Colour render target at 0x80, linear. */
	rt = job + 0x80/4;
	rt[0] = 1 << 26;
	rt[1] = 1 | (19 << 3) | (2 << 8) | (1 << 15) | (1672 << 16) | (1u << 31);
	rt[8] = (uint32_t)rva;
	rt[9] = (uint32_t)(rva >> 32);
	rt[10] = STRIDE;
	rt[11] = 0;
	rt[12] = COLOR;
	rt[13] = COLOR;
	rt[14] = COLOR;
	rt[15] = COLOR;

	/* Fragment job at 0xc0. */
	job[0xc0/4 + 4] = 1 | (9 << 1) | (1 << 16);
	job[0xc0/4 + 8 + 0] = 0;
	job[0xc0/4 + 8 + 1] = ((W/16) - 1) | (((H/16) - 1) << 16);
	job[0xc0/4 + 8 + 2] = (uint32_t)(jva | 1);
	job[0xc0/4 + 8 + 3] = (uint32_t)((jva | 1) >> 32);

	handles[0] = jh;
	handles[1] = rh;
	memset(&s, 0, sizeof s);
	s.jc = jva + 0xc0;
	s.bo_handles = (uint64_t)(uintptr_t)handles;
	s.bo_handle_count = 2;
	s.requirements = 1;		/* PANFROST_JD_REQ_FS -> slot 0 */
	if(ioctl(fd, IOCTL_SUBMIT, &s) < 0){
		perror("SUBMIT");
		return 1;
	}
	printf("PANCLR submitted\n");

	memset(&wb, 0, sizeof wb);
	wb.handle = rh;
	wb.timeout_ns = 2000000000LL;	/* absolute; 0 means now, so use now+2s */
	{
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		wb.timeout_ns = (int64_t)ts.tv_sec*1000000000LL + ts.tv_nsec + 2000000000LL;
	}
	if(ioctl(fd, IOCTL_WAIT_BO, &wb) < 0){
		perror("WAIT_BO");
	} else {
		w = ((volatile uint32_t *)rmap)[0];
		pix = w;
		printf("PANCLR pixel %#x", pix);
		if(pix == COLOR)
			printf("  PASS\n");
		else
			printf("  (expected %#x)\n", COLOR);
	}
	return 0;
}
