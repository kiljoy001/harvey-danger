/**
Run a command pipeline.

Copyright (C) 2025  Anand Tamariya

Author: Anand Tamariya <anand@gmail.com>
Keywords: plan9, pipe

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.

*/
#include <u.h>
#include <libc.h>

void
spawnstage(char **argv, int infd, int outfd)
{
	switch(rfork(RFPROC|RFFDG|RFNOTEG|RFCENVG|RFNOWAIT)){
	case -1:
		sysfatal("rfork: %r");
	case 0:
		if(infd >= 0){
			dup(infd, 0);
			close(infd);
		}
		if(outfd >= 0){
			dup(outfd, 1);
			close(outfd);
		}
		exec(argv[0], argv);
		sysfatal("exec %s: %r", argv[0]);
	default:
		break;
	}
}

void
main(int, char**)
{
	int n, audio;
	uchar buf[8192];
	int pipe1[2], pipe2[2], pipe3[2];

	char *dec[] = { "/bin/echo", "one", "two", nil };
	char *res[] = { "/bin/cat", nil };
	char *vol[] = { "/bin/awk", "{print $2}", nil };

	if(pipe(pipe1) < 0) sysfatal("pipe1: %r");
	if(pipe(pipe2) < 0) sysfatal("pipe2: %r");
	if(pipe(pipe3) < 0) sysfatal("pipe3: %r");

	spawnstage(dec, -1, pipe1[1]);
	close(pipe1[1]);

	spawnstage(res, pipe1[0], pipe2[1]);
	close(pipe1[0]);
	close(pipe2[1]);

	spawnstage(vol, pipe2[0], pipe3[1]);
	close(pipe2[0]);
	close(pipe3[1]);

	while((n = read(pipe3[0], buf, sizeof buf)) > 0) print("%s", buf);

	close(pipe3[0]);
}
