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

#ifndef _KEYSTORE_H_
#define _KEYSTORE_H_

#define TARGET_MAX		32

/* ASN.1 grammar for keystores
 *
 * AndroidVerifiedBoot DEFINITIONS ::= BEGIN
 *   -- From PKCS #1/RFC3279 ASN.1 module
 *   RSAPublicKey ::= SEQUENCE {
 *       modulus           INTEGER,  -- n
 *       publicExponent    INTEGER   -- e
 *   }
 *
 *   AlgorithmIdentifier ::= SEQUENCE {
 *       algorithm OBJECT IDENTIFIER,
 *       parameters ANY DEFINED BY algorithm OPTIONAL
 *   }
 *
 *   AuthenticatedAttributes ::= SEQUENCE {
 *       target PrintableString,  -- specific version of CHARACTER STRING accepted by a compiler
 *       length INTEGER
 *   }
 *
 *   AndroidVerifiedBootSignature ::= SEQUENCE {
 *       formatVersion INTEGER,
 *       algorithmId AlgorithmIdentifier,
 *       attributes AuthenticatedAttributes,
 *       signature OCTET STRING
 *   }
 *
 *   KeyBag ::= SEQUENCE OF KeyInfo
 *
 *   KeyInfo ::= SEQUENCE {
 *       algorithm AlgorithmIdentifier,
 *       keyMaterial RSAPublicKey
 *   }
 *
 *   InnerKeystore ::= SEQUENCE {
 *       formatVersion INTEGER,
 *       bag KeyBag
 *   }
 *
 *   AndroidVerifiedBootKeystore ::= SEQUENCE {
 *       formatVersion INTEGER,
 *       bag KeyBag,
 *       signature AndroidVerifiedBootSignature
 *   }
 * END
 */

struct rsa_public_key {
	unsigned char *modulus;
	long modulus_len;
	long exponent;
};

struct algorithm_identifier {
	int nid;
	void *parameters;
	long parameters_len;
};

struct keyinfo {
	struct algorithm_identifier id;
	struct rsa_public_key key_material;
};

struct auth_attributes {
	char target[TARGET_MAX];
	long length;
};

struct keybag {
	struct keybag *next;
	struct keyinfo info;
};

struct boot_signature {
	long format_version;
	struct algorithm_identifier id;
	struct auth_attributes attributes;
	void *signature;
	long signature_len;
};

struct keystore {
	long format_version;
	struct keybag *bag; // linked list of these
	struct boot_signature sig;
};

struct keystore *get_keystore(const void *data, long size);
struct boot_signature *get_boot_signature(const void *data, long size);

void free_keystore(struct keystore *ks);
void free_boot_signature(struct boot_signature *bs);

void dump_boot_signature(struct boot_signature *bs);
void dump_keystore(struct keystore *ks);

#endif

/* vim: cindent:noexpandtab:softtabstop=8:shiftwidth=8:noshiftround
 */

