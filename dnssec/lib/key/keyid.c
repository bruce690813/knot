#include <assert.h>
#include <gnutls/abstract.h>
#include <string.h>

#include "hex.h"
#include "key.h"
#include "key/keyid.h"
#include "shared.h"

/* -- internal API --------------------------------------------------------- */

#define convert(type, key, id) \
{ \
	size_t id_size = DNSSEC_KEY_ID_SIZE; \
	gnutls_##type##_get_key_id(key, 0, id, &id_size); \
	assert(id_size == DNSSEC_KEY_ID_SIZE); \
}

void gnutls_pubkey_to_key_id(gnutls_pubkey_t key, dnssec_key_id_t id)
{
	convert(pubkey, key, id);
}

void gnutls_x509_privkey_to_key_id(gnutls_x509_privkey_t key, dnssec_key_id_t id)
{
	convert(x509_privkey, key, id);
}

#undef convert

/* -- public API ----------------------------------------------------------- */

_public_
char *dnssec_key_id_to_string(const dnssec_key_id_t id)
{
	const dnssec_binary_t binary = {
		.data = (uint8_t *)id,
		.size = DNSSEC_KEY_ID_SIZE
	};

	return hex_to_string(&binary);
}

_public_
void dnssec_key_id_copy(const dnssec_key_id_t from, dnssec_key_id_t to)
{
	if (!from || !to || from == to) {
		return;
	}

	memmove(to, from, DNSSEC_KEY_ID_SIZE);
}

_public_
int dnssec_key_id_cmp(const dnssec_key_id_t one, const dnssec_key_id_t two)
{
	if (one == two) {
		return 0;
	} else {
		return memcmp(one, two, DNSSEC_KEY_ID_SIZE);
	}
}

_public_
bool dnssec_key_id_equal(const dnssec_key_id_t one, const dnssec_key_id_t two)
{
	return dnssec_key_id_cmp(one, two) == 0;
}
