/*
 * ringfs.h - named #~ (9ring) helpers for the browser stage servers.
 *
 * A service is announced once by its owner and stays announced for as long
 * as the announce fd is open.  `listen' blocks until a client connects and
 * returns that connection as a 9P byte stream; `connect' enqueues a client
 * and returns its end of the same stream, which is then mounted.
 */

#ifndef RINGFS_H
#define RINGFS_H

int	ringannounce(char *name);
int	ringlisten(char *name);
int	ringconnect(char *name);
int	ringmount(char *name, char *mtpt);

#endif
