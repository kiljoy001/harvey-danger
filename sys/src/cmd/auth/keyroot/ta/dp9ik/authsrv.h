#ifndef KEYROOT_TA_AUTHSRV_H
#define KEYROOT_TA_AUTHSRV_H

/*
 * Minimal authsrv constants for the trusted-application build.
 * The wire values must match sys/include/authsrv.h.
 */
enum
{
	ANAMELEN	= 28,
	DOMLEN		= 48,
	DESKEYLEN	= 7,
	CHALLEN		= 8,

	AuthTreq	= 1,
	AuthPass	= 3,
	AuthOK		= 4,
	AuthErr		= 5,
	AuthTs		= 64,
	AuthTc		= 65,
	AuthAs		= 66,
	AuthAc		= 67,
	AuthTp		= 68,
	AuthHr		= 69,
	AuthPAK		= 19,
};

#define TICKREQLEN (3*ANAMELEN+CHALLEN+DOMLEN+1)

typedef struct Ticketreq Ticketreq;
struct Ticketreq
{
	char	type;
	char	authid[ANAMELEN];
	char	authdom[DOMLEN];
	char	chal[CHALLEN];
	char	hostid[ANAMELEN];
	char	uid[ANAMELEN];
};

#endif
