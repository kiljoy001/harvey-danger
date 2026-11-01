/*
 * opteeprobe - userspace OP-TEE client probe.
 *
 * Drives #P/tee/smc and #P/dma, registers a shared-memory page, opens a
 * session to the keyroot probe TA, services the REE-FS TA-load RPC, and
 * invokes PING/INFO/DERIVE.
 *
 * This is a bring-up tool, not the keyroot server.  It must run as the
 * hostowner because #P/tee/smc is 0600.
 */
#include <u.h>
#include <libc.h>
#include "api.h"
#include "ta/des/des.h"

enum {
	/* OP-TEE SMC function IDs */
	SmcCallsUid	= 0xbf00ff01,
	SmcCallsRev	= 0xbf00ff03,
	SmcCallWithArg	= 0x32000004,
	SmcReturnFromRpc= 0x32000003,

	/* OP-TEE SMC returns */
	SmcOk		= 0x0,
	RpcPrefix	= 0xffff0000,

	/* message commands */
	CmdOpenSession	= 0,
	CmdInvoke	= 1,
	CmdCloseSession	= 2,
	CmdRegisterShm	= 4,
	CmdUnregisterShm= 5,

	/* parameter attributes */
	AttrValueIn	= 0x1,
	AttrValueOut	= 0x2,
	AttrRmemIn	= 0x5,
	AttrRmemOut	= 0x6,
	AttrTmemIn	= 0x9,
	AttrTmemOut	= 0xa,
	AttrTmemInout	= 0xb,
	AttrMeta	= 0x100,

	/* RPC supplicant commands */
	RpcLoadTa	= 0,
	RpcFs		= 2,
	RpcGetTime	= 3,
	RpcShmAlloc	= 6,
	RpcShmFree	= 7,

	/* RPC_CMD_FS operations */
	FsOpen		= 0,
	FsCreate	= 1,
	FsClose		= 2,
	FsRead		= 3,
	FsWrite		= 4,
	FsTruncate	= 5,
	FsRemove	= 6,
	Maxfd		= 8,

	/* TEE results */
	TeeSuccess	= 0,
	TeeShortBuffer	= 0xffff0010,
	TeeBadParameters= 0xffff0006,
	TeeItemNotFound	= 0xffff0008,
	TeeNotImplemented= 0xffff0009,
	TeeOriginComms	= 2,

	MaxParams	= 4,
	ShmSize		= 16384,
	TaMax		= 512*1024,
	BlockSize	= TaMax + 3*ShmSize,
};

typedef struct MsgParam MsgParam;
struct MsgParam {
	u64 attr;
	union {
		struct { u64 buf_ptr, size, shm_ref; } tmem;
		struct { u64 offs, size, shm_ref; } rmem;
		struct { u64 a, b, c; } value;
	} u;
};

typedef struct MsgArg MsgArg;
struct MsgArg {
	u32 cmd, func, session, cancel_id, pad;
	u32 ret, ret_origin, num_params;
	MsgParam params[MaxParams];
};

static u64 teeshmbase, teeshmsize;
static u8 *teeshmva;
static u64 teebump;
static u64 blockpa;
static MsgArg *arg;
static u64 argpa;
static u8 *shm;
static u64 shmpa;
static u32 session;
static int smcfd;
static char *tapath;
static int ptamode;
static int mldsamode;
static int p9mode;
static int dp9ikmode;
static int mlkemmode;
static int userkeymode;
static int fds[Maxfd];

static u8 tauuid[16] = {
	0x6b, 0x1d, 0x7c, 0x2e, 0x5a, 0x41, 0x4f, 0x0b,
	0x9c, 0x7e, 0x2d, 0x1a, 0x8e, 0x6f, 0x3b, 0x90,
};

/* Vendor rk_crypto_service pseudo-TA. */
static u8 ptauuid[16] = {
	0x0c, 0xac, 0xdb, 0x5d, 0x46, 0x6c, 0x4f, 0xea,
	0x8f, 0x83, 0x52, 0x16, 0x54, 0x3d, 0x16, 0x97,
};

static void
fail(char *fmt, ...)
{
	char buf[256];
	va_list va;

	va_start(va, fmt);
	vsnprint(buf, sizeof buf, fmt, va);
	va_end(va);
	fprint(2, "opteeprobe: %s\n", buf);
	exits("fail");
}

static u64
readnum(char *path)
{
	char buf[64];
	int fd, n;

	fd = open(path, OREAD);
	if(fd < 0)
		fail("open %s: %r", path);
	n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if(n <= 0)
		fail("read %s: %r", path);
	buf[n] = 0;
	return strtoull(buf, nil, 0);
}

/* Bump allocator inside the OP-TEE static NSEC SHM. */
static u64
teeshmalloc(u64 size, u64 align)
{
	u64 a;

	if(align == 0)
		align = 4096;
	a = (teebump + align - 1) & ~(align - 1);
	if(a < teebump || a + size > teeshmbase + teeshmsize)
		fail("teeshm full: need %llud", size);
	teebump = a + size;
	return a;
}

static u8 *
pava(u64 pa)
{
	if(pa < teeshmbase || pa + 8 > teeshmbase + teeshmsize)
		fail("physical %#llx outside teeshm", pa);
	return teeshmva + (pa - teeshmbase);
}

static void
dmacache(int clean, u64 pa, u64 len)
{
	char buf[64], *path;
	int fd;

	path = clean ? "#P/dma/cleaninval" : "#P/dma/inval";
	if((fd = open(path, OWRITE)) < 0)
		fail("open %s: %r", path);
	snprint(buf, sizeof buf, "0x%llx 0x%llx\n", pa, len);
	if(write(fd, buf, strlen(buf)) != strlen(buf))
		print("opteeprobe: dmacache %s pa %#llx len %#llx: %r\n",
		      path, pa, len);
	close(fd);
}

static void
smc(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7,
    u64 *r)
{
	u64 req[8], res[4];
	int n;

	req[0] = a0; req[1] = a1; req[2] = a2; req[3] = a3;
	req[4] = a4; req[5] = a5; req[6] = a6; req[7] = a7;
	n = pwrite(smcfd, req, sizeof req, 0);
	if(n != sizeof req)
		fail("smc write: %r (%d)", n);
	n = pread(smcfd, res, sizeof res, 0);
	if(n != sizeof res)
		fail("smc read: %r (%d)", n);
	r[0] = res[0]; r[1] = res[1]; r[2] = res[2]; r[3] = res[3];
}

static int
isrpc(u64 a0)
{
	return (a0 & 0xffff0000) == RpcPrefix && a0 != 0xffffffff;
}

static void
uuidpath(u32 *vp, char *path, int npath)
{
	char u[40];
	u8 *p = (u8*)vp;

	if(tapath != nil){
		snprint(path, npath, "%s", tapath);
		return;
	}
	snprint(u, sizeof u, "%02x%02x%02x%02x%02x%02x%02x%02x"
		"%02x%02x%02x%02x%02x%02x%02x%02x",
		p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
		p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
	snprint(path, npath, "/lib/optee_armtz/%.8s-%.4s-%.4s-%.4s-%.12s.ta",
		u, u+8, u+12, u+16, u+20);
}

/*
 * Resolve a memref parameter to a physical address and size.  The FS
 * supplicant uses RMEM params whose shm_ref is the cookie we returned
 * from SHM_ALLOC (the physical address).
 */
static int
parampa(MsgParam *p, u64 *pa, u64 *size)
{
	switch(p->attr & 0xff){
	case AttrTmemIn:
	case AttrTmemOut:
		*pa = p->u.tmem.buf_ptr;
		*size = p->u.tmem.size;
		return 0;
	case AttrRmemIn:
	case AttrRmemOut:
		*pa = p->u.rmem.shm_ref + p->u.rmem.offs;
		*size = p->u.rmem.size;
		return 0;
	}
	return -1;
}

static void
setparamsize(MsgParam *p, u64 size)
{
	switch(p->attr & 0xff){
	case AttrTmemIn:
	case AttrTmemOut:
		p->u.tmem.size = size;
		break;
	case AttrRmemIn:
	case AttrRmemOut:
		p->u.rmem.size = size;
		break;
	}
}

/*
 * Map an OP-TEE FS path to a normal-world file.  The TA binary comes
 * from /boot (or the standard path), and the secure-storage files live
 * in the root ramfs.
 */
static void
fsname(char *path, char *name, int nname)
{
	char *p;

	if(strstr(path, "optee_armtz") != nil){
		snprint(name, nname, "%s", tapath != nil ? tapath : path);
		return;
	}
	p = strrchr(path, '/');
	snprint(name, nname, "/mnt/%s", p != nil ? p+1 : path);
}

/*
 * Service one RPC request and rewrite r[] for the resume call.  The
 * request is in a shared-memory message argument allocated by RPC_ALLOC;
 * a single outstanding call keeps this simple.
 */
static void
handle_rpc(u64 *r)
{
	MsgArg *a;
	char path[128];
	u64 pa, size, fsz;
	u8 *va;
	long n;
	int fd;

	switch(r[0] & 0xffff){
	case 0:		/* ALLOC */
		size = r[1];
		pa = teeshmalloc(size, 4096);
		print("rpc: alloc %llud -> pa %#llx\n", size, pa);
		r[0] = SmcReturnFromRpc;
		r[1] = pa >> 32;
		r[2] = pa & 0xffffffff;
		r[4] = pa >> 32;	/* cookie, split like a pointer */
		r[5] = pa & 0xffffffff;
		return;
	case 2:		/* FREE */
		pa = (r[1] << 32) | (r[2] & 0xffffffff);
		print("rpc: free cookie %#llx\n", pa);
		r[0] = SmcReturnFromRpc;
		return;
	case 4:		/* FOREIGN_INTR */
		print("rpc: foreign interrupt\n");
		r[0] = SmcReturnFromRpc;
		return;
	case 5:		/* CMD */
		pa = (r[1] << 32) | (r[2] & 0xffffffff);
		if(pa < teeshmbase ||
		   pa + sizeof(MsgArg) > teeshmbase + teeshmsize){
			print("rpc: bad argument cookie %#llx\n", pa);
			r[0] = SmcReturnFromRpc;
			return;
		}
		dmacache(0, pa, sizeof(MsgArg));
		a = (MsgArg*)pava(pa);
		print("rpc: cmd %ud attrs %#llx/%#llx num %ud\n",
		      a->cmd, a->params[0].attr, a->params[1].attr,
		      a->num_params);
		switch(a->cmd){
		case RpcLoadTa:
			if(a->num_params != 2 ||
			   (a->params[0].attr & 0xff) != AttrValueIn ||
			   (((a->params[1].attr & 0xff) != AttrTmemOut) &&
			    ((a->params[1].attr & 0xff) != AttrRmemOut))){
				a->ret = TeeBadParameters;
				break;
			}
			uuidpath((u32*)&a->params[0].u.value, path,
				 sizeof path);
			if((a->params[1].attr & 0xff) == AttrRmemOut){
				pa = a->params[1].u.rmem.shm_ref;
				size = a->params[1].u.rmem.size;
			}else{
				pa = a->params[1].u.tmem.buf_ptr;
				size = a->params[1].u.tmem.size;
			}
			print("rpc: load ta %s size %llud pa %#llx attr %#llx\n",
			      path, size, pa, a->params[1].attr);
			if((fd = open(path, OREAD)) < 0){
				print("rpc: load ta %s: %r\n", path);
				a->ret = TeeItemNotFound;
				break;
			}
			fsz = seek(fd, 0, 2);
			seek(fd, 0, 0);
			/* A null/short buffer is a size query. */
			if(pa == 0 || size == 0){
				close(fd);
				if((a->params[1].attr & 0xff) == AttrRmemOut)
					a->params[1].u.rmem.size = fsz;
				else
					a->params[1].u.tmem.size = fsz;
				a->ret = TeeSuccess;
				a->ret_origin = TeeOriginComms;
				print("rpc: ta size %llud\n", fsz);
				break;
			}
			if(fsz > size){
				close(fd);
				a->ret = TeeShortBuffer;
				break;
			}
			va = pava(pa);
			n = readn(fd, va, fsz);
			close(fd);
			if((u64)n != fsz){
				a->ret = TeeItemNotFound;
				break;
			}
			dmacache(1, pa, size);
			if((a->params[1].attr & 0xff) == AttrRmemOut)
				a->params[1].u.rmem.size = fsz;
			else
				a->params[1].u.tmem.size = fsz;
			a->ret = TeeSuccess;
			a->ret_origin = TeeOriginComms;
			print("rpc: loaded TA %s (%llud bytes)\n", path, fsz);
			break;
		case RpcFs:
			if(a->num_params < 1 ||
			   (a->params[0].attr & 0xff) != AttrValueIn){
				a->ret = TeeBadParameters;
				break;
			}
			print("rpc: fs op %llud num %ud p1 %#llx/%#llx/%#llx\n",
			      a->params[0].u.value.a, a->num_params,
			      a->num_params > 1 ? a->params[1].u.value.a : 0,
			      a->num_params > 1 ? a->params[1].u.value.b : 0,
			      a->num_params > 1 ? a->params[1].u.value.c : 0);
			switch(a->params[0].u.value.a){
			case FsOpen: {
				u64 ppa, psize;
				char name[128];
				int i, fd;

				if(a->num_params != 3 ||
				   parampa(&a->params[1], &ppa, &psize) < 0 ||
				   psize == 0){
					print("rpc: fs open bad params: %llud %#llx\n",
					      psize, ppa);
					a->ret = TeeBadParameters;
					break;
				}
				if(psize > sizeof path - 1)
					psize = sizeof path - 1;
				memmove(path, pava(ppa), psize);
				path[psize] = 0;
				fsname(path, name, sizeof name);
				fd = open(name, OREAD);
				if(fd < 0){
					print("rpc: fs open %s: %r\n", name);
					a->ret = TeeItemNotFound;
					break;
				}
				for(i = 0; i < Maxfd; i++)
					if(fds[i] < 0)
						break;
				if(i == Maxfd){
					close(fd);
					a->ret = TeeItemNotFound;
					break;
				}
				fds[i] = fd;
				a->params[2].attr = AttrValueOut;
				a->params[2].u.value.a = i;
				a->params[2].u.value.b = 0;
				a->params[2].u.value.c = 0;
				a->ret = TeeSuccess;
				print("rpc: fs open %s -> %d\n", name, i);
				break;
			}
			case FsCreate: {
				u64 ppa, psize;
				char name[128];
				int i, fd;

				if(a->num_params != 3 ||
				   parampa(&a->params[1], &ppa, &psize) < 0 ||
				   psize == 0){
					a->ret = TeeBadParameters;
					break;
				}
				if(psize > sizeof path - 1)
					psize = sizeof path - 1;
				memmove(path, pava(ppa), psize);
				path[psize] = 0;
				fsname(path, name, sizeof name);
				fd = create(name, OWRITE, 0600);
				if(fd < 0){
					print("rpc: fs create %s: %r\n", name);
					a->ret = TeeItemNotFound;
					break;
				}
				for(i = 0; i < Maxfd; i++)
					if(fds[i] < 0)
						break;
				if(i == Maxfd){
					close(fd);
					a->ret = TeeItemNotFound;
					break;
				}
				fds[i] = fd;
				a->params[2].attr = AttrValueOut;
				a->params[2].u.value.a = i;
				a->params[2].u.value.b = 0;
				a->params[2].u.value.c = 0;
				a->ret = TeeSuccess;
				print("rpc: fs create %s -> %d\n", name, i);
				break;
			}
			case FsRead: {
				u64 ppa, psize;
				long n;
				int fd, off;

				if(a->num_params != 2 ||
				   parampa(&a->params[1], &ppa, &psize) < 0){
					a->ret = TeeBadParameters;
					break;
				}
				fd = a->params[0].u.value.b;
				off = a->params[0].u.value.c;
				if(fd < 0 || fd >= Maxfd || fds[fd] < 0){
					a->ret = TeeBadParameters;
					break;
				}
				if(fds[fd] == -2){
					setparamsize(&a->params[1], 0);
					a->ret = TeeSuccess;
					break;
				}
				n = pread(fds[fd], pava(ppa), psize, off);
				if(n < 0){
					a->ret = TeeItemNotFound;
					break;
				}
				setparamsize(&a->params[1], n);
				a->ret = TeeSuccess;
				break;
			}
			case FsClose:
				if(a->num_params < 1){
					a->ret = TeeBadParameters;
					break;
				}
				{
					int fd = a->params[0].u.value.b;
					if(fd >= 0 && fd < Maxfd){
						if(fds[fd] >= 0)
							close(fds[fd]);
						fds[fd] = -1;
					}
				}
				a->ret = TeeSuccess;
				break;
			case FsWrite: {
				u64 ppa, psize;
				long n;
				int fd, off;

				if(a->num_params != 2 ||
				   parampa(&a->params[1], &ppa, &psize) < 0){
					a->ret = TeeBadParameters;
					break;
				}
				fd = a->params[0].u.value.b;
				off = a->params[0].u.value.c;
				if(fd < 0 || fd >= Maxfd || fds[fd] < 0){
					a->ret = TeeBadParameters;
					break;
				}
				if(fds[fd] == -2){
					a->ret = TeeSuccess;
					break;
				}
				n = pwrite(fds[fd], pava(ppa), psize, off);
				a->ret = n < 0 ? TeeItemNotFound : TeeSuccess;
				break;
			}
			case FsTruncate:
				a->ret = TeeSuccess;
				break;
			case FsRemove: {
				u64 ppa, psize;
				char name[128];

				if(a->num_params >= 2 &&
				   parampa(&a->params[1], &ppa, &psize) == 0 &&
				   psize > 0){
					if(psize > sizeof path - 1)
						psize = sizeof path - 1;
					memmove(path, pava(ppa), psize);
					path[psize] = 0;
					fsname(path, name, sizeof name);
					remove(name);
				}
				a->ret = TeeSuccess;
				break;
			}
			default:
				print("rpc: fs op %llud unsupported\n",
				      a->params[0].u.value.a);
				a->ret = TeeNotImplemented;
				break;
			}
			break;
		case RpcShmAlloc:
			if(a->num_params != 1 ||
			   (a->params[0].attr & 0xff) != AttrValueIn){
				a->ret = TeeBadParameters;
				break;
			}
			size = a->params[0].u.value.b;
			print("rpc: shm_alloc type %llud size %llud align %llud\n",
			      a->params[0].u.value.a, size,
			      a->params[0].u.value.c);
			pa = teeshmalloc(size, 4096);
			print("rpc: shm_alloc -> pa %#llx\n", pa);
			a->params[0].attr = AttrTmemOut;
			a->params[0].u.tmem.buf_ptr = pa;
			a->params[0].u.tmem.size = size;
			a->params[0].u.tmem.shm_ref = pa;
			a->ret = TeeSuccess;
			break;
		case RpcShmFree:
			a->ret = TeeSuccess;
			break;
		case RpcGetTime:
			a->params[0].attr = AttrValueOut;
			a->params[0].u.value.a = nsec() / 1000000000ULL;
			a->params[0].u.value.b = nsec() % 1000000000ULL;
			a->ret = TeeSuccess;
			break;
		default:
			a->ret = TeeNotImplemented;
			break;
		}
		a->ret_origin = TeeOriginComms;
		dmacache(1, pa, sizeof(MsgArg));
		r[0] = SmcReturnFromRpc;
		return;
	}
	print("rpc: unknown %#llx\n", r[0]);
	r[0] = SmcReturnFromRpc;
}

static u32
docall(MsgArg *a, u64 pa)
{
	u64 r[8];
	u64 a0, a1, a2, a3, a4, a5, a6, a7;

	a0 = SmcCallWithArg;
	a1 = pa >> 32;
	a2 = pa & 0xffffffff;
	a3 = 1;		/* OPTEE_SMC_SHM_CACHED */
	a4 = 0; a5 = 0; a6 = 0; a7 = 0;
	for(;;){
		r[4] = r[5] = r[6] = r[7] = 0;
		dmacache(1, blockpa, BlockSize);
		smc(a0, a1, a2, a3, a4, a5, a6, a7, r);
		dmacache(0, blockpa, BlockSize);
		if(!isrpc(r[0])){
			if(r[0] != SmcOk){
				print("smc: return %#llx\n", r[0]);
				return (u32)r[0];
			}
			return a->ret;
		}
		handle_rpc(r);
		a0 = r[0]; a1 = r[1]; a2 = r[2]; a3 = r[3];
		a4 = r[4]; a5 = r[5]; a6 = r[6]; a7 = r[7];
	}
}

static void
checkuid(void)
{
	u64 r[4];

	smc(SmcCallsUid, 0, 0, 0, 0, 0, 0, 0, r);
	print("calls_uid: %08llux %08llux %08llux %08llux\n",
	      r[0], r[1], r[2], r[3]);
	smc(SmcCallsRev, 0, 0, 0, 0, 0, 0, 0, r);
	print("calls_rev: %llux.%llux\n", r[0], r[1]);
	smc(0x32000001, 0, 0, 0, 0, 0, 0, 0, r);
	print("shm_config: ret %llux pa %#llx size %#llx cache %llux\n",
	      r[0], r[1], r[2], r[3]);
}

static void
invoke(u32 func, int nin, int nout, char *in, int noutbytes)
{
	u32 ret;
	int i;

	memset(arg, 0, sizeof *arg);
	arg->cmd = CmdInvoke;
	arg->func = func;
	arg->session = session;
	arg->num_params = nin + nout;
	if(nin > 0){
		arg->params[0].attr = AttrTmemIn;
		arg->params[0].u.tmem.buf_ptr = shmpa;
		arg->params[0].u.tmem.size = strlen(in);
		arg->params[0].u.tmem.shm_ref = 0;
		memmove(shm, in, strlen(in));
	}
	if(nout > 0){
		arg->params[nin].attr = AttrTmemOut;
		arg->params[nin].u.tmem.buf_ptr = shmpa + 256;
		arg->params[nin].u.tmem.size = noutbytes;
		arg->params[nin].u.tmem.shm_ref = 0;
	}
	ret = docall(arg, argpa);
	if(ret != TeeSuccess)
		fail("invoke %ud: ret %#ux origin %#x", func, ret,
		     arg->ret_origin);
	if(nout > 0){
		print("invoke %ud: ", func);
		for(i = 0; i < noutbytes; i++)
			print("%02x", shm[256+i]);
		print("\n");
	}
}

static void
ensure_dir(char *path)
{
	int fd;

	fd = create(path, OREAD, DMDIR|0777);
	if(fd >= 0)
		close(fd);
}

static int
savefile(char *path, void *buf, int n)
{
	int fd;

	fd = create(path, OWRITE, 0600);
	if(fd < 0){
		print("keyroot: create %s: %r\n", path);
		return -1;
	}
	if(write(fd, buf, n) != n){
		print("keyroot: write %s: %r\n", path);
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

static int
loadfile(char *path, void *buf, int n)
{
	int fd, r;

	fd = open(path, OREAD);
	if(fd < 0){
		print("keyroot: open %s: %r\n", path);
		return -1;
	}
	r = read(fd, buf, n);
	close(fd);
	return r;
}

static void
hexdump8(char *tag, u8 *p, int n)
{
	int i;

	print("%s: ", tag);
	for(i = 0; i < n; i++)
		print("%02x", p[i]);
	print("\n");
}

static void
mldsa_probe(void)
{
	u8 *msg, *pub, *blob, *sig;
	u32 publen, bloblen, siglen, ret;
	char *lab = "mldsa/identity";

	msg = shm + 64;
	pub = shm + 256;
	blob = shm + 2048;
	sig = shm + 8192;
	memset(shm, 0, ShmSize);
	memmove(shm, lab, strlen(lab));
	memset(msg, 0x42, 32);

	memset(arg, 0, sizeof *arg);
	arg->cmd = CmdInvoke;
	arg->func = 3;		/* MLDSA_KEYGEN */
	arg->session = session;
	arg->num_params = 3;
	arg->params[0].attr = AttrTmemIn;
	arg->params[0].u.tmem.buf_ptr = shmpa;
	arg->params[0].u.tmem.size = strlen(lab);
	arg->params[1].attr = AttrTmemOut;
	arg->params[1].u.tmem.buf_ptr = shmpa + 256;
	arg->params[1].u.tmem.size = 4096;
	arg->params[2].attr = AttrTmemOut;
	arg->params[2].u.tmem.buf_ptr = shmpa + 2048;
	arg->params[2].u.tmem.size = 4096;
	ret = docall(arg, argpa);
	if(ret != TeeSuccess)
		fail("mldsa keygen: ret %#ux", ret);
	publen = arg->params[1].u.tmem.size;
	bloblen = arg->params[2].u.tmem.size;
	print("mldsa keygen: pk=%ud blob=%ud\n", publen, bloblen);
	hexdump8("mldsa pk", pub, 16);

	memset(arg, 0, sizeof *arg);
	arg->cmd = CmdInvoke;
	arg->func = 5;		/* MLDSA_SIGN */
	arg->session = session;
	arg->num_params = 3;
	arg->params[0].attr = AttrTmemIn;
	arg->params[0].u.tmem.buf_ptr = shmpa + 2048;
	arg->params[0].u.tmem.size = bloblen;
	arg->params[1].attr = AttrTmemIn;
	arg->params[1].u.tmem.buf_ptr = shmpa + 64;
	arg->params[1].u.tmem.size = 32;
	arg->params[2].attr = AttrTmemOut;
	arg->params[2].u.tmem.buf_ptr = shmpa + 8192;
	arg->params[2].u.tmem.size = 4096;
	ret = docall(arg, argpa);
	if(ret != TeeSuccess)
		fail("mldsa sign: ret %#ux", ret);
	siglen = arg->params[2].u.tmem.size;
	print("mldsa sign: sig=%ud\n", siglen);
	hexdump8("mldsa sig", sig, 16);

	memset(arg, 0, sizeof *arg);
	arg->cmd = CmdInvoke;
	arg->func = 6;		/* MLDSA_VERIFY */
	arg->session = session;
	arg->num_params = 3;
	arg->params[0].attr = AttrTmemIn;
	arg->params[0].u.tmem.buf_ptr = shmpa + 256;
	arg->params[0].u.tmem.size = publen;
	arg->params[1].attr = AttrTmemIn;
	arg->params[1].u.tmem.buf_ptr = shmpa + 64;
	arg->params[1].u.tmem.size = 32;
	arg->params[2].attr = AttrTmemIn;
	arg->params[2].u.tmem.buf_ptr = shmpa + 8192;
	arg->params[2].u.tmem.size = siglen;
	ret = docall(arg, argpa);
	print("mldsa verify: ret %#ux\n", ret);
	{
		int uv;

		uv = PQCLEAN_MLDSA44_CLEAN_crypto_sign_verify(
			sig, siglen, msg, 32, pub);
		print("mldsa user verify: %d\n", uv);
	}
	ensure_dir("/mnt/keyroot");
	savefile("/mnt/keyroot/identity.blob", blob, bloblen);
	savefile("/mnt/keyroot/identity.pub", pub, publen);
	savefile("/mnt/keyroot/challenge", msg, 32);
	savefile("/mnt/keyroot/signature", sig, siglen);
	{
		u8 fpub[1312], fsig[2420], fmsg[32];
		int fv;

		if(loadfile("/mnt/keyroot/identity.pub", fpub, publen) ==
		   (int)publen &&
		   loadfile("/mnt/keyroot/signature", fsig, siglen) ==
		   (int)siglen &&
		   loadfile("/mnt/keyroot/challenge", fmsg, 32) == 32){
			fv = PQCLEAN_MLDSA44_CLEAN_crypto_sign_verify(
				fsig, siglen, fmsg, 32, fpub);
			print("mldsa file verify: %d\n", fv);
		}
	}

	memset(arg, 0, sizeof *arg);
	arg->cmd = CmdInvoke;
	arg->func = 7;		/* MLDSA_SELFTEST */
	arg->session = session;
	arg->num_params = 0;
	ret = docall(arg, argpa);
	print("mldsa selftest: ret %#ux\n", ret);
}

enum {
	P9OffHostKey	= 0,
	P9OffAuthKey	= 16,
	P9OffHostLab	= 32,
	P9OffAuthLab	= 64,
	P9OffUsage	= 96,
	P9OffTr		= 128,
	P9OffHostBlob	= 320,
	P9OffAuthBlob	= 448,
	P9OffTickets	= 640,
	P9OffAuthAc	= 800,
	P9OffReply	= 832,
	P9OffCchal	= 864,
	P9OffSchal	= 872,
};

static void
p9_set(MsgParam *p, int out, u32 off, u32 size)
{
	memset(p, 0, sizeof *p);
	p->attr = out ? AttrTmemOut : AttrTmemIn;
	p->u.tmem.buf_ptr = shmpa + off;
	p->u.tmem.size = size;
	p->u.tmem.shm_ref = 0;
}

static u32
p9call(u32 func, MsgParam *pp, int n)
{
	memset(arg, 0, sizeof *arg);
	arg->cmd = CmdInvoke;
	arg->func = func;
	arg->session = session;
	arg->num_params = n;
	if(n > 0)
		memmove(arg->params, pp, n * sizeof(MsgParam));
	return docall(arg, argpa);
}

static void
p9_probe(void)
{
	static u8 hkey[7] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd };
	static u8 akey[7] = { 0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32 };
	MsgParam pp[4];
	u8 *tr, *hostblob, *authblob, *tickets, *authac, *reply;
	u8 t[72], a[13];
	u32 nh, na, ret, id;

	memset(shm, 0, ShmSize);
	tr = shm + P9OffTr;
	hostblob = shm + P9OffHostBlob;
	authblob = shm + P9OffAuthBlob;
	tickets = shm + P9OffTickets;
	authac = shm + P9OffAuthAc;
	reply = shm + P9OffReply;

	memmove(shm + P9OffHostKey, hkey, 7);
	memmove(shm + P9OffAuthKey, akey, 7);
	memmove(shm + P9OffHostLab, "p9/host", 7);
	memmove(shm + P9OffAuthLab, "p9/auth", 7);
	memmove(shm + P9OffCchal, "CLIENTCH", 8);
	memmove(shm + P9OffSchal, "SERVERCH", 8);

	/* Import the two test keys as sealed host and auth blobs. */
	shm[P9OffUsage] = 2;
	p9_set(&pp[0], 0, P9OffHostLab, 7);
	p9_set(&pp[1], 0, P9OffHostKey, 7);
	p9_set(&pp[2], 0, P9OffUsage, 1);
	p9_set(&pp[3], 1, P9OffHostBlob, 128);
	ret = p9call(8, pp, 4);
	if(ret != TeeSuccess)
		fail("p9 import host: ret %#ux", ret);
	nh = arg->params[3].u.tmem.size;

	shm[P9OffUsage] = 3;
	p9_set(&pp[0], 0, P9OffAuthLab, 7);
	p9_set(&pp[1], 0, P9OffAuthKey, 7);
	p9_set(&pp[2], 0, P9OffUsage, 1);
	p9_set(&pp[3], 1, P9OffAuthBlob, 128);
	ret = p9call(8, pp, 4);
	if(ret != TeeSuccess)
		fail("p9 import auth: ret %#ux", ret);
	na = arg->params[3].u.tmem.size;
	print("p9 import: host blob %ud auth blob %ud\n", nh, na);

	/* Build a ticket request as the server would. */
	tr[0] = 1;
	memmove(tr + 1, "auth", 4);
	memmove(tr + 29, "harvey.test", 11);
	memmove(tr + 77, shm + P9OffSchal, 8);
	memmove(tr + 85, "host", 4);
	memmove(tr + 113, "glenda", 6);

	p9_set(&pp[0], 0, P9OffAuthBlob, na);
	p9_set(&pp[1], 0, P9OffHostBlob, nh);
	p9_set(&pp[2], 0, P9OffTr, 141);
	p9_set(&pp[3], 1, P9OffTickets, 144);
	ret = p9call(9, pp, 4);
	if(ret != TeeSuccess)
		fail("p9 ticket: ret %#ux", ret);
	print("p9 tickets: %ud bytes\n", arg->params[3].u.tmem.size);

	p9_set(&pp[0], 0, P9OffHostBlob, nh);
	p9_set(&pp[1], 0, P9OffTickets, 72);
	p9_set(&pp[2], 0, P9OffSchal, 8);
	p9_set(&pp[3], 1, P9OffAuthAc, 13);
	ret = p9call(10, pp, 4);
	if(ret != TeeSuccess)
		fail("p9 auth: ret %#ux", ret);
	print("p9 authenticator: %ud bytes\n", arg->params[3].u.tmem.size);

	/* Normal-world server simulation: decrypt, check, reply. */
	memmove(t, tickets + 72, 72);
	decrypt(akey, t, 72);
	if(t[0] != 64 || memcmp(t + 1, shm + P9OffSchal, 8) != 0)
		fail("p9 server: bad ticket");
	memmove(a, authac, 13);
	decrypt(t + 65, a, 13);
	id = a[9] | (a[10] << 8) | (a[11] << 16) | ((u32)a[12] << 24);
	if(a[0] != 67 || memcmp(a + 1, shm + P9OffSchal, 8) != 0 || id != 0)
		fail("p9 server: bad authenticator");
	print("p9 server: ticket and authenticator verified\n");

	memset(reply, 0, 13);
	reply[0] = 66;
	memmove(reply + 1, shm + P9OffCchal, 8);
	encrypt(t + 65, reply, 13);

	p9_set(&pp[0], 0, P9OffHostBlob, nh);
	p9_set(&pp[1], 0, P9OffTickets, 72);
	p9_set(&pp[2], 0, P9OffCchal, 8);
	p9_set(&pp[3], 0, P9OffReply, 13);
	ret = p9call(11, pp, 4);
	print("p9 ta verify server reply: ret %#ux\n", ret);

	ret = p9call(12, pp, 0);
	print("p9 selftest: ret %#ux\n", ret);

	ensure_dir("/mnt/keyroot");
	savefile("/mnt/keyroot/p9.hostblob", hostblob, nh);
	savefile("/mnt/keyroot/p9.authblob", authblob, na);
	savefile("/mnt/keyroot/p9.tickets", tickets, 144);
	savefile("/mnt/keyroot/p9.authenticator", authac, 13);
}


static void
dp9ik_probe(void)
{
	MsgParam pp[4];
	u8 *blob;
	u8 key[16] = {
		0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
		0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
	};
	u32 nblob, ret;

	memset(shm, 0, ShmSize);
	memmove(shm, "dp9ik/test", 10);
	memmove(shm + 32, key, sizeof(key));
	memmove(shm + 64, "testuser", 8);
	blob = shm + 256;

	p9_set(&pp[0], 0, 0, 10);
	p9_set(&pp[1], 0, 32, sizeof(key));
	p9_set(&pp[2], 0, 64, 8);
	p9_set(&pp[3], 1, 256, 4096);
	ret = p9call(13, pp, 4);
	if(ret != TeeSuccess)
		fail("dp9ik import: ret %#ux", ret);
	nblob = arg->params[3].u.tmem.size;
	print("dp9ik import: blob %ud bytes\n", nblob);
	hexdump8("dp9ik blob", blob, 16);

	/* Build a ticket request and exercise the client PAK init. */
	memset(shm + 1024, 0, 1024);
	shm[1024] = 19;			/* AuthPAK */
	memmove(shm + 1025, "auth", 4);
	memmove(shm + 1053, "test", 4);
	memmove(shm + 1101, "SRVCHAL1", 8);
	memmove(shm + 1109, "host", 4);
	memmove(shm + 1137, "glenda", 6);
	p9_set(&pp[0], 0, 256, nblob);
	p9_set(&pp[1], 0, 1024, 141);
	p9_set(&pp[2], 0, 1280, 56);
	p9_set(&pp[3], 1, 1400, 56);
	ret = p9call(15, pp, 4);
	if(ret != TeeSuccess)
		fail("dp9ik init: ret %#ux", ret);
	print("dp9ik init: clientY %ud bytes\n", arg->params[3].u.tmem.size);
	hexdump8("dp9ik clientY", shm + 1400, 8);

	/* A zero AS reply must be rejected, not crash the TA. */
	memset(shm + 1500, 0, 112);
	memset(shm + 1700, 0, 248);
	p9_set(&pp[0], 0, 1500, 112);
	p9_set(&pp[1], 0, 1700, 248);
	p9_set(&pp[2], 1, 2000, 56);
	p9_set(&pp[3], 1, 2100, 192);
	ret = p9call(16, pp, 4);
	print("dp9ik finish with zero AS reply: ret %#ux\n", ret);

	ret = p9call(14, pp, 0);
	print("dp9ik selftest: ret %#ux\n", ret);

	ensure_dir("/mnt/keyroot");
	savefile("/mnt/keyroot/dp9ik.blob", blob, nblob);
}


static void
mlkem_probe(void)
{
	MsgParam pp[4];
	u8 *pk, *blob, *ct, *ss1, *ss2;
	u32 npk, nblob, nct, nss1, nss2, ret;

	memset(shm, 0, ShmSize);
	pk = shm + 256;
	blob = shm + 1536;
	ct = shm + 5632;
	ss1 = shm + 6784;
	ss2 = shm + 6848;
	memmove(shm, "mlkem/test", 10);

	p9_set(&pp[0], 0, 0, 10);
	p9_set(&pp[1], 1, 256, 2048);
	p9_set(&pp[2], 1, 1536, 4096);
	p9_set(&pp[3], 0, 0, 0);
	ret = p9call(18, pp, 3);
	if(ret != TeeSuccess)
		fail("mlkem keygen: ret %#ux", ret);
	npk = arg->params[1].u.tmem.size;
	nblob = arg->params[2].u.tmem.size;
	print("mlkem keygen: pk=%ud blob=%ud\n", npk, nblob);

	p9_set(&pp[0], 0, 256, npk);
	p9_set(&pp[1], 1, 5632, 2048);
	p9_set(&pp[2], 1, 6784, 64);
	p9_set(&pp[3], 0, 0, 0);
	ret = p9call(19, pp, 3);
	if(ret != TeeSuccess)
		fail("mlkem encaps: ret %#ux", ret);
	nct = arg->params[1].u.tmem.size;
	nss1 = arg->params[2].u.tmem.size;
	print("mlkem encaps: ct=%ud ss=%ud\n", nct, nss1);

	p9_set(&pp[0], 0, 1536, nblob);
	p9_set(&pp[1], 0, 5632, nct);
	p9_set(&pp[2], 1, 6848, 64);
	p9_set(&pp[3], 0, 0, 0);
	ret = p9call(20, pp, 3);
	if(ret != TeeSuccess)
		fail("mlkem decaps: ret %#ux", ret);
	nss2 = arg->params[2].u.tmem.size;
	print("mlkem decaps: ss=%ud\n", nss2);
	print("mlkem shared match: %d\n",
	      nss1 == nss2 && memcmp(ss1, ss2, nss1) == 0);

	ret = p9call(21, pp, 0);
	print("mlkem selftest: ret %#ux\n", ret);

	ensure_dir("/mnt/keyroot");
	savefile("/mnt/keyroot/mlkem.pub", pk, npk);
	savefile("/mnt/keyroot/mlkem.blob", blob, nblob);
}


static void
userkey_probe(void)
{
	MsgParam pp[4];
	u8 *idpk, *idblob, *ublob, *upk;
	u32 nidpk, nidblob, nublob, nupk, ret;

	memset(shm, 0, ShmSize);
	idpk = shm + 256;
	idblob = shm + 1600;
	ublob = shm + 6000;
	upk = shm + 10240;
	memmove(shm, "userkey/id", 10);
	memmove(shm + 64, "userkey/user", 12);

	p9_set(&pp[0], 0, 0, 10);
	p9_set(&pp[1], 1, 256, 2048);
	p9_set(&pp[2], 1, 1600, 4096);
	p9_set(&pp[3], 0, 0, 0);
	ret = p9call(3, pp, 3);
	if(ret != TeeSuccess)
		fail("userkey identity: ret %#ux", ret);
	nidpk = arg->params[1].u.tmem.size;
	nidblob = arg->params[2].u.tmem.size;
	print("userkey identity: pk=%ud blob=%ud\n", nidpk, nidblob);

	shm[128] = 1;	/* ML-DSA-44 user key */
	p9_set(&pp[0], 0, 1600, nidblob);
	p9_set(&pp[1], 0, 128, 1);
	p9_set(&pp[2], 0, 64, 12);
	p9_set(&pp[3], 1, 6000, 4096);
	ret = p9call(22, pp, 4);
	if(ret != TeeSuccess)
		fail("userkey keygen: ret %#ux", ret);
	nublob = arg->params[3].u.tmem.size;
	print("userkey keygen: blob=%ud\n", nublob);
	hexdump8("userkey blob", ublob, 16);

	p9_set(&pp[0], 0, 6000, nublob);
	p9_set(&pp[1], 1, 10240, 2048);
	p9_set(&pp[2], 0, 0, 0);
	p9_set(&pp[3], 0, 0, 0);
	ret = p9call(23, pp, 2);
	if(ret != TeeSuccess)
		fail("userkey public: ret %#ux", ret);
	nupk = arg->params[1].u.tmem.size;
	print("userkey public: %ud bytes\n", nupk);

	ret = p9call(24, pp, 0);
	print("userkey selftest: ret %#ux\n", ret);

	ensure_dir("/mnt/keyroot");
	savefile("/mnt/keyroot/userkey.blob", ublob, nublob);
	savefile("/mnt/keyroot/userkey.pub", upk, nupk);
}

static void
pta_probe(void)
{
	u64 infopa, datapa;
	u32 ret;

	infopa = teeshmalloc(104, 16);
	datapa = teeshmalloc(64, 16);
	memset(pava(infopa), 0, 104);
	memset(pava(datapa), 0, 64);

	memset(arg, 0, sizeof *arg);
	arg->cmd = CmdOpenSession;
	arg->num_params = 2;
	arg->params[0].attr = AttrValueIn | AttrMeta;
	memmove(&arg->params[0].u.value, ptauuid, 16);
	arg->params[1].attr = AttrValueIn | AttrMeta;
	memset(&arg->params[1].u.value, 0, sizeof arg->params[1].u.value);
	ret = docall(arg, argpa);
	if(ret != TeeSuccess)
		fail("pta open: ret %#ux origin %#x", ret, arg->ret_origin);
	session = arg->session;
	print("pta session %ud opened\n", session);

	memset(arg, 0, sizeof *arg);
	arg->cmd = CmdInvoke;
	arg->func = 1;		/* rk_oem_otp_key_cipher */
	arg->session = session;
	arg->num_params = 3;
	arg->params[0].attr = AttrValueIn;
	arg->params[0].u.value.a = 0;	/* key id */
	arg->params[1].attr = AttrTmemIn;
	arg->params[1].u.tmem.buf_ptr = infopa;
	arg->params[1].u.tmem.size = 104;
	arg->params[2].attr = AttrTmemInout;
	arg->params[2].u.tmem.buf_ptr = datapa;
	arg->params[2].u.tmem.size = 64;
	ret = docall(arg, argpa);
	print("pta cipher: ret %#ux origin %#x\n", ret, arg->ret_origin);

	memset(arg, 0, sizeof *arg);
	arg->cmd = CmdCloseSession;
	arg->session = session;
	docall(arg, argpa);
}

void
main(int argc, char **argv)
{
	u32 ret;
	int i;

	for(i = 0; i < Maxfd; i++)
		fds[i] = -1;
	if(argc > 1 && strcmp(argv[1], "mldsa") == 0){
		mldsamode = 1;
		if(argc > 2)
			tapath = argv[2];
	} else if(argc > 1 && strcmp(argv[1], "p9") == 0){
		p9mode = 1;
		if(argc > 2)
			tapath = argv[2];
	} else if(argc > 1 && strcmp(argv[1], "dp9ik") == 0){
		dp9ikmode = 1;
		if(argc > 2)
			tapath = argv[2];
	} else if(argc > 1 && strcmp(argv[1], "mlkem") == 0){
		mlkemmode = 1;
		if(argc > 2)
			tapath = argv[2];
	} else if(argc > 1 && strcmp(argv[1], "userkey") == 0){
		userkeymode = 1;
		if(argc > 2)
			tapath = argv[2];
	} else if(argc > 1 && strcmp(argv[1], "pta") == 0)
		ptamode = 1;
	else if(argc > 1)
		tapath = argv[1];
	teeshmbase = readnum("#P/tee/shmbase");
	teeshmsize = readnum("#P/tee/shmsize");
	if(teeshmbase == 0 || teeshmsize == 0)
		fail("no #P/tee/teeshm region");
	teeshmva = segattach(0, "teeshm", 0, teeshmsize);
	if(teeshmva == (void*)-1)
		fail("segattach teeshm: %r");
	teebump = teeshmbase;
	if((smcfd = open("#P/tee/smc", ORDWR)) < 0)
		fail("open #P/tee/smc: %r");
	readnum("#P/tee/version");

	checkuid();

	blockpa = teeshmalloc(BlockSize, 4096);
	arg = (MsgArg*)pava(blockpa);
	argpa = blockpa;
	shm = pava(blockpa + ShmSize);
	shmpa = blockpa + ShmSize;
	memset(arg, 0, BlockSize);
	if(ptamode){
		pta_probe();
		exits(nil);
	}

	/* temporary memrefs: no REGISTER_SHM needed for this probe */
	print("tmem: pa %#llx size %d\n", shmpa, ShmSize);

	/* open session to the TA */
	memset(arg, 0, sizeof *arg);
	arg->cmd = CmdOpenSession;
	arg->num_params = 2;
	arg->params[0].attr = AttrValueIn | AttrMeta;
	memmove(&arg->params[0].u.value, tauuid, 16);
	arg->params[1].attr = AttrValueIn | AttrMeta;
	memset(&arg->params[1].u.value, 0, sizeof arg->params[1].u.value);
	arg->params[1].u.value.c = 0;	/* TEE_LOGIN_PUBLIC */
	ret = docall(arg, argpa);
	if(ret != TeeSuccess)
		fail("open session: ret %#ux origin %#x", ret, arg->ret_origin);
	session = arg->session;
	print("session %ud opened\n", session);
	if(mldsamode){
		mldsa_probe();
		exits(nil);
	}
	if(p9mode){
		p9_probe();
		exits(nil);
	}
	if(dp9ikmode){
		dp9ik_probe();
		exits(nil);
	}
	if(mlkemmode){
		mlkem_probe();
		exits(nil);
	}
	if(userkeymode){
		userkey_probe();
		exits(nil);
	}

	/* PING: output only */
	invoke(0, 0, 1, nil, 64);

	/* INFO */
	invoke(1, 0, 1, nil, 128);
	shm[256 + 127] = 0;
	print("info: %s", (char*)&shm[256]);

	/* DERIVE */
	invoke(2, 1, 1, "probe/label", 32);

	/* close */
	memset(arg, 0, sizeof *arg);
	arg->cmd = CmdCloseSession;
	arg->session = session;
	docall(arg, argpa);

	exits(nil);
}
