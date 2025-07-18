/* Copyright  (C) 2010-2020 The RetroArch team
 *
 * ---------------------------------------------------------------------------------------
 * The following license statement only applies to this file (lrc_hash.h).
 * ---------------------------------------------------------------------------------------
 *
 * Permission is hereby granted, free of charge,
 * to any person obtaining a copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef __LIBRETRO_SDK_HASH_H
#define __LIBRETRO_SDK_HASH_H

#include <stdint.h>
#include <stddef.h>

#include <compat/msvc.h>
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <retro_inline.h>

#include <retro_common_api.h>

#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#endif

RETRO_BEGIN_DECLS

/**
 * sha256_hash:
 * @s                 : Output.
 * @in                : Input.
 * @len               : Size of @out.
 *
 * Hashes SHA256 and outputs a human readable string.
 **/
void sha256_hash(char *s, const uint8_t *in, size_t len);

int sha1_calculate(const char *path, char *result);

uint32_t djb2_calculate(const char *str);

#ifdef __APPLE__
typedef CC_MD5_CTX MD5_CTX;
#define MD5_Init CC_MD5_Init
#define MD5_Update CC_MD5_Update
#define MD5_Final CC_MD5_Final
#else

/* Any 32-bit or wider unsigned integer data type will do */
typedef unsigned int MD5_u32plus;

typedef struct {
	MD5_u32plus lo, hi;
	MD5_u32plus a, b, c, d;
	unsigned char buffer[64];
	MD5_u32plus block[16];
} MD5_CTX;

/*
 * This is an OpenSSL-compatible implementation of the RSA Data Security, Inc.
 * MD5 Message-Digest Algorithm (RFC 1321).
 *
 * Homepage:
 * http://openwall.info/wiki/people/solar/software/public-domain-source-code/md5
 *
 * Author:
 * Alexander Peslyak, better known as Solar Designer <solar at openwall.com>
 *
 * This software was written by Alexander Peslyak in 2001.  No copyright is
 * claimed, and the software is hereby placed in the public domain.
 * In case this attempt to disclaim copyright and place the software in the
 * public domain is deemed null and void, then the software is
 * Copyright (c) 2001 Alexander Peslyak and it is hereby released to the
 * general public under the following terms:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted.
 *
 * There's ABSOLUTELY NO WARRANTY, express or implied.
 *
 * See md5.c for more information.
 */

void MD5_Init(MD5_CTX *ctx);
void MD5_Update(MD5_CTX *ctx, const void *data, unsigned long size);
void MD5_Final(unsigned char *result, MD5_CTX *ctx);

#endif

RETRO_END_DECLS

#endif
