/*
 * Copyright (c) 2006-2007 Chelsio, Inc. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include <config.h>

#include <stdio.h>
#include <pthread.h>
#include <sys/errno.h>

#include <infiniband/opcode.h>

#include "iwch.h"
#include "iwch-abi.h"

int iwch_arm_cq(struct ibv_cq *ibcq, int solicited)
{
	int ret;
	struct iwch_cq *chp = to_iwch_cq(ibcq);

	pthread_spin_lock(&chp->lock);
	ret = ibv_cmd_req_notify_cq(ibcq, solicited);
	pthread_spin_unlock(&chp->lock);

	return ret;
}

static inline void flush_completed_wrs(struct t3_wq *wq, struct t3_cq *cq)
{
	struct t3_swsq *sqp;
	uint32_t ptr = wq->sq_rptr;
	int count = Q_COUNT(wq->sq_rptr, wq->sq_wptr);
	
	sqp = wq->sq + Q_PTR2IDX(ptr, wq->sq_size_log2);
	while (count--) {
		if (!sqp->signaled) {
			ptr++;
			sqp = wq->sq + Q_PTR2IDX(ptr,  wq->sq_size_log2);
		} else if (sqp->complete) {

			/* 
			 * Insert this completed cqe into the swcq.
			 */
			sqp->cqe.header |= htobe32(V_CQE_SWCQE(1));
			*(cq->sw_queue + Q_PTR2IDX(cq->sw_wptr, cq->size_log2)) 
				= sqp->cqe;
			cq->sw_wptr++;
			sqp->signaled = 0;
			break;
		} else
			break;
	}
}

static inline void create_read_req_cqe(struct t3_wq *wq,
				       struct t3_cqe *hw_cqe,
				       struct t3_cqe *read_cqe)
{
	CQE_WRID_SQ_WPTR(*read_cqe) = wq->oldest_read->sq_wptr;
	read_cqe->len = wq->oldest_read->read_len;
	read_cqe->header = htobe32(V_CQE_QPID(CQE_QPID(*hw_cqe)) |
				 V_CQE_SWCQE(SW_CQE(*hw_cqe)) |
				 V_CQE_OPCODE(T3_READ_REQ) |
				 V_CQE_TYPE(1));
}

/*
 * Return a ptr to the next read wr in the SWSQ or NULL.
 */
static inline void advance_oldest_read(struct t3_wq *wq)
{

	uint32_t rptr = wq->oldest_read - wq->sq + 1;
	uint32_t wptr = Q_PTR2IDX(wq->sq_wptr, wq->sq_size_log2);

	while (Q_PTR2IDX(rptr, wq->sq_size_log2) != wptr) {
		wq->oldest_read = wq->sq + Q_PTR2IDX(rptr, wq->sq_size_log2);

		if (wq->oldest_read->opcode == T3_READ_REQ) {
			return;
		}
		rptr++;
	}
	wq->oldest_read = NULL;
}

static inline int cxio_poll_cq(struct t3_wq *wq, struct t3_cq *cq,
		   struct t3_cqe *cqe, uint8_t *cqe_flushed,
		   uint64_t *cookie)
{
	int ret = 0;
	struct t3_cqe *hw_cqe, read_cqe;

	*cqe_flushed = 0;
	hw_cqe = cxio_next_cqe(cq);
	udma_from_device_barrier();

	/* 
	 * Skip cqes not affiliated with a QP.
	 */
	if (wq == NULL) {
		ret = -1;
		goto skip_cqe;
	}

	/*
	 * Gotta tweak READ completions:
	 * 	1) the cqe doesn't contain the sq_wptr from the wr.
	 *	2) opcode not reflected from the wr.
	 *	3) read_len not reflected from the wr.
	 *	4) cq_type is RQ_TYPE not SQ_TYPE.
	 */
	if (CQE_OPCODE(*hw_cqe) == T3_READ_RESP) {

                /*
		 * If this is an unsolicited read response to local stag 1, 
		 * then the read was generated by the kernel driver as part 
		 * of peer-2-peer connection setup.  So ignore the completion.
		 */
		if (CQE_WRID_STAG(*hw_cqe) == 1) {
			if (CQE_STATUS(*hw_cqe))
				wq->error = 1;
			ret = -1;
			goto skip_cqe;
		}
		
		/* 
	 	 * Don't write to the HWCQ, so create a new read req CQE 
		 * in local memory.
		 */
		create_read_req_cqe(wq, hw_cqe, &read_cqe);
		hw_cqe = &read_cqe;
		advance_oldest_read(wq);
	}

	/* 
	 * Errors.
	 */
	if (CQE_STATUS(*hw_cqe) || t3_wq_in_error(wq)) {
		*cqe_flushed = t3_wq_in_error(wq);
		t3_set_wq_in_error(wq);
		goto proc_cqe;
	}

	/*
	 * RECV completion.
	 */
	if (RQ_TYPE(*hw_cqe)) {

		/* 
		 * HW only validates 4 bits of MSN.  So we must validate that
		 * the MSN in the SEND is the next expected MSN.  If its not,
		 * then we complete this with TPT_ERR_MSN and mark the wq in 
		 * error.
		 */
		if ((CQE_WRID_MSN(*hw_cqe) != (wq->rq_rptr + 1))) {
			t3_set_wq_in_error(wq);
			hw_cqe->header |= htobe32(V_CQE_STATUS(TPT_ERR_MSN));
		}
		goto proc_cqe;
	}

	/* 
 	 * If we get here its a send completion.
	 *
	 * Handle out of order completion. These get stuffed
	 * in the SW SQ. Then the SW SQ is walked to move any
	 * now in-order completions into the SW CQ.  This handles
	 * 2 cases:
	 * 	1) reaping unsignaled WRs when the first subsequent
	 *	   signaled WR is completed.
	 *	2) out of order read completions.
	 */
	if (!SW_CQE(*hw_cqe) && (CQE_WRID_SQ_WPTR(*hw_cqe) != wq->sq_rptr)) {
		struct t3_swsq *sqp;

		sqp = wq->sq + 
		      Q_PTR2IDX(CQE_WRID_SQ_WPTR(*hw_cqe), wq->sq_size_log2);
		sqp->cqe = *hw_cqe;
		sqp->complete = 1;
		ret = -1;
		goto flush_wq;
	}

proc_cqe:
	*cqe = *hw_cqe;

	/*
	 * Reap the associated WR(s) that are freed up with this
	 * completion.
	 */
	if (SQ_TYPE(*hw_cqe)) {
		wq->sq_rptr = CQE_WRID_SQ_WPTR(*hw_cqe);
		*cookie = (wq->sq + 
			   Q_PTR2IDX(wq->sq_rptr, wq->sq_size_log2))->wr_id;
		wq->sq_rptr++;
	} else {
		*cookie = *(wq->rq + Q_PTR2IDX(wq->rq_rptr, wq->rq_size_log2));
		wq->rq_rptr++;
	}

flush_wq:
	/*
	 * Flush any completed cqes that are now in-order.
	 */
	flush_completed_wrs(wq, cq);

skip_cqe:
	if (SW_CQE(*hw_cqe)) {
		PDBG("%s cq %p cqid 0x%x skip sw cqe sw_rptr 0x%x\n", 
		     __FUNCTION__, cq, cq->cqid, cq->sw_rptr);
		++cq->sw_rptr;
	} else {
		PDBG("%s cq %p cqid 0x%x skip hw cqe sw_rptr 0x%x\n", 
		     __FUNCTION__, cq, cq->cqid, cq->rptr);
		++cq->rptr;
	}

	return ret;
}

/*
 * Get one cq entry from cxio and map it to openib.
 *
 * Returns:
 * 	0 			EMPTY;
 *	1			cqe returned
 *	-EAGAIN 		caller must try again
 * 	any other -errno	fatal error
 */
static int iwch_poll_cq_one(struct iwch_device *rhp, struct iwch_cq *chp,
		     struct ibv_wc *wc)
{
	struct iwch_qp *qhp = NULL;
	struct t3_cqe cqe, *hw_cqe;
	struct t3_wq *wq;
	uint8_t cqe_flushed;
	uint64_t cookie;
	int ret = 1;

	hw_cqe = cxio_next_cqe(&chp->cq);
	udma_from_device_barrier();

	if (!hw_cqe)
		return 0;

	qhp = rhp->qpid2ptr[CQE_QPID(*hw_cqe)];
	if (!qhp)
		wq = NULL;
	else {
		pthread_spin_lock(&qhp->lock);
		wq = &(qhp->wq);
	}
	ret = cxio_poll_cq(wq, &(chp->cq), &cqe, &cqe_flushed, &cookie);
	if (ret) {
		ret = -EAGAIN;
		goto out;
	}
	ret = 1;

	wc->wr_id = cookie;
	wc->qp_num = qhp->wq.qpid;
	wc->vendor_err = CQE_STATUS(cqe);
	wc->wc_flags = 0;

	PDBG("%s qpid 0x%x type %d opcode %d status 0x%x wrid hi 0x%x "
	     "lo 0x%x cookie 0x%" PRIx64 "\n", 
	     __FUNCTION__, CQE_QPID(cqe), CQE_TYPE(cqe),
	     CQE_OPCODE(cqe), CQE_STATUS(cqe), CQE_WRID_HI(cqe),
	     CQE_WRID_LOW(cqe), cookie);

	if (CQE_TYPE(cqe) == 0) {
		if (!CQE_STATUS(cqe))
			wc->byte_len = CQE_LEN(cqe);
		else
			wc->byte_len = 0;
		wc->opcode = IBV_WC_RECV;
	} else {
		switch (CQE_OPCODE(cqe)) {
		case T3_RDMA_WRITE:
			wc->opcode = IBV_WC_RDMA_WRITE;
			break;
		case T3_READ_REQ:
			wc->opcode = IBV_WC_RDMA_READ;
			wc->byte_len = CQE_LEN(cqe);
			break;
		case T3_SEND:
		case T3_SEND_WITH_SE:
			wc->opcode = IBV_WC_SEND;
			break;
		case T3_BIND_MW:
			wc->opcode = IBV_WC_BIND_MW;
			break;

		/* these aren't supported yet */
		case T3_SEND_WITH_INV:
		case T3_SEND_WITH_SE_INV:
		case T3_LOCAL_INV:
		case T3_FAST_REGISTER:
		default:
			PDBG("%s Unexpected opcode %d CQID 0x%x QPID 0x%x\n", 
			     __FUNCTION__, CQE_OPCODE(cqe), chp->cq.cqid, 
			     CQE_QPID(cqe));
			ret = -EINVAL;
			goto out;
		}
	}

	if (cqe_flushed) {
		wc->status = IBV_WC_WR_FLUSH_ERR;
	} else {
		
		switch (CQE_STATUS(cqe)) {
		case TPT_ERR_SUCCESS:
			wc->status = IBV_WC_SUCCESS;
			break;
		case TPT_ERR_STAG:
			wc->status = IBV_WC_LOC_ACCESS_ERR;
			break;
		case TPT_ERR_PDID:
			wc->status = IBV_WC_LOC_PROT_ERR;
			break;
		case TPT_ERR_QPID:
		case TPT_ERR_ACCESS:
			wc->status = IBV_WC_LOC_ACCESS_ERR;
			break;
		case TPT_ERR_WRAP:
			wc->status = IBV_WC_GENERAL_ERR;
			break;
		case TPT_ERR_BOUND:
			wc->status = IBV_WC_LOC_LEN_ERR;
			break;
		case TPT_ERR_INVALIDATE_SHARED_MR:
		case TPT_ERR_INVALIDATE_MR_WITH_MW_BOUND:
			wc->status = IBV_WC_MW_BIND_ERR;
			break;
		case TPT_ERR_CRC:
		case TPT_ERR_MARKER:
		case TPT_ERR_PDU_LEN_ERR:
		case TPT_ERR_OUT_OF_RQE:
		case TPT_ERR_DDP_VERSION:
		case TPT_ERR_RDMA_VERSION:
		case TPT_ERR_DDP_QUEUE_NUM:
		case TPT_ERR_MSN:
		case TPT_ERR_TBIT:
		case TPT_ERR_MO:
		case TPT_ERR_MSN_RANGE:
		case TPT_ERR_IRD_OVERFLOW:
		case TPT_ERR_OPCODE:
			wc->status = IBV_WC_FATAL_ERR;
			break;
		case TPT_ERR_SWFLUSH:
			wc->status = IBV_WC_WR_FLUSH_ERR;
			break;
		default:
			PDBG("%s Unexpected status 0x%x CQID 0x%x QPID 0x%0x\n",
			     __FUNCTION__, CQE_STATUS(cqe), chp->cq.cqid, 
			     CQE_QPID(cqe));
			ret = -EINVAL;
		}
	}
out:
	if (wq)
		pthread_spin_unlock(&qhp->lock);
	return ret;
}

int t3b_poll_cq(struct ibv_cq *ibcq, int num_entries, struct ibv_wc *wc)
{
	struct iwch_device *rhp;
	struct iwch_cq *chp;
	int npolled;
	int err = 0;

	chp = to_iwch_cq(ibcq);
	rhp = chp->rhp;

	if (rhp->abi_version > 0 && t3_cq_in_error(&chp->cq)) {
		t3_reset_cq_in_error(&chp->cq);
		iwch_flush_qps(rhp);
	}

	pthread_spin_lock(&chp->lock);
	for (npolled = 0; npolled < num_entries; ++npolled) {

		/*
	 	 * Because T3 can post CQEs that are out of order,
	 	 * we might have to poll again after removing
	 	 * one of these.  
		 */
		do {
			err = iwch_poll_cq_one(rhp, chp, wc + npolled);
		} while (err == -EAGAIN);
		if (err <= 0)
			break;
	}
	pthread_spin_unlock(&chp->lock);

	if (err < 0)
		return err;
	else {
		return npolled;
	}
}

int t3a_poll_cq(struct ibv_cq *ibcq, int num_entries, struct ibv_wc *wc)
{
	int ret;
	struct iwch_cq *chp = to_iwch_cq(ibcq);
	
	pthread_spin_lock(&chp->lock);
	ret = ibv_cmd_poll_cq(ibcq, num_entries, wc);
	pthread_spin_unlock(&chp->lock);
	return ret;
}
