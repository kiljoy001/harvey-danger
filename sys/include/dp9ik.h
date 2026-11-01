/*
 * dp9ik: the modern 9front authentication protocol.
 *
 * dp9ik extends p9sk1 with AuthPAK (SPAKE2 over Ed448) and uses
 * ChaCha20-Poly1305 ("form1") for tickets and authenticators.  This
 * header is separate from authsrv.h so the existing p9sk1 code keeps
 * its old Ticket/Authenticator layout.
 */
#ifndef _DP9IK_H_
#define _DP9IK_H_

#include <authsrv.h>

enum
{
	AESKEYLEN	= 16,	/* AES key from the password */
	NONCELEN	= 32,	/* ticket key and authenticator nonce */
	PASSWDLEN	= 28,

	/* AuthPAK constants */
	PAKKEYLEN	= 32,
	PAKSLEN		= (448+7)/8,	/* Ed448 scalar */
	PAKPLEN		= 4*PAKSLEN,	/* extended point X,Y,Z,T */
	PAKHASHLEN	= 2*PAKPLEN,	/* hashed points PM,PN */
	PAKXLEN		= PAKSLEN,
	PAKYLEN		= PAKSLEN,

	AuthPAK		= 19,	/* authenticated key agreement */

	MAXTICKETLEN	= 12+CHALLEN+2*ANAMELEN+NONCELEN+16,
	MAXAUTHENTLEN	= 12+CHALLEN+NONCELEN+16,
};

typedef struct Authkey Authkey;
struct Authkey
{
	uchar	des[DESKEYLEN];		/* password-derived DES key */
	uchar	aes[AESKEYLEN];		/* password-derived AES key */
	uchar	pakkey[PAKKEYLEN];	/* AuthPAK shared key */
	uchar	pakhash[PAKHASHLEN];	/* hashed AuthPAK points */
};

typedef struct PAKpriv PAKpriv;
struct PAKpriv
{
	int	isclient;
	uchar	x[PAKXLEN];		/* ephemeral scalar */
	uchar	y[PAKYLEN];		/* public point */
};

typedef struct Dp9ikTicket Dp9ikTicket;
struct Dp9ikTicket
{
	char	num;
	char	chal[CHALLEN];
	char	cuid[ANAMELEN];
	char	suid[ANAMELEN];
	uchar	key[NONCELEN];
	int	form;			/* not transmitted */
};

typedef struct Dp9ikAuthenticator Dp9ikAuthenticator;
struct Dp9ikAuthenticator
{
	char	num;
	char	chal[CHALLEN];
	uchar	rand[NONCELEN];
};

/* password */
void	dp9ik_passtokey(Authkey*, char*);

/* AuthPAK */
void	authpak_hash(Authkey*, char*);
void	authpak_new(PAKpriv*, Authkey*, uchar y[PAKYLEN], int isclient);
int	authpak_finish(PAKpriv*, Authkey*, uchar y[PAKYLEN]);

/* form1 (ChaCha20-Poly1305) */
int	dp9ik_form1check(char*, int);
int	dp9ik_form1B2M(char*, int, uchar key[PAKKEYLEN]);
int	dp9ik_form1M2B(char*, int, uchar key[PAKKEYLEN]);

/* ticket and authenticator wire format */
int	dp9ik_convT2M(Dp9ikTicket*, char*, int, Authkey*);
int	dp9ik_convM2T(char*, int, Dp9ikTicket*, Authkey*);
int	dp9ik_convA2M(Dp9ikAuthenticator*, char*, int, Dp9ikTicket*);
int	dp9ik_convM2A(char*, int, Dp9ikAuthenticator*, Dp9ikTicket*);

#endif
