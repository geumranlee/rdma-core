/*
 * Copyright (c) 2016-2017 Hisilicon Limited.
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

#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include "hns_roce_u.h"
#include "hns_roce_u_db.h"
#include "hns_roce_u_hw_v2.h"

static void *get_send_sge_ex(struct hns_roce_qp *qp, int n);

static void set_data_seg_v2(struct hns_roce_v2_wqe_data_seg *dseg,
			 struct ibv_sge *sg)
{
	dseg->lkey = htole32(sg->lkey);
	dseg->addr = htole64(sg->addr);
	dseg->len = htole32(sg->length);
}

static void set_extend_atomic_seg(struct hns_roce_qp *qp,
				  unsigned int atomic_buf,
				  struct hns_roce_sge_info *sge_info,
				  void *buf)
{
	unsigned int sge_mask = qp->ex_sge.sge_cnt - 1;
	int i;

	for (i = 0; i < atomic_buf; i++, sge_info->start_idx++)
		memcpy(get_send_sge_ex(qp, sge_info->start_idx & sge_mask),
		       buf + i * HNS_ROCE_SGE_SIZE, HNS_ROCE_SGE_SIZE);
}

static int set_atomic_seg(struct hns_roce_qp *qp, struct ibv_send_wr *wr,
			  unsigned int msg_len, void *dseg,
			  struct hns_roce_sge_info *sge_info)
{
	struct hns_roce_wqe_atomic_seg *aseg;
	unsigned int ext_sg_num;

	aseg = dseg;

	if (msg_len == STANDARD_ATOMIC_U_BYTE_8) {
		if (wr->opcode == IBV_WR_ATOMIC_CMP_AND_SWP) {
			aseg->fetchadd_swap_data = htole64(wr->wr.atomic.swap);
			aseg->cmp_data = htole64(wr->wr.atomic.compare_add);
		} else {
			aseg->fetchadd_swap_data =
					htole64(wr->wr.atomic.compare_add);
			aseg->cmp_data = 0;
		}
	} else if (msg_len == EXTEND_ATOMIC_U_BYTE_16 ||
		   msg_len == EXTEND_ATOMIC_U_BYTE_32 ||
		   msg_len == EXTEND_ATOMIC_U_BYTE_64) {
		ext_sg_num = msg_len * DATA_TYPE_NUM >> HNS_ROCE_SGE_SHIFT;
		aseg->fetchadd_swap_data = 0;
		aseg->cmp_data = 0;
		if (wr->opcode == IBV_WR_ATOMIC_CMP_AND_SWP) {
			if (!wr->wr.atomic.swap || !wr->wr.atomic.compare_add)
				return -EINVAL;

			set_extend_atomic_seg(qp, ext_sg_num / DATA_TYPE_NUM,
					      sge_info,
					      (void *) (uintptr_t) wr->wr.atomic.swap);
			set_extend_atomic_seg(qp, ext_sg_num / DATA_TYPE_NUM,
					      sge_info,
					      (void *) (uintptr_t) wr->wr.atomic.compare_add);
		} else {
			uint8_t buf[EXTEND_ATOMIC_U_BYTE_64] = {};

			if (!wr->wr.atomic.compare_add)
				return -EINVAL;

			set_extend_atomic_seg(qp, ext_sg_num / DATA_TYPE_NUM,
					      sge_info,
					      (void *) (uintptr_t) wr->wr.atomic.compare_add);
			set_extend_atomic_seg(qp, ext_sg_num / DATA_TYPE_NUM,
					      sge_info, buf);
		}
	} else
		return -EINVAL;

	return 0;
}

static void hns_roce_v2_handle_error_cqe(struct hns_roce_v2_cqe *cqe,
					 struct ibv_wc *wc)
{
	unsigned int status = roce_get_field(cqe->byte_4, CQE_BYTE_4_STATUS_M,
					     CQE_BYTE_4_STATUS_S);
	unsigned int cqe_status = status & HNS_ROCE_V2_CQE_STATUS_MASK;

	switch (cqe_status) {
	case HNS_ROCE_V2_CQE_LOCAL_LENGTH_ERR:
		wc->status = IBV_WC_LOC_LEN_ERR;
		break;
	case HNS_ROCE_V2_CQE_LOCAL_QP_OP_ERR:
		wc->status = IBV_WC_LOC_QP_OP_ERR;
		break;
	case HNS_ROCE_V2_CQE_LOCAL_PROT_ERR:
		wc->status = IBV_WC_LOC_PROT_ERR;
		break;
	case HNS_ROCE_V2_CQE_WR_FLUSH_ERR:
		wc->status = IBV_WC_WR_FLUSH_ERR;
		break;
	case HNS_ROCE_V2_CQE_MEM_MANAGERENT_OP_ERR:
		wc->status = IBV_WC_MW_BIND_ERR;
		break;
	case HNS_ROCE_V2_CQE_BAD_RESP_ERR:
		wc->status = IBV_WC_BAD_RESP_ERR;
		break;
	case HNS_ROCE_V2_CQE_LOCAL_ACCESS_ERR:
		wc->status = IBV_WC_LOC_ACCESS_ERR;
		break;
	case HNS_ROCE_V2_CQE_REMOTE_INVAL_REQ_ERR:
		wc->status = IBV_WC_REM_INV_REQ_ERR;
		break;
	case HNS_ROCE_V2_CQE_REMOTE_ACCESS_ERR:
		wc->status = IBV_WC_REM_ACCESS_ERR;
		break;
	case HNS_ROCE_V2_CQE_REMOTE_OP_ERR:
		wc->status = IBV_WC_REM_OP_ERR;
		break;
	case HNS_ROCE_V2_CQE_TRANSPORT_RETRY_EXC_ERR:
		wc->status = IBV_WC_RETRY_EXC_ERR;
		break;
	case HNS_ROCE_V2_CQE_RNR_RETRY_EXC_ERR:
		wc->status = IBV_WC_RNR_RETRY_EXC_ERR;
		break;
	case HNS_ROCE_V2_CQE_REMOTE_ABORTED_ERR:
		wc->status = IBV_WC_REM_ABORT_ERR;
		break;
	default:
		wc->status = IBV_WC_GENERAL_ERR;
		break;
	}
}

static struct hns_roce_v2_cqe *get_cqe_v2(struct hns_roce_cq *cq, int entry)
{
	return cq->buf.buf + entry * HNS_ROCE_CQE_ENTRY_SIZE;
}

static void *get_sw_cqe_v2(struct hns_roce_cq *cq, int n)
{
	struct hns_roce_v2_cqe *cqe = get_cqe_v2(cq, n & cq->ibv_cq.cqe);

	return (!!(roce_get_bit(cqe->byte_4, CQE_BYTE_4_OWNER_S)) ^
		!!(n & (cq->ibv_cq.cqe + 1))) ? cqe : NULL;
}

static struct hns_roce_v2_cqe *next_cqe_sw_v2(struct hns_roce_cq *cq)
{
	return get_sw_cqe_v2(cq, cq->cons_index);
}

static void *get_recv_wqe_v2(struct hns_roce_qp *qp, int n)
{
	if ((n < 0) || (n > qp->rq.wqe_cnt)) {
		printf("rq wqe index:%d,rq wqe cnt:%d\r\n", n, qp->rq.wqe_cnt);
		return NULL;
	}

	return qp->buf.buf + qp->rq.offset + (n << qp->rq.wqe_shift);
}

static void *get_send_wqe(struct hns_roce_qp *qp, int n)
{
	return qp->buf.buf + qp->sq.offset + (n << qp->sq.wqe_shift);
}

static void *get_send_sge_ex(struct hns_roce_qp *qp, int n)
{
	return qp->buf.buf + qp->ex_sge.offset + (n << qp->ex_sge.sge_shift);
}

static void *get_srq_wqe(struct hns_roce_srq *srq, int n)
{
	return srq->buf.buf + (n << srq->wqe_shift);
}

static void hns_roce_free_srq_wqe(struct hns_roce_srq *srq, uint16_t ind)
{
	uint32_t bitmap_num;
	int bit_num;

	pthread_spin_lock(&srq->lock);

	bitmap_num = ind / BIT_CNT_PER_U64;
	bit_num = ind % BIT_CNT_PER_U64;
	srq->idx_que.bitmap[bitmap_num] |= (1ULL << bit_num);
	srq->tail++;

	pthread_spin_unlock(&srq->lock);
}

static int hns_roce_v2_wq_overflow(struct hns_roce_wq *wq, int nreq,
				   struct hns_roce_cq *cq)
{
	unsigned int cur;

	cur = wq->head - wq->tail;
	if (cur + nreq < wq->max_post)
		return 0;

	pthread_spin_lock(&cq->lock);
	cur = wq->head - wq->tail;
	pthread_spin_unlock(&cq->lock);

	return cur + nreq >= wq->max_post;
}

static void hns_roce_update_rq_db(struct hns_roce_context *ctx,
				  unsigned int qpn, unsigned int rq_head)
{
	struct hns_roce_db rq_db = {};

	roce_set_field(rq_db.byte_4, DB_BYTE_4_TAG_M, DB_BYTE_4_TAG_S, qpn);
	roce_set_field(rq_db.byte_4, DB_BYTE_4_CMD_M, DB_BYTE_4_CMD_S,
		       HNS_ROCE_V2_RQ_DB);
	roce_set_field(rq_db.parameter, DB_PARAM_RQ_PRODUCER_IDX_M,
		       DB_PARAM_RQ_PRODUCER_IDX_S, rq_head);

	hns_roce_write64(ctx->uar + ROCEE_VF_DB_CFG0_OFFSET, (__le32 *)&rq_db);
}

static void hns_roce_update_sq_db(struct hns_roce_context *ctx,
				  unsigned int qpn, unsigned int sl,
				  unsigned int sq_head)
{
	struct hns_roce_db sq_db = {};

	/* cmd: 0 sq db; 1 rq db; 2; 2 srq db; 3 cq db ptr; 4 cq db ntr */
	roce_set_field(sq_db.byte_4, DB_BYTE_4_CMD_M, DB_BYTE_4_CMD_S,
		       HNS_ROCE_V2_SQ_DB);
	roce_set_field(sq_db.byte_4, DB_BYTE_4_TAG_M, DB_BYTE_4_TAG_S, qpn);

	roce_set_field(sq_db.parameter, DB_PARAM_SQ_PRODUCER_IDX_M,
		       DB_PARAM_SQ_PRODUCER_IDX_S, sq_head);
	roce_set_field(sq_db.parameter, DB_PARAM_SL_M, DB_PARAM_SL_S, sl);

	hns_roce_write64(ctx->uar + ROCEE_VF_DB_CFG0_OFFSET, (__le32 *)&sq_db);
}

static void hns_roce_v2_update_cq_cons_index(struct hns_roce_context *ctx,
					     struct hns_roce_cq *cq)
{
	struct hns_roce_db cq_db = {};

	roce_set_field(cq_db.byte_4, DB_BYTE_4_TAG_M, DB_BYTE_4_TAG_S, cq->cqn);
	roce_set_field(cq_db.byte_4, DB_BYTE_4_CMD_M, DB_BYTE_4_CMD_S,
		       HNS_ROCE_V2_CQ_DB_PTR);

	roce_set_field(cq_db.parameter, DB_PARAM_CQ_CONSUMER_IDX_M,
		       DB_PARAM_CQ_CONSUMER_IDX_S,
		       cq->cons_index & ((cq->cq_depth << 1) - 1));
	roce_set_field(cq_db.parameter, DB_PARAM_CQ_CMD_SN_M,
		       DB_PARAM_CQ_CMD_SN_S, 1);
	roce_set_bit(cq_db.parameter, DB_PARAM_CQ_NOTIFY_S, 0);

	hns_roce_write64(ctx->uar + ROCEE_VF_DB_CFG0_OFFSET, (__le32 *)&cq_db);
}

static struct hns_roce_qp *hns_roce_v2_find_qp(struct hns_roce_context *ctx,
					       uint32_t qpn)
{
	uint32_t tind = to_hr_qp_table_index(qpn, ctx);

	if (ctx->qp_table[tind].refcnt)
		return ctx->qp_table[tind].table[qpn & ctx->qp_table_mask];
	else
		return NULL;
}

static void hns_roce_v2_clear_qp(struct hns_roce_context *ctx, uint32_t qpn)
{
	uint32_t tind = to_hr_qp_table_index(qpn, ctx);

	if (!--ctx->qp_table[tind].refcnt)
		free(ctx->qp_table[tind].table);
	else
		ctx->qp_table[tind].table[qpn & ctx->qp_table_mask] = NULL;
}

static int hns_roce_u_v2_modify_qp(struct ibv_qp *qp, struct ibv_qp_attr *attr,
				   int attr_mask);

static int hns_roce_flush_cqe(struct hns_roce_qp **cur_qp, struct ibv_wc *wc)
{
	struct ibv_qp_attr attr;
	int attr_mask;
	int ret;

	if ((wc->status != IBV_WC_SUCCESS) &&
	    (wc->status != IBV_WC_WR_FLUSH_ERR)) {
		attr_mask = IBV_QP_STATE;
		attr.qp_state = IBV_QPS_ERR;
		ret = hns_roce_u_v2_modify_qp(&(*cur_qp)->ibv_qp,
						      &attr, attr_mask);
		if (ret)
			fprintf(stderr, PFX "failed to modify qp!\n");

		(*cur_qp)->ibv_qp.state = IBV_QPS_ERR;
	}

	return V2_CQ_OK;
}

static void hns_roce_v2_get_opcode_from_sender(struct hns_roce_v2_cqe *cqe,
					       struct ibv_wc *wc)
{
	/* Get opcode and flag before update the tail point for send */
	switch (roce_get_field(cqe->byte_4, CQE_BYTE_4_OPCODE_M,
		CQE_BYTE_4_OPCODE_S) & HNS_ROCE_V2_CQE_OPCODE_MASK) {
	case HNS_ROCE_SQ_OP_SEND:
		wc->opcode = IBV_WC_SEND;
		wc->wc_flags = 0;
		break;
	case HNS_ROCE_SQ_OP_SEND_WITH_IMM:
		wc->opcode = IBV_WC_SEND;
		wc->wc_flags = IBV_WC_WITH_IMM;
		break;
	case HNS_ROCE_SQ_OP_SEND_WITH_INV:
		wc->opcode = IBV_WC_SEND;
		break;
	case HNS_ROCE_SQ_OP_RDMA_READ:
		wc->opcode = IBV_WC_RDMA_READ;
		wc->byte_len = le32toh(cqe->byte_cnt);
		wc->wc_flags = 0;
		break;
	case HNS_ROCE_SQ_OP_RDMA_WRITE:
		wc->opcode = IBV_WC_RDMA_WRITE;
		wc->wc_flags = 0;
		break;

	case HNS_ROCE_SQ_OP_RDMA_WRITE_WITH_IMM:
		wc->opcode = IBV_WC_RDMA_WRITE;
		wc->wc_flags = IBV_WC_WITH_IMM;
		break;
	case HNS_ROCE_SQ_OP_LOCAL_INV:
		wc->opcode = IBV_WC_LOCAL_INV;
		wc->wc_flags = IBV_WC_WITH_INV;
		break;
	case HNS_ROCE_SQ_OP_ATOMIC_COMP_AND_SWAP:
		wc->opcode = IBV_WC_COMP_SWAP;
		wc->byte_len = le32toh(cqe->byte_cnt);
		wc->wc_flags = 0;
		break;
	case HNS_ROCE_SQ_OP_ATOMIC_FETCH_AND_ADD:
		wc->opcode = IBV_WC_FETCH_ADD;
		wc->byte_len = le32toh(cqe->byte_cnt);
		wc->wc_flags = 0;
		break;
	case HNS_ROCE_SQ_OP_BIND_MW:
		wc->opcode = IBV_WC_BIND_MW;
		wc->wc_flags = 0;
		break;
	default:
		wc->status = IBV_WC_GENERAL_ERR;
		wc->wc_flags = 0;
		break;
	}
}

static void hns_roce_v2_get_opcode_from_receiver(struct hns_roce_v2_cqe *cqe,
						 struct ibv_wc *wc,
						 uint32_t opcode)
{
	switch (opcode) {
	case HNS_ROCE_RECV_OP_RDMA_WRITE_IMM:
		wc->opcode = IBV_WC_RECV_RDMA_WITH_IMM;
		wc->wc_flags = IBV_WC_WITH_IMM;
		wc->imm_data = htobe32(le32toh(cqe->immtdata));
		break;
	case HNS_ROCE_RECV_OP_SEND:
		wc->opcode = IBV_WC_RECV;
		wc->wc_flags = 0;
		break;
	case HNS_ROCE_RECV_OP_SEND_WITH_IMM:
		wc->opcode = IBV_WC_RECV;
		wc->wc_flags = IBV_WC_WITH_IMM;
		wc->imm_data = htobe32(le32toh(cqe->immtdata));
		break;
	case HNS_ROCE_RECV_OP_SEND_WITH_INV:
		wc->opcode = IBV_WC_RECV;
		wc->wc_flags = IBV_WC_WITH_INV;
		wc->invalidated_rkey = le32toh(cqe->rkey);
		break;
	default:
		wc->status = IBV_WC_GENERAL_ERR;
		break;
	}
}

static int hns_roce_handle_recv_inl_wqe(struct hns_roce_v2_cqe *cqe,
					struct hns_roce_qp **cur_qp,
					struct ibv_wc *wc, uint32_t opcode)
{
	if (((*cur_qp)->ibv_qp.qp_type == IBV_QPT_RC ||
	    (*cur_qp)->ibv_qp.qp_type == IBV_QPT_UC) &&
	    (opcode == HNS_ROCE_RECV_OP_SEND ||
	     opcode == HNS_ROCE_RECV_OP_SEND_WITH_IMM ||
	     opcode == HNS_ROCE_RECV_OP_SEND_WITH_INV) &&
	     (roce_get_bit(cqe->byte_4, CQE_BYTE_4_RQ_INLINE_S))) {
		struct hns_roce_rinl_sge *sge_list;
		uint32_t wr_num, wr_cnt, sge_num, data_len;
		uint8_t *wqe_buf;
		uint32_t sge_cnt, size;

		wr_num = (uint16_t)roce_get_field(cqe->byte_4,
						CQE_BYTE_4_WQE_IDX_M,
						CQE_BYTE_4_WQE_IDX_S) & 0xffff;
		wr_cnt = wr_num & ((*cur_qp)->rq.wqe_cnt - 1);

		sge_list = (*cur_qp)->rq_rinl_buf.wqe_list[wr_cnt].sg_list;
		sge_num = (*cur_qp)->rq_rinl_buf.wqe_list[wr_cnt].sge_cnt;
		wqe_buf = (uint8_t *)get_recv_wqe_v2(*cur_qp, wr_cnt);
		if (!wqe_buf)
			return V2_CQ_POLL_ERR;

		data_len = wc->byte_len;

		for (sge_cnt = 0; (sge_cnt < sge_num) && (data_len);
		     sge_cnt++) {
			size = sge_list[sge_cnt].len < data_len ?
			       sge_list[sge_cnt].len : data_len;

			memcpy((void *)sge_list[sge_cnt].addr,
				(void *)wqe_buf, size);
			data_len -= size;
			wqe_buf += size;
		}

		if (data_len) {
			wc->status = IBV_WC_LOC_LEN_ERR;
			return V2_CQ_POLL_ERR;
		}
	}

	return V2_CQ_OK;
}

static int hns_roce_v2_poll_one(struct hns_roce_cq *cq,
				struct hns_roce_qp **cur_qp, struct ibv_wc *wc)
{
	uint32_t qpn;
	int is_send;
	uint16_t wqe_ctr;
	struct hns_roce_wq *wq = NULL;
	struct hns_roce_v2_cqe *cqe;
	struct hns_roce_srq *srq;
	uint32_t opcode;
	int ret;

	/* According to CI, find the relative cqe */
	cqe = next_cqe_sw_v2(cq);
	if (!cqe)
		return V2_CQ_EMPTY;

	/* Get the next cqe, CI will be added gradually */
	++cq->cons_index;

	udma_from_device_barrier();

	qpn = roce_get_field(cqe->byte_16, CQE_BYTE_16_LCL_QPN_M,
			     CQE_BYTE_16_LCL_QPN_S);

	is_send = (roce_get_bit(cqe->byte_4, CQE_BYTE_4_S_R_S) ==
		   HNS_ROCE_V2_CQE_IS_SQ);

	/* if qp is zero, it will not get the correct qpn */
	if (!*cur_qp || qpn != (*cur_qp)->ibv_qp.qp_num) {
		*cur_qp = hns_roce_v2_find_qp(to_hr_ctx(cq->ibv_cq.context),
					      qpn);
		if (!*cur_qp) {
			fprintf(stderr, PFX "can't find qp!\n");
			return V2_CQ_POLL_ERR;
		}
	}
	wc->qp_num = qpn;

	srq = (*cur_qp)->ibv_qp.srq ? to_hr_srq((*cur_qp)->ibv_qp.srq) : NULL;
	if (is_send) {
		wq = &(*cur_qp)->sq;
		/*
		 * if sq_signal_bits is 1, the tail pointer first update to
		 * the wqe corresponding the current cqe
		 */
		if ((*cur_qp)->sq_signal_bits) {
			wqe_ctr = (uint16_t)(roce_get_field(cqe->byte_4,
						CQE_BYTE_4_WQE_IDX_M,
						CQE_BYTE_4_WQE_IDX_S));
			/*
			 * wq->tail will plus a positive number every time,
			 * when wq->tail exceeds 32b, it is 0 and acc
			 */
			wq->tail += (wqe_ctr - (uint16_t) wq->tail) &
				    (wq->wqe_cnt - 1);
		}
		/* write the wr_id of wq into the wc */
		wc->wr_id = wq->wrid[wq->tail & (wq->wqe_cnt - 1)];
		++wq->tail;
	} else if (srq) {
		wqe_ctr = (uint16_t)(roce_get_field(cqe->byte_4,
						    CQE_BYTE_4_WQE_IDX_M,
						    CQE_BYTE_4_WQE_IDX_S));
		wc->wr_id = srq->wrid[wqe_ctr & (srq->max_wqe - 1)];
		hns_roce_free_srq_wqe(srq, wqe_ctr);
	} else {
		wq = &(*cur_qp)->rq;
		wc->wr_id = wq->wrid[wq->tail & (wq->wqe_cnt - 1)];
		++wq->tail;
	}

	/*
	 * HW maintains wc status, set the err type and directly return, after
	 * generated the incorrect CQE
	 */
	if (roce_get_field(cqe->byte_4, CQE_BYTE_4_STATUS_M,
			   CQE_BYTE_4_STATUS_S) != HNS_ROCE_V2_CQE_SUCCESS) {
		hns_roce_v2_handle_error_cqe(cqe, wc);
		return hns_roce_flush_cqe(cur_qp, wc);
	}

	wc->status = IBV_WC_SUCCESS;

	/*
	 * According to the opcode type of cqe, mark the opcode and other
	 * information of wc
	 */
	if (is_send) {
		hns_roce_v2_get_opcode_from_sender(cqe, wc);
	} else {
		/* Get opcode and flag in rq&srq */
		wc->byte_len = le32toh(cqe->byte_cnt);
		opcode = roce_get_field(cqe->byte_4, CQE_BYTE_4_OPCODE_M,
			 CQE_BYTE_4_OPCODE_S) & HNS_ROCE_V2_CQE_OPCODE_MASK;
		hns_roce_v2_get_opcode_from_receiver(cqe, wc, opcode);

		ret = hns_roce_handle_recv_inl_wqe(cqe, cur_qp, wc, opcode);
		if (ret) {
			fprintf(stderr,
				PFX "failed to handle recv inline wqe!\n");
			return ret;
		}

		wc->sl = (uint8_t)roce_get_field(cqe->byte_32, CQE_BYTE_32_SL_M,
						 CQE_BYTE_32_SL_S);
		wc->src_qp = roce_get_field(cqe->byte_32, CQE_BYTE_32_RMT_QPN_M,
					    CQE_BYTE_32_RMT_QPN_S);
		wc->slid = 0;
		wc->wc_flags |= roce_get_bit(cqe->byte_32, CQE_BYTE_32_GRH_S) ?
				IBV_WC_GRH : 0;
		wc->pkey_index = 0;
	}

	return V2_CQ_OK;
}

static int hns_roce_u_v2_poll_cq(struct ibv_cq *ibvcq, int ne,
				 struct ibv_wc *wc)
{
	int npolled;
	int err = V2_CQ_OK;
	struct hns_roce_qp *qp = NULL;
	struct hns_roce_cq *cq = to_hr_cq(ibvcq);
	struct hns_roce_context *ctx = to_hr_ctx(ibvcq->context);

	pthread_spin_lock(&cq->lock);

	for (npolled = 0; npolled < ne; ++npolled) {
		err = hns_roce_v2_poll_one(cq, &qp, wc + npolled);
		if (err != V2_CQ_OK)
			break;
	}

	if (npolled || err == V2_CQ_POLL_ERR) {
		mmio_ordered_writes_hack();

		if (cq->flags & HNS_ROCE_SUPPORT_CQ_RECORD_DB)
			*cq->set_ci_db = (unsigned int)(cq->cons_index &
						((cq->cq_depth << 1) - 1));
		else
			hns_roce_v2_update_cq_cons_index(ctx, cq);
	}

	pthread_spin_unlock(&cq->lock);

	return err == V2_CQ_POLL_ERR ? err : npolled;
}

static int hns_roce_u_v2_arm_cq(struct ibv_cq *ibvcq, int solicited)
{
	struct hns_roce_context *ctx = to_hr_ctx(ibvcq->context);
	struct hns_roce_cq *cq = to_hr_cq(ibvcq);
	struct hns_roce_db cq_db = {};
	uint32_t solicited_flag;
	uint32_t cmd_sn;
	uint32_t ci;

	ci  = cq->cons_index & ((cq->cq_depth << 1) - 1);
	cmd_sn = cq->arm_sn & HNS_ROCE_CMDSN_MASK;
	solicited_flag = solicited ? HNS_ROCE_V2_CQ_DB_REQ_SOL :
				     HNS_ROCE_V2_CQ_DB_REQ_NEXT;

	roce_set_field(cq_db.byte_4, DB_BYTE_4_TAG_M, DB_BYTE_4_TAG_S, cq->cqn);
	roce_set_field(cq_db.byte_4, DB_BYTE_4_CMD_M, DB_BYTE_4_CMD_S,
		       HNS_ROCE_V2_CQ_DB_NTR);

	roce_set_field(cq_db.parameter, DB_PARAM_CQ_CONSUMER_IDX_M,
		       DB_PARAM_CQ_CONSUMER_IDX_S, ci);

	roce_set_field(cq_db.parameter, DB_PARAM_CQ_CMD_SN_M,
		       DB_PARAM_CQ_CMD_SN_S, cmd_sn);
	roce_set_bit(cq_db.parameter, DB_PARAM_CQ_NOTIFY_S, solicited_flag);

	hns_roce_write64(ctx->uar + ROCEE_VF_DB_CFG0_OFFSET, (__le32 *)&cq_db);

	return 0;
}

static void set_sge(struct hns_roce_v2_wqe_data_seg *dseg,
		    struct hns_roce_qp *qp, struct ibv_send_wr *wr,
		    struct hns_roce_sge_info *sge_info)
{
	int i;

	sge_info->valid_num = 0;
	sge_info->total_len = 0;

	for (i = 0; i < wr->num_sge; i++) {
		if (unlikely(!wr->sg_list[i].length))
			continue;

		sge_info->total_len += wr->sg_list[i].length;
		sge_info->valid_num++;

		/* No inner sge in UD wqe */
		if (sge_info->valid_num <= HNS_ROCE_SGE_IN_WQE &&
		    qp->ibv_qp.qp_type != IBV_QPT_UD) {
			set_data_seg_v2(dseg, wr->sg_list + i);
			dseg++;
		} else {
			dseg = get_send_sge_ex(qp, sge_info->start_idx &
					       (qp->ex_sge.sge_cnt - 1));
			set_data_seg_v2(dseg, wr->sg_list + i);
			sge_info->start_idx++;
		}
	}
}

static int set_rc_wqe(void *wqe, struct hns_roce_qp *qp, struct ibv_send_wr *wr,
		      int nreq, struct hns_roce_sge_info *sge_info)
{
	struct hns_roce_rc_sq_wqe *rc_sq_wqe = wqe;
	struct hns_roce_v2_wqe_data_seg *dseg;
	int hr_op;
	int i;

	memset(rc_sq_wqe, 0, sizeof(struct hns_roce_rc_sq_wqe));

	switch (wr->opcode) {
	case IBV_WR_RDMA_READ:
		hr_op = HNS_ROCE_WQE_OP_RDMA_READ;
		rc_sq_wqe->va = htole64(wr->wr.rdma.remote_addr);
		rc_sq_wqe->rkey = htole32(wr->wr.rdma.rkey);
		break;
	case IBV_WR_RDMA_WRITE:
		hr_op = HNS_ROCE_WQE_OP_RDMA_WRITE;
		rc_sq_wqe->va = htole64(wr->wr.rdma.remote_addr);
		rc_sq_wqe->rkey = htole32(wr->wr.rdma.rkey);
		break;
	case IBV_WR_RDMA_WRITE_WITH_IMM:
		hr_op = HNS_ROCE_WQE_OP_RDMA_WRITE_WITH_IMM;
		rc_sq_wqe->va = htole64(wr->wr.rdma.remote_addr);
		rc_sq_wqe->rkey = htole32(wr->wr.rdma.rkey);
		rc_sq_wqe->immtdata = htole32(be32toh(wr->imm_data));
		break;
	case IBV_WR_SEND:
		hr_op = HNS_ROCE_WQE_OP_SEND;
		break;
	case IBV_WR_SEND_WITH_INV:
		hr_op = HNS_ROCE_WQE_OP_SEND_WITH_INV;
		rc_sq_wqe->inv_key = htole32(wr->invalidate_rkey);
		break;
	case IBV_WR_SEND_WITH_IMM:
		hr_op = HNS_ROCE_WQE_OP_SEND_WITH_IMM;
		rc_sq_wqe->immtdata = htole32(be32toh(wr->imm_data));
		break;
	case IBV_WR_LOCAL_INV:
		hr_op = HNS_ROCE_WQE_OP_LOCAL_INV;
		roce_set_bit(rc_sq_wqe->byte_4, RC_SQ_WQE_BYTE_4_SO_S, 1);
		rc_sq_wqe->inv_key = htole32(wr->invalidate_rkey);
		break;
	case IBV_WR_BIND_MW:
		hr_op = HNS_ROCE_WQE_OP_BIND_MW_TYPE;
		roce_set_bit(rc_sq_wqe->byte_4, RC_SQ_WQE_BYTE_4_MW_TYPE_S,
			     wr->bind_mw.mw->type - 1);
		roce_set_bit(rc_sq_wqe->byte_4, RC_SQ_WQE_BYTE_4_ATOMIC_S,
			     (wr->bind_mw.bind_info.mw_access_flags &
			     IBV_ACCESS_REMOTE_ATOMIC) ? 1 : 0);
		roce_set_bit(rc_sq_wqe->byte_4, RC_SQ_WQE_BYTE_4_RDMA_READ_S,
			     (wr->bind_mw.bind_info.mw_access_flags &
			     IBV_ACCESS_REMOTE_READ) ? 1 : 0);
		roce_set_bit(rc_sq_wqe->byte_4, RC_SQ_WQE_BYTE_4_RDMA_WRITE_S,
			     (wr->bind_mw.bind_info.mw_access_flags &
			     IBV_ACCESS_REMOTE_WRITE) ? 1 : 0);
		rc_sq_wqe->new_rkey = htole32(wr->bind_mw.rkey);
		rc_sq_wqe->byte_16 = htole32(wr->bind_mw.bind_info.length &
					     HNS_ROCE_ADDRESS_MASK);
		rc_sq_wqe->byte_20 = htole32(wr->bind_mw.bind_info.length >>
					     HNS_ROCE_ADDRESS_SHIFT);
		rc_sq_wqe->rkey = htole32(wr->bind_mw.bind_info.mr->rkey);
		rc_sq_wqe->va = htole64(wr->bind_mw.bind_info.addr);
		break;
	case IBV_WR_ATOMIC_CMP_AND_SWP:
		hr_op = HNS_ROCE_WQE_OP_ATOMIC_COM_AND_SWAP;
		rc_sq_wqe->rkey = htole32(wr->wr.atomic.rkey);
		rc_sq_wqe->va = htole64(wr->wr.atomic.remote_addr);
		roce_set_field(rc_sq_wqe->byte_16, RC_SQ_WQE_BYTE_16_SGE_NUM_M,
			       RC_SQ_WQE_BYTE_16_SGE_NUM_S,
			       sge_info->valid_num);
		break;
	case IBV_WR_ATOMIC_FETCH_AND_ADD:
		hr_op = HNS_ROCE_WQE_OP_ATOMIC_FETCH_AND_ADD;
		rc_sq_wqe->rkey = htole32(wr->wr.atomic.rkey);
		rc_sq_wqe->va = htole64(wr->wr.atomic.remote_addr);
		roce_set_field(rc_sq_wqe->byte_16, RC_SQ_WQE_BYTE_16_SGE_NUM_M,
			       RC_SQ_WQE_BYTE_16_SGE_NUM_S,
			       sge_info->valid_num);
		break;
	default:
		hr_op = HNS_ROCE_WQE_OP_MASK;
		return -EINVAL;
	}

	roce_set_field(rc_sq_wqe->byte_4, RC_SQ_WQE_BYTE_4_OPCODE_M,
		       RC_SQ_WQE_BYTE_4_OPCODE_S, hr_op);

	roce_set_bit(rc_sq_wqe->byte_4, RC_SQ_WQE_BYTE_4_CQE_S,
		     (wr->send_flags & IBV_SEND_SIGNALED) ? 1 : 0);

	roce_set_bit(rc_sq_wqe->byte_4, RC_SQ_WQE_BYTE_4_FENCE_S,
		     (wr->send_flags & IBV_SEND_FENCE) ? 1 : 0);

	roce_set_bit(rc_sq_wqe->byte_4, RC_SQ_WQE_BYTE_4_SE_S,
		     (wr->send_flags & IBV_SEND_SOLICITED) ? 1 : 0);

	roce_set_bit(rc_sq_wqe->byte_4, RC_SQ_WQE_BYTE_4_OWNER_S,
		     ~(((qp->sq.head + nreq) >> qp->sq.shift) & 0x1));

	roce_set_field(rc_sq_wqe->byte_20,
		       RC_SQ_WQE_BYTE_20_MSG_START_SGE_IDX_M,
		       RC_SQ_WQE_BYTE_20_MSG_START_SGE_IDX_S,
		       sge_info->start_idx & (qp->ex_sge.sge_cnt - 1));

	if (wr->opcode == IBV_WR_BIND_MW)
		return 0;

	wqe += sizeof(struct hns_roce_rc_sq_wqe);
	dseg = wqe;

	set_sge(dseg, qp, wr, sge_info);

	rc_sq_wqe->msg_len = htole32(sge_info->total_len);

	roce_set_field(rc_sq_wqe->byte_16, RC_SQ_WQE_BYTE_16_SGE_NUM_M,
		       RC_SQ_WQE_BYTE_16_SGE_NUM_S, sge_info->valid_num);

	if (wr->opcode == IBV_WR_ATOMIC_FETCH_AND_ADD ||
	    wr->opcode == IBV_WR_ATOMIC_CMP_AND_SWP) {
		dseg++;
		return set_atomic_seg(qp, wr, le32toh(rc_sq_wqe->msg_len),
				      dseg, sge_info);
	}

	if (wr->send_flags & IBV_SEND_INLINE) {
		if (wr->opcode == IBV_WR_RDMA_READ)
			return -EINVAL;

		if (sge_info->total_len > qp->max_inline_data)
			return -EINVAL;

		for (i = 0; i < wr->num_sge; i++) {
			memcpy(dseg, (void *)(uintptr_t)(wr->sg_list[i].addr),
			       wr->sg_list[i].length);
			dseg += wr->sg_list[i].length;
		}
		roce_set_bit(rc_sq_wqe->byte_4, RC_SQ_WQE_BYTE_4_INLINE_S, 1);
	}

	return 0;
}

int hns_roce_u_v2_post_send(struct ibv_qp *ibvqp, struct ibv_send_wr *wr,
			    struct ibv_send_wr **bad_wr)
{
	struct hns_roce_context *ctx = to_hr_ctx(ibvqp->context);
	struct hns_roce_qp *qp = to_hr_qp(ibvqp);
	struct hns_roce_sge_info sge_info = {};
	struct ibv_qp_attr attr;
	unsigned int wqe_idx;
	int attr_mask;
	int ret = 0;
	void *wqe;
	int nreq;

	pthread_spin_lock(&qp->sq.lock);

	/* check that state is OK to post send */
	if (ibvqp->state == IBV_QPS_RESET || ibvqp->state == IBV_QPS_INIT ||
	    ibvqp->state == IBV_QPS_RTR) {
		pthread_spin_unlock(&qp->sq.lock);
		*bad_wr = wr;
		return EINVAL;
	}

	sge_info.start_idx = qp->next_sge; /* start index of extend sge */

	for (nreq = 0; wr; ++nreq, wr = wr->next) {
		if (hns_roce_v2_wq_overflow(&qp->sq, nreq,
					    to_hr_cq(qp->ibv_qp.send_cq))) {
			ret = ENOMEM;
			*bad_wr = wr;
			goto out;
		}

		if (wr->num_sge > qp->sq.max_gs) {
			ret = EINVAL;
			*bad_wr = wr;
			goto out;
		}

		wqe_idx = (qp->sq.head + nreq) & (qp->sq.wqe_cnt - 1);
		wqe = get_send_wqe(qp, wqe_idx);
		qp->sq.wrid[wqe_idx] = wr->wr_id;

		switch (ibvqp->qp_type) {
		case IBV_QPT_RC:
			ret = set_rc_wqe(wqe, qp, wr, nreq, &sge_info);
			if (ret) {
				*bad_wr = wr;
				goto out;
			}
			break;
		case IBV_QPT_UC:
		case IBV_QPT_UD:
		default:
			ret = -EINVAL;
			*bad_wr = wr;
			goto out;
		}
	}

out:
	if (likely(nreq)) {
		qp->sq.head += nreq;
		qp->next_sge = sge_info.start_idx;

		udma_to_device_barrier();

		hns_roce_update_sq_db(ctx, qp->ibv_qp.qp_num, qp->sl,
				     qp->sq.head & ((qp->sq.wqe_cnt << 1) - 1));

		if (qp->flags & HNS_ROCE_SUPPORT_SQ_RECORD_DB)
			*(qp->sdb) = qp->sq.head & 0xffff;
	}

	pthread_spin_unlock(&qp->sq.lock);

	if (ibvqp->state == IBV_QPS_ERR) {
		attr_mask = IBV_QP_STATE;
		attr.qp_state = IBV_QPS_ERR;

		hns_roce_u_v2_modify_qp(ibvqp, &attr, attr_mask);
	}

	return ret;
}

static int hns_roce_u_v2_post_recv(struct ibv_qp *ibvqp, struct ibv_recv_wr *wr,
				   struct ibv_recv_wr **bad_wr)
{
	struct hns_roce_qp *qp = to_hr_qp(ibvqp);
	struct hns_roce_context *ctx = to_hr_ctx(ibvqp->context);
	struct hns_roce_v2_wqe_data_seg *dseg;
	struct hns_roce_rinl_sge *sge_list;
	struct ibv_qp_attr attr;
	int attr_mask;
	int ret = 0;
	int wqe_idx;
	void *wqe;
	int nreq;
	int i;

	pthread_spin_lock(&qp->rq.lock);

	/* check that state is OK to post receive */
	if (ibvqp->state == IBV_QPS_RESET) {
		pthread_spin_unlock(&qp->rq.lock);
		*bad_wr = wr;
		return -1;
	}

	for (nreq = 0; wr; ++nreq, wr = wr->next) {
		if (hns_roce_v2_wq_overflow(&qp->rq, nreq,
					    to_hr_cq(qp->ibv_qp.recv_cq))) {
			ret = -ENOMEM;
			*bad_wr = wr;
			goto out;
		}

		wqe_idx = (qp->rq.head + nreq) & (qp->rq.wqe_cnt - 1);

		if (wr->num_sge > qp->rq.max_gs) {
			ret = -EINVAL;
			*bad_wr = wr;
			goto out;
		}

		wqe = get_recv_wqe_v2(qp, wqe_idx);
		if (!wqe) {
			ret = -EINVAL;
			*bad_wr = wr;
			goto out;
		}

		dseg = (struct hns_roce_v2_wqe_data_seg *)wqe;

		for (i = 0; i < wr->num_sge; i++) {
			if (!wr->sg_list[i].length)
				continue;
			set_data_seg_v2(dseg, wr->sg_list + i);
			dseg++;
		}

		/* hw stop reading when identify the last one */
		if (i < qp->rq.max_gs) {
			dseg->lkey = htole32(0x100);
			dseg->addr = 0;
		}

		/* QP support receive inline wqe */
		sge_list = qp->rq_rinl_buf.wqe_list[wqe_idx].sg_list;
		qp->rq_rinl_buf.wqe_list[wqe_idx].sge_cnt =
						(unsigned int)wr->num_sge;

		for (i = 0; i < wr->num_sge; i++) {
			sge_list[i].addr =
					(void *)(uintptr_t)wr->sg_list[i].addr;
			sge_list[i].len = wr->sg_list[i].length;
		}

		qp->rq.wrid[wqe_idx] = wr->wr_id;
	}

out:
	if (nreq) {
		qp->rq.head += nreq;

		udma_to_device_barrier();

		if (qp->flags & HNS_ROCE_SUPPORT_RQ_RECORD_DB)
			*qp->rdb = qp->rq.head & 0xffff;
		else
			hns_roce_update_rq_db(ctx, qp->ibv_qp.qp_num,
				     qp->rq.head & ((qp->rq.wqe_cnt << 1) - 1));
	}

	pthread_spin_unlock(&qp->rq.lock);

	if (ibvqp->state == IBV_QPS_ERR) {
		attr_mask = IBV_QP_STATE;
		attr.qp_state = IBV_QPS_ERR;

		hns_roce_u_v2_modify_qp(ibvqp, &attr, attr_mask);
	}

	return ret;
}

static void __hns_roce_v2_cq_clean(struct hns_roce_cq *cq, uint32_t qpn,
				   struct hns_roce_srq *srq)
{
	int nfreed = 0;
	bool is_recv_cqe;
	uint16_t wqe_index;
	uint32_t prod_index;
	uint8_t owner_bit = 0;
	struct hns_roce_v2_cqe *cqe, *dest;
	struct hns_roce_context *ctx = to_hr_ctx(cq->ibv_cq.context);

	for (prod_index = cq->cons_index; get_sw_cqe_v2(cq, prod_index);
	     ++prod_index)
		if (prod_index > cq->cons_index + cq->ibv_cq.cqe)
			break;

	while ((int) --prod_index - (int) cq->cons_index >= 0) {
		cqe = get_cqe_v2(cq, prod_index & cq->ibv_cq.cqe);
		if ((roce_get_field(cqe->byte_16, CQE_BYTE_16_LCL_QPN_M,
			      CQE_BYTE_16_LCL_QPN_S) & 0xffffff) == qpn) {
			is_recv_cqe = roce_get_bit(cqe->byte_4,
						   CQE_BYTE_4_S_R_S);

			if (srq && is_recv_cqe) {
				wqe_index = roce_get_field(cqe->byte_4,
						CQE_BYTE_4_WQE_IDX_M,
						CQE_BYTE_4_WQE_IDX_S);
				hns_roce_free_srq_wqe(srq, wqe_index);
			}
			++nfreed;
		} else if (nfreed) {
			dest = get_cqe_v2(cq,
				       (prod_index + nfreed) & cq->ibv_cq.cqe);
			owner_bit = roce_get_bit(dest->byte_4,
						 CQE_BYTE_4_OWNER_S);
			memcpy(dest, cqe, sizeof(*cqe));
			roce_set_bit(dest->byte_4, CQE_BYTE_4_OWNER_S,
				     owner_bit);
		}
	}

	if (nfreed) {
		cq->cons_index += nfreed;
		udma_to_device_barrier();
		hns_roce_v2_update_cq_cons_index(ctx, cq);
	}
}

static void hns_roce_v2_cq_clean(struct hns_roce_cq *cq, unsigned int qpn,
				 struct hns_roce_srq *srq)
{
	pthread_spin_lock(&cq->lock);
	__hns_roce_v2_cq_clean(cq, qpn, srq);
	pthread_spin_unlock(&cq->lock);
}

static int hns_roce_u_v2_modify_qp(struct ibv_qp *qp, struct ibv_qp_attr *attr,
				   int attr_mask)
{
	int ret;
	struct ibv_modify_qp cmd;
	struct hns_roce_qp *hr_qp = to_hr_qp(qp);
	bool flag = false; /* modify qp to error */

	if ((attr_mask & IBV_QP_STATE) && (attr->qp_state == IBV_QPS_ERR)) {
		pthread_spin_lock(&hr_qp->sq.lock);
		pthread_spin_lock(&hr_qp->rq.lock);
		flag = true;
	}

	ret = ibv_cmd_modify_qp(qp, attr, attr_mask, &cmd, sizeof(cmd));

	if (flag) {
		pthread_spin_unlock(&hr_qp->rq.lock);
		pthread_spin_unlock(&hr_qp->sq.lock);
	}

	if (ret)
		return ret;

	if (attr_mask & IBV_QP_STATE)
		qp->state = attr->qp_state;

	if ((attr_mask & IBV_QP_STATE) && attr->qp_state == IBV_QPS_RESET) {
		hns_roce_v2_cq_clean(to_hr_cq(qp->recv_cq), qp->qp_num,
				     qp->srq ? to_hr_srq(qp->srq) : NULL);
		if (qp->send_cq != qp->recv_cq)
			hns_roce_v2_cq_clean(to_hr_cq(qp->send_cq), qp->qp_num,
					     NULL);

		hns_roce_init_qp_indices(to_hr_qp(qp));
	}

	if (attr_mask & IBV_QP_PORT)
		hr_qp->port_num = attr->port_num;

	if (attr_mask & IBV_QP_AV)
		hr_qp->sl = attr->ah_attr.sl;

	return ret;
}

static void hns_roce_lock_cqs(struct ibv_qp *qp)
{
	struct hns_roce_cq *send_cq = to_hr_cq(qp->send_cq);
	struct hns_roce_cq *recv_cq = to_hr_cq(qp->recv_cq);

	if (send_cq && recv_cq) {
		if (send_cq == recv_cq) {
			pthread_spin_lock(&send_cq->lock);
		} else if (send_cq->cqn < recv_cq->cqn) {
			pthread_spin_lock(&send_cq->lock);
			pthread_spin_lock(&recv_cq->lock);
		} else {
			pthread_spin_lock(&recv_cq->lock);
			pthread_spin_lock(&send_cq->lock);
		}
	} else if (send_cq) {
		pthread_spin_lock(&send_cq->lock);
	} else if (recv_cq) {
		pthread_spin_lock(&recv_cq->lock);
	}
}

static void hns_roce_unlock_cqs(struct ibv_qp *qp)
{
	struct hns_roce_cq *send_cq = to_hr_cq(qp->send_cq);
	struct hns_roce_cq *recv_cq = to_hr_cq(qp->recv_cq);

	if (send_cq && recv_cq) {
		if (send_cq == recv_cq) {
			pthread_spin_unlock(&send_cq->lock);
		} else if (send_cq->cqn < recv_cq->cqn) {
			pthread_spin_unlock(&recv_cq->lock);
			pthread_spin_unlock(&send_cq->lock);
		} else {
			pthread_spin_unlock(&send_cq->lock);
			pthread_spin_unlock(&recv_cq->lock);
		}
	} else if (send_cq) {
		pthread_spin_unlock(&send_cq->lock);
	} else if (recv_cq) {
		pthread_spin_unlock(&recv_cq->lock);
	}
}

static int hns_roce_u_v2_destroy_qp(struct ibv_qp *ibqp)
{
	int ret;
	struct hns_roce_qp *qp = to_hr_qp(ibqp);

	pthread_mutex_lock(&to_hr_ctx(ibqp->context)->qp_table_mutex);
	ret = ibv_cmd_destroy_qp(ibqp);
	if (ret) {
		pthread_mutex_unlock(&to_hr_ctx(ibqp->context)->qp_table_mutex);
		return ret;
	}

	hns_roce_lock_cqs(ibqp);

	if (ibqp->recv_cq)
		__hns_roce_v2_cq_clean(to_hr_cq(ibqp->recv_cq), ibqp->qp_num,
				       ibqp->srq ? to_hr_srq(ibqp->srq) : NULL);

	if (ibqp->send_cq && ibqp->send_cq != ibqp->recv_cq)
		__hns_roce_v2_cq_clean(to_hr_cq(ibqp->send_cq), ibqp->qp_num,
				       NULL);

	hns_roce_v2_clear_qp(to_hr_ctx(ibqp->context), ibqp->qp_num);

	hns_roce_unlock_cqs(ibqp);
	pthread_mutex_unlock(&to_hr_ctx(ibqp->context)->qp_table_mutex);

	if (qp->rq.max_gs)
		hns_roce_free_db(to_hr_ctx(ibqp->context), qp->rdb,
				 HNS_ROCE_QP_TYPE_DB);
	if (qp->sq.wqe_cnt)
		hns_roce_free_db(to_hr_ctx(ibqp->context), qp->sdb,
				 HNS_ROCE_QP_TYPE_DB);

	hns_roce_free_buf(&qp->buf);
	if (qp->rq_rinl_buf.wqe_list) {
		if (qp->rq_rinl_buf.wqe_list[0].sg_list) {
			free(qp->rq_rinl_buf.wqe_list[0].sg_list);
			qp->rq_rinl_buf.wqe_list[0].sg_list = NULL;
		}

		free(qp->rq_rinl_buf.wqe_list);
		qp->rq_rinl_buf.wqe_list = NULL;
	}

	free(qp->sq.wrid);
	if (qp->rq.wqe_cnt)
		free(qp->rq.wrid);

	free(qp);

	return ret;
}

static void fill_idx_que(struct hns_roce_idx_que *idx_que,
			 int cur_idx, int wqe_idx)
{
	unsigned int *addr;

	addr = idx_que->buf.buf + cur_idx * idx_que->entry_sz;
	*addr = wqe_idx;
}

static int find_empty_entry(struct hns_roce_idx_que *idx_que)
{
	int bit_num;
	int i;

	/* bitmap[i] is set zero if all bits are allocated */
	for (i = 0; idx_que->bitmap[i] == 0; ++i)
		;
	bit_num = ffsl(idx_que->bitmap[i]);
	idx_que->bitmap[i] &= ~(1ULL << (bit_num - 1));

	return i * BIT_CNT_PER_U64 + (bit_num - 1);
}

static int hns_roce_u_v2_post_srq_recv(struct ibv_srq *ib_srq,
				       struct ibv_recv_wr *wr,
				       struct ibv_recv_wr **bad_wr)
{
	struct hns_roce_context *ctx = to_hr_ctx(ib_srq->context);
	struct hns_roce_srq *srq = to_hr_srq(ib_srq);
	struct hns_roce_v2_wqe_data_seg *dseg;
	struct hns_roce_db srq_db;
	int ret = 0;
	int wqe_idx;
	void *wqe;
	int nreq;
	int ind;
	int i;

	pthread_spin_lock(&srq->lock);

	/* current idx of srqwq */
	ind = srq->head & (srq->max_wqe - 1);

	for (nreq = 0; wr; ++nreq, wr = wr->next) {
		if (wr->num_sge > srq->max_gs) {
			ret = -1;
			*bad_wr = wr;
			break;
		}

		if (srq->head == srq->tail) {
			/* SRQ is full */
			ret = -1;
			*bad_wr = wr;
			break;
		}

		wqe_idx = find_empty_entry(&srq->idx_que);
		fill_idx_que(&srq->idx_que, ind, wqe_idx);

		wqe = get_srq_wqe(srq, wqe_idx);
		dseg = (struct hns_roce_v2_wqe_data_seg *)wqe;

		for (i = 0; i < wr->num_sge; ++i) {
			dseg[i].len = htole32(wr->sg_list[i].length);
			dseg[i].lkey = htole32(wr->sg_list[i].lkey);
			dseg[i].addr = htole64(wr->sg_list[i].addr);
		}

		/* hw stop reading when identify the last one */
		if (i < srq->max_gs) {
			dseg[i].len = 0;
			dseg[i].lkey = htole32(0x100);
			dseg[i].addr = 0;
		}

		srq->wrid[wqe_idx] = wr->wr_id;
		ind = (ind + 1) & (srq->max_wqe - 1);
	}

	if (nreq) {
		srq->head += nreq;

		/*
		 * Make sure that descriptors are written before
		 * we write doorbell record.
		 */
		udma_to_device_barrier();

		srq_db.byte_4 = htole32(HNS_ROCE_V2_SRQ_DB << DB_BYTE_4_CMD_S
					| srq->srqn);
		srq_db.parameter = htole32(srq->head);

		hns_roce_write64(ctx->uar + ROCEE_VF_DB_CFG0_OFFSET,
				 (__le32 *)&srq_db);
	}

	pthread_spin_unlock(&srq->lock);

	return ret;
}

const struct hns_roce_u_hw hns_roce_u_hw_v2 = {
	.hw_version = HNS_ROCE_HW_VER2,
	.hw_ops = {
		.poll_cq = hns_roce_u_v2_poll_cq,
		.req_notify_cq = hns_roce_u_v2_arm_cq,
		.post_send = hns_roce_u_v2_post_send,
		.post_recv = hns_roce_u_v2_post_recv,
		.modify_qp = hns_roce_u_v2_modify_qp,
		.destroy_qp = hns_roce_u_v2_destroy_qp,
		.post_srq_recv = hns_roce_u_v2_post_srq_recv,
	},
};
