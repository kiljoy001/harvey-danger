#ifndef KEYROOT_TA_DP9IK_H
#define KEYROOT_TA_DP9IK_H

#include "authsrv.h"

enum
{
	AESKEYLEN	= 16,
	NONCELEN	= 32,
	PAKKEYLEN	= 32,
	PAKSLEN		= (448+7)/8,
	PAKPLEN		= 4*PAKSLEN,
	PAKHASHLEN	= 2*PAKPLEN,
	PAKXLEN		= PAKSLEN,
	PAKYLEN		= PAKSLEN,

	MAXTICKETLEN	= 12+CHALLEN+2*ANAMELEN+NONCELEN+16,
	MAXAUTHENTLEN	= 12+CHALLEN+NONCELEN+16,
};

typedef struct Authkey Authkey;
struct Authkey
{
	u8	des[DESKEYLEN];
	u8	aes[AESKEYLEN];
	u8	pakkey[PAKKEYLEN];
	u8	pakhash[PAKHASHLEN];
};

typedef struct PAKpriv PAKpriv;
struct PAKpriv
{
	int	isclient;
	u8	x[PAKXLEN];
	u8	y[PAKYLEN];
};

typedef struct Dp9ikTicket Dp9ikTicket;
struct Dp9ikTicket
{
	char	num;
	char	chal[CHALLEN];
	char	cuid[ANAMELEN];
	char	suid[ANAMELEN];
	u8	key[NONCELEN];
	int	form;
};

typedef struct Dp9ikAuthenticator Dp9ikAuthenticator;
struct Dp9ikAuthenticator
{
	char	num;
	char	chal[CHALLEN];
	u8	rand[NONCELEN];
};

void	authpak_hash(Authkey*, char*);
void	authpak_new(PAKpriv*, Authkey*, u8 y[PAKYLEN], int isclient);
int	authpak_finish(PAKpriv*, Authkey*, u8 y[PAKYLEN]);

int	dp9ik_form1check(char*, int);
int	dp9ik_form1B2M(char*, int, u8 key[PAKKEYLEN]);
int	dp9ik_form1M2B(char*, int, u8 key[PAKKEYLEN]);

int	dp9ik_convT2M(Dp9ikTicket*, char*, int, Authkey*);
int	dp9ik_convM2T(char*, int, Dp9ikTicket*, Authkey*);
int	dp9ik_convA2M(Dp9ikAuthenticator*, char*, int, Dp9ikTicket*);
int	dp9ik_convM2A(char*, int, Dp9ikAuthenticator*, Dp9ikTicket*);

#endif
