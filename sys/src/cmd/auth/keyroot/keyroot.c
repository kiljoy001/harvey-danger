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
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "api.h"
#include "ta/des/des.h"
#include "ta/dp9ik/dp9ik.h"

static void fail(char*, ...);

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

static int
boundedlen(u8 *p, int n)
{
	int i;

	for(i = 0; i < n && p[i] != 0; i++)
		;
	return i;
}

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

static int
kr_init(char *ta)
{
	u32 ret;
	int i;

	for(i = 0; i < Maxfd; i++)
		fds[i] = -1;
	tapath = ta;
	teeshmbase = readnum("#P/tee/shmbase");
	teeshmsize = readnum("#P/tee/shmsize");
	if(teeshmbase == 0 || teeshmsize == 0)
		return -1;
	teeshmva = segattach(0, "teeshm", 0, teeshmsize);
	if(teeshmva == (void*)-1)
		return -1;
	teebump = teeshmbase;
	if((smcfd = open("#P/tee/smc", ORDWR)) < 0)
		return -1;
	readnum("#P/tee/version");

	blockpa = teeshmalloc(BlockSize, 4096);
	arg = (MsgArg*)pava(blockpa);
	argpa = blockpa;
	shm = pava(blockpa + ShmSize);
	shmpa = blockpa + ShmSize;
	memset(arg, 0, BlockSize);

	memset(arg, 0, sizeof *arg);
	arg->cmd = CmdOpenSession;
	arg->num_params = 2;
	arg->params[0].attr = AttrValueIn | AttrMeta;
	memmove(&arg->params[0].u.value, tauuid, 16);
	arg->params[1].attr = AttrValueIn | AttrMeta;
	memset(&arg->params[1].u.value, 0, sizeof arg->params[1].u.value);
	arg->params[1].u.value.c = 0;
	ret = docall(arg, argpa);
	if(ret != TeeSuccess)
		return -1;
	session = arg->session;
	return 0;
}

enum {
	MaxBlob		= 4096,
	MaxOut		= 4096,
};

static u8 kr_blob[MaxBlob];
static int kr_nblob;
static u8 kr_out[MaxOut];
static int kr_nout;

static int
kr_import(u8 *in, int n)
{
	MsgParam pp[4];
	u32 ret;

	if(n != 16 + 28 + 32){
		fprint(2, "keyroot: import bad size %d\n", n);
		return -1;
	}
	memmove(shm + 0, in + 16 + 28, 32);	/* label */
	memmove(shm + 64, in, 16);		/* AES key */
	memmove(shm + 128, in + 16, 28);	/* user */
	p9_set(&pp[0], 0, 0, boundedlen(shm, 32));
	p9_set(&pp[1], 0, 64, 16);
	p9_set(&pp[2], 0, 128, boundedlen(shm + 128, 28));
	p9_set(&pp[3], 1, 512, MaxBlob);
	ret = p9call(13, pp, 4);
	if(ret != TeeSuccess){
		fprint(2, "keyroot: import tee ret %#ux\n", ret);
		return -1;
	}
	kr_nblob = arg->params[3].u.tmem.size;
	fprint(2, "keyroot: import blob %d\n", kr_nblob);
	if(kr_nblob > MaxBlob)
		return -1;
	memmove(kr_blob, shm + 512, kr_nblob);
	return 0;
}

static int
kr_dp9ik_init(u8 *in, int n)
{
	MsgParam pp[4];
	u32 ret;

	if(n != TICKREQLEN + PAKYLEN || kr_nblob == 0)
		return -1;
	memmove(shm + 0, in, TICKREQLEN);
	memmove(shm + 256, in + TICKREQLEN, PAKYLEN);
	memmove(shm + 512, kr_blob, kr_nblob);
	p9_set(&pp[0], 0, 512, kr_nblob);
	p9_set(&pp[1], 0, 0, TICKREQLEN);
	p9_set(&pp[2], 0, 256, PAKYLEN);
	p9_set(&pp[3], 1, 1024, PAKYLEN);
	ret = p9call(15, pp, 4);
	if(ret != TeeSuccess)
		return -1;
	kr_nout = arg->params[3].u.tmem.size;
	memmove(kr_out, shm + 1024, kr_nout);
	return 0;
}

static int
kr_dp9ik_finish(u8 *in, int n)
{
	MsgParam pp[4];
	u32 ret;

	if(n != 2*PAKYLEN + 2*MAXTICKETLEN || kr_nblob == 0)
		return -1;
	memmove(shm + 0, in, 2*PAKYLEN);
	memmove(shm + 128, in + 2*PAKYLEN, 2*MAXTICKETLEN);
	p9_set(&pp[0], 0, 0, 2*PAKYLEN);
	p9_set(&pp[1], 0, 128, 2*MAXTICKETLEN);
	p9_set(&pp[2], 1, 512, PAKYLEN);
	p9_set(&pp[3], 1, 640, MAXTICKETLEN + MAXAUTHENTLEN);
	ret = p9call(16, pp, 4);
	if(ret != TeeSuccess)
		return -1;
	kr_nout = arg->params[3].u.tmem.size;
	memmove(kr_out, shm + 512, PAKYLEN);
	memmove(kr_out + PAKYLEN, shm + 640, kr_nout);
	kr_nout += PAKYLEN;
	return 0;
}

static int
kr_dp9ik_reply(u8 *in, int n)
{
	MsgParam pp[4];
	u32 ret;

	if(n != MAXAUTHENTLEN + CHALLEN || kr_nblob == 0)
		return -1;
	memmove(shm + 0, in, MAXAUTHENTLEN);
	memmove(shm + 128, in + MAXAUTHENTLEN, CHALLEN);
	p9_set(&pp[0], 0, 0, MAXAUTHENTLEN);
	p9_set(&pp[1], 0, 128, CHALLEN);
	p9_set(&pp[2], 1, 256, 256);
	p9_set(&pp[3], 0, 0, 0);
	ret = p9call(17, pp, 4);
	if(ret != TeeSuccess)
		return -1;
	kr_nout = arg->params[2].u.tmem.size;
	memmove(kr_out, shm + 256, kr_nout);
	return 0;
}

enum {
	Fctl,
	Fblob,
	Fimport,
	Finit,
	Ffinish,
	Freply,
};

static void
fsread(Req *r)
{
	char buf[64];

	switch((uintptr)r->fid->file->aux){
	case Fctl:
		snprint(buf, sizeof buf, "keyroot dp9ik blob=%d\n", kr_nblob);
		readstr(r, buf);
		respond(r, nil);
		return;
	case Fblob:
		readbuf(r, kr_blob, kr_nblob);
		respond(r, nil);
		return;
	case Fimport:
	case Finit:
	case Ffinish:
	case Freply:
		readbuf(r, kr_out, kr_nout);
		respond(r, nil);
		return;
	}
	respond(r, "permission denied");
}

static void
fswrite(Req *r)
{
	int tag, n, ok;

	tag = (uintptr)r->fid->file->aux;
	n = r->ifcall.count;
	ok = -1;
	switch(tag){
	case Fblob:
		if(n <= MaxBlob){
			memmove(kr_blob, r->ifcall.data, n);
			kr_nblob = n;
			ok = 0;
		}
		break;
	case Fimport:
		ok = kr_import((u8*)r->ifcall.data, n);
		break;
	case Finit:
		ok = kr_dp9ik_init((u8*)r->ifcall.data, n);
		break;
	case Ffinish:
		ok = kr_dp9ik_finish((u8*)r->ifcall.data, n);
		break;
	case Freply:
		ok = kr_dp9ik_reply((u8*)r->ifcall.data, n);
		break;
	}
	if(ok < 0){
		fprint(2, "keyroot: write tag %d n %d failed\n", tag, n);
		respond(r, "operation failed");
	} else {
		if(tag == Fblob)
			r->fid->file->Dir.length = kr_nblob;
		else if(tag != Fimport)
			r->fid->file->Dir.length = kr_nout;
		r->ofcall.count = r->ifcall.count;
		respond(r, nil);
	}
}

static void
fsdestroyfile(File *f)
{
	USED(f);
}

static Srv fs = {
	.read = fsread,
	.write = fswrite,
};

static void
usage(void)
{
	fprint(2, "usage: keyroot [-m mtpt] [-t ta]\n");
	exits("usage");
}

void
main(int argc, char **argv)
{
	char *mtpt = "/mnt/keyroot";
	char *ta = "/boot/optee-keyroot.ta";
	File *d;

	ARGBEGIN{
	case 'm':
		mtpt = EARGF(usage());
		break;
	case 't':
		ta = EARGF(usage());
		break;
	default:
		usage();
	}ARGEND

	if(kr_init(ta) < 0)
		sysfatal("keyroot: cannot initialize #P/tee: %r");

	{
		int fd;

		fd = create(mtpt, OREAD, DMDIR | 0777);
		if(fd >= 0)
			close(fd);
	}
	fs.tree = alloctree(nil, nil, DMDIR | 0777, fsdestroyfile);
	createfile(fs.tree->root, "ctl", nil, 0444,
		   (void*)Fctl)->Dir.length = 64;
	d = createfile(fs.tree->root, "dp9ik", nil, DMDIR | 0777, nil);
	createfile(d, "blob", nil, 0666, (void*)Fblob)->Dir.length = MaxBlob;
	createfile(d, "import", nil, 0666, (void*)Fimport)->Dir.length = 0;
	createfile(d, "init", nil, 0666, (void*)Finit)->Dir.length = 0;
	createfile(d, "finish", nil, 0666, (void*)Ffinish)->Dir.length = 0;
	createfile(d, "reply", nil, 0666, (void*)Freply)->Dir.length = 0;

	postmountsrv(&fs, "keyroot", mtpt, MREPL | MCREATE);
	exits(nil);
}
