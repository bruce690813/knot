	/*  Copyright (C) 2011 CZ.NIC, z.s.p.o. <knot-dns@labs.nic.cz>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>

#include "common.h"
#include "common/mempattern.h"
#include "rrset.h"
#include "common/descriptor_new.h"
#include "util/debug.h"
#include "util/utils.h"
#include "packet/response.h"
#include "util/wire.h"

static const size_t KNOT_RESPONSE_MAX_PTR = 16383;

/*----------------------------------------------------------------------------*/
/* Non-API functions                                                          */
/*----------------------------------------------------------------------------*/

static size_t rrset_rdata_offset(const knot_rrset_t *rrset,
                                 size_t pos)
{
	if (rrset == NULL || rrset->rdata_indices == NULL ||
	    pos >= rrset->rdata_count || pos == 0) {
		return 0;
	}
	
	assert(rrset->rdata_count >= 2);
	return rrset->rdata_indices[pos - 1];
}

static uint8_t *rrset_rdata_pointer(const knot_rrset_t *rrset,
                                    size_t pos)
{
	if (rrset == NULL || rrset->rdata == NULL
	    || pos >= rrset->rdata_count) {
		return NULL;
	}
	
	return rrset->rdata + rrset_rdata_offset(rrset, pos);
}

void knot_rrset_rdata_dump(const knot_rrset_t *rrset, size_t rdata_pos)
{
	fprintf(stderr, "      ------- RDATA pos=%d -------\n", rdata_pos);
	if (rrset->rdata_count == 0) {
		fprintf(stderr, "      There are no rdata in this RRset!\n");
		fprintf(stderr, "      ------- RDATA -------\n");
		return;
	}
	const rdata_descriptor_t *desc =
		get_rdata_descriptor(knot_rrset_type(rrset));
	assert(desc != NULL);
	
	size_t offset = 0;
	for (int i = 0; desc->block_types[i] != KNOT_RDATA_WF_END; i++) {
		int item = desc->block_types[i];
		uint8_t *rdata = rrset_rdata_pointer(rrset, rdata_pos);
		if (descriptor_item_is_dname(item)) {
			knot_dname_t *dname;
			memcpy(&dname, rdata + offset, sizeof(knot_dname_t *));
			char *name = knot_dname_to_str(dname);
			if (dname == NULL) {
				fprintf(stderr, "DNAME error.\n");
				return;
			}
			fprintf(stderr, "block=%d: (%p) DNAME=%s\n",
			        i, dname, name);
			free(name);
			offset += sizeof(knot_dname_t *);
		} else if (descriptor_item_is_fixed(item)) {
			fprintf(stderr, "block=%d Raw data (size=%d):\n",
			        i, item);
			hex_print((char *)(rdata + offset), item);
			offset += item;
		} else if (descriptor_item_is_remainder(item)) {
			fprintf(stderr, "block=%d Remainder (size=%d):\n",
			        i, rrset_rdata_item_size(rrset,
			                                 rdata_pos) - offset);
			hex_print((char *)(rdata + offset),
			          rrset_rdata_item_size(rrset,
			                                rdata_pos) - offset);
		} else {
			fprintf(stderr, "NAPTR, failing miserably\n");
			assert(rrset->type == KNOT_RRTYPE_NAPTR);
			assert(0);
		}
	}
}

static size_t rrset_rdata_naptr_bin_chunk_size(const knot_rrset_t *rrset,
                                               size_t pos)
{
	if (rrset == NULL || rrset->rdata_count >= pos) {
		return 0;
	}
	
	size_t size = 0;
	uint8_t *rdata = rrset_rdata_pointer(rrset, pos);
	assert(rdata);
	
	/* Two shorts at the beginning. */
	size += 4;
	/* 3 binary TXTs with length in the first byte. */
	for (int i = 0; i < 3; i++) {
		size += *(rdata + size);
	}
	
	/* 
	 * Dname remaning, but we usually want to get to the DNAME, so
	 * there's no need to include it in the returned size.
	 */
	
	return size;
}

/*----------------------------------------------------------------------------*/
/* API functions                                                              */
/*----------------------------------------------------------------------------*/

uint32_t rrset_rdata_size_total(const knot_rrset_t *rrset)
{
	if (rrset == NULL || rrset->rdata_indices == NULL ||
	    rrset->rdata_count == 0) {
		return 0;
	}
	
	/* Last index denotes end of all RRs. */
	return (rrset->rdata_indices[rrset->rdata_count - 1]);
}

knot_rrset_t *knot_rrset_new(knot_dname_t *owner, uint16_t type,
                             uint16_t rclass, uint32_t ttl)
{
	knot_rrset_t *ret = xmalloc(sizeof(knot_rrset_t));

	ret->rdata = NULL;
	ret->rdata_count = 0;
	ret->rdata_indices = NULL;

	/* Retain reference to owner. */
	knot_dname_retain(owner);

	ret->owner = owner;
	ret->type = type;
	ret->rclass = rclass;
	ret->ttl = ttl;
	ret->rrsigs = NULL;

	return ret;
}

/*----------------------------------------------------------------------------*/

int knot_rrset_add_rdata_single(knot_rrset_t *rrset, uint8_t *rdata,
                                uint32_t size)
{
	rrset->rdata_indices = xmalloc(sizeof(uint32_t));
	rrset->rdata_indices[0] = size;
	rrset->rdata = rdata;
	rrset->rdata_count = 1;
	return KNOT_EOK;
}

int knot_rrset_add_rdata(knot_rrset_t *rrset,
                         uint8_t *rdata, uint16_t size)
{
	if (rrset == NULL || rdata == NULL || size == 0) {
		return KNOT_EINVAL;
	}
	
	uint8_t *p = knot_rrset_create_rdata(rrset, size);
	memcpy(p, rdata, size);
	
	return KNOT_EOK;
}

/*----------------------------------------------------------------------------*/

uint8_t* knot_rrset_create_rdata(knot_rrset_t *rrset, uint16_t size)
{
	if (rrset == NULL || size == 0) {
		return NULL;
	}
	
	uint32_t total_size = rrset_rdata_size_total(rrset);
	
	/* Realloc indices. We will allocate exact size to save space. */
	/* TODO this sucks big time - allocation of length 1. */
	/* But another variable holding allocated count is out of question. What now?*/
	rrset->rdata_indices = xrealloc(rrset->rdata_indices,
	                                (rrset->rdata_count + 1) * sizeof(uint32_t));

	/* Realloc actual data. */
	rrset->rdata = xrealloc(rrset->rdata, total_size + size);
	
	/* Pointer to new memory. */
	uint8_t *dst = rrset->rdata + total_size;
	
	/* Update indices. */
	if (rrset->rdata_count == 0) {
		rrset->rdata_indices[0] = size;
	} else {
		rrset->rdata_indices[rrset->rdata_count - 1] = total_size;
		rrset->rdata_indices[rrset->rdata_count] = total_size + size;
	}
	
	++rrset->rdata_count;
	
	return dst;
}

/*----------------------------------------------------------------------------*/


uint16_t rrset_rdata_item_size(const knot_rrset_t *rrset,
                               size_t pos)
{
	if (rrset == NULL || rrset->rdata_indices == NULL ||
	    rrset->rdata_count == 0) {
		return 0;
	}
	
	if (rrset->rdata_count == 1) {
		return rrset_rdata_size_total(rrset);
	}
	
	assert(rrset->rdata_count >= 2);
	return rrset_rdata_offset(rrset, pos) -
	                          rrset_rdata_offset(rrset, pos - 1);
}

/*----------------------------------------------------------------------------*/

int knot_rrset_set_rrsigs(knot_rrset_t *rrset, knot_rrset_t *rrsigs)
{
	if (rrset == NULL) {
		return KNOT_EINVAL;
	}

	rrset->rrsigs = rrsigs;
	return KNOT_EOK;
}

/*----------------------------------------------------------------------------*/

int knot_rrset_add_rrsigs(knot_rrset_t *rrset, knot_rrset_t *rrsigs,
                          knot_rrset_dupl_handling_t dupl)
{
	if (rrset == NULL || rrsigs == NULL
	    || knot_dname_compare_non_canon(rrset->owner, rrsigs->owner) != 0) {
		return KNOT_EINVAL;
	}

	int rc;
	if (rrset->rrsigs != NULL) {
		if (dupl == KNOT_RRSET_DUPL_MERGE) {
			rc = knot_rrset_merge_no_dupl((void **)&rrset->rrsigs,
			                              (void **)&rrsigs);
			if (rc != KNOT_EOK) {
				return rc;
			} else {
				return 1;
			}
		} else if (dupl == KNOT_RRSET_DUPL_SKIP) {
			return 2;
		} else if (dupl == KNOT_RRSET_DUPL_REPLACE) {
			rrset->rrsigs = rrsigs;
		}
	} else {
		if (rrset->ttl != rrsigs->ttl) {
			rrsigs->ttl = rrset->ttl;
		}
		rrset->rrsigs = rrsigs;
	}

	return KNOT_EOK;
}

/*----------------------------------------------------------------------------*/

const knot_dname_t *knot_rrset_owner(const knot_rrset_t *rrset)
{
	return rrset->owner;
}

/*----------------------------------------------------------------------------*/

knot_dname_t *knot_rrset_get_owner(const knot_rrset_t *rrset)
{
	return rrset->owner;
}

/*----------------------------------------------------------------------------*/

void knot_rrset_set_owner(knot_rrset_t *rrset, knot_dname_t* owner)
{
	if (rrset) {
		/* Retain new owner and release old owner. */
		knot_dname_retain(owner);
		knot_dname_release(rrset->owner);
		rrset->owner = owner;
	}
}

/*----------------------------------------------------------------------------*/

void knot_rrset_set_ttl(knot_rrset_t *rrset, uint32_t ttl)
{
	if (rrset) {
		rrset->ttl = ttl;
	}
}

/*----------------------------------------------------------------------------*/

uint16_t knot_rrset_type(const knot_rrset_t *rrset)
{
	return rrset->type;
}

/*----------------------------------------------------------------------------*/

uint16_t knot_rrset_class(const knot_rrset_t *rrset)
{
	return rrset->rclass;
}

/*----------------------------------------------------------------------------*/

uint32_t knot_rrset_ttl(const knot_rrset_t *rrset)
{
	return rrset->ttl;
}

/*----------------------------------------------------------------------------*/

uint8_t *knot_rrset_get_rdata(const knot_rrset_t *rrset, size_t rdata_pos)
{
	return rrset_rdata_pointer(rrset, rdata_pos);
}

/*----------------------------------------------------------------------------*/

uint16_t knot_rrset_rdata_rr_count(const knot_rrset_t *rrset)
{
	if (rrset != NULL) {
		return rrset->rdata_count;
	} else {
		return 0;
	}
}

/*----------------------------------------------------------------------------*/

const knot_rrset_t *knot_rrset_rrsigs(const knot_rrset_t *rrset)
{
	if (rrset == NULL) {
		return NULL;
	} else {
		return rrset->rrsigs;
	}
}

/*----------------------------------------------------------------------------*/

knot_rrset_t *knot_rrset_get_rrsigs(knot_rrset_t *rrset)
{
	if (rrset == NULL) {
		assert(0);
		return NULL;
	} else {
		return rrset->rrsigs;
	}
}

static size_t rrset_rdata_remainder_size(const knot_rrset_t *rrset,
                                         size_t offset,
                                         size_t pos)
{
	if (pos == 0) {
		return rrset->rdata_indices[1] - offset;
	} else {
		return (rrset->rdata_indices[pos + 1] -
		        rrset->rdata_indices[pos]) - offset;
	}
}

/*----------------------------------------------------------------------------*/

static int rrset_rdata_compare_one(const knot_rrset_t *rrset1,
                                   const knot_rrset_t *rrset2,
                                   size_t pos1, size_t pos2)
{
	uint8_t *r1 = rrset_rdata_pointer(rrset1, pos1);
	uint8_t *r2 = rrset_rdata_pointer(rrset2, pos2);
	assert(rrset1->type == rrset2->type);
	const rdata_descriptor_t *desc = get_rdata_descriptor(rrset1->type);
	int cmp = 0;
	size_t offset = 0;

	for (int i = 0; desc->block_types[i] != KNOT_RDATA_WF_END; i++) {
		if (descriptor_item_is_dname(desc->block_types[i])) {
			cmp = knot_dname_compare((knot_dname_t *)(r1 + offset),
			                         (knot_dname_t *)(r2 + offset));
			offset += sizeof(knot_dname_t *);
		} else if (descriptor_item_is_fixed(desc->block_types[i])) {
			cmp = memcmp(r1 + offset, r2 + offset,
			             desc->block_types[i]);
			offset += desc->block_types[i];
		} else if (descriptor_item_is_remainder(desc->block_types[i])) {
			size_t size1 = rrset_rdata_remainder_size(rrset1, offset,
			                                          pos1);
			size_t size2 = rrset_rdata_remainder_size(rrset2, offset,
			                                          pos2);
			cmp = memcmp(r1 + offset, r2 + offset,
			             size1 <= size2 ? size1 : size2);
			/* No need to move offset, this should be end anyway. */
			assert(desc->block_types[i + 1] == KNOT_RDATA_WF_END);
		} else {
			assert(rrset1->type == KNOT_RRTYPE_NAPTR);
			size_t naptr_chunk_size1 =
				rrset_rdata_naptr_bin_chunk_size(rrset1, pos1);
			size_t naptr_chunk_size2 =
				rrset_rdata_naptr_bin_chunk_size(rrset2, pos2);
			cmp = memcmp(r1, r2,
			             naptr_chunk_size1 <= naptr_chunk_size2 ?
			             naptr_chunk_size1 : naptr_chunk_size2);
			if (cmp != 0) {
				return cmp;
			}
		
			/* Binary part was equal, we have to compare DNAMEs. */
			assert(naptr_chunk_size1 == naptr_chunk_size2);
			offset += naptr_chunk_size1;
			cmp = knot_dname_compare((knot_dname_t *)(r1 + offset),
			                         (knot_dname_t *)(r2 + offset));
			offset += sizeof(knot_dname_t *);
		}

		if (cmp != 0) {
			return cmp;
		}
	}

	assert(cmp == 0);
	return 0;
}

int knot_rrset_compare_rdata(const knot_rrset_t *r1, const knot_rrset_t *r2)
{
	if (r1 == NULL || r2 == NULL ||( r1->type != r2->type)) {
		return KNOT_EINVAL;
	}

	const rdata_descriptor_t *desc =
		get_rdata_descriptor(r1->type);
	if (desc == NULL) {
		return KNOT_EINVAL;
	}
	
	/*
	 * This actually does not make a lot of sense, but easiest solution
	 * is to order the arrays before we compare them all. It is okay since
	 * this full compare operation is needed scarcely.
	 */
	
	/* TODO */
	
	return 0;
}

int knot_rrset_rdata_equal(const knot_rrset_t *r1, const knot_rrset_t *r2)
{
	if (r1 == NULL || r2 == NULL ||( r1->type != r2->type)) {
		return KNOT_EINVAL;
	}

	const rdata_descriptor_t *desc =
		get_rdata_descriptor(r1->type);
	if (desc == NULL) {
		return KNOT_EINVAL;
	}

	// compare RDATA sets (order is not significant)

	// find all RDATA from r1 in r2
	int found = 0;
	for (uint16_t i = 0; i < r1->rdata_count; i++) {
		found = 0;
		for (uint16_t j = 0; j < r2->rdata_count && !found; j++) {
			found = !rrset_rdata_compare_one(r1, r2, i, j);
		}
	}
	
	if (!found) {
		return 0;
	}
	
	// other way around
	for (uint16_t i = 0; i < r2->rdata_count; i++) {
		found = 0;
		for (uint16_t j = 0; j < r1->rdata_count && !found; j++) {
			found = !rrset_rdata_compare_one(r1, r2, i, j);
		}
	}
	
	if (!found) {
		return 0;
	}
	
	return 1;
}

/*----------------------------------------------------------------------------*/

static int knot_rrset_rdata_to_wire_one(const knot_rrset_t *rrset,
                                        size_t rdata_pos,
                                        uint8_t **pos,
                                        size_t max_size, size_t *rr_size,
                                        compression_param_t *comp)
{
	assert(rrset);
	assert(pos);
	
	size_t size = 0;
	
	if (comp) {
		// put owner if needed (already compressed)
		knot_compr_t *compr = comp->compr;
		if (compr->owner.pos == 0 ||
		    compr->owner.pos > KNOT_RESPONSE_MAX_PTR) {
			memcpy(*pos, compr->owner.wire,
			       compr->owner.size);
			compr->owner.pos = compr->wire_pos;
			*pos += compr->owner.size;
			size += compr->owner.size;
			dbg_response_detail("Saving compressed owner: size %zu\n",
			                    compr->owner.size);
		} else {
			dbg_response_detail("Putting pointer: %zu\n",
			                    compr->owner.pos);
			knot_wire_put_pointer(*pos, compr->owner.pos);
			*pos += 2;
			size += 2;
		}
	} else {
		// check if owner fits
		if (size + knot_dname_size(rrset->owner) + 10 > max_size) {
			dbg_rrset_detail("Owner does not fit into wire.\n");
			return KNOT_ESPACE;
		}

		memcpy(*pos, knot_dname_name(rrset->owner), 
		       knot_dname_size(rrset->owner));
		*pos += knot_dname_size(rrset->owner);
		size += knot_dname_size(rrset->owner);
	}

	dbg_rrset_detail("Max size: %zu, size: %d\n", max_size, size);

	// put rest of RR 'header'
	knot_wire_write_u16(*pos, rrset->type);
	dbg_rrset_detail("  Type: %u\n", rrset->type);
	*pos += 2;

	knot_wire_write_u16(*pos, rrset->rclass);
	dbg_rrset_detail("  Class: %u\n", rrset->rclass);
	*pos += 2;

	knot_wire_write_u32(*pos, rrset->ttl);
	dbg_rrset_detail("  TTL: %u\n", rrset->ttl);
	*pos += 4;

	// save space for RDLENGTH
	uint8_t *rdlength_pos = *pos;
	*pos += 2;

	size += 10;

	/* Get pointer into RDATA array. */
	uint8_t *rdata = rrset_rdata_pointer(rrset, rdata_pos);
	assert(rdata);
	/* Offset into one RDATA array. */
	size_t offset = 0;
	/* Actual RDLENGHTH. */
	uint16_t rdlength = 0;
	
	const rdata_descriptor_t *desc = get_rdata_descriptor(rrset->type);
	assert(desc);

	for (int i = 0; desc->block_types[i] != KNOT_RDATA_WF_END; i++) {
		int item = desc->block_types[i];
		if (comp && descriptor_item_is_compr_dname(item)) {
			knot_dname_t *dname;
			memcpy(&dname, rdata + offset, sizeof(knot_dname_t *));
			assert(dname);
			int ret =
				knot_response_compress_dname(dname,
				comp->compr, *pos,
				max_size - size - rdlength,
				comp->compr_cs);

			if (ret < 0) {
				return KNOT_ESPACE;
			}

			dbg_response_detail("Compressed dname size: %d\n", ret);
			*pos += ret;
			rdlength += ret;
			comp->compr->wire_pos += ret;
			size += ret;
			// TODO: compress domain name ??? for LS
		} else if (descriptor_item_is_dname(item)) {
			knot_dname_t *dname;
			memcpy(&dname, rdata + offset, sizeof(knot_dname_t *));
			assert(dname);
			if (size + rdlength + dname->size > max_size) {
				return KNOT_ESPACE;
			}
dbg_rrset_exec_detail(
			char *name = knot_dname_to_str(dname);
			dbg_rrset_detail("Saving this DNAME=%s\n", name);
			free(name);
);
			// save whole domain name
			memcpy(*pos, knot_dname_name(dname), 
			       knot_dname_size(dname));
			dbg_rrset_detail("Uncompressed dname size: %d\n",
			                 knot_dname_size(dname));
			*pos += knot_dname_size(dname);
			rdlength += knot_dname_size(dname);
			if (comp) {
				comp->compr->wire_pos += knot_dname_size(dname);
			}
			offset += sizeof(knot_dname_t *);
			size += knot_dname_size(dname);
		} else if (descriptor_item_is_fixed(item)) {
			dbg_rrset_detail("Saving static chunk, size=%d\n",
			                 item);
			/* Fixed length chunk. */
			if (size + rdlength + item > max_size) {
				return KNOT_ESPACE;
			}
			memcpy(*pos, rdata + offset, item);
			*pos += item;
			rdlength += item;
			offset += item;
			size += item;
		} else if (descriptor_item_is_remainder(item)) {
			/* Check that the remainder fits to stream. */
			size_t remainder_size =
				rrset_rdata_remainder_size(rrset, offset,
			                                   rdata_pos);
			dbg_rrset_detail("Saving remaining chunk, size=%d\n",
			                 remainder_size);
			if (size + rdlength + remainder_size > max_size) {
				return KNOT_ESPACE;
			}
			memcpy(*pos, rdata + offset, remainder_size);
			*pos += remainder_size;
			rdlength += remainder_size;
			offset += remainder_size;
			size += remainder_size;
		} else {
			assert(rrset->type == KNOT_RRTYPE_NAPTR);
			/* Store the binary chunk. */
			size_t chunk_size =
			rrset_rdata_naptr_bin_chunk_size(rrset, rdata_pos);
			if (size + rdlength + chunk_size > max_size) {
				return KNOT_ESPACE;
			}
			*pos += chunk_size;
			rdlength += chunk_size;
			offset += chunk_size;
			size += chunk_size;
			/* Store domain name. */
			knot_dname_t *dname = 
				(knot_dname_t *)(rdata + offset);
			assert(dname);
			if (size + rdlength + dname->size > max_size) {
				return KNOT_ESPACE;
			}

			// save whole domain name
			memcpy(*pos, knot_dname_name(dname), 
			       knot_dname_size(dname));
			dbg_rrset_detail("Uncompressed dname size: %d\n",
			                 knot_dname_size(dname));
			*pos += knot_dname_size(dname);
			rdlength += knot_dname_size(dname);
			offset += sizeof(knot_dname_t *);
			size += knot_dname_size(dname);
		}
	}
	
	knot_wire_write_u16(rdlength_pos, rdlength);

	*rr_size = size;
	return KNOT_EOK;
}

static int knot_rrset_to_wire_aux(const knot_rrset_t *rrset, uint8_t **pos,
                                  size_t max_size, compression_param_t *comp)
{
	size_t size = 0;
	
	assert(rrset != NULL);
	assert(rrset->owner != NULL);
	assert(pos != NULL);
	assert(*pos != NULL);
	
	dbg_rrset_detail("Max size: %zu, owner: %p, owner size: %d\n",
	                 max_size, rrset->owner, rrset->owner->size);
	if (comp) {
		/*
		 * We may pass the current position to the compression function
		 * because if the owner will be put somewhere, it will be on the
		 * current position (first item of a RR). If it will not be put into
		 * the wireformat, we may remove the dname (and possibly its parents)
		 * from the compression table.
		 */
		dbg_response_detail("Compressing RR owner: %s.\n",
		                    rrset->owner->name);
		knot_compr_t compr_info;
		compr_info.table = comp->compressed_dnames;
		compr_info.wire_pos = comp->wire_pos;
		compr_info.owner.pos = 0;
		compr_info.owner.wire = comp->owner_tmp;
		compr_info.owner.size =
			knot_response_compress_dname(rrset->owner, &compr_info,
			                             comp->owner_tmp, max_size,
		                                     comp->compr_cs);
		dbg_response_detail("Compressed owner has size=%zu\n",
		                    compr_info.owner.size);
		comp->compr = &compr_info;
	}
	
	
	/* This should be checked in the calling function. TODO not an assert*/
//	assert(max_size >= size + *rdlength);
	
	dbg_rrset_detail("Max size: %zu, size: %d\n", max_size, size);

	const rdata_descriptor_t *desc = get_rdata_descriptor(rrset->type);
	assert(desc);

	for (uint16_t i = 0; i < rrset->rdata_count; i++) {
		size_t rr_size = 0;
		int ret = knot_rrset_rdata_to_wire_one(rrset, i,
		                                       pos, max_size, &rr_size,
		                                       comp);
		if (ret != KNOT_EOK) {
			dbg_rrset("rrset: to_wire: Cannot convert RR. "
			          "Reason: %s.\n",
			          knot_strerror(ret));
			return ret;
		}
		dbg_rrset_detail("Converted RR nr=%d, size=%d\n", i, rr_size);
		size += rr_size;
	}
	
	dbg_rrset_detail("Max size: %zu, size: %d\n", max_size, size);

	return size;
}

/*----------------------------------------------------------------------------*/

int knot_rrset_to_wire(const knot_rrset_t *rrset, uint8_t *wire, size_t *size,
                       size_t max_size, uint16_t *rr_count, void *data)
{
	// if no RDATA in RRSet, return
	if (rrset->rdata == NULL) {
		*size = 0;
		*rr_count = 0;
		return KNOT_EOK;
	}
	
	compression_param_t *comp_data = (compression_param_t *)data;
	
	uint8_t *pos = wire;
	
dbg_rrset_exec_detail(
	dbg_rrset_detail("Converting following RRSet:\n");
	knot_rrset_dump(rrset);
);

	int ret = knot_rrset_to_wire_aux(rrset, &pos, max_size, comp_data);
	
	assert(ret != 0);

	if (ret < 0) {
		// some RR didn't fit in, so no RRs should be used
		// TODO: remove last entries from compression table
		dbg_rrset_verb("Some RR didn't fit in.\n");
		return KNOT_ESPACE;
	}

	// the whole RRSet did fit in
	assert(ret <= max_size);
	assert(pos - wire == ret);
	*size = ret;
	
	dbg_rrset_detail("Size after: %zu\n", *size);

	*rr_count = rrset->rdata_count;

	return *rr_count;
}

static int knot_rrset_rdata_store_binary(uint8_t *rdata,
                                         size_t offset,
                                         const uint8_t *wire,
                                         size_t *pos,
                                         size_t rdlength,
                                         size_t size)
{
	assert(rdata);
	assert(wire);
	
	/* Check that size is OK. */
	if (*pos + size > rdlength) {
		dbg_rrset("rrset: rdata_store_binary: Exceeded RDLENGTH.\n");
		return KNOT_ESPACE;
	}
	
	/* Store actual data. */
	memcpy(rdata + offset, wire + *pos, size);
	
	/* Adjust pos acordlingly. */
	*pos += size;
	return KNOT_EOK;
}

/* This should never be called directly now i guess. */
int knot_rrset_rdata_from_wire_one(uint8_t **rdata, uint16_t type,
                                   const uint8_t *wire,
                                   size_t *pos, size_t total_size,
                                   size_t rdlength)
{
	int i = 0;
	size_t parsed = 0;

	if (rdlength == 0) {
		return KNOT_EOK;
	}
	
	// TODO is there a better way?
	uint8_t rdata_buffer[65536];
	size_t offset = 0;

	const rdata_descriptor_t *desc = get_rdata_descriptor(type);
	assert(desc);

	while (desc->block_types[i] != KNOT_RDATA_WF_END
	       && parsed < rdlength) {
		
		size_t pos2 = 0;
		
		if (descriptor_item_is_dname(desc->block_types[i])) {
			/* Since dnames can be compressed, */
			pos2 = *pos;
			knot_dname_t *dname =
				knot_dname_parse_from_wire(
					wire, &pos2, total_size, NULL);
			if (dname == NULL) {
				return KNOT_ERROR;
			}
			*((knot_dname_t **)rdata_buffer + offset) = dname;
			parsed += pos2 - *pos;
			*pos = pos2;
		} else if (descriptor_item_is_fixed(desc->block_types[i])) {
			int ret = knot_rrset_rdata_store_binary(rdata_buffer,
			                                        offset,
			                                        wire,
			                                        pos,
			                                        rdlength,
			                                        desc->block_types[i]);
			if (ret != KNOT_EOK) {
				dbg_rrset("rrset: rdata_from_wire: "
				          "Cannot store fixed RDATA chunk. "
				          "Reason: %s.\n", knot_strerror(ret));
				return ret;
			}
		} else if (descriptor_item_is_remainder(desc->block_types[i])) {
			/* Item size has to be calculated. */
			size_t remainder_size = rdlength - parsed;
			int ret = knot_rrset_rdata_store_binary(rdata_buffer,
			                                        offset,
			                                        wire,
			                                        pos,
			                                        rdlength,
			                                        remainder_size);
			if (ret != KNOT_EOK) {
				dbg_rrset("rrset: rdata_from_wire: "
				          "Cannot store RDATA remainder. "
				          "Reason: %s.\n", knot_strerror(ret));
				return ret;
			}
		} else {
			assert(type = KNOT_RRTYPE_NAPTR);
			/* Read fixed part - 2 shorts. */
			const size_t naptr_fixed_part_size = 4;
			int ret = knot_rrset_rdata_store_binary(rdata_buffer,
			                                        offset,
			                                        wire,
			                                        pos,
			                                        rdlength,
			                                        naptr_fixed_part_size);
			if (ret != KNOT_EOK) {
				dbg_rrset("rrset: rdata_from_wire: "
				          "Cannot store NAPTR fixed part. "
				          "Reason: %s.\n", knot_strerror(ret));
				return ret;
			}
			offset += naptr_fixed_part_size;
			
			// TODO +1? Boundary checks!!!
			/* Read three binary TXTs. */
			for (int i = 0; i < 3; i++) {
				//maybe store the whole thing using store binary
				uint8_t txt_size = *(wire + (*pos + 1));
				offset += 1;
				int ret = knot_rrset_rdata_store_binary(rdata_buffer,
				                                        offset,
				                                        wire,
				                                        pos,
				                                        rdlength,
				                                        txt_size);
				if (ret != KNOT_EOK) {
					dbg_rrset("rrset: rdata_from_wire: "
					          "Cannot store NAPTR TXTs. "
					          "Reason: %s.\n", knot_strerror(ret));
					return ret;
				}
				offset += txt_size + 1;
			}
			
			/* Dname remaining. No need to note read size. */
			knot_dname_t *dname =
				knot_dname_parse_from_wire(
					wire, pos, total_size, NULL);
			if (dname == NULL) {
				return KNOT_ERROR;
			}
			*((knot_dname_t **)rdata_buffer + offset) = dname;
			offset += sizeof(knot_dname_t *);
		}
	}
	
	return KNOT_EOK;
}

int knot_rrset_compare(const knot_rrset_t *r1,
                       const knot_rrset_t *r2,
                       knot_rrset_compare_type_t cmp)
{
	if (cmp == KNOT_RRSET_COMPARE_PTR) {
		if ((size_t)r1 > (size_t)r2) {
			return 1;
		} else if ((size_t)r1 < (size_t)r2) {
			return -1;
		} else {
			return 0;
		}
	}

	int res = knot_dname_compare(r1->owner, r2->owner);
	if (res) {
		return res;
	}
	
	if (r1->rclass > r2->rclass) {
		return 1;
	} else if (r1->rclass < r2->rclass) {
		return -1;
	}
	
	if (r1->type > r2->type) {
		return 1;
	} else if (r1->type < r2->type) {
		return -1;
	}
	
	if (cmp == KNOT_RRSET_COMPARE_WHOLE) {
		res = knot_rrset_compare_rdata(r1, r2);
	}

	return res;
}

int knot_rrset_equal(const knot_rrset_t *r1,
                     const knot_rrset_t *r2,
                     knot_rrset_compare_type_t cmp)
{
	if (cmp == KNOT_RRSET_COMPARE_PTR) {
		return r1 == r2;
	}

	int res = knot_dname_compare_non_canon(r1->owner, r2->owner);
	if (res != 0) {
		return 0;
	}
	
	if (r1->rclass == r2->rclass &&
	    r1->type == r2->type) {
		res = 1;
	} else {
		res = 0;
	}
	
	if (cmp == KNOT_RRSET_COMPARE_WHOLE) {
		res *= knot_rrset_rdata_equal(r1, r2);
	}

	return res;
}

int knot_rrset_deep_copy(const knot_rrset_t *from, knot_rrset_t **to,
                         int copy_rdata_dnames)
{
	if (from == NULL || to == NULL) {
		return KNOT_EINVAL;
	}
	
	*to = xmalloc(sizeof(knot_rrset_t));

	(*to)->owner = from->owner;
	knot_dname_retain((*to)->owner);
	(*to)->rclass = from->rclass;
	(*to)->ttl = from->ttl;
	(*to)->type = from->type;
	(*to)->rdata_count = from->rdata_count;
	if (from->rrsigs != NULL) {
		int ret = knot_rrset_deep_copy(from->rrsigs, &(*to)->rrsigs,
		                           copy_rdata_dnames);
		if (ret != KNOT_EOK) {
			knot_rrset_deep_free(to, 1, 0);
			return ret;
		}
	} else {
		(*to)->rrsigs = NULL;
	}
	
	/* Just copy arrays - actual data + indices. */
	(*to)->rdata = xmalloc(rrset_rdata_size_total(from));
	memcpy((*to)->rdata, from->rdata, rrset_rdata_size_total(from));
	
	(*to)->rdata_indices = xmalloc(sizeof(uint32_t) * from->rdata_count);
	memcpy((*to)->rdata_indices, from->rdata_indices,
	       sizeof(uint32_t) * from->rdata_count);
	
	/* Here comes the hard part. */
	if (copy_rdata_dnames) {
		knot_dname_t *dname_from = NULL;
		knot_dname_t **dname_to = NULL;
		knot_dname_t *dname_copy = NULL;
		while ((dname_from =
		        knot_rrset_get_next_dname(from, dname_from)) != NULL) {
			dname_to =
				knot_rrset_get_next_dname_pointer(*to,
					dname_to);
			/* These pointers have to be the same. */
			assert(dname_from == *dname_to);
			dname_copy = knot_dname_deep_copy(dname_from);
			if (dname_copy == NULL) {
				dbg_rrset("rrset: deep_copy: Cannot copy RDATA"
				          " dname.\n");
				/*! \todo This will leak. Is it worth fixing? */
				knot_rrset_deep_free(&(*to)->rrsigs, 1,
				                     copy_rdata_dnames);
				free((*to)->rdata);
				free((*to)->rdata_indices);
				free(*to);
				return KNOT_ENOMEM;
			}
			
			*dname_to = dname_copy;
		}
	}
	
	return KNOT_EOK;
}

/*----------------------------------------------------------------------------*/

int knot_rrset_shallow_copy(const knot_rrset_t *from, knot_rrset_t **to)
{
	*to = (knot_rrset_t *)malloc(sizeof(knot_rrset_t));
	CHECK_ALLOC_LOG(*to, KNOT_ENOMEM);
	
	memcpy(*to, from, sizeof(knot_rrset_t));

	/* Retain owner. */
	knot_dname_retain((*to)->owner);
	
	return KNOT_EOK;
}

/*----------------------------------------------------------------------------*/

void knot_rrset_rotate(knot_rrset_t *rrset)
{
	/*! \todo Maybe implement properly one day. */
	//rrset->rdata = rrset->rdata->next;
}

/*----------------------------------------------------------------------------*/

void knot_rrset_free(knot_rrset_t **rrset)
{
	if (rrset == NULL || *rrset == NULL) {
		return;
	}

	/*! \todo Shouldn't we always release owner reference? */
	knot_dname_release((*rrset)->owner);

	free(*rrset);
	*rrset = NULL;
}

/*----------------------------------------------------------------------------*/

void knot_rrset_rdata_deep_free_one(knot_rrset_t *rrset, size_t pos,
                                    int free_dnames)
{
	if (rrset == NULL || rrset->rdata == NULL ||
	    rrset->rdata_indices == NULL) {
		return;
	}
	
	size_t offset = 0;
	uint8_t *rdata = rrset_rdata_pointer(rrset, pos);
	if (rdata == NULL) {
		return;
	}
	
	if (free_dnames) {
		/* Go through the data and free dnames. Pointers can stay. */
		const rdata_descriptor_t *desc =
			get_rdata_descriptor(rrset->type);
		assert(desc);
		for (int i = 0; desc->block_types[i] != KNOT_RDATA_WF_END;i++) {
			int item = desc->block_types[i];
			if (descriptor_item_is_dname(item)) {
				knot_dname_t *dname;
				memcpy(&dname, rdata + offset,
				       sizeof(knot_dname_t *));
//				printf("%Freeing dname: %s\n",
//				       knot_dname_to_str(dname));
				knot_dname_release(dname);
				offset += sizeof(knot_dname_t *);
			} else if (descriptor_item_is_fixed(item)) {
				offset += item;
			} else if (!descriptor_item_is_remainder(item)) {
				assert(rrset->type == KNOT_RRTYPE_NAPTR);
				/* Skip the binary beginning. */
				offset +=
					rrset_rdata_naptr_bin_chunk_size(rrset,
				                                         pos);
				knot_dname_t *dname =
					(knot_dname_t *)rdata + offset;
				knot_dname_release(dname);
			}
		}
	}
	
	return;
}

void knot_rrset_deep_free(knot_rrset_t **rrset, int free_owner,
                          int free_rdata_dnames)
{
	if (rrset == NULL || *rrset == NULL) {
		return;
	}
	
	// rdata have to be freed no matter what
	for (uint16_t i = 0; i < (*rrset)->rdata_count; i++) {
		knot_rrset_rdata_deep_free_one(*rrset, i,
		                               free_rdata_dnames);
	}

	// RRSIGs should have the same owner as this RRSet, so do not delete it
	if ((*rrset)->rrsigs != NULL) {
		knot_rrset_deep_free(&(*rrset)->rrsigs, 0, free_rdata_dnames);
	}
	
	free((*rrset)->rdata);
	free((*rrset)->rdata_indices);

	if (free_owner) {
		knot_dname_release((*rrset)->owner);
	}

	free(*rrset);
	*rrset = NULL;
}

/*----------------------------------------------------------------------------*/

// This might not be needed, we have to store the last index anyway
//	/*
//	 * The last one has to be traversed.
//	 */
//	rdata_descriptor_t *desc = get_rdata_descriptor(rrset->type);
//	assert(desc);
//	size_t size = 0;
//	for (int i = 0; desc->block_types[i] != KNOT_RDATA_WF_END; i++) {
//		int tyt_rrset->rrset.rdata_count = test_rrset->rr_count;e = desc->block_types[i];
//		if (descriptor_item_is_dname(type)) {
//			size += sizeof(knot_dname_t *);
//		} else if (descriptor_item_is_fixed(type)) {
//			size += type;
//		} else if (descriptor_item_is_remainder(type)) {
//			// size has to be computed from index
//			size += 
//		} else {
//			//TODO naptr
//		}
//	}
//}


int knot_rrset_merge(void **r1, void **r2)
{
	knot_rrset_t *rrset1 = (knot_rrset_t *)(*r1);
	knot_rrset_t *rrset2 = (knot_rrset_t *)(*r2);
	if (rrset1 == NULL || rrset2 == NULL) {
		return KNOT_EINVAL;
	}
	
	/* Check, that we really merge RRSets? */
	if (rrset1->type != rrset2->type ||
	    rrset1->rclass != rrset2->rclass ||
	    (knot_dname_compare_non_canon(rrset1->owner, rrset2->owner) != 0) ||
	    (rrset1->rdata_count == 0 && rrset2->rdata_count)) {
		return KNOT_EINVAL;
	}
	
	/* Add all RDATAs from rrset2 to rrset1 (i.e. concatenate two arrays) */
	
	/*! \note The following code should work for
	 *        all the cases i.e. R1 or R2 are empty.
	 */
	
	/* Reallocate actual RDATA array. */
	rrset1->rdata = xrealloc(rrset1->rdata, rrset_rdata_size_total(rrset1) +
	                         rrset_rdata_size_total(rrset2));
	
	/* The space is ready, copy the actual data. */
	memcpy(rrset1->rdata + rrset_rdata_size_total(rrset1),
	       rrset2->rdata, rrset_rdata_size_total(rrset2));
	
	/* Indices have to be readjusted. But space has to be made first. */
	rrset1->rdata_indices = 
		xrealloc(rrset1->rdata_indices,
	        (rrset1->rdata_count + rrset2->rdata_count) *
	         sizeof(uint32_t));
	
	uint32_t rrset1_total_size = rrset_rdata_size_total(rrset1);
	uint32_t rrset2_total_size = rrset_rdata_size_total(rrset2);
	
	/*
	 * Move the indices. Discard the last item in the first array, as it 
	 * contains total length of the data, which is now different.
	 */
	memcpy(rrset1->rdata_indices + rrset1->rdata_count,
	       rrset2->rdata_indices, rrset2->rdata_count);
	
	/* Go through the second part of index array and adjust offsets. */
	for (uint16_t i = 0; i < rrset2->rdata_count - 1; i++) {
		rrset1->rdata_indices[rrset1->rdata_count + i] +=
			rrset1_total_size;
	}
	
	rrset1->rdata_indices[rrset1->rdata_count + rrset2->rdata_count - 1] = 
		rrset1_total_size + rrset2_total_size;
	
	rrset1->rdata_count += rrset2->rdata_count;
	
	return KNOT_EOK;
}

int knot_rrset_merge_no_dupl(void **r1, void **r2)
{
	if (r1 == NULL || r2 == NULL) {
		dbg_rrset("rrset: merge_no_dupl: NULL arguments.");
		return KNOT_EINVAL;
	}
	
	knot_rrset_t *rrset1 = (knot_rrset_t *)(*r1);
	knot_rrset_t *rrset2 = (knot_rrset_t *)(*r2);
	if (rrset1 == NULL || rrset2 == NULL) {
		dbg_rrset("rrset: merge_no_dupl: NULL arguments.");
		return KNOT_EINVAL;
	}

dbg_rrset_exec_detail(
	char *name = knot_dname_to_str(rrset1->owner);
	dbg_rrset_detail("rrset: merge_no_dupl: Merging %s.\n", name);
	free(name);
);

	if ((knot_dname_compare_non_canon(rrset1->owner, rrset2->owner) != 0)
	    || rrset1->rclass != rrset2->rclass
	    || rrset1->type != rrset2->type) {
		dbg_rrset("rrset: merge_no_dupl: Trying to merge "
		          "different RRs.\n");
		return KNOT_EINVAL;
	}
	
	/* For each item in second RRSet, make sure it is not duplicated. */
	for (uint16_t i = 0; i < rrset2->rdata_count; i++) {
		int duplicated = 0;
		/* Compare with all RRs in the first RRSet. */
		for (uint16_t j = 0; j < rrset1->rdata_count && !duplicated;
		     j++) {
			duplicated = !rrset_rdata_compare_one(rrset2, rrset1,
			                                      i, j);
		}
		
		if (!duplicated) {
			// This index goes to merged RRSet.
			knot_rrset_add_rdata(rrset1,
			                     rrset_rdata_pointer(rrset2, i),
			                     rrset_rdata_item_size(rrset2, i));
		}
	}
	
	return KNOT_EOK;
}

/*----------------------------------------------------------------------------*/

const knot_dname_t *knot_rrset_rdata_cname_name(const knot_rrset_t *rrset)
{
	if (rrset == NULL) {
		return NULL;
	}
	
	knot_dname_t *dname;
	memcpy(dname, rrset->rdata, sizeof(knot_dname_t *));
	return dname;
}

/*----------------------------------------------------------------------------*/

const knot_dname_t *knot_rrset_rdata_dname_target(const knot_rrset_t *rrset)
{
	if (rrset == NULL) {
		return NULL;
	}
	knot_dname_t *dname;
	memcpy(dname, rrset->rdata, sizeof(knot_dname_t *));
	return dname;
}

/*---------------------------------------------------------------------------*/

int64_t knot_rrset_rdata_soa_serial(const knot_rrset_t *rrset)
{
	if (rrset == NULL) {
		return 0;
	}
	
	//read u64??? TODO
	return knot_wire_read_u32(rrset->rdata + 
	                          sizeof(knot_dname_t *) * 2);

}

/*---------------------------------------------------------------------------*/
void knot_rrset_rdata_soa_serial_set(knot_rrset_t *rrset, uint32_t serial)
{
	if (rrset == NULL) {
		return;
	}

	// the number is in network byte order, transform it
	knot_wire_write_u32(rrset->rdata + sizeof(knot_dname_t *) * 2,
	                    serial);
}

/*---------------------------------------------------------------------------*/

uint32_t knot_rrset_rdata_soa_refresh(const knot_rrset_t *rrset)
{
	if (rrset == NULL) {
		return 0;
	}
	
	return knot_wire_read_u32(rrset->rdata + 
	                          sizeof(knot_dname_t *) * 2 + 4);
}

/*---------------------------------------------------------------------------*/


uint32_t knot_rrset_rdata_soa_retry(const knot_rrset_t *rrset)
{
	if (rrset == NULL) {
		return 0;
	}
	
	return knot_wire_read_u32(rrset->rdata + 
	                          sizeof(knot_dname_t *) * 2 + 8);
}

/*---------------------------------------------------------------------------*/

uint32_t knot_rrset_rdata_soa_expire(const knot_rrset_t *rrset)
{
	if (rrset == NULL) {
		return 0;
	}
	
	return knot_wire_read_u32(rrset->rdata + 
	                          sizeof(knot_dname_t *) * 2 + 12);
}

/*---------------------------------------------------------------------------*/

uint32_t knot_rrset_rdata_soa_minimum(const knot_rrset_t *rrset)
{
	if (rrset == NULL) {
		return 0;
	}
	
	return knot_wire_read_u32(rrset->rdata + 
	                          sizeof(knot_dname_t *) * 2 + 16);
}

/*---------------------------------------------------------------------------*/

uint16_t knot_rrset_rdata_rrsig_type_covered(const knot_rrset_t *rrset)
{
	if (rrset == NULL) {
		return 0;
	}
	
	return knot_wire_read_u16(rrset->rdata);
}

/*---------------------------------------------------------------------------*/

uint8_t knot_rrset_rdata_nsec3_algorithm(const knot_rrset_t *rrset,
                                         size_t pos)
{
	if (rrset == NULL || pos >= rrset->rdata_count) {
		return 0;
	}
	
	return *(rrset_rdata_pointer(rrset, pos));
}

/*---------------------------------------------------------------------------*/

uint16_t knot_rrset_rdata_nsec3_iterations(const knot_rrset_t *rrset,
                                           size_t pos)
{
	if (rrset == NULL || pos >= rrset->rdata_count) {
		return 0;
	}
	
	return knot_wire_read_u16(rrset_rdata_pointer(rrset, pos) + 2);
}

/*---------------------------------------------------------------------------*/

uint8_t knot_rrset_rdata_nsec3param_flags(const knot_rrset_t *rrset)
{
	if (rrset == NULL) {
		return 0;
	}
	
	return *(rrset_rdata_pointer(rrset, 0) + 1);
}

/*---------------------------------------------------------------------------*/

uint8_t knot_rrset_rdata_nsec3param_algorithm(const knot_rrset_t *rrset)
{
	if (rrset == NULL) {
		return 0;
	}
	
	return *(rrset_rdata_pointer(rrset, 0));
}

/*---------------------------------------------------------------------------*/

uint16_t knot_rrset_rdata_nsec3param_iterations(const knot_rrset_t *rrset)
{
	if (rrset == NULL) {
		return 0;
	}
	
	return knot_wire_read_u16(rrset_rdata_pointer(rrset, 0) + 2);
}

/*---------------------------------------------------------------------------*/

uint8_t knot_rrset_rdata_nsec3param_salt_length(const knot_rrset_t *rrset)
{
	if (rrset == NULL) {
		return 0;
	}
	
	return *(rrset_rdata_pointer(rrset, 0) + 4);
}

/*---------------------------------------------------------------------------*/

const uint8_t *knot_rrset_rdata_nsec3param_salt(const knot_rrset_t *rrset)
{
	if (rrset == NULL) {
		return NULL;
	}
	
	return rrset_rdata_pointer(rrset, 0) + 4;
}

/*---------------------------------------------------------------------------*/


uint8_t knot_rrset_rdata_nsec3_salt_length(const knot_rrset_t *rrset,
                                           size_t pos)
{
	if (rrset == NULL || pos >= rrset->rdata_count) {
		return 0;
	}
	
	return *(rrset_rdata_pointer(rrset, pos) + 4);
}

/*---------------------------------------------------------------------------*/

const uint8_t *knot_rrset_rdata_nsec3_salt(const knot_rrset_t *rrset,
                                           size_t pos)
{
	if (rrset == NULL || pos >= rrset->rdata_count) {
		return NULL;
	}
	
	return rrset_rdata_pointer(rrset, pos) + 4;
}

static knot_dname_t **knot_rrset_rdata_get_next_dname_pointer(
	const knot_rrset_t *rrset,
	knot_dname_t **prev_dname, size_t pos)
{
	if (rrset == NULL) {
		return NULL;
	}
	
	// Get descriptor
	const rdata_descriptor_t *desc =
		get_rdata_descriptor(rrset->type);
	int next = 0;
	size_t offset = 0;
	uint8_t *rdata = rrset_rdata_pointer(rrset, pos);
	if (prev_dname == NULL) {
		next = 1;
	}
	// Cycle through blocks and find dnames
	for (int i = 0; desc->block_types[i] != KNOT_RDATA_WF_END; i++) {
		if (descriptor_item_is_dname(desc->block_types[i])) {
			if (next) {
				assert(rdata + offset);
				return (knot_dname_t **)(rdata + offset);
			}
			
			knot_dname_t **dname =
				(knot_dname_t **)(rdata +
			                         offset);

			assert(prev_dname);
			
			if (dname == prev_dname) {
				//we need to return next dname
				next = 1;
			}
			offset += sizeof(knot_dname_t *);
		} else if (descriptor_item_is_fixed(desc->block_types[i])) {
			offset += desc->block_types[i];
		} else if (!descriptor_item_is_remainder(desc->block_types[i])) {
			assert(rrset->type == KNOT_RRTYPE_NAPTR);
			offset += rrset_rdata_naptr_bin_chunk_size(rrset, pos);
			if (next) {
				return (knot_dname_t **)(rdata + offset);
			}
			
			knot_dname_t *dname =
				(knot_dname_t *)(rdata +
			                         offset);
			
			assert(prev_dname);
			
			if (dname == *prev_dname) {
				//we need to return next dname
				next = 1;
			}
			
			/* 
			 * Offset does not contain dname from NAPTR yet.
			 * It should not matter, since this block type
			 * is the only one in the RR anyway, but to be sure...
			 */
			offset += sizeof(knot_dname_t *); // now it does
		}
	}
	
	return NULL;
}

const knot_dname_t *knot_rrset_next_dname(const knot_rrset_t *rrset,
                                          const knot_dname_t *prev_dname)
{
	return (const knot_dname_t *)knot_rrset_get_next_dname(rrset,
	                                  (knot_dname_t *)prev_dname);
}

knot_dname_t *knot_rrset_get_next_dname(const knot_rrset_t *rrset,
                                        knot_dname_t *prev_dname)
{
	if (rrset == NULL) {
		return NULL;
	}
	
	for (uint16_t i = 0; i < rrset->rdata_count; i++) {
		knot_dname_t **ret =
			knot_rrset_rdata_get_next_dname_pointer(rrset,
		                                                &prev_dname, i);
		if (ret != NULL) {
			return *ret;
		}
	}
	
	return NULL;
}

knot_dname_t **knot_rrset_get_next_dname_pointer(const knot_rrset_t *rrset,
                                                 knot_dname_t **prev_dname)
{
	if (rrset == NULL) {
		return NULL;
	}
	
	// Get descriptor
	const rdata_descriptor_t *desc =
		get_rdata_descriptor(rrset->type);
	int next = 0;
	if (prev_dname == NULL) {
		next = 1;
	}

	for (uint16_t pos = 0; pos < rrset->rdata_count; pos++) {
		//TODO following code needs to be in a function
		size_t offset = 0;
		uint8_t *rdata = rrset_rdata_pointer(rrset, pos);
		// Cycle through blocks and fid dnames
		for (int i = 0; desc->block_types[i] != KNOT_RDATA_WF_END; i++) {
			if (descriptor_item_is_dname(desc->block_types[i])) {
				if (next) {
					assert(rdata + offset);
					return (knot_dname_t **)(rdata + offset);
				}
			
				knot_dname_t **dname =
					(knot_dname_t **)(rdata +
				                         offset);
	
				assert(prev_dname);
			
				if (dname == prev_dname) {
					//we need to return next dname
					next = 1;
				}
				offset += sizeof(knot_dname_t *);
			} else if (descriptor_item_is_fixed(desc->block_types[i])) {
				offset += desc->block_types[i];
			} else if (descriptor_item_is_remainder(desc->block_types[i])) {
				uint32_t remainder_size =
					rrset_rdata_item_size(rrset, pos) - offset;
				offset += remainder_size;
			} else {
				assert(rrset->type == KNOT_RRTYPE_NAPTR);
				offset += rrset_rdata_naptr_bin_chunk_size(rrset, pos);
				if (next) {
					return (knot_dname_t **)(rdata + offset);
				}
			
				knot_dname_t *dname =
					(knot_dname_t *)(rdata +
				                         offset);
			
				assert(prev_dname);
			
				if (dname == *prev_dname) {
					//we need to return next dname
					next = 1;
				}
			
				/* 
				 * Offset does not contain dname from NAPTR yet.
				 * It should not matter, since this block type
				 * is the only one in the RR anyway, but to be sure...
				 */
				offset += sizeof(knot_dname_t *); // now it does
			}
		}
	}
	return NULL;
}

uint8_t *knot_rrset_rdata_prealloc(const knot_rrset_t *rrset,
                                   size_t *rdata_size)
{
	/*
	 * Length of data can be sometimes guessed
	 * easily. Well, for some types anyway.
	 */
	const rdata_descriptor_t *desc = get_rdata_descriptor(rrset->type);
	assert(desc);
	*rdata_size = 0;
	for (int i = 0; desc->block_types[i] != KNOT_RDATA_WF_END; i++) {
		int item = desc->block_types[i];
		if (descriptor_item_is_fixed(item)) {
			*rdata_size += item;
		} else if (descriptor_item_is_dname(item)) {
			*rdata_size += sizeof(knot_dname_t *);
		} else if (descriptor_item_is_remainder(item)) {
			//TODO
			switch(rrset->type) {
				case KNOT_RRTYPE_DS:
					*rdata_size += 64;
				break;
				case KNOT_RRTYPE_RRSIG:
					*rdata_size += 256;
				break;
				case KNOT_RRTYPE_DNSKEY:
					*rdata_size += 1024;
				break;
				default:
					*rdata_size += 512;
			} //switch
		} else {
			assert(0);
		}
	}
	
	uint8_t *ret = malloc(*rdata_size);
	if (ret == NULL) {
		ERR_ALLOC_FAILED;
		*rdata_size = 0;
		return NULL;
	}
	/* TODO do properly. */
	
	return ret;
}


void knot_rrset_dump(const knot_rrset_t *rrset)
{
	if (rrset == NULL) {
		return;
	}
	
	fprintf(stderr, "      ------- RRSET -------\n");
	
	char *name = knot_dname_to_str(rrset->owner);
	fprintf(stderr, "  owner: %s\n", name);
	free(name);
	fprintf(stderr, "  type: %u\n", rrset->type);
	fprintf(stderr, "  class: %d\n",  rrset->rclass);
	fprintf(stderr, "  ttl: %d\n", rrset->ttl);
	fprintf(stderr, "  RDATA count: %d\n", rrset->rdata_count);
	
	fprintf(stderr, "  RRSIGs:\n");
	if (rrset->rrsigs != NULL) {
	        knot_rrset_dump(rrset->rrsigs);
	} else {
	        fprintf(stderr, "  none\n");
	}
	
	fprintf(stderr, "RDATA indices (total=%d):\n",
	        rrset_rdata_size_total(rrset));
	
	for (uint16_t i = 0; i < rrset->rdata_count; i++) {
		fprintf(stderr, "%d=%d ", i, rrset_rdata_offset(rrset, i));
	}
	fprintf(stderr, "\n");
	
	if (knot_rrset_rdata_rr_count(rrset) == 0) {
		fprintf(stderr, "NO RDATA\n");
	}
	
	for (uint16_t i = 0; i < knot_rrset_rdata_rr_count(rrset);i ++) {
		knot_rrset_rdata_dump(rrset, i);
	}
}

static size_t rrset_binary_length_one(const knot_rrset_t *rrset,
                                      size_t rdata_pos)
{
	const rdata_descriptor_t *desc =
		get_rdata_descriptor(knot_rrset_type(rrset));
	assert(desc != NULL);
	
	size_t offset = 0;
	size_t size = 0;
	for (int i = 0; desc->block_types[i] != KNOT_RDATA_WF_END; i++) {
		int item = desc->block_types[i];
		uint8_t *rdata = rrset_rdata_pointer(rrset, rdata_pos);
		if (descriptor_item_is_dname(item)) {
			knot_dname_t *dname;
			memcpy(&dname, rdata + offset, sizeof(knot_dname_t *));
			assert(dname);
			offset += sizeof(knot_dname_t *);
			size += knot_dname_size(dname) + 1; // extra 1 - we need a size
		} else if (descriptor_item_is_fixed(item)) {
			offset += item;
			size += item;
		} else if (descriptor_item_is_remainder(item)) {
			size += rrset_rdata_item_size(rrset, rdata_pos) -
			        offset;
		} else {
			fprintf(stderr, "NAPTR, failing miserably\n");
			assert(rrset->type == KNOT_RRTYPE_NAPTR);
			assert(0);
		}
	}
	
	return size;
}

uint64_t rrset_binary_length(const knot_rrset_t *rrset)
{
	if (rrset == NULL || rrset->rdata_count == 0) {
		return 0;
	}
	uint64_t size = sizeof(uint64_t) + // size at the beginning
	              1 + // owner size
	              knot_dname_size(knot_rrset_owner(rrset)) + // owner data
	              sizeof(uint16_t) + // type
	              sizeof(uint16_t) + // class
	              sizeof(uint32_t) + // ttl
	              sizeof(uint16_t) +  //RR count
	              2 * sizeof(uint32_t) * rrset->rdata_count; //RR indices + binary lengths
	for (uint16_t i = 0; i < rrset->rdata_count; i++) {
		size += rrset_binary_length_one(rrset, i);
	}
	
	return size;
}

static void rrset_serialize_rr(const knot_rrset_t *rrset, size_t rdata_pos,
                               uint8_t *stream, size_t *size)
{
	const rdata_descriptor_t *desc =
		get_rdata_descriptor(knot_rrset_type(rrset));
	assert(desc != NULL);
	
	size_t offset = 0;
	*size = 0;
	for (int i = 0; desc->block_types[i] != KNOT_RDATA_WF_END; i++) {
		int item = desc->block_types[i];
		uint8_t *rdata = rrset_rdata_pointer(rrset, rdata_pos);
		if (descriptor_item_is_dname(item)) {
			knot_dname_t *dname;
			memcpy(&dname, rdata + offset, sizeof(knot_dname_t *));
			assert(dname);
			memcpy(stream + *size, &dname->size, 1);
			*size += 1;
			memcpy(stream + *size, dname->name, dname->size);
			offset += sizeof(knot_dname_t *);
			*size += dname->size;
		} else if (descriptor_item_is_fixed(item)) {
			memcpy(stream + *size, rdata + offset, item);
			offset += item;
			*size += item;
		} else if (descriptor_item_is_remainder(item)) {
			uint32_t remainder_size =
				rrset_rdata_item_size(rrset,
			                              rdata_pos) - offset;
			memcpy(stream + *size, rdata + offset, remainder_size);
			size += remainder_size;
		} else {
			fprintf(stderr, "NAPTR, failing miserably\n");
			assert(rrset->type == KNOT_RRTYPE_NAPTR);
			assert(0);
		}
	}
}

int rrset_serialize(const knot_rrset_t *rrset, uint8_t *stream, size_t *size)
{
	if (rrset == NULL || rrset->rdata_count == 0) {
		return KNOT_EINVAL;
	}
	
	uint64_t rrset_length = rrset_binary_length(rrset);
	memcpy(stream, &rrset_length, sizeof(uint64_t));
	
	size_t offset = sizeof(uint64_t);
	/* Save RR indices. Size first. */
	memcpy(stream + offset, &rrset->rdata_count, sizeof(uint16_t));
	offset += sizeof(uint16_t);
	memcpy(stream + offset, rrset->rdata_indices,
	       rrset->rdata_count * sizeof(uint32_t));
	/* Save owner. Size first. */
	memcpy(stream + offset, &rrset->owner->size, 1);
	++offset;
	memcpy(stream, knot_dname_name(rrset->owner),
	       knot_dname_size(rrset->owner));
	offset += knot_dname_size(rrset->owner);
	
	/* Save static data. */
	memcpy(stream + offset, &rrset->type, sizeof(uint16_t));
	offset += sizeof(uint16_t);
	memcpy(stream + offset, &rrset->rclass, sizeof(uint16_t));
	offset += sizeof(uint16_t);
	memcpy(stream + offset, &rrset->ttl, sizeof(uint32_t));
	offset += sizeof(uint32_t);
	
	/* Copy RDATA. */
	size_t actual_size = 0;
	for (uint16_t i = 0; i < rrset->rdata_count; i++) {
		size_t size_one = 0;
		/* This cannot fail, if it does, RDATA are malformed. TODO */
		/* TODO this can be written later. */
		uint32_t rr_size = rrset_binary_length_one(rrset, i);
		memcpy(stream + offset, &rr_size, sizeof(uint32_t));
		offset += sizeof(uint32_t);
		rrset_serialize_rr(rrset, i, stream + offset + actual_size,
		                   &size_one);
		assert(size_one == rr_size);
		actual_size += size_one;
	}
	
	assert(actual_size == *size);
	return KNOT_EOK;

}

int rrset_serialize_alloc(const knot_rrset_t *rrset, uint8_t **stream,
                          size_t *size)
{
	/* Get the binary size. */
	*size = rrset_binary_length(rrset);
	if (*size == 0) {
		/* Nothing to serialize. */
		dbg_rrset("rrset: serialize alloc: No data to serialize.\n");
		return KNOT_EINVAL;
	}
	
	/* Prepare memory. */
	*stream = xmalloc(*size);
	
	return rrset_serialize(rrset, *stream, size);
}


static int rrset_deserialize_rr(knot_rrset_t *rrset, size_t rdata_pos,
                                uint8_t *stream, uint32_t rdata_size,
                                size_t *read)
{
	const rdata_descriptor_t *desc =
		get_rdata_descriptor(knot_rrset_type(rrset));
	assert(desc != NULL);
	
	size_t stream_offset = 0;
	size_t rdata_offset = 0;
	for (int i = 0; desc->block_types[i] != KNOT_RDATA_WF_END; i++) {
		int item = desc->block_types[i];
		uint8_t *rdata = rrset_rdata_pointer(rrset, rdata_pos);
		if (descriptor_item_is_dname(item)) {
			/* Read dname size. */
			uint8_t dname_size = 0;
			memcpy(&dname_size, stream + stream_offset, 1);
			stream_offset += 1;
			knot_dname_t *dname =
				knot_dname_new_from_str((char *)(stream + stream_offset),
			                                dname_size, NULL);
			memcpy(rdata + rdata_offset, &dname,
			       sizeof(knot_dname_t *));
			stream_offset += dname_size;
			rdata_offset += sizeof(knot_dname_t *);
		} else if (descriptor_item_is_fixed(item)) {
			memcpy(rdata + rdata_offset, stream + stream_offset, item);
			rdata_offset += item;
			stream_offset += item;
		} else if (descriptor_item_is_remainder(item)) {
			size_t remainder_size = rdata_size - stream_offset;
			memcpy(rdata + rdata_offset, stream + stream_offset,
			       remainder_size);
			stream_offset += remainder_size;
		} else {
			assert(rrset->type == KNOT_RRTYPE_NAPTR);
			assert(0);
		}
	}
	*read = stream_offset;
	return KNOT_EOK;
}

int rrset_deserialize(uint8_t *stream, size_t *stream_size,
                      knot_rrset_t **rrset)
{
	if (sizeof(uint64_t) > *stream_size) {
		return KNOT_ESPACE;
	}
	uint64_t rrset_length = 0;
	memcpy(&rrset_length, stream, sizeof(uint64_t));
	if (rrset_length > *stream_size) {
		return KNOT_ESPACE;
	}
	
	size_t offset = sizeof(uint64_t);
	uint16_t rdata_count = 0;
	memcpy(&rdata_count, stream + offset, sizeof(uint16_t));
	offset += sizeof(uint16_t);
	uint32_t *rdata_indices = xmalloc(rdata_count * sizeof(uint32_t));
	memcpy(rdata_indices, stream + offset,
	       rdata_count * sizeof(uint32_t));
	offset += rdata_count * sizeof(uint32_t);
	/* Read owner from the stream. */
	uint8_t owner_size = *(stream + offset);
	offset += 1;
	knot_dname_t *owner = knot_dname_new_from_wire(stream + offset,
	                                               owner_size, NULL);
	assert(owner);
	offset += owner_size;
	/* Read type. */
	uint16_t type = 0;
	memcpy(&type, stream + offset, sizeof(uint16_t));
	offset += sizeof(uint16_t);
	/* Read class. */
	uint16_t rclass = 0;
	memcpy(&rclass, stream + offset, sizeof(uint16_t));
	offset += sizeof(uint16_t);
	/* Read TTL. */
	uint32_t ttl = 0;
	memcpy(&ttl, stream + offset, sizeof(uint32_t));
	offset += sizeof(uint32_t);
	
	/* Create new RRSet. */
	*rrset = knot_rrset_new(owner, type, rclass, ttl);
	(*rrset)->rdata_indices = rdata_indices;
	(*rrset)->rdata_count = rdata_count;
	(*rrset)->rdata = xmalloc(rdata_indices[rdata_count - 1]);
	memset((*rrset)->rdata, 0, rdata_indices[rdata_count - 1]);
	/* Read RRs. */
	for (uint16_t i = 0; i < (*rrset)->rdata_count; i++) {
		/*
		 * There's always size of rdata in the beginning.
		 * Needed because of remainders.
		 */
		uint32_t rdata_size = 0;
		memcpy(&rdata_size, stream + offset, sizeof(uint32_t));
		size_t read = 0;
		rrset_deserialize_rr((*rrset), i,
		                     stream + offset, rdata_size, &read);
		/* TODO handle malformations. */
		assert(read == rdata_size);
		offset += read;
	}
	
	*stream_size = *stream_size - offset;
	
	return KNOT_EOK;
}

const knot_dname_t *knot_rrset_rdata_ns_name(const knot_rrset_t *rrset,
                                             size_t rdata_pos)
{
	if (rrset == NULL) {
		return NULL;
	}
	
	knot_dname_t *dname;
	memcpy(&dname, rrset_rdata_pointer(rrset, rdata_pos),
	       sizeof(knot_dname_t *));
	return dname;
}

const knot_dname_t *knot_rrset_rdata_mx_name(const knot_rrset_t *rrset,
                                             size_t rdata_pos)
{
	if (rrset == NULL) {
		return NULL;
	}
	
	knot_dname_t *dname;
	memcpy(&dname, rrset_rdata_pointer(rrset, rdata_pos) + 2,
	       sizeof(knot_dname_t *));
	return dname;
}

const knot_dname_t *knot_rrset_rdata_srv_name(const knot_rrset_t *rrset,
                                              size_t rdata_pos)
{
	if (rrset == NULL) {
		return NULL;
	}
	
	knot_dname_t *dname;
	memcpy(&dname, rrset_rdata_pointer(rrset, rdata_pos) + 6,
	       sizeof(knot_dname_t *));
	return dname;
}

const knot_dname_t *knot_rrset_rdata_name(const knot_rrset_t *rrset,
                                          size_t rdata_pos)
{
	if (rrset == NULL || rrset->rdata_count <= rdata_pos) {
		return NULL;
	}
	
	// iterate over the rdata items or act as if we knew where the name is?

	switch (rrset->type) {
		case KNOT_RRTYPE_NS:
			return knot_rrset_rdata_ns_name(rrset, rdata_pos);
		case KNOT_RRTYPE_MX:
			return knot_rrset_rdata_mx_name(rrset, rdata_pos);
		case KNOT_RRTYPE_SRV:
			return knot_rrset_rdata_srv_name(rrset, rdata_pos);
		case KNOT_RRTYPE_CNAME:
			return knot_rrset_rdata_cname_name(rrset);
	}

	return NULL;
}

static int knot_rrset_find_rr_pos(const knot_rrset_t *rr_search,
                                  const knot_rrset_t *rr_input, size_t pos,
                                  size_t *pos_out)
{
	int found = 0;
	for (uint16_t i = 0; i < rr_search->rdata_count && !found; i++) {
		if (rrset_rdata_compare_one(rr_search, rr_input, i, pos) == 0) {
			*pos_out = i;
			found = 1;
		}
	}
	
	return found ? KNOT_EOK : KNOT_ENOENT;
}

int knot_rrset_remove_rdata_pos(knot_rrset_t *rrset, size_t pos)
{
	if (rrset == NULL || pos >= rrset->rdata_count) {
		return KNOT_EINVAL;
	}

	/* Reorganize the actual RDATA array. */
	uint8_t *rdata_to_remove = rrset_rdata_pointer(rrset, pos);
	assert(rdata_to_remove);
	if (pos != rrset->rdata_count - 1) {
		/* Not the last item in array - we need to move the data. */
		uint8_t *next_rdata = rrset_rdata_pointer(rrset, pos + 1);
		assert(next_rdata);
		/* TODO free DNAMES inside. */
		/* Get size of remaining RDATA. */ 
		size_t last_rdata_size =
		                rrset_rdata_item_size(rrset,
		                                      rrset->rdata_count - 1);
		/* Get offset of last item. */
		uint8_t *last_rdata_offset =
			rrset_rdata_pointer(rrset, rrset->rdata_count - 1);
		size_t remainder_size =
			(last_rdata_offset - next_rdata) + last_rdata_size;
		/* 
		 * Copy the all following RR data to where this item is.
		 * No need to worry about exceeding allocated space now.
		 */
		memmove(rdata_to_remove, next_rdata, remainder_size);
	}
	
	uint32_t removed_size = rrset_rdata_item_size(rrset, pos);
	uint32_t new_size = rrset_rdata_size_total(rrset) - removed_size;
	
	/*! \todo Realloc might not be needed. Only if the RRSet is large. */
	rrset->rdata = xrealloc(rrset->rdata, new_size);
	
	/*
	 * Handle RDATA indices. All indices larger than the removed one have
	 * to be adjusted. Last index will be changed later.
	 */
	for (uint16_t i = pos - 1; i < rrset->rdata_count - 1; ++i) {
		rrset->rdata_indices[i] = rrset->rdata_indices[i + 1];
	}

	/* Save size of the whole RDATA array. */
	rrset->rdata_indices[rrset->rdata_count - 1] = new_size;
	
	/* Resize indices, might not be needed, but we'll do it to be proper. */
	rrset->rdata_indices =
		xrealloc(rrset->rdata_indices,
	                 (rrset->rdata_count - 1) * sizeof(uint32_t));
	--rrset->rdata_count;

	return KNOT_EOK;
}

int knot_rrset_remove_rr(knot_rrset_t *rrset,
                         const knot_rrset_t *rr_from, size_t rdata_pos)
{
	/*
	 * Position in first and second rrset can differ, we have
	 * to search for position first.
	 */
	size_t pos_to_remove = 0;
	int ret = knot_rrset_find_rr_pos(rrset, rr_from, rdata_pos,
	                                 &pos_to_remove);
	if (ret == KNOT_EOK) {
		/* Position found, can be removed. */
		ret = knot_rrset_remove_rdata_pos(rrset, pos_to_remove);
		if (ret != KNOT_EOK) {
			dbg_rrset("Cannot remove RDATA from RRSet (%s).\n",
			          knot_strerror(ret));
			return ret;
		}
	} else {
		dbg_rrset_verb("rr: remove_rr: RDATA not found (%s).\n",
		               knot_strerror(ret));
		return ret;
	}
	
	return KNOT_EOK;
}

