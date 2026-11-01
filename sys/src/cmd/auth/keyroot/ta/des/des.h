#ifndef KEYROOT_DES_H
#define KEYROOT_DES_H

#include <stdint.h>

typedef uint8_t u8;
typedef uint32_t u32;

enum {
	DES3E	= 0,
	DES3D	= 1,
	DES3EEE	= 0,
	DES3EDE	= 2,
	DES3DED	= 5,
	DES3DDD	= 7,
};

void block_cipher(u32 key[32], u8 text[8], int decrypting);
void triple_block_cipher(u32 expanded_key[3][32], u8 text[8], int ende);
void des_key_setup(u8 key[8], u32 ek[32]);
void des56to64(u8 *k56, u8 *k64);
void des64to56(u8 *k64, u8 *k56);
void key_setup(u8 key[7], u32 ek[32]);
int encrypt(void *key, void *vbuf, int n);
int decrypt(void *key, void *vbuf, int n);

#endif
