#include "knot/nameserver/ixfr.h"
#include "knot/nameserver/axfr.h"
#include "knot/nameserver/internet.h"
#include "knot/nameserver/process_query.h"
#include "knot/nameserver/process_answer.h"
#include "knot/updates/apply.h"
#include "common/debug.h"
#include "common/descriptor.h"
#include "libknot/util/utils.h"
#include "libknot/rdata/soa.h"

/* ------------------------ IXFR-out processing ----------------------------- */

/*! \brief Current IXFR answer sections. */
enum {
	SOA_REMOVE = 0,
	REMOVE,
	SOA_ADD,
	ADD
};

/*! \brief Extended structure for IXFR-out processing. */
struct ixfrout_proc {
	struct xfr_proc proc;
	node_t *cur;
	unsigned state;
	knot_changesets_t *changesets;
	struct query_data *qdata;
	const knot_rrset_t *soa_from, *soa_to;
};

/* IXFR-out-specific logging (internal, expects 'qdata' variable set). */
#define IXFROUT_LOG(severity, msg...) \
	QUERY_LOG(severity, qdata, "Outgoing IXFR", msg)

/*! \brief Helper macro for putting RRs into packet. */
#define IXFR_SAFE_PUT(pkt, rr) \
	ret = knot_pkt_put((pkt), 0, (rr), KNOT_PF_NOTRUNC); \
	if (ret != KNOT_EOK) { \
		return ret; \
	}

static int ixfr_put_rrlist(knot_pkt_t *pkt, struct ixfrout_proc *ixfr, list_t *list)
{
	assert(pkt);
	assert(ixfr);
	assert(list);

	/* If at the beginning, fetch first RR. */
	int ret = KNOT_EOK;
	if (ixfr->cur == NULL) {
		ixfr->cur = HEAD(*list);
	}
	/* Now iterate until it hits the last one,
	 * this is done without for() loop because we can
	 * rejoin the iteration at any point. */
	while(ixfr->cur->next) {
		knot_rr_ln_t *rr_item = (knot_rr_ln_t *)(ixfr->cur);
		if (rr_item->rr->rrs.rr_count > 0) {
			IXFR_SAFE_PUT(pkt, rr_item->rr);
		} else {
			dbg_ns("%s: empty RR %p, skipping\n", __func__, rr_item->rr);
		}

		ixfr->cur = ixfr->cur->next;
	}

	ixfr->cur = NULL;
	return ret;
}

/*!
 * \brief Process single changeset.
 * \note Keep in mind that this function must be able to resume processing,
 *       for example if it fills a packet and returns ESPACE, it is called again
 *       with next empty answer and it must resume the processing exactly where
 *       it's left off.
 */
static int ixfr_process_changeset(knot_pkt_t *pkt, const void *item, struct xfr_proc *xfer)
{
	int ret = KNOT_EOK;
	struct ixfrout_proc *ixfr = (struct ixfrout_proc *)xfer;
	knot_changeset_t *chgset = (knot_changeset_t *)item;

	/* Put former SOA. */
	if (ixfr->state == SOA_REMOVE) {
		IXFR_SAFE_PUT(pkt, chgset->soa_from);
		dbg_ns("%s: put 'REMOVE' SOA\n", __func__);
		ixfr->state = REMOVE;
	}

	/* Put REMOVE RRSets. */
	if (ixfr->state == REMOVE) {
		ret = ixfr_put_rrlist(pkt, ixfr, &chgset->remove);
		if (ret != KNOT_EOK) {
			return ret;
		}
		dbg_ns("%s: put 'REMOVE' RRs\n", __func__);
		ixfr->state = SOA_ADD;
	}

	/* Put next SOA. */
	if (ixfr->state == SOA_ADD) {
		IXFR_SAFE_PUT(pkt, chgset->soa_to);
		dbg_ns("%s: put 'ADD' SOA\n", __func__);
		ixfr->state = ADD;
	}

	/* Put REMOVE RRSets. */
	if (ixfr->state == ADD) {
		ret = ixfr_put_rrlist(pkt, ixfr, &chgset->add);
		if (ret != KNOT_EOK) {
			return ret;
		}
		dbg_ns("%s: put 'ADD' RRs\n", __func__);
		ixfr->state = SOA_REMOVE;
	}

	/* Finished change set. */
	struct query_data *qdata = ixfr->qdata; /*< Required for IXFROUT_LOG() */
	IXFROUT_LOG(LOG_INFO, "Serial %u -> %u.", chgset->serial_from, chgset->serial_to);

	return ret;
}

#undef IXFR_SAFE_PUT

static int ixfr_load_chsets(knot_changesets_t **chgsets, const zone_t *zone,
			    const knot_rrset_t *their_soa)
{
	assert(chgsets);
	assert(zone);

	/* Compare serials. */
	uint32_t serial_to = zone_contents_serial(zone->contents);
	uint32_t serial_from = knot_soa_serial(&their_soa->rrs);
	int ret = knot_serial_compare(serial_to, serial_from);
	if (ret <= 0) { /* We have older/same age zone. */
		return KNOT_EUPTODATE;
	}

	*chgsets = knot_changesets_create(0);
	if (*chgsets == NULL) {
		return KNOT_ENOMEM;
	}

	ret = journal_load_changesets(zone->conf->ixfr_db, *chgsets,
	                              serial_from, serial_to);
	if (ret != KNOT_EOK) {
		knot_changesets_free(chgsets, NULL);
	}

	return ret;
}

static int ixfr_query_check(struct query_data *qdata)
{
	/* Check if zone exists. */
	NS_NEED_ZONE(qdata, KNOT_RCODE_NOTAUTH);

	/* Need IXFR query type. */
	NS_NEED_QTYPE(qdata, KNOT_RRTYPE_IXFR, KNOT_RCODE_FORMERR);
	/* Need SOA authority record. */
	const knot_pktsection_t *authority = knot_pkt_section(qdata->query, KNOT_AUTHORITY);
	const knot_rrset_t *their_soa = &authority->rr[0];
	if (authority->count < 1 || their_soa->type != KNOT_RRTYPE_SOA) {
		qdata->rcode = KNOT_RCODE_FORMERR;
		return NS_PROC_FAIL;
	}
	/* SOA needs to match QNAME. */
	NS_NEED_QNAME(qdata, their_soa->owner, KNOT_RCODE_FORMERR);

	/* Check transcation security and zone contents. */
	NS_NEED_AUTH(qdata->zone->xfr_out, qdata);
	NS_NEED_ZONE_CONTENTS(qdata, KNOT_RCODE_SERVFAIL); /* Check expiration. */

	return NS_PROC_DONE;
}

static void ixfr_answer_cleanup(struct query_data *qdata)
{
	struct ixfrout_proc *ixfr = (struct ixfrout_proc *)qdata->ext;
	mm_ctx_t *mm = qdata->mm;

	ptrlist_free(&ixfr->proc.nodes, mm);
	knot_changesets_free(&ixfr->changesets, NULL);
	mm->free(qdata->ext);

	/* Allow zone changes (finished). */
	rcu_read_unlock();
}

static int ixfr_answer_init(struct query_data *qdata)
{
	/* Check IXFR query validity. */
	int state = ixfr_query_check(qdata);
	if (state == NS_PROC_FAIL) {
		if (qdata->rcode == KNOT_RCODE_FORMERR) {
			return KNOT_EMALF;
		} else {
			return KNOT_EDENIED;
		}
	}

	/* Compare serials. */
	const knot_rrset_t *their_soa = &knot_pkt_section(qdata->query, KNOT_AUTHORITY)->rr[0];
	knot_changesets_t *chgsets = NULL;
	int ret = ixfr_load_chsets(&chgsets, qdata->zone, their_soa);
	if (ret != KNOT_EOK) {
		dbg_ns("%s: failed to load changesets => %d\n", __func__, ret);
		return ret;
	}

	/* Initialize transfer processing. */
	mm_ctx_t *mm = qdata->mm;
	struct ixfrout_proc *xfer = mm->alloc(mm->ctx, sizeof(struct ixfrout_proc));
	if (xfer == NULL) {
		knot_changesets_free(&chgsets, NULL);
		return KNOT_ENOMEM;
	}
	memset(xfer, 0, sizeof(struct ixfrout_proc));
	gettimeofday(&xfer->proc.tstamp, NULL);
	init_list(&xfer->proc.nodes);
	xfer->qdata = qdata;

	/* Put all changesets to processing queue. */
	xfer->changesets = chgsets;
	knot_changeset_t *chs = NULL;
	WALK_LIST(chs, chgsets->sets) {
		ptrlist_add(&xfer->proc.nodes, chs, mm);
		dbg_ns("%s: preparing %u -> %u\n", __func__, chs->serial_from, chs->serial_to);
	}

	/* Keep first and last serial. */
	chs = HEAD(chgsets->sets);
	xfer->soa_from = chs->soa_from;
	chs = TAIL(chgsets->sets);
	xfer->soa_to = chs->soa_to;

	/* Set up cleanup callback. */
	qdata->ext = xfer;
	qdata->ext_cleanup = &ixfr_answer_cleanup;

	/* No zone changes during multipacket answer (unlocked in axfr_answer_cleanup) */
	rcu_read_lock();

	return KNOT_EOK;
}

static int ixfr_answer_soa(knot_pkt_t *pkt, struct query_data *qdata)
{
	dbg_ns("%s: answering IXFR/SOA\n", __func__);
	if (pkt == NULL || qdata == NULL) {
		return NS_PROC_FAIL;
	}

	/* Check query. */
	int state = ixfr_query_check(qdata);
	if (state == NS_PROC_FAIL) {
		return state; /* Malformed query. */
	}

	/* Reserve space for TSIG. */
	knot_pkt_reserve(pkt, tsig_wire_maxsize(qdata->sign.tsig_key));

	/* Guaranteed to have zone contents. */
	const zone_node_t *apex = qdata->zone->contents->apex;
	knot_rrset_t soa_rr = node_rrset(apex, KNOT_RRTYPE_SOA);
	if (knot_rrset_empty(&soa_rr)) {
		return NS_PROC_FAIL;
	}
	int ret = knot_pkt_put(pkt, 0, &soa_rr, 0);
	if (ret != KNOT_EOK) {
		qdata->rcode = KNOT_RCODE_SERVFAIL;
		return NS_PROC_FAIL;
	}

	return NS_PROC_DONE;
}

/* ------------------------- IXFR-in processing ----------------------------- */

/*! \brief IXFR-in processing states. */
enum ixfrin_states {
	IXFR_START = 0,  /* IXFR-in starting, expecting final SOA. */
	IXFR_SOA_FROM,   /* Expecting starting SOA. */
	IXFR_SOA_TO,     /* Expecting ending SOA. */
	IXFR_DEL,        /* Expecting RR to delete. */
	IXFR_ADD,        /* Expecting RR to add. */
	IXFR_DONE        /* Processing done, IXFR-in complete. */
};

/*! \brief Extended structure for IXFR-in processing. */
struct ixfrin_proc {
	struct xfr_proc *xfr_proc;      /* Generic transfer processing context. */
	int state;                      /* IXFR-in state. */
	knot_changesets_t *changesets;  /* Created changesets. */
	zone_t *zone;                   /* Modified zone. */
	mm_ctx_t *mm;                   /* Memory context for RR allocations. */
};

/* IXFR-in-specific logging (internal, expects 'adata' variable set). */
#define IXFRIN_LOG(severity, msg...) \
	ANSWER_LOG(severity, adata, "Incoming IXFR", msg)

/*! \brief Cleans up data allocated by IXFR-in processing. */
static void ixfrin_cleanup(struct answer_data *data)
{
	struct ixfrin_proc *proc = data->ext;
	if (proc) {
		knot_changesets_free(&proc->changesets, data->mm);
		mm_free(data->mm, proc);
		data->ext = NULL;
	}
}

/*! \brief Initializes IXFR-in processing context. */
static int ixfrin_answer_init(struct answer_data *data)
{
	struct ixfrin_proc *proc = mm_alloc(data->mm, sizeof(struct ixfrin_proc));
	data->ext = malloc(sizeof(struct ixfrin_proc));
	if (proc == NULL) {
		return KNOT_ENOMEM;
	}
	memset(proc, 0, sizeof(struct ixfrin_proc));

	proc->changesets = knot_changesets_create(0);
	if (proc->changesets == NULL) {
		mm_free(data->mm, proc);
		return KNOT_ENOMEM;
	}
	proc->state = IXFR_START;
	proc->zone = data->param->zone;

	data->ext = proc;
	data->ext_cleanup = &ixfrin_cleanup;

	return KNOT_EOK;
}

/*! \brief Finalizes IXFR-in processing. */
static int ixfrin_finalize(struct answer_data *adata)
{
	struct ixfrin_proc *proc = adata->ext;
	zone_t *zone = proc->zone;
	knot_changesets_t *changesets = proc->changesets;

#warning if we need to check serials, here's the place

	if (knot_changesets_empty(changesets) || proc->state != IXFR_DONE) {
		ixfrin_cleanup(adata);
		IXFRIN_LOG(LOG_INFO, "Fallback to AXFR.");
		return KNOT_ENOIXFR;
	}

	int ret = zone_change_apply_and_store(changesets, zone, "IXFR", adata->mm);
	if (ret != KNOT_EOK) {
		free(proc);
		return ret;
	}

	proc->changesets = NULL; // Free'd by apply_and_store()
	ixfrin_cleanup(adata);

	IXFRIN_LOG(LOG_INFO, "Finished.");
#warning TODO: schedule zone events, count transfer size and message count, time

	return KNOT_EOK;
}

/*! \brief Stores starting SOA into changesets structure. */
static int solve_start(const knot_rrset_t *rr, knot_changesets_t *changesets, mm_ctx_t *mm)
{
	assert(changesets->first_soa == NULL);
	if (rr->type != KNOT_RRTYPE_SOA) {
		return NS_PROC_FAIL;
	}

	// Store the first SOA for later use.
	changesets->first_soa = knot_rrset_copy(rr, mm);
	if (changesets->first_soa == NULL) {
		return NS_PROC_FAIL;
	}

	return NS_PROC_MORE;
}

/*!
 * \brief Decides what to do with a starting SOA - either ends the processing or
 *        creates a new changeset and stores the SOA into it.
 */
static int solve_soa_from(const knot_rrset_t *rr, knot_changesets_t *changesets,
                          int *state, mm_ctx_t *mm)
{
	if (rr->type != KNOT_RRTYPE_SOA) {
		return NS_PROC_FAIL;
	}

	if (knot_rrset_equal(rr, changesets->first_soa, KNOT_RRSET_COMPARE_WHOLE)) {
		// Last SOA encountered, transfer done.
		*state = IXFR_DONE;
		return NS_PROC_DONE;
	}

	// Create new changeset.
	knot_changeset_t *change = knot_changesets_create_changeset(changesets);
	if (change == NULL) {
		return NS_PROC_FAIL;
	}

	// Store SOA into changeset.
	change->soa_from = knot_rrset_copy(rr, mm);
	if (change->soa_from == NULL) {
		return NS_PROC_FAIL;
	}
	change->serial_from = knot_soa_serial(&rr->rrs);

	return NS_PROC_MORE;
}

/*! \brief Stores ending SOA into changeset. */
static int solve_soa_to(const knot_rrset_t *rr, knot_changeset_t *change, mm_ctx_t *mm)
{
	if (rr->type != KNOT_RRTYPE_SOA) {
		return NS_PROC_FAIL;
	}

	change->soa_to= knot_rrset_copy(rr, mm);
	if (change->soa_to == NULL) {
		return NS_PROC_FAIL;
	}
	change->serial_to = knot_soa_serial(&rr->rrs);

	return NS_PROC_MORE;
}

/*! \brief Adds single RR into given section of changeset. */
static int add_part(const knot_rrset_t *rr, knot_changeset_t *change, int part, mm_ctx_t *mm)
{
	assert(rr->type != KNOT_RRTYPE_SOA);
	knot_rrset_t *copy = knot_rrset_copy(rr, mm);
	if (copy) {
		int ret = knot_changeset_add_rrset(change, copy, part);
		if (ret != KNOT_EOK) {
			return NS_PROC_FAIL;
		} else {
			return NS_PROC_MORE;
		}
	} else {
		return NS_PROC_FAIL;
	}
}

/*! \brief Adds single RR into REMOVE section of changeset. */
static int solve_del(const knot_rrset_t *rr, knot_changeset_t *change, mm_ctx_t *mm)
{
	return add_part(rr, change, KNOT_CHANGESET_REMOVE, mm);
}

/*! \brief Adds single RR into ADD section of changeset. */
static int solve_add(const knot_rrset_t *rr, knot_changeset_t *change, mm_ctx_t *mm)
{
	return add_part(rr, change, KNOT_CHANGESET_ADD, mm);
}

/*!
 * \brief Processes single RR according to current IXFR-in state. The states
 *        correspond with IXFR-in message structure, in the order they are
 *        mentioned in the code.
 *
 * \param rr          RR to process.
 * \param changesets  Output changesets.
 * \param state       Current IXFR-in state.
 * \param next        Output parameter - set to true if next RR should be fetched.
 * \param mm          Memory context used to create RR copies.
 *
 * \return NS_PROC_MORE, NS_PROC_DONE, NS_PROC_FAIL.
 */
static int ixfrin_step(const knot_rrset_t *rr, knot_changesets_t *changesets,
                       int *state, bool *next, mm_ctx_t *mm)
{
	switch (*state) {
	case IXFR_START:
		*state = IXFR_SOA_FROM;
		*next = true;
		return solve_start(rr, changesets, mm);
	case IXFR_SOA_FROM:
		*state = IXFR_DEL;
		*next = true;
		return solve_soa_from(rr, changesets, state, mm);
	case IXFR_DEL:
		if (rr->type == KNOT_RRTYPE_SOA) {
			// Encountered SOA, do not consume the RR.
			*state = IXFR_SOA_TO;
			*next = false;
			return NS_PROC_MORE;
		}
		*next = true;
		return solve_del(rr, knot_changesets_get_last(changesets), mm);
	case IXFR_SOA_TO:
		*state = IXFR_ADD;
		*next = true;
		return solve_soa_to(rr, knot_changesets_get_last(changesets), mm);
	case IXFR_ADD:
		if (rr->type == KNOT_RRTYPE_SOA) {
			// Encountered SOA, do not consume the RR.
			*state = IXFR_SOA_FROM;
			*next = false;
			return NS_PROC_MORE;
		}
		*next = true;
		return solve_add(rr, knot_changesets_get_last(changesets), mm);
	default:
		return NS_PROC_FAIL;
	}
}

/*! \brief Checks whether journal node limit has not been exceeded. */
static bool journal_limit_exceeded(struct ixfrin_proc *proc)
{
	return proc->changesets->count > JOURNAL_NCOUNT;
}

/*! \brief Checks whether RR belongs into zone. */
static bool out_of_zone(const knot_rrset_t *rr, struct ixfrin_proc *proc)
{
	return !knot_dname_is_sub(rr->owner, proc->zone->name) &&
	       !knot_dname_is_equal(rr->owner, proc->zone->name);
}

/*!
 * \brief Processes IXFR reply packet and fills in the changesets structure.
 *
 * \param pkt   Packet containing the IXFR reply in wire format.
 * \param proc  Processing context.
 *
 * \return NS_PROC_MORE, NS_PROC_DONE, NS_PROC_FAIL
 */
static int xfrin_process_ixfr_packet(knot_pkt_t *pkt, struct ixfrin_proc *proc)
{
	const knot_pktsection_t *answer = knot_pkt_section(pkt, KNOT_ANSWER);
	int ret = NS_PROC_NOOP;
	for (uint16_t i = 0; i < answer->count; /* NOOP */) {
		if (journal_limit_exceeded(proc)) {
			// Will revert to AXFR.
			assert(proc->state != IXFR_DONE);
			return NS_PROC_DONE;
		}

		const knot_rrset_t *rr = &answer->rr[i];
		if (out_of_zone(rr, proc)) {
			continue;
		}

		// Process RR.
		bool next = false;
		ret = ixfrin_step(rr, proc->changesets,
		                  &proc->state, &next, proc->mm);
		if (ret == NS_PROC_FAIL || ret == NS_PROC_DONE) {
			// Quit on errors and if we're done.
			return ret;
		}
		if (next) {
			++i;
		}
	}

#warning TODO TSIG
	assert(ret == NS_PROC_MORE);
	return ret;
}

/* --------------------------------- API ------------------------------------ */

int ixfr_query(knot_pkt_t *pkt, struct query_data *qdata)
{
	if (pkt == NULL || qdata == NULL) {
		return NS_PROC_FAIL;
	}

	int ret = KNOT_EOK;
	struct timeval now = {0};
	struct ixfrout_proc *ixfr = (struct ixfrout_proc*)qdata->ext;

	/* If IXFR is disabled, respond with SOA. */
	if (qdata->param->proc_flags & NS_QUERY_NO_IXFR) {
		return ixfr_answer_soa(pkt, qdata);
	}

	/* Initialize on first call. */
	if (qdata->ext == NULL) {
		ret = ixfr_answer_init(qdata);
		switch(ret) {
		case KNOT_EOK:      /* OK */
			ixfr = (struct ixfrout_proc*)qdata->ext;
			IXFROUT_LOG(LOG_INFO, "Started (serial %u -> %u).",
			            knot_soa_serial(&ixfr->soa_from->rrs),
			            knot_soa_serial(&ixfr->soa_to->rrs));
			break;
		case KNOT_EUPTODATE: /* Our zone is same age/older, send SOA. */
			IXFROUT_LOG(LOG_INFO, "Zone is up-to-date.");
			return ixfr_answer_soa(pkt, qdata);
		case KNOT_ERANGE:   /* No history -> AXFR. */
		case KNOT_ENOENT:
			IXFROUT_LOG(LOG_INFO, "Incomplete history, fallback to AXFR.");
			qdata->packet_type = KNOT_QUERY_AXFR; /* Solve as AXFR. */
			return axfr_query(pkt, qdata);
		default:            /* Server errors. */
			IXFROUT_LOG(LOG_ERR, "Failed to start (%s).", knot_strerror(ret));
			return NS_PROC_FAIL;
		}
	}

	/* Reserve space for TSIG. */
	knot_pkt_reserve(pkt, tsig_wire_maxsize(qdata->sign.tsig_key));

	/* Answer current packet (or continue). */
	ret = xfr_process_list(pkt, &ixfr_process_changeset, qdata);
	switch(ret) {
	case KNOT_ESPACE: /* Couldn't write more, send packet and continue. */
		return NS_PROC_FULL; /* Check for more. */
	case KNOT_EOK:    /* Last response. */
		gettimeofday(&now, NULL);
		IXFROUT_LOG(LOG_INFO, "Finished in %.02fs (%u messages, ~%.01fkB).",
		            time_diff(&ixfr->proc.tstamp, &now) / 1000.0,
		            ixfr->proc.npkts, ixfr->proc.nbytes / 1024.0);
		ret = NS_PROC_DONE;
		break;
	default:          /* Generic error. */
		IXFROUT_LOG(LOG_ERR, "%s", knot_strerror(ret));
		ret = NS_PROC_FAIL;
		break;
	}

	return ret;
}

int ixfrin_process_answer(knot_pkt_t *pkt, struct answer_data *adata)
{
	if (adata->ext == NULL) {
		IXFRIN_LOG(LOG_INFO, "Starting.");
		// First packet with IXFR, init context
		int ret = ixfrin_answer_init(adata);
		if (ret != KNOT_EOK) {
			IXFRIN_LOG(LOG_ERR, "Failed - %s", knot_strerror(ret));
			return NS_PROC_FAIL;
		}
	}

	int ret = xfrin_process_ixfr_packet(pkt, (struct ixfrin_proc *)adata->ext);
	if (ret == NS_PROC_DONE) {
		int fret = ixfrin_finalize(adata);
#warning get rid of this rcode mix
		if (fret != KNOT_EOK) {
			if (fret != KNOT_ENOIXFR) {
				ret = NS_PROC_FAIL;
			} else {
				return KNOT_ENOIXFR;
			}
		}
	}

	if (ret == NS_PROC_FAIL) {
		IXFRIN_LOG(LOG_ERR, "Failed.");
	}

	return ret;
}

#undef IXFROUT_LOG
