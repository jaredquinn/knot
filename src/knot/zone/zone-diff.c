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

#include <assert.h>
#include <stdlib.h>
#include <inttypes.h>

#include "common/debug.h"
#include "common/errcode.h"
#include "knot/zone/zone-diff.h"
#include "common/descriptor.h"
#include "libknot/rdata.h"
#include "libknot/util/utils.h"

struct zone_diff_param {
	knot_zone_tree_t *nodes;
	knot_changeset_t *changeset;
};

// forward declaration
static int knot_zone_diff_rdata(const knot_rrset_t *rrset1,
                                const knot_rrset_t *rrset2,
                                knot_changeset_t *changeset);

static int knot_zone_diff_load_soas(const knot_zone_contents_t *zone1,
                                    const knot_zone_contents_t *zone2,
                                    knot_changeset_t *changeset)
{
	if (zone1 == NULL || zone2 == NULL || changeset == NULL) {
		return KNOT_EINVAL;
	}

	const knot_node_t *apex1 = knot_zone_contents_apex(zone1);
	const knot_node_t *apex2 = knot_zone_contents_apex(zone2);
	if (apex1 == NULL || apex2 == NULL) {
		dbg_zonediff("zone_diff: "
		             "both zones must have apex nodes.\n");
		return KNOT_EINVAL;
	}

	knot_rrset_t *soa_rrset1 = knot_node_get_rrset(apex1, KNOT_RRTYPE_SOA);
	knot_rrset_t *soa_rrset2 = knot_node_get_rrset(apex2, KNOT_RRTYPE_SOA);
	if (soa_rrset1 == NULL || soa_rrset2 == NULL) {
		dbg_zonediff("zone_diff: "
		             "both zones must have apex nodes.\n");
		return KNOT_EINVAL;
	}

	if (knot_rrset_rr_count(soa_rrset1) == 0 ||
	    knot_rrset_rr_count(soa_rrset2) == 0) {
		dbg_zonediff("zone_diff: "
		             "both zones must have apex nodes with SOA "
		             "RRs.\n");
		return KNOT_EINVAL;
	}

	int64_t soa_serial1 =
		knot_rdata_soa_serial(soa_rrset1);
	if (soa_serial1 == -1) {
		dbg_zonediff("zone_diff: load_soas: Got bad SOA.\n");
	}

	int64_t soa_serial2 =
		knot_rdata_soa_serial(soa_rrset2);
	if (soa_serial2 == -1) {
		dbg_zonediff("zone_diff: load_soas: Got bad SOA.\n");
	}

	if (knot_serial_compare(soa_serial1, soa_serial2) == 0) {
		dbg_zonediff("zone_diff: "
		             "second zone must have higher serial than the "
		             "first one. (%"PRId64" vs. %"PRId64")\n",
		             soa_serial1, soa_serial2);
		return KNOT_ENODIFF;
	}

	if (knot_serial_compare(soa_serial1, soa_serial2) > 0) {
		dbg_zonediff("zone_diff: "
		             "second zone must have higher serial than the "
		             "first one. (%"PRId64" vs. %"PRId64")\n",
		             soa_serial1, soa_serial2);
		return KNOT_ERANGE;
	}

	assert(changeset);

	int ret = knot_rrset_copy(soa_rrset1, &changeset->soa_from, NULL);
	if (ret != KNOT_EOK) {
		dbg_zonediff("zone_diff: load_soas: Cannot copy RRSet.\n");
		return ret;
	}

	ret = knot_rrset_copy(soa_rrset2, &changeset->soa_to, NULL);
	if (ret != KNOT_EOK) {
		dbg_zonediff("zone_diff: load_soas: Cannot copy RRSet.\n");
		return ret;
	}

	changeset->serial_from = soa_serial1;
	changeset->serial_to = soa_serial2;

	dbg_zonediff_verb("zone_diff: load_soas: SOAs diffed. (%"PRId64" -> %"PRId64")\n",
	            soa_serial1, soa_serial2);

	return KNOT_EOK;
}

/*!< \todo Only use add or remove function, not both as they are the same. */
/*!< \todo Also, this might be all handled by function in changesets.h!!! */
static int knot_zone_diff_changeset_add_rrset(knot_changeset_t *changeset,
                                              const knot_rrset_t *rrset)
{
	/* Remove all RRs of the RRSet. */
	if (changeset == NULL || rrset == NULL) {
		dbg_zonediff("zone_diff: add_rrset: NULL parameters.\n");
		return KNOT_EINVAL;
	}

	if (knot_rrset_rr_count(rrset) == 0) {
		dbg_zonediff_detail("zone_diff: Nothing to add.\n");
		return KNOT_EOK;
	}

	knot_rrset_t *rrset_copy = NULL;
	int ret = knot_rrset_copy(rrset, &rrset_copy, NULL);
	if (ret != KNOT_EOK) {
		dbg_zonediff("zone_diff: add_rrset: Cannot copy RRSet.\n");
		return ret;
	}

	ret = knot_changeset_add_rrset(changeset, rrset_copy,
	                               KNOT_CHANGESET_ADD);
	if (ret != KNOT_EOK) {
		/* We have to free the copy now! */
		knot_rrset_free(&rrset_copy, NULL);
		dbg_zonediff("zone_diff: add_rrset: Could not add RRSet. "
		             "Reason: %s.\n", knot_strerror(ret));
		return ret;
	}

	return KNOT_EOK;
}

static int knot_zone_diff_changeset_remove_rrset(knot_changeset_t *changeset,
                                                 const knot_rrset_t *rrset)
{
	/* Remove all RRs of the RRSet. */
	if (changeset == NULL) {
		dbg_zonediff("zone_diff: remove_rrset: NULL parameters.\n");
		return KNOT_EINVAL;
	}

	if (rrset == NULL) {
		return KNOT_EOK;
	}

	if (knot_rrset_rr_count(rrset) == 0) {
		/* RDATA are the same, however*/
		dbg_zonediff_detail("zone_diff: Nothing to remove.\n");
		return KNOT_EOK;
	}

	knot_rrset_t *rrset_copy = NULL;
	int ret = knot_rrset_copy(rrset, &rrset_copy, NULL);
	if (ret != KNOT_EOK) {
		dbg_zonediff("zone_diff: remove_rrset: Cannot copy RRSet.\n");
		return ret;
	}

	ret = knot_changeset_add_rrset(changeset, rrset_copy,
	                               KNOT_CHANGESET_REMOVE);
	if (ret != KNOT_EOK) {
		/* We have to free the copy now. */
		knot_rrset_free(&rrset_copy, NULL);
		dbg_zonediff("zone_diff: remove_rrset: Could not remove RRSet. "
		             "Reason: %s.\n", knot_strerror(ret));
		return ret;
	}

	return KNOT_EOK;
}

static int knot_zone_diff_add_node(const knot_node_t *node,
                                   knot_changeset_t *changeset)
{
	if (node == NULL || changeset == NULL) {
		dbg_zonediff("zone_diff: add_node: NULL arguments.\n");
		return KNOT_EINVAL;
	}

	/* Add all rrsets from node. */
	const knot_rrset_t **rrsets = knot_node_rrsets(node);
	if (rrsets == NULL) {
		/* Empty non-terminals - legal case. */
		dbg_zonediff_detail("zone_diff: Node has no RRSets.\n");
		return KNOT_EOK;
	}

	for (uint i = 0; i < knot_node_rrset_count(node); i++) {
		assert(rrsets[i]);
		int ret = knot_zone_diff_changeset_add_rrset(changeset,
		                                         rrsets[i]);
		if (ret != KNOT_EOK) {
			dbg_zonediff("zone_diff: add_node: Cannot add RRSet (%s).\n",
			       knot_strerror(ret));
			free(rrsets);
			return ret;
		}
	}

	free(rrsets);

	return KNOT_EOK;
}

static int knot_zone_diff_remove_node(knot_changeset_t *changeset,
                                                const knot_node_t *node)
{
	if (changeset == NULL || node == NULL) {
		dbg_zonediff("zone_diff: remove_node: NULL parameters.\n");
		return KNOT_EINVAL;
	}

	dbg_zonediff("zone_diff: remove_node: Removing node: ...\n");

	const knot_rrset_t **rrsets = knot_node_rrsets(node);
	if (rrsets == NULL) {
		dbg_zonediff_verb("zone_diff: remove_node: "
		                  "Nothing to remove.\n");
		return KNOT_EOK;
	}

	dbg_zonediff_detail("zone_diff: remove_node: Will be removing %d RRSets.\n",
	              knot_node_rrset_count(node));

	/* Remove all the RRSets of the node. */
	for (uint i = 0; i < knot_node_rrset_count(node); i++) {
		int ret = knot_zone_diff_changeset_remove_rrset(changeset,
		                                            rrsets[i]);
		if (ret != KNOT_EOK) {
			dbg_zonediff("zone_diff: remove_node: Failed to "
			             "remove rrset. Error: %s\n",
			             knot_strerror(ret));
			free(rrsets);
			return ret;
		}
	}

	free(rrsets);

	return KNOT_EOK;
}

static int knot_zone_diff_rdata_return_changes(const knot_rrset_t *rrset1,
                                               const knot_rrset_t *rrset2,
                                               knot_rrset_t **changes)
{
	if (rrset1 == NULL || rrset2 == NULL) {
		dbg_zonediff("zone_diff: diff_rdata: NULL arguments. (%p) (%p).\n",
		       rrset1, rrset2);
		return KNOT_EINVAL;
	}

	/*
	* Take one rdata from first list and search through the second list
	* looking for an exact match. If no match occurs, it means that this
	* particular RR has changed.
	* After the list has been traversed, we have a list of
	* changed/removed rdatas. This has awful computation time.
	*/
	/* Create fake RRSet, it will be easier to handle. */
	knot_dname_t *owner_copy = knot_dname_copy(knot_rrset_get_owner(rrset1));
	*changes = knot_rrset_new(owner_copy,
	                          knot_rrset_type(rrset1),
	                          knot_rrset_class(rrset1),
	                          NULL);
	if (*changes == NULL) {
		knot_dname_free(&owner_copy);
		dbg_zonediff("zone_diff: diff_rdata: "
		             "Could not create RRSet with changes.\n");
		return KNOT_ENOMEM;
	}

	const rdata_descriptor_t *desc =
		get_rdata_descriptor(knot_rrset_type(rrset1));
	assert(desc);

	uint16_t rr1_count = knot_rrset_rr_count(rrset1);
	for (uint16_t i = 0; i < rr1_count; ++i) {
		size_t rr_pos = 0;
		int ret = knot_rrset_find_rr_pos(rrset2, rrset1, i, &rr_pos);
		if (ret == KNOT_ENOENT) {
			/* No such RR is present in 'rrset2'. */
			dbg_zonediff("zone_diff: diff_rdata: "
			       "No match for RR (type=%u owner=%p).\n",
			       knot_rrset_type(rrset1),
			       rrset1->owner);
			/* We'll copy index 'i' into 'changes' RRSet. */
			ret = knot_rrset_add_rr_from_rrset(*changes, rrset1, i, NULL);
			if (ret != KNOT_EOK) {
				knot_rrset_free(changes, NULL);
				return ret;
			}
		} else if (ret == KNOT_EOK) {
			if (knot_rrset_rr_ttl(rrset1, i) !=
			    knot_rrset_rr_ttl(rrset2, rr_pos)) {
				// TTLs differ add a change
				ret = knot_rrset_add_rr_from_rrset(*changes, rrset1, i, NULL);
				if (ret != KNOT_EOK) {
					knot_rrset_free(changes, NULL);
					return ret;
				}
			}
		} else {
			dbg_zonediff("zone_diff: diff_rdata: Could not search "
			             "for RR (%s).\n", knot_strerror(ret));
			knot_rrset_free(changes, NULL);
			return ret;
		}
	}

	return KNOT_EOK;
}

static int knot_zone_diff_rdata(const knot_rrset_t *rrset1,
                                const knot_rrset_t *rrset2,
                                knot_changeset_t *changeset)
{
	if ((changeset == NULL) || (rrset1 == NULL && rrset2 == NULL)) {
		dbg_zonediff("zone_diff: diff_rdata: NULL arguments.\n");
		return KNOT_EINVAL;
	}
	/*
	 * The easiest solution is to remove all the RRs that had no match and
	 * to add all RRs that had no match, but those from second RRSet. */

	/* Get RRs to remove from zone. */
	knot_rrset_t *to_remove = NULL;
	if (rrset1 != NULL && rrset2 != NULL) {
		int ret = knot_zone_diff_rdata_return_changes(rrset1, rrset2,
		                                              &to_remove);
		if (ret != KNOT_EOK) {
			dbg_zonediff("zone_diff: diff_rdata: Could not get changes. "
			             "Error: %s.\n", knot_strerror(ret));
			return ret;
		}
	}

	int ret = knot_zone_diff_changeset_remove_rrset(changeset,
	                                            to_remove);
	if (ret != KNOT_EOK) {
		knot_rrset_free(&to_remove, NULL);
		dbg_zonediff("zone_diff: diff_rdata: Could not remove RRs. "
		             "Error: %s.\n", knot_strerror(ret));
		return ret;
	}

	/* Copy was made in add_rrset function, we can free now. */
	knot_rrset_free(&to_remove, NULL);

	/* Get RRs to add to zone. */ // TODO move to extra function, same for remove
	knot_rrset_t *to_add = NULL;
	if (rrset1 != NULL && rrset2 != NULL) {
		ret = knot_zone_diff_rdata_return_changes(rrset2, rrset1,
		                                          &to_add);
		if (ret != KNOT_EOK) {
			dbg_zonediff("zone_diff: diff_rdata: Could not get changes. "
			             "Error: %s.\n", knot_strerror(ret));
			return ret;
		}
	}

	ret = knot_zone_diff_changeset_add_rrset(changeset,
	                                         to_add);
	if (ret != KNOT_EOK) {
		knot_rrset_free(&to_add, NULL);
		dbg_zonediff("zone_diff: diff_rdata: Could not remove RRs. "
		             "Error: %s.\n", knot_strerror(ret));
		return ret;
	}

	/* Copy was made in add_rrset function, we can free now. */
	knot_rrset_free(&to_add, NULL);

	return KNOT_EOK;
}

static int knot_zone_diff_rrsets(const knot_rrset_t *rrset1,
                                 const knot_rrset_t *rrset2,
                                 knot_changeset_t *changeset)
{
	/* RRs (=rdata) have to be cross-compared, unfortunalely. */
	return knot_zone_diff_rdata(rrset1, rrset2, changeset);
}

/*!< \todo this could be generic function for adding / removing. */
static int knot_zone_diff_node(knot_node_t **node_ptr, void *data)
{
	if (node_ptr == NULL || *node_ptr == NULL || data == NULL) {
		dbg_zonediff("zone_diff: diff_node: NULL arguments.\n");
		return KNOT_EINVAL;
	}

	knot_node_t *node = *node_ptr;

	struct zone_diff_param *param = (struct zone_diff_param *)data;
	if (param->changeset == NULL || param->nodes == NULL) {
		dbg_zonediff("zone_diff: diff_node: NULL arguments.\n");
		return KNOT_EINVAL;
	}

	/*
	 * First, we have to search the second tree to see if there's according
	 * node, if not, the whole node has been removed.
	 */
	const knot_node_t *node_in_second_tree = NULL;
	const knot_dname_t *node_owner = knot_node_owner(node);
	assert(node_owner);

	knot_zone_tree_find(param->nodes, node_owner, &node_in_second_tree);

	if (node_in_second_tree == NULL) {
		dbg_zonediff_detail("zone_diff: diff_node: Node %p is not "
		              "in the second tree.\n",
		              node_owner);
		int ret = knot_zone_diff_remove_node(param->changeset,
		                                               node);
		if (ret != KNOT_EOK) {
			dbg_zonediff("zone_diff: failed to remove node.\n");
		}
		return ret;
	}

	assert(node_in_second_tree != node);

	dbg_zonediff_detail("zone_diff: diff_node: Node %p is present in "
	              "both trees.\n", node_owner);
	/* The nodes are in both trees, we have to diff each RRSet. */
	const knot_rrset_t **rrsets = knot_node_rrsets(node);
	if (rrsets == NULL) {
		dbg_zonediff("zone_diff: Node in first tree has no RRSets.\n");
		/*
		 * If there are no RRs in the first tree, all of the RRs
		 * in the second tree will have to be inserted to ADD section.
		 */
		int ret = knot_zone_diff_add_node(node_in_second_tree,
		                                  param->changeset);
		if (ret != KNOT_EOK) {
			dbg_zonediff("zone_diff: diff_node: "
			             "Could not add node from second tree. "
			             "Reason: %s.\n", knot_strerror(ret));
		}
		return ret;
	}

	for (uint i = 0; i < knot_node_rrset_count(node); i++) {
		/* Search for the RRSet in the node from the second tree. */
		const knot_rrset_t *rrset = rrsets[i];
		assert(rrset);

		/* SOAs are handled explicitly. */
		if (knot_rrset_type(rrset) == KNOT_RRTYPE_SOA) {
			continue;
		}

		const knot_rrset_t *rrset_from_second_node =
			knot_node_rrset(node_in_second_tree,
			                knot_rrset_type(rrset));
		if (rrset_from_second_node == NULL) {
			dbg_zonediff("zone_diff: diff_node: There is no counterpart "
			       "for RRSet of type %u in second tree.\n",
			       knot_rrset_type(rrset));
			/* RRSet has been removed. Make a copy and remove. */
			assert(rrset);
			int ret = knot_zone_diff_changeset_remove_rrset(
				param->changeset,
				rrset);
			if (ret != KNOT_EOK) {
				dbg_zonediff("zone_diff: diff_node: "
				             "Failed to remove RRSet.\n");
				free(rrsets);
				return ret;
			}
		} else {
			dbg_zonediff("zone_diff: diff_node: There is a counterpart "
			       "for RRSet of type %u in second tree.\n",
			       knot_rrset_type(rrset));
			/* Diff RRSets. */
			int ret = knot_zone_diff_rrsets(rrset,
			                                rrset_from_second_node,
			                                param->changeset);
			if (ret != KNOT_EOK) {
				dbg_zonediff("zone_diff: "
				             "Failed to diff RRSets.\n");
				free(rrsets);
				return ret;
			}

//			dbg_zonediff_verb("zone_diff: diff_node: Changes in "
//			            "RRSIGs.\n");
//			/*! \todo There is ad-hoc solution in the function, maybe handle here. */
//			ret = knot_zone_diff_rrsets(rrset->rrsigs,
//			                                rrset_from_second_node->rrsigs,
//			                                param->changeset);
//			if (ret != KNOT_EOK) {
//				dbg_zonediff("zone_diff: "
//				             "Failed to diff RRSIGs.\n");
//				return ret;
//			}
		}
	}

	free(rrsets);

	/*! \todo move to one function with the code above. */
	rrsets = knot_node_rrsets(node_in_second_tree);
	if (rrsets == NULL) {
		dbg_zonediff("zone_diff: Node in second tree has no RRSets.\n");
		/*
		 * This can happen when node in second
		 * tree is empty non-terminal and as such has no RRs.
		 * Whole node from the first tree has to be removed.
		 */
		// TODO following code creates duplicated RR in diff.
		// IHMO such case should be handled here
//		int ret = knot_zone_diff_remove_node(param->changeset,
//		                                     node);
//		if (ret != KNOT_EOK) {
//			dbg_zonediff("zone_diff: diff_node: "
//			             "Cannot remove node. Reason: %s.\n",
//			             knot_strerror(ret));
//		}
		return KNOT_EOK;
	}

	for (uint i = 0; i < knot_node_rrset_count(node_in_second_tree); i++) {
		/* Search for the RRSet in the node from the second tree. */
		const knot_rrset_t *rrset = rrsets[i];
		assert(rrset);

		/* SOAs are handled explicitly. */
		if (knot_rrset_type(rrset) == KNOT_RRTYPE_SOA) {
			continue;
		}

		const knot_rrset_t *rrset_from_first_node =
			knot_node_rrset(node,
			                knot_rrset_type(rrset));
		if (rrset_from_first_node == NULL) {
			dbg_zonediff("zone_diff: diff_node: There is no counterpart "
			       "for RRSet of type %u in first tree.\n",
			       knot_rrset_type(rrset));
			/* RRSet has been added. Make a copy and add. */
			assert(rrset);
			int ret = knot_zone_diff_changeset_add_rrset(
				param->changeset,
				rrset);
			if (ret != KNOT_EOK) {
				dbg_zonediff("zone_diff: diff_node: "
				             "Failed to add RRSet.\n");
				free(rrsets);
				return ret;
			}
		} else {
			/* Already handled. */
			;
		}
	}

	free(rrsets);

	return KNOT_EOK;
}

/*!< \todo possibly not needed! */
static int knot_zone_diff_add_new_nodes(knot_node_t **node_ptr, void *data)
{
	if (node_ptr == NULL || *node_ptr == NULL || data == NULL) {
		dbg_zonediff("zone_diff: add_new_nodes: NULL arguments.\n");
		return KNOT_EINVAL;
	}

	knot_node_t *node = *node_ptr;

	struct zone_diff_param *param = (struct zone_diff_param *)data;
	if (param->changeset == NULL || param->nodes == NULL) {
		dbg_zonediff("zone_diff: add_new_nodes: NULL arguments.\n");
		return KNOT_EINVAL;
	}

	/*
	* If a node is not present in the second zone, it is a new node
	* and has to be added to changeset. Differencies on the RRSet level are
	* already handled.
	*/

	const knot_dname_t *node_owner = knot_node_owner(node);
	/*
	 * Node should definitely have an owner, otherwise it would not be in
	 * the tree.
	 */
	assert(node_owner);

	knot_node_t *new_node = NULL;
	knot_zone_tree_get(param->nodes, node_owner, &new_node);

	int ret = KNOT_EOK;

	if (!new_node) {
		assert(node);
		ret = knot_zone_diff_add_node(node, param->changeset);
		if (ret != KNOT_EOK) {
			dbg_zonediff("zone_diff: add_new_nodes: Cannot add "
			             "node: %p to changeset. Reason: %s.\n",
			             node->owner,
			             knot_strerror(ret));
		}
	}

	return ret;
}

static int knot_zone_diff_load_trees(knot_zone_tree_t *nodes1,
				     knot_zone_tree_t *nodes2,
				     knot_changeset_t *changeset)
{
	assert(nodes1);
	assert(nodes2);
	assert(changeset);

	struct zone_diff_param param = { 0 };
	param.changeset = changeset;

	// Traverse one tree, compare every node, each RRSet with its rdata.
	param.nodes = nodes2;
	int result = knot_zone_tree_apply(nodes1, knot_zone_diff_node, &param);
	if (result != KNOT_EOK) {
		return result;
	}

	// Some nodes may have been added. Add missing nodes to changeset.
	param.nodes = nodes1;
	result = knot_zone_tree_apply(nodes2, knot_zone_diff_add_new_nodes,
	                              &param);

	return result;
}


static int knot_zone_diff_load_content(const knot_zone_contents_t *zone1,
                                       const knot_zone_contents_t *zone2,
                                       knot_changeset_t *changeset)
{
	int result;

	result = knot_zone_diff_load_trees(zone1->nodes, zone2->nodes, changeset);
	if (result != KNOT_EOK)
		return result;

	result = knot_zone_diff_load_trees(zone1->nsec3_nodes, zone2->nsec3_nodes,
					   changeset);

	return result;
}


static int knot_zone_contents_diff(const knot_zone_contents_t *zone1,
                            const knot_zone_contents_t *zone2,
                            knot_changeset_t *changeset)
{
	if (zone1 == NULL || zone2 == NULL) {
		return KNOT_EINVAL;
	}

	int result = knot_zone_diff_load_soas(zone1, zone2, changeset);
	if (result != KNOT_EOK) {
		return result;
	}

	return knot_zone_diff_load_content(zone1, zone2, changeset);
}

int knot_zone_contents_create_diff(const knot_zone_contents_t *z1,
                                   const knot_zone_contents_t *z2,
                                   knot_changeset_t *changeset)
{
	if (z1 == NULL || z2 == NULL) {
		dbg_zonediff("zone_diff: create_changesets: NULL arguments.\n");
		return KNOT_EINVAL;
	}
	int ret = knot_zone_contents_diff(z1, z2, changeset);
	if (ret != KNOT_EOK) {
		dbg_zonediff("zone_diff: create_changesets: "
		             "Could not diff zones. "
		             "Reason: %s.\n", knot_strerror(ret));
		return ret;
	}

	dbg_zonediff("Changesets created successfully!\n");
	return KNOT_EOK;
}

int knot_zone_tree_add_diff(knot_zone_tree_t *t1, knot_zone_tree_t *t2,
                            knot_changeset_t *changeset)
{
	if (!t1 || !t2 || !changeset)
		return KNOT_EINVAL;

	return knot_zone_diff_load_trees(t1, t2, changeset);
}