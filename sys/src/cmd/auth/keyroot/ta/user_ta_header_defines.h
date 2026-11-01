/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef __USER_TA_HEADER_DEFINES_H
#define __USER_TA_HEADER_DEFINES_H

#define TA_UUID \
	{ 0x6b1d7c2e, 0x5a41, 0x4f0b, \
	  { 0x9c, 0x7e, 0x2d, 0x1a, 0x8e, 0x6f, 0x3b, 0x90 } }

#define TA_FLAGS		(TA_FLAG_SINGLE_INSTANCE | TA_FLAG_MULTI_SESSION)
#define TA_STACK_SIZE		(128 * 1024)
#define TA_DATA_SIZE		(512 * 1024)
#define TA_VERSION		"1.0"
#define TA_DESCRIPTION		"keyroot probe"

#endif /* __USER_TA_HEADER_DEFINES_H */
