/* Guest driver: exercise the shipped LLVM/Clang tool set. */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>

extern int wait4(int, int *, int, void *);

static int
run(char *label, char *const argv[], const char *out, const char *want)
{
	char buf[4096];
	pid_t pid;
	int st, fd = -1, n, ok = 1;

	fflush(stdout);
	pid = fork();
	if(pid == 0){
		if(out != NULL){
			fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0666);
			if(fd < 0){
				fprintf(stderr, "TOOLTEST %s open: %s\n", label,
				    strerror(errno));
				_exit(126);
			}
			dup2(fd, 1);
			dup2(fd, 2);
			close(fd);
		}
		execv(argv[0], argv);
		fprintf(stderr, "TOOLTEST %s: %s\n", label, strerror(errno));
		_exit(127);
	}
	if(pid < 0 || wait4(pid, &st, 0, 0) < 0){
		printf("TOOLTEST %s FAIL wait\n", label);
		return -1;
	}
	if(!WIFEXITED(st) || WEXITSTATUS(st) != 0){
		printf("TOOLTEST %s FAIL %d\n", label,
		    WIFEXITED(st) ? WEXITSTATUS(st) : -1);
		return -1;
	}
	if(out != NULL && want != NULL){
		fd = open(out, O_RDONLY);
		if(fd < 0){
			printf("TOOLTEST %s FAIL read\n", label);
			return -1;
		}
		n = read(fd, buf, sizeof buf - 1);
		close(fd);
		if(n < 0)
			n = 0;
		buf[n] = 0;
		if(strstr(buf, want) == NULL){
			printf("TOOLTEST %s FAIL output (%s)\n", label, buf);
			ok = 0;
		}
	}
	if(ok)
		printf("TOOLTEST %s OK\n", label);
	return ok ? 0 : -1;
}

int
main(void)
{
	const char *sdk = "/initramfs/sdk/bin/";
	char ccpath[128], arpath[128], nm[128], oc[128], sp[128], re[128];
	char od[128], sz[128], st[128], cf[128], tidy[128], chk[128];
	char lv[128], cxx[128];

	snprintf(ccpath, sizeof ccpath, "%sclang", sdk);
	snprintf(arpath, sizeof arpath, "%sllvm-ar", sdk);
	snprintf(nm, sizeof nm, "%sllvm-nm", sdk);
	snprintf(oc, sizeof oc, "%sllvm-objcopy", sdk);
	snprintf(sp, sizeof sp, "%sllvm-strip", sdk);
	snprintf(re, sizeof re, "%sllvm-readelf", sdk);
	snprintf(od, sizeof od, "%sllvm-objdump", sdk);
	snprintf(sz, sizeof sz, "%sllvm-size", sdk);
	snprintf(st, sizeof st, "%sllvm-strings", sdk);
	snprintf(cf, sizeof cf, "%sclang-format", sdk);
	snprintf(tidy, sizeof tidy, "%sclang-tidy", sdk);
	snprintf(chk, sizeof chk, "%sclang-check", sdk);
	snprintf(lv, sizeof lv, "%sllvm", sdk);
	snprintf(cxx, sizeof cxx, "%sllvm-cxxfilt", sdk);

	char *cc[] = { ccpath, "--no-default-config",
	    "--config=/initramfs/sdk/bin/clang.cfg", "-c",
	    "/initramfs/t/x.c", "-o", "/initramfs/x.o", 0 };
	char *ac[] = { arpath, "rcs", "/initramfs/libx.a",
	    "/initramfs/x.o", 0 };
	char *at[] = { arpath, "t", "/initramfs/libx.a", 0 };
	char *rl[] = { arpath, "s", "/initramfs/libx.a", 0 };
	char *nmv[] = { nm, "/initramfs/x.o", 0 };
	char *occ[] = { oc, "/initramfs/x.o", "/initramfs/y.o", 0 };
	char *spv[] = { sp, "/initramfs/y.o", 0 };
	char *rev[] = { re, "-h", "/initramfs/x.o", 0 };
	char *odv[] = { od, "-d", "/initramfs/x.o", 0 };
	char *szv[] = { sz, "/initramfs/x.o", 0 };
	char *stv[] = { st, "/initramfs/x.o", 0 };
	char *cfv[] = { cf, "/initramfs/t/fmtme.c", 0 };
	char *cxv[] = { cxx, "_Z3foov", 0 };
	char *lvh[] = { lv, "--help", 0 };
	char *tdv[] = { tidy, "--help", 0 };
	char *ckv[] = { chk, "/initramfs/t/x.c", "--",
	    "--target=aarch64-unknown-linux-elf", "-fsyntax-only", 0 };

	if(run("cc", cc, 0, 0) != 0 ||
	   run("ar-create", ac, 0, 0) != 0 ||
	   run("ar-list", at, "/initramfs/ar.txt", "x.o") != 0 ||
	   run("ranlib", rl, 0, 0) != 0 ||
	   run("nm", nmv, "/initramfs/nm.txt", " x") != 0 ||
	   run("objcopy", occ, 0, 0) != 0 ||
	   run("strip", spv, 0, 0) != 0 ||
	   run("readelf", rev, "/initramfs/re.txt", "ELF") != 0 ||
	   run("objdump", odv, "/initramfs/od.txt", "file format") != 0 ||
	   run("size", szv, "/initramfs/sz.txt", "text") != 0 ||
	   run("strings", stv, 0, 0) != 0 ||
	   run("cxxfilt", cxv, "/initramfs/cx.txt", "foo()") != 0 ||
	   run("format", cfv, "/initramfs/cf.txt", "int main()") != 0 ||
	   run("llvm-help", lvh, "/initramfs/lv.txt", "subcommand") != 0 ||
	   run("clang-tidy", tdv, "/initramfs/td.txt", "clang-tidy") != 0 ||
	   run("clang-check", ckv, 0, 0) != 0)
		return 1;
	printf("TOOLTEST-DONE\n");
	fflush(stdout);
	return 0;
}
