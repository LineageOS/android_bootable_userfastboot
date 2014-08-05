/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *	  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#include <openssl/asn1.h>
#include <openssl/err.h>
#include <openssl/objects.h>

#include "keystore.h"
#include "asn1.h"

#ifndef KERNELFLINGER
#include "userfastboot_ui.h"
#else
#define pr_error(x...) do { } while(0)
#define pr_debug(x...) do { } while(0)
#endif

static void free_keybag(struct keybag *kb)
{
	while (kb) {
		struct keybag *n = kb;
		kb = kb->next;

		free(n->info.id.parameters);
		free(n->info.key_material.modulus);
		free(n);
	}
}


void free_keystore(struct keystore *ks)
{
	if (!ks)
		return;

	free(ks->sig.signature);
	free(ks->sig.id.parameters);
	free_keybag(ks->bag);
	free(ks);
}


void free_boot_signature(struct boot_signature *bs)
{
	if (!bs)
		return;

	free(bs->signature);
	free(bs->id.parameters);
	free(bs);
}

void dump_boot_signature(struct boot_signature *bs)
{
	pr_debug("boot sig format       %ld\n", bs->format_version);
	pr_debug("boot sig algo id      %d\n", bs->id.nid);
	pr_debug("target                %s\n", bs->attributes.target);
	pr_debug("length                %ld\n", bs->attributes.length);
	pr_debug("signature len         %ld\n", bs->signature_len);
}


void dump_keystore(struct keystore *ks)
{
	struct keybag *kb;
	if (!ks)
		return;

	pr_debug("keystore-----------\n");
	pr_debug("format_version        %ld\n", ks->format_version);
	kb = ks->bag;
	pr_debug("key-bag------------\n");
	while (kb) {
		struct keyinfo *ki = &kb->info;
		pr_debug("key-info ---------\n");
		pr_debug("algo id               %d\n", ki->id.nid);
		pr_debug("modulus len           %ld\n", ki->key_material.modulus_len);
		pr_debug("exponent              %lx\n", ki->key_material.exponent);
		kb = kb->next;
		pr_debug("--end-key-info----\n");
	}
	pr_debug("-end-key-bag------\n");
	dump_boot_signature(&ks->sig);
	pr_debug("-end-keystore-------\n");
}


static int decode_algorithm_identifier(const unsigned char **datap, long *sizep,
		struct algorithm_identifier *ai)
{
	long seq_size = *sizep;
	const unsigned char *orig = *datap;

	if (consume_sequence(datap, &seq_size) < 0)
		return -1;

	if (decode_object(datap, &seq_size, &ai->nid))
		return -1;

	if (seq_size) {
		pr_error("parameters not supported yet\n");
		return -1;
	} else {
		ai->parameters = NULL;
	}

	*sizep = *sizep - (*datap - orig);
	return 0;
}


static int decode_auth_attributes(const unsigned char **datap, long *sizep,
		struct auth_attributes *aa)
{
	long seq_size = *sizep;
	const unsigned char *orig = *datap;

	if (consume_sequence(datap, &seq_size) < 0)
		return -1;

	if (decode_printable_string(datap, &seq_size, aa->target,
				sizeof(aa->target)))
		return -1;

	if (decode_integer(datap, &seq_size, 0, &aa->length,
				NULL, NULL))
		return -1;

	*sizep = *sizep - (*datap - orig);
	return 0;
}


static int decode_boot_signature(const unsigned char **datap, long *sizep,
		struct boot_signature *bs)
{
	long seq_size = *sizep;
	const unsigned char *orig = *datap;

	if (consume_sequence(datap, &seq_size) < 0)
		return -1;

	if (decode_integer(datap, &seq_size, 0, &bs->format_version,
				NULL, NULL))
		return -1;

	if (decode_algorithm_identifier(datap, &seq_size, &bs->id)) {
		pr_error("bad algorithm identifier\n");
		return -1;
	}

	if (decode_auth_attributes(datap, &seq_size, &bs->attributes)) {
		pr_error("bad authenticated attributes\n");
		free(bs->id.parameters);
		return -1;
	}

	if (decode_octet_string(datap, &seq_size, (unsigned char **)&bs->signature,
				&bs->signature_len)) {
		free(bs->id.parameters);
		return -1;
	}

	*sizep = *sizep - (*datap - orig);
	return 0;
}


static int decode_rsa_public_key(const unsigned char **datap, long *sizep,
		struct rsa_public_key *rpk)
{
	long seq_size = *sizep;
	const unsigned char *orig = *datap;

	if (consume_sequence(datap, &seq_size) < 0)
		return -1;

	if (decode_integer(datap, &seq_size, 1, NULL, &rpk->modulus,
				&rpk->modulus_len))
		return -1;

	if (decode_integer(datap, &seq_size, 0, &rpk->exponent,
				NULL, NULL)) {
		free(rpk->modulus);
		return -1;
	}

	*sizep = *sizep - (*datap - orig);
	return 0;
}


static int decode_keyinfo(const unsigned char **datap, long *sizep,
		struct keyinfo *ki)
{
	long seq_size = *sizep;
	const unsigned char *orig = *datap;

	if (consume_sequence(datap, &seq_size) < 0)
		return -1;

	if (decode_algorithm_identifier(datap, &seq_size, &ki->id)) {
		pr_error("bad algorithm identifier\n");
		return -1;
	}

	if (decode_rsa_public_key(datap, &seq_size, &ki->key_material)) {
		pr_error("bad RSA public key data\n");
		free(ki->id.parameters);
		ki->id.parameters = NULL;
		return -1;
	}

	*sizep = *sizep - (*datap - orig);
	return 0;
}


static int decode_keybag(const unsigned char **datap, long *sizep,
		struct keybag **kbp)
{
	long seq_size = *sizep;
	const unsigned char *orig = *datap;
	struct keybag *ret = NULL;

	if (consume_sequence(datap, &seq_size) < 0)
		goto error;

	while (seq_size > 0) {
		struct keybag *kb = malloc(sizeof *kb);
		if (!kb) {
			pr_error("out of memory\n");
			goto error;
		}

		if (decode_keyinfo(datap, &seq_size, &kb->info)) {
			pr_error("bad keyinfo data\n");
			free(kb);
			goto error;
		}
		kb->next = ret;
		ret = kb;
	}

	*sizep = *sizep - (*datap - orig);
	*kbp = ret;
	return 0;
error:
	free_keybag(ret);
	return -1;
}


static int decode_keystore(const unsigned char **datap, long *sizep,
		struct keystore *ks)
{
	long seq_size = *sizep;
	const unsigned char *orig = *datap;

	if (consume_sequence(datap, &seq_size) < 0)
		return -1;

	if (decode_integer(datap, &seq_size, 0, &ks->format_version,
			NULL, NULL))
		return -1;

	if (decode_keybag(datap, &seq_size, &ks->bag)) {
		pr_error("bad keybag data\n");
		return -1;
	}

	if (decode_boot_signature(datap, &seq_size, &ks->sig)) {
		free_keybag(ks->bag);
		pr_error("bad boot signature data\n");
		return -1;
	}

	*sizep = *sizep - (*datap - orig);
	return 0;
}


struct keystore *get_keystore(const void *data, long size)
{
	const unsigned char *pos = data;
	long remain = size;
	struct keystore *ks = malloc(sizeof(*ks));
	if (!ks)
		return NULL;

	if (decode_keystore(&pos, &remain, ks)) {
		free(ks);
		return NULL;
	}
	return ks;
}

struct boot_signature *get_boot_signature(const void *data, long size)
{
	const unsigned char *pos = data;
	long remain = size;
	struct boot_signature *bs = malloc(sizeof(*bs));
	if (!bs)
		return NULL;

	if (decode_boot_signature(&pos, &remain, bs)) {
		free(bs);
		return NULL;
	}
	return bs;
}

/* vim: cindent:noexpandtab:softtabstop=8:shiftwidth=8:noshiftround
 */

