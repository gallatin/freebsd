/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 2017-2018 Chelsio Communications, Inc.
 * All rights reserved.
 * Written by: John Baldwin <jhb@FreeBSD.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "opt_inet.h"
#include "opt_kern_tls.h"

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/sglist.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#ifdef KERN_TLS
#include <sys/protosw.h>
#include <sys/sockbuf_tls.h>
#endif
#include <sys/systm.h>
#include <netinet/in.h>
#include <netinet/in_pcb.h>
#ifdef KERN_TLS
#include <netinet/ip.h>
#endif
#include <netinet/tcp_var.h>
#include <netinet/toecore.h>
#ifdef KERN_TLS
#include <opencrypto/cryptodev.h>
#include <opencrypto/xform.h>
#include <vm/vm.h> /* XXX */
#include <vm/pmap.h> /* XXX */
#include <vm/vm_param.h> /* XXX */
#endif

#ifdef TCP_OFFLOAD
#include "common/common.h"
#include "common/t4_tcb.h"
#include "tom/t4_tom_l2t.h"
#include "tom/t4_tom.h"
#include "t4_mp_ring.h"
#ifdef KERN_TLS
#include "crypto/t4_crypto.h"
#endif

/*
 * The TCP sequence number of a CPL_TLS_DATA mbuf is saved here while
 * the mbuf is in the ulp_pdu_reclaimq.
 */
#define	tls_tcp_seq	PH_loc.thirtytwo[0]

/*
 * Handshake lock used for the handshake timer.  Having a global lock
 * is perhaps not ideal, but it avoids having to use callout_drain()
 * in tls_uninit_toep() which can't block.  Also, the timer shouldn't
 * actually fire for most connections.
 */
static struct mtx tls_handshake_lock;

static void
t4_set_tls_tcb_field(struct toepcb *toep, uint16_t word, uint64_t mask,
    uint64_t val)
{
	struct adapter *sc = td_adapter(toep->td);

	t4_set_tcb_field(sc, toep->ofld_txq, toep, word, mask, val, 0, 0);
}

/* TLS and DTLS common routines */
bool
can_tls_offload(struct adapter *sc)
{

	return (sc->tt.tls && sc->cryptocaps & FW_CAPS_CONFIG_TLSKEYS);
}

int
tls_tx_key(struct toepcb *toep)
{
	struct tls_ofld_info *tls_ofld = &toep->tls;

	return (tls_ofld->tx_key_addr >= 0);
}

int
tls_rx_key(struct toepcb *toep)
{
	struct tls_ofld_info *tls_ofld = &toep->tls;

	return (tls_ofld->rx_key_addr >= 0);
}

static int
key_size(struct toepcb *toep)
{
	struct tls_ofld_info *tls_ofld = &toep->tls;

	return ((tls_ofld->key_location == TLS_SFO_WR_CONTEXTLOC_IMMEDIATE) ?
		tls_ofld->k_ctx.tx_key_info_size : KEY_IN_DDR_SIZE);
}

/* Set TLS Key-Id in TCB */
static void
t4_set_tls_keyid(struct toepcb *toep, unsigned int key_id)
{

	t4_set_tls_tcb_field(toep, W_TCB_RX_TLS_KEY_TAG,
			 V_TCB_RX_TLS_KEY_TAG(M_TCB_RX_TLS_BUF_TAG),
			 V_TCB_RX_TLS_KEY_TAG(key_id));
}

/* Clear TF_RX_QUIESCE to re-enable receive. */
static void
t4_clear_rx_quiesce(struct toepcb *toep)
{

	t4_set_tls_tcb_field(toep, W_TCB_T_FLAGS, V_TF_RX_QUIESCE(1), 0);
}

static void
tls_clr_ofld_mode(struct toepcb *toep)
{

	tls_stop_handshake_timer(toep);

	/* Operate in PDU extraction mode only. */
	t4_set_tls_tcb_field(toep, W_TCB_ULP_RAW,
	    V_TCB_ULP_RAW(M_TCB_ULP_RAW),
	    V_TCB_ULP_RAW(V_TF_TLS_ENABLE(1)));
	t4_clear_rx_quiesce(toep);
}

static void
tls_clr_quiesce(struct toepcb *toep)
{

	tls_stop_handshake_timer(toep);
	t4_clear_rx_quiesce(toep);
}

/*
 * Calculate the TLS data expansion size
 */
static int
tls_expansion_size(struct toepcb *toep, int data_len, int full_pdus_only,
    unsigned short *pdus_per_ulp)
{
	struct tls_ofld_info *tls_ofld = &toep->tls;
	struct tls_scmd *scmd = &tls_ofld->scmd0;
	int expn_size = 0, frag_count = 0, pad_per_pdu = 0,
	    pad_last_pdu = 0, last_frag_size = 0, max_frag_size = 0;
	int exp_per_pdu = 0;
	int hdr_len = TLS_HEADER_LENGTH;

	do {
		max_frag_size = tls_ofld->k_ctx.frag_size;
		if (G_SCMD_CIPH_MODE(scmd->seqno_numivs) ==
		   SCMD_CIPH_MODE_AES_GCM) {
			frag_count = (data_len / max_frag_size);
			exp_per_pdu = GCM_TAG_SIZE + AEAD_EXPLICIT_DATA_SIZE +
				hdr_len;
			expn_size =  frag_count * exp_per_pdu;
			if (full_pdus_only) {
				*pdus_per_ulp = data_len / (exp_per_pdu +
					max_frag_size);
				if (*pdus_per_ulp > 32)
					*pdus_per_ulp = 32;
				else if(!*pdus_per_ulp)
					*pdus_per_ulp = 1;
				expn_size = (*pdus_per_ulp) * exp_per_pdu;
				break;
			}
			if ((last_frag_size = data_len % max_frag_size) > 0) {
				frag_count += 1;
				expn_size += exp_per_pdu;
			}
			break;
		} else if (G_SCMD_CIPH_MODE(scmd->seqno_numivs) !=
			   SCMD_CIPH_MODE_NOP) {
			/* Calculate the number of fragments we can make */
			frag_count  = (data_len / max_frag_size);
			if (frag_count > 0) {
				pad_per_pdu = (((howmany((max_frag_size +
						       tls_ofld->mac_length),
						      CIPHER_BLOCK_SIZE)) *
						CIPHER_BLOCK_SIZE) -
					       (max_frag_size +
						tls_ofld->mac_length));
				if (!pad_per_pdu)
					pad_per_pdu = CIPHER_BLOCK_SIZE;
				exp_per_pdu = pad_per_pdu +
				       	tls_ofld->mac_length +
					hdr_len + CIPHER_BLOCK_SIZE;
				expn_size = frag_count * exp_per_pdu;
			}
			if (full_pdus_only) {
				*pdus_per_ulp = data_len / (exp_per_pdu +
					max_frag_size);
				if (*pdus_per_ulp > 32)
					*pdus_per_ulp = 32;
				else if (!*pdus_per_ulp)
					*pdus_per_ulp = 1;
				expn_size = (*pdus_per_ulp) * exp_per_pdu;
				break;
			}
			/* Consider the last fragment */
			if ((last_frag_size = data_len % max_frag_size) > 0) {
				pad_last_pdu = (((howmany((last_frag_size +
							tls_ofld->mac_length),
						       CIPHER_BLOCK_SIZE)) *
						 CIPHER_BLOCK_SIZE) -
						(last_frag_size +
						 tls_ofld->mac_length));
				if (!pad_last_pdu)
					pad_last_pdu = CIPHER_BLOCK_SIZE;
				expn_size += (pad_last_pdu +
					      tls_ofld->mac_length + hdr_len +
					      CIPHER_BLOCK_SIZE);
			}
		}
	} while (0);

	return (expn_size);
}

/* Copy Key to WR */
static void
tls_copy_tx_key(struct toepcb *toep, void *dst)
{
	struct tls_ofld_info *tls_ofld = &toep->tls;
	struct ulptx_sc_memrd *sc_memrd;
	struct ulptx_idata *sc;

	if (tls_ofld->k_ctx.tx_key_info_size <= 0)
		return;

	if (tls_ofld->key_location == TLS_SFO_WR_CONTEXTLOC_DDR) {
		sc = dst;
		sc->cmd_more = htobe32(V_ULPTX_CMD(ULP_TX_SC_NOOP));
		sc->len = htobe32(0);
		sc_memrd = (struct ulptx_sc_memrd *)(sc + 1);
		sc_memrd->cmd_to_len = htobe32(V_ULPTX_CMD(ULP_TX_SC_MEMRD) |
		    V_ULP_TX_SC_MORE(1) |
		    V_ULPTX_LEN16(tls_ofld->k_ctx.tx_key_info_size >> 4));
		sc_memrd->addr = htobe32(tls_ofld->tx_key_addr >> 5);
	} else if (tls_ofld->key_location == TLS_SFO_WR_CONTEXTLOC_IMMEDIATE) {
		memcpy(dst, &tls_ofld->k_ctx.tx,
		    tls_ofld->k_ctx.tx_key_info_size);
	}
}

/* TLS/DTLS content type  for CPL SFO */
static inline unsigned char
tls_content_type(unsigned char content_type)
{
	/*
	 * XXX: Shouldn't this map CONTENT_TYPE_APP_DATA to DATA and
	 * default to "CUSTOM" for all other types including
	 * heartbeat?
	 */
	switch (content_type) {
	case CONTENT_TYPE_CCS:
		return CPL_TX_TLS_SFO_TYPE_CCS;
	case CONTENT_TYPE_ALERT:
		return CPL_TX_TLS_SFO_TYPE_ALERT;
	case CONTENT_TYPE_HANDSHAKE:
		return CPL_TX_TLS_SFO_TYPE_HANDSHAKE;
	case CONTENT_TYPE_HEARTBEAT:
		return CPL_TX_TLS_SFO_TYPE_HEARTBEAT;
	}
	return CPL_TX_TLS_SFO_TYPE_DATA;
}

static unsigned char
get_cipher_key_size(unsigned int ck_size)
{
	switch (ck_size) {
	case AES_NOP: /* NOP */
		return 15;
	case AES_128: /* AES128 */
		return CH_CK_SIZE_128;
	case AES_192: /* AES192 */
		return CH_CK_SIZE_192;
	case AES_256: /* AES256 */
		return CH_CK_SIZE_256;
	default:
		return CH_CK_SIZE_256;
	}
}

static unsigned char
get_mac_key_size(unsigned int mk_size)
{
	switch (mk_size) {
	case SHA_NOP: /* NOP */
		return CH_MK_SIZE_128;
	case SHA_GHASH: /* GHASH */
	case SHA_512: /* SHA512 */
		return CH_MK_SIZE_512;
	case SHA_224: /* SHA2-224 */
		return CH_MK_SIZE_192;
	case SHA_256: /* SHA2-256*/
		return CH_MK_SIZE_256;
	case SHA_384: /* SHA384 */
		return CH_MK_SIZE_512;
	case SHA1: /* SHA1 */
	default:
		return CH_MK_SIZE_160;
	}
}

static unsigned int
get_proto_ver(int proto_ver)
{
	switch (proto_ver) {
	case TLS1_2_VERSION:
		return TLS_1_2_VERSION;
	case TLS1_1_VERSION:
		return TLS_1_1_VERSION;
	case DTLS1_2_VERSION:
		return DTLS_1_2_VERSION;
	default:
		return TLS_VERSION_MAX;
	}
}

static void
tls_rxkey_flit1(struct tls_keyctx *kwr, struct tls_key_context *kctx)
{

	if (kctx->state.enc_mode == CH_EVP_CIPH_GCM_MODE) {
		kwr->u.rxhdr.ivinsert_to_authinsrt =
		    htobe64(V_TLS_KEYCTX_TX_WR_IVINSERT(6ULL) |
			V_TLS_KEYCTX_TX_WR_AADSTRTOFST(1ULL) |
			V_TLS_KEYCTX_TX_WR_AADSTOPOFST(5ULL) |
			V_TLS_KEYCTX_TX_WR_AUTHSRTOFST(14ULL) |
			V_TLS_KEYCTX_TX_WR_AUTHSTOPOFST(16ULL) |
			V_TLS_KEYCTX_TX_WR_CIPHERSRTOFST(14ULL) |
			V_TLS_KEYCTX_TX_WR_CIPHERSTOPOFST(0ULL) |
			V_TLS_KEYCTX_TX_WR_AUTHINSRT(16ULL));
		kwr->u.rxhdr.ivpresent_to_rxmk_size &=
			~(V_TLS_KEYCTX_TX_WR_RXOPAD_PRESENT(1));
		kwr->u.rxhdr.authmode_to_rxvalid &=
			~(V_TLS_KEYCTX_TX_WR_CIPHAUTHSEQCTRL(1));
	} else {
		kwr->u.rxhdr.ivinsert_to_authinsrt =
		    htobe64(V_TLS_KEYCTX_TX_WR_IVINSERT(6ULL) |
			V_TLS_KEYCTX_TX_WR_AADSTRTOFST(1ULL) |
			V_TLS_KEYCTX_TX_WR_AADSTOPOFST(5ULL) |
			V_TLS_KEYCTX_TX_WR_AUTHSRTOFST(22ULL) |
			V_TLS_KEYCTX_TX_WR_AUTHSTOPOFST(0ULL) |
			V_TLS_KEYCTX_TX_WR_CIPHERSRTOFST(22ULL) |
			V_TLS_KEYCTX_TX_WR_CIPHERSTOPOFST(0ULL) |
			V_TLS_KEYCTX_TX_WR_AUTHINSRT(0ULL));
	}
}

/* Rx key */
static void
prepare_rxkey_wr(struct tls_keyctx *kwr, struct tls_key_context *kctx)
{
	unsigned int ck_size = kctx->cipher_secret_size;
	unsigned int mk_size = kctx->mac_secret_size;
	int proto_ver = kctx->proto_ver;

	kwr->u.rxhdr.flitcnt_hmacctrl =
		((kctx->tx_key_info_size >> 4) << 3) | kctx->hmac_ctrl;

	kwr->u.rxhdr.protover_ciphmode =
		V_TLS_KEYCTX_TX_WR_PROTOVER(get_proto_ver(proto_ver)) |
		V_TLS_KEYCTX_TX_WR_CIPHMODE(kctx->state.enc_mode);

	kwr->u.rxhdr.authmode_to_rxvalid =
		V_TLS_KEYCTX_TX_WR_AUTHMODE(kctx->state.auth_mode) |
		V_TLS_KEYCTX_TX_WR_CIPHAUTHSEQCTRL(1) |
		V_TLS_KEYCTX_TX_WR_SEQNUMCTRL(3) |
		V_TLS_KEYCTX_TX_WR_RXVALID(1);

	kwr->u.rxhdr.ivpresent_to_rxmk_size =
		V_TLS_KEYCTX_TX_WR_IVPRESENT(0) |
		V_TLS_KEYCTX_TX_WR_RXOPAD_PRESENT(1) |
		V_TLS_KEYCTX_TX_WR_RXCK_SIZE(get_cipher_key_size(ck_size)) |
		V_TLS_KEYCTX_TX_WR_RXMK_SIZE(get_mac_key_size(mk_size));

	tls_rxkey_flit1(kwr, kctx);

	/* No key reversal for GCM */
	if (kctx->state.enc_mode != CH_EVP_CIPH_GCM_MODE) {
		t4_aes_getdeckey(kwr->keys.edkey, kctx->rx.key,
				 (kctx->cipher_secret_size << 3));
		memcpy(kwr->keys.edkey + kctx->cipher_secret_size,
		       kctx->rx.key + kctx->cipher_secret_size,
		       (IPAD_SIZE + OPAD_SIZE));
	} else {
		memcpy(kwr->keys.edkey, kctx->rx.key,
		       (kctx->tx_key_info_size - SALT_SIZE));
		memcpy(kwr->u.rxhdr.rxsalt, kctx->rx.salt, SALT_SIZE);
	}
}

/* Tx key */
static void
prepare_txkey_wr(struct tls_keyctx *kwr, struct tls_key_context *kctx)
{
	unsigned int ck_size = kctx->cipher_secret_size;
	unsigned int mk_size = kctx->mac_secret_size;

	kwr->u.txhdr.ctxlen =
		(kctx->tx_key_info_size >> 4);
	kwr->u.txhdr.dualck_to_txvalid =
		V_TLS_KEYCTX_TX_WR_TXOPAD_PRESENT(1) |
		V_TLS_KEYCTX_TX_WR_SALT_PRESENT(1) |
		V_TLS_KEYCTX_TX_WR_TXCK_SIZE(get_cipher_key_size(ck_size)) |
		V_TLS_KEYCTX_TX_WR_TXMK_SIZE(get_mac_key_size(mk_size)) |
		V_TLS_KEYCTX_TX_WR_TXVALID(1);

	memcpy(kwr->keys.edkey, kctx->tx.key, HDR_KCTX_SIZE);
	if (kctx->state.enc_mode == CH_EVP_CIPH_GCM_MODE) {
		memcpy(kwr->u.txhdr.txsalt, kctx->tx.salt, SALT_SIZE);
		kwr->u.txhdr.dualck_to_txvalid &=
			~(V_TLS_KEYCTX_TX_WR_TXOPAD_PRESENT(1));
	}
	kwr->u.txhdr.dualck_to_txvalid = htons(kwr->u.txhdr.dualck_to_txvalid);
}

/* TLS Key memory management */
int
tls_init_kmap(struct adapter *sc, struct tom_data *td)
{

	td->key_map = vmem_create("T4TLS key map", sc->vres.key.start,
	    sc->vres.key.size, 8, 0, M_FIRSTFIT | M_NOWAIT);
	if (td->key_map == NULL)
		return (ENOMEM);
	return (0);
}

void
tls_free_kmap(struct tom_data *td)
{

	if (td->key_map != NULL)
		vmem_destroy(td->key_map);
}

static int
get_new_keyid(struct toepcb *toep)
{
	struct tom_data *td = toep->td;
	vmem_addr_t addr;

	if (vmem_alloc(td->key_map, TLS_KEY_CONTEXT_SZ, M_NOWAIT | M_FIRSTFIT,
	    &addr) != 0)
		return (-1);

	return (addr);
}

static void
free_keyid(struct toepcb *toep, int keyid)
{
	struct tom_data *td = toep->td;

	CTR3(KTR_CXGBE, "%s: tid %d key addr %#x", __func__, toep->tid, keyid);
	vmem_free(td->key_map, keyid, TLS_KEY_CONTEXT_SZ);
}

static void
clear_tls_keyid(struct toepcb *toep)
{
	struct tls_ofld_info *tls_ofld = &toep->tls;

	if (tls_ofld->rx_key_addr >= 0) {
		free_keyid(toep, tls_ofld->rx_key_addr);
		tls_ofld->rx_key_addr = -1;
	}
	if (tls_ofld->tx_key_addr >= 0) {
		free_keyid(toep, tls_ofld->tx_key_addr);
		tls_ofld->tx_key_addr = -1;
	}
}

static int
get_keyid(struct tls_ofld_info *tls_ofld, unsigned int ops)
{
	return (ops & KEY_WRITE_RX ? tls_ofld->rx_key_addr :
		((ops & KEY_WRITE_TX) ? tls_ofld->rx_key_addr : -1));
}

static int
get_tp_plen_max(struct tls_ofld_info *tls_ofld)
{
	int plen = ((min(3*4096, TP_TX_PG_SZ))/1448) * 1448;

	return (tls_ofld->k_ctx.frag_size <= 8192 ? plen : FC_TP_PLEN_MAX);
}

/* Send request to get the key-id */
static int
tls_program_key_id(struct toepcb *toep, struct tls_key_context *k_ctx)
{
	struct tls_ofld_info *tls_ofld = &toep->tls;
	struct adapter *sc = td_adapter(toep->td);
	struct ofld_tx_sdesc *txsd;
	int kwrlen, kctxlen, keyid, len;
	struct wrqe *wr;
	struct tls_key_req *kwr;
	struct tls_keyctx *kctx;

	kwrlen = roundup2(sizeof(*kwr), 16);
	kctxlen = roundup2(sizeof(*kctx), 32);
	len = kwrlen + kctxlen;

	if (toep->txsd_avail == 0)
		return (EAGAIN);

	/* Dont initialize key for re-neg */
	if (!G_KEY_CLR_LOC(k_ctx->l_p_key)) {
		if ((keyid = get_new_keyid(toep)) < 0) {
			return (ENOSPC);
		}
	} else {
		keyid = get_keyid(tls_ofld, k_ctx->l_p_key);
	}

	wr = alloc_wrqe(len, toep->ofld_txq);
	if (wr == NULL) {
		free_keyid(toep, keyid);
		return (ENOMEM);
	}
	kwr = wrtod(wr);
	memset(kwr, 0, kwrlen);

	kwr->wr_hi = htobe32(V_FW_WR_OP(FW_ULPTX_WR) | F_FW_WR_COMPL |
	    F_FW_WR_ATOMIC);
	kwr->wr_mid = htobe32(V_FW_WR_LEN16(DIV_ROUND_UP(len, 16)) |
	    V_FW_WR_FLOWID(toep->tid));
	kwr->protocol = get_proto_ver(k_ctx->proto_ver);
	kwr->mfs = htons(k_ctx->frag_size);
	kwr->reneg_to_write_rx = k_ctx->l_p_key;

	/* master command */
	kwr->cmd = htobe32(V_ULPTX_CMD(ULP_TX_MEM_WRITE) |
	    V_T5_ULP_MEMIO_ORDER(1) | V_T5_ULP_MEMIO_IMM(1));
	kwr->dlen = htobe32(V_ULP_MEMIO_DATA_LEN(kctxlen >> 5));
	kwr->len16 = htobe32((toep->tid << 8) |
	    DIV_ROUND_UP(len - sizeof(struct work_request_hdr), 16));
	kwr->kaddr = htobe32(V_ULP_MEMIO_ADDR(keyid >> 5));

	/* sub command */
	kwr->sc_more = htobe32(V_ULPTX_CMD(ULP_TX_SC_IMM));
	kwr->sc_len = htobe32(kctxlen);

	/* XXX: This assumes that kwrlen == sizeof(*kwr). */
	kctx = (struct tls_keyctx *)(kwr + 1);
	memset(kctx, 0, kctxlen);

	if (G_KEY_GET_LOC(k_ctx->l_p_key) == KEY_WRITE_TX) {
		tls_ofld->tx_key_addr = keyid;
		prepare_txkey_wr(kctx, k_ctx);
	} else if (G_KEY_GET_LOC(k_ctx->l_p_key) == KEY_WRITE_RX) {
		tls_ofld->rx_key_addr = keyid;
		prepare_rxkey_wr(kctx, k_ctx);
	}

	txsd = &toep->txsd[toep->txsd_pidx];
	txsd->tx_credits = DIV_ROUND_UP(len, 16);
	txsd->plen = 0;
	toep->tx_credits -= txsd->tx_credits;
	if (__predict_false(++toep->txsd_pidx == toep->txsd_total))
		toep->txsd_pidx = 0;
	toep->txsd_avail--;

	t4_wrq_tx(sc, wr);

	return (0);
}

/* Store a key received from SSL in DDR. */
static int
program_key_context(struct tcpcb *tp, struct toepcb *toep,
    struct tls_key_context *uk_ctx)
{
	struct adapter *sc = td_adapter(toep->td);
	struct tls_ofld_info *tls_ofld = &toep->tls;
	struct tls_key_context *k_ctx;
	int error, key_offset;

	if (tp->t_state != TCPS_ESTABLISHED) {
		/*
		 * XXX: Matches Linux driver, but not sure this is a
		 * very appropriate error.
		 */
		return (ENOENT);
	}

	/* Stop timer on handshake completion */
	tls_stop_handshake_timer(toep);

	toep->flags &= ~TPF_FORCE_CREDITS;

	CTR4(KTR_CXGBE, "%s: tid %d %s proto_ver %#x", __func__, toep->tid,
	    G_KEY_GET_LOC(uk_ctx->l_p_key) == KEY_WRITE_RX ? "KEY_WRITE_RX" :
	    "KEY_WRITE_TX", uk_ctx->proto_ver);

	if (G_KEY_GET_LOC(uk_ctx->l_p_key) == KEY_WRITE_RX &&
	    toep->ulp_mode != ULP_MODE_TLS)
		return (EOPNOTSUPP);

	/* Don't copy the 'tx' and 'rx' fields. */
	k_ctx = &tls_ofld->k_ctx;
	memcpy(&k_ctx->l_p_key, &uk_ctx->l_p_key,
	    sizeof(*k_ctx) - offsetof(struct tls_key_context, l_p_key));

	/* TLS version != 1.1 and !1.2 OR DTLS != 1.2 */
	if (get_proto_ver(k_ctx->proto_ver) > DTLS_1_2_VERSION) {
		if (G_KEY_GET_LOC(k_ctx->l_p_key) == KEY_WRITE_RX) {
			tls_ofld->rx_key_addr = -1;
			t4_clear_rx_quiesce(toep);
		} else {
			tls_ofld->tx_key_addr = -1;
		}
		return (0);
	}

	if (k_ctx->state.enc_mode == CH_EVP_CIPH_GCM_MODE) {
		k_ctx->iv_size = 4;
		k_ctx->mac_first = 0;
		k_ctx->hmac_ctrl = 0;
	} else {
		k_ctx->iv_size = 8; /* for CBC, iv is 16B, unit of 2B */
		k_ctx->mac_first = 1;
	}

	tls_ofld->scmd0.seqno_numivs =
		(V_SCMD_SEQ_NO_CTRL(3) |
		 V_SCMD_PROTO_VERSION(get_proto_ver(k_ctx->proto_ver)) |
		 V_SCMD_ENC_DEC_CTRL(SCMD_ENCDECCTRL_ENCRYPT) |
		 V_SCMD_CIPH_AUTH_SEQ_CTRL((k_ctx->mac_first == 0)) |
		 V_SCMD_CIPH_MODE(k_ctx->state.enc_mode) |
		 V_SCMD_AUTH_MODE(k_ctx->state.auth_mode) |
		 V_SCMD_HMAC_CTRL(k_ctx->hmac_ctrl) |
		 V_SCMD_IV_SIZE(k_ctx->iv_size));

	tls_ofld->scmd0.ivgen_hdrlen =
		(V_SCMD_IV_GEN_CTRL(k_ctx->iv_ctrl) |
		 V_SCMD_KEY_CTX_INLINE(0) |
		 V_SCMD_TLS_FRAG_ENABLE(1));

	tls_ofld->mac_length = k_ctx->mac_secret_size;

	if (G_KEY_GET_LOC(k_ctx->l_p_key) == KEY_WRITE_RX) {
		k_ctx->rx = uk_ctx->rx;
		/* Dont initialize key for re-neg */
		if (!G_KEY_CLR_LOC(k_ctx->l_p_key))
			tls_ofld->rx_key_addr = -1;
	} else {
		k_ctx->tx = uk_ctx->tx;
		/* Dont initialize key for re-neg */
		if (!G_KEY_CLR_LOC(k_ctx->l_p_key))
			tls_ofld->tx_key_addr = -1;
	}

	/* Flush pending data before new Tx key becomes active */
	if (G_KEY_GET_LOC(k_ctx->l_p_key) == KEY_WRITE_TX) {
		struct sockbuf *sb;

		/* XXX: This might not drain everything. */
		t4_push_frames(sc, toep, 0);
		sb = &toep->inp->inp_socket->so_snd;
		SOCKBUF_LOCK(sb);

		/* XXX: This asserts that everything has been pushed. */
		MPASS(sb->sb_sndptr == NULL || sb->sb_sndptr->m_next == NULL);
		sb->sb_sndptr = NULL;
		tls_ofld->sb_off = sbavail(sb);
		SOCKBUF_UNLOCK(sb);
		tls_ofld->tx_seq_no = 0;
	}

	if ((G_KEY_GET_LOC(k_ctx->l_p_key) == KEY_WRITE_RX) ||
	    (tls_ofld->key_location == TLS_SFO_WR_CONTEXTLOC_DDR)) {
		error = tls_program_key_id(toep, k_ctx);
		if (error) {
			/* XXX: Only clear quiesce for KEY_WRITE_RX? */
			t4_clear_rx_quiesce(toep);
			return (error);
		}
	}

	if (G_KEY_GET_LOC(k_ctx->l_p_key) == KEY_WRITE_RX) {
		/*
		 * RX key tags are an index into the key portion of MA
		 * memory stored as an offset from the base address in
		 * units of 64 bytes.
		 */
		key_offset = tls_ofld->rx_key_addr - sc->vres.key.start;
		t4_set_tls_keyid(toep, key_offset / 64);
		t4_set_tls_tcb_field(toep, W_TCB_ULP_RAW,
				 V_TCB_ULP_RAW(M_TCB_ULP_RAW),
				 V_TCB_ULP_RAW((V_TF_TLS_KEY_SIZE(3) |
						V_TF_TLS_CONTROL(1) |
						V_TF_TLS_ACTIVE(1) |
						V_TF_TLS_ENABLE(1))));
		t4_set_tls_tcb_field(toep, W_TCB_TLS_SEQ,
				 V_TCB_TLS_SEQ(M_TCB_TLS_SEQ),
				 V_TCB_TLS_SEQ(0));
		t4_clear_rx_quiesce(toep);
	} else {
		unsigned short pdus_per_ulp;

		if (tls_ofld->key_location == TLS_SFO_WR_CONTEXTLOC_IMMEDIATE)
			tls_ofld->tx_key_addr = 1;

		tls_ofld->fcplenmax = get_tp_plen_max(tls_ofld);
		tls_ofld->expn_per_ulp = tls_expansion_size(toep,
				tls_ofld->fcplenmax, 1, &pdus_per_ulp);
		tls_ofld->pdus_per_ulp = pdus_per_ulp;
		tls_ofld->adjusted_plen = tls_ofld->pdus_per_ulp *
			((tls_ofld->expn_per_ulp/tls_ofld->pdus_per_ulp) +
			 tls_ofld->k_ctx.frag_size);
	}

	return (0);
}

/*
 * In some cases a client connection can hang without sending the
 * ServerHelloDone message from the NIC to the host.  Send a dummy
 * RX_DATA_ACK with RX_MODULATE to unstick the connection.
 */
static void
tls_send_handshake_ack(void *arg)
{
	struct toepcb *toep = arg;
	struct tls_ofld_info *tls_ofld = &toep->tls;
	struct adapter *sc = td_adapter(toep->td);

	/*
	 * XXX: Does not have the t4_get_tcb() checks to refine the
	 * workaround.
	 */
	callout_schedule(&tls_ofld->handshake_timer, TLS_SRV_HELLO_RD_TM * hz);

	CTR2(KTR_CXGBE, "%s: tid %d sending RX_DATA_ACK", __func__, toep->tid);
	send_rx_modulate(sc, toep);
}

static void
tls_start_handshake_timer(struct toepcb *toep)
{
	struct tls_ofld_info *tls_ofld = &toep->tls;

	mtx_lock(&tls_handshake_lock);
	callout_reset(&tls_ofld->handshake_timer, TLS_SRV_HELLO_BKOFF_TM * hz,
	    tls_send_handshake_ack, toep);
	mtx_unlock(&tls_handshake_lock);
}

void
tls_stop_handshake_timer(struct toepcb *toep)
{
	struct tls_ofld_info *tls_ofld = &toep->tls;

	mtx_lock(&tls_handshake_lock);
	callout_stop(&tls_ofld->handshake_timer);
	mtx_unlock(&tls_handshake_lock);
}

int
t4_ctloutput_tls(struct socket *so, struct sockopt *sopt)
{
	struct tls_key_context uk_ctx;
	struct inpcb *inp;
	struct tcpcb *tp;
	struct toepcb *toep;
	int error, optval;

	error = 0;
	if (sopt->sopt_dir == SOPT_SET &&
	    sopt->sopt_name == TCP_TLSOM_SET_TLS_CONTEXT) {
		error = sooptcopyin(sopt, &uk_ctx, sizeof(uk_ctx),
		    sizeof(uk_ctx));
		if (error)
			return (error);
	}

	inp = sotoinpcb(so);
	KASSERT(inp != NULL, ("tcp_ctloutput: inp == NULL"));
	INP_WLOCK(inp);
	if (inp->inp_flags & (INP_TIMEWAIT | INP_DROPPED)) {
		INP_WUNLOCK(inp);
		return (ECONNRESET);
	}
	tp = intotcpcb(inp);
	toep = tp->t_toe;
	switch (sopt->sopt_dir) {
	case SOPT_SET:
		switch (sopt->sopt_name) {
		case TCP_TLSOM_SET_TLS_CONTEXT:
			error = program_key_context(tp, toep, &uk_ctx);
			INP_WUNLOCK(inp);
			break;
		case TCP_TLSOM_CLR_TLS_TOM:
			if (toep->ulp_mode == ULP_MODE_TLS) {
				CTR2(KTR_CXGBE, "%s: tid %d CLR_TLS_TOM",
				    __func__, toep->tid);
				tls_clr_ofld_mode(toep);
			} else
				error = EOPNOTSUPP;
			INP_WUNLOCK(inp);
			break;
		case TCP_TLSOM_CLR_QUIES:
			if (toep->ulp_mode == ULP_MODE_TLS) {
				CTR2(KTR_CXGBE, "%s: tid %d CLR_QUIES",
				    __func__, toep->tid);
				tls_clr_quiesce(toep);
			} else
				error = EOPNOTSUPP;
			INP_WUNLOCK(inp);
			break;
		default:
			INP_WUNLOCK(inp);
			error = EOPNOTSUPP;
			break;
		}
		break;
	case SOPT_GET:
		switch (sopt->sopt_name) {
		case TCP_TLSOM_GET_TLS_TOM:
			/*
			 * TLS TX is permitted on any TOE socket, but
			 * TLS RX requires a TLS ULP mode.
			 */
			optval = TLS_TOM_NONE;
			if (can_tls_offload(td_adapter(toep->td))) {
				switch (toep->ulp_mode) {
				case ULP_MODE_NONE:
				case ULP_MODE_TCPDDP:
					optval = TLS_TOM_TXONLY;
					break;
				case ULP_MODE_TLS:
					optval = TLS_TOM_BOTH;
					break;
				}
			}
			CTR3(KTR_CXGBE, "%s: tid %d GET_TLS_TOM = %d",
			    __func__, toep->tid, optval);
			INP_WUNLOCK(inp);
			error = sooptcopyout(sopt, &optval, sizeof(optval));
			break;
		default:
			INP_WUNLOCK(inp);
			error = EOPNOTSUPP;
			break;
		}
		break;
	}
	return (error);
}

void
tls_init_toep(struct toepcb *toep)
{
	struct tls_ofld_info *tls_ofld = &toep->tls;

	tls_ofld->key_location = TLS_SFO_WR_CONTEXTLOC_DDR;
	tls_ofld->rx_key_addr = -1;
	tls_ofld->tx_key_addr = -1;
	if (toep->ulp_mode == ULP_MODE_TLS)
		callout_init_mtx(&tls_ofld->handshake_timer,
		    &tls_handshake_lock, 0);
}

void
tls_establish(struct toepcb *toep)
{

	/*
	 * Enable PDU extraction.
	 *
	 * XXX: Supposedly this should be done by the firmware when
	 * the ULP_MODE FLOWC parameter is set in send_flowc_wr(), but
	 * in practice this seems to be required.
	 */
	CTR2(KTR_CXGBE, "%s: tid %d setting TLS_ENABLE", __func__, toep->tid);
	t4_set_tls_tcb_field(toep, W_TCB_ULP_RAW, V_TCB_ULP_RAW(M_TCB_ULP_RAW),
	    V_TCB_ULP_RAW(V_TF_TLS_ENABLE(1)));

	toep->flags |= TPF_FORCE_CREDITS;

	tls_start_handshake_timer(toep);
}

void
tls_uninit_toep(struct toepcb *toep)
{

	if (toep->ulp_mode == ULP_MODE_TLS)
		tls_stop_handshake_timer(toep);
	clear_tls_keyid(toep);
}

#define MAX_OFLD_TX_CREDITS (SGE_MAX_WR_LEN / 16)
#define	MIN_OFLD_TLSTX_CREDITS(toep)					\
	(howmany(sizeof(struct fw_tlstx_data_wr) +			\
	    sizeof(struct cpl_tx_tls_sfo) + key_size((toep)) +		\
	    CIPHER_BLOCK_SIZE + 1, 16))

static inline u_int
max_imm_tls_space(int tx_credits)
{
	const int n = 2;	/* Use only up to 2 desc for imm. data WR */
	int space;

	KASSERT(tx_credits >= 0 &&
		tx_credits <= MAX_OFLD_TX_CREDITS,
		("%s: %d credits", __func__, tx_credits));

	if (tx_credits >= (n * EQ_ESIZE) / 16)
		space = (n * EQ_ESIZE);
	else
		space = tx_credits * 16;
	return (space);
}

static int
count_mbuf_segs(struct mbuf *m, int skip, int len, int *max_nsegs_1mbufp)
{
	int max_nsegs_1mbuf, n, nsegs;

	while (skip >= m->m_len) {
		skip -= m->m_len;
		m = m->m_next;
	}

	nsegs = 0;
	max_nsegs_1mbuf = 0;
	while (len > 0) {
		n = sglist_count(mtod(m, char *) + skip, m->m_len - skip);
		if (n > max_nsegs_1mbuf)
			max_nsegs_1mbuf = n;
		nsegs += n;
		len -= m->m_len - skip;
		skip = 0;
		m = m->m_next;
	}
	*max_nsegs_1mbufp = max_nsegs_1mbuf;
	return (nsegs);
}

static void
write_tlstx_wr(struct fw_tlstx_data_wr *txwr, struct toepcb *toep,
    unsigned int immdlen, unsigned int plen, unsigned int expn,
    unsigned int pdus, uint8_t credits, int shove, int imm_ivs)
{
	struct tls_ofld_info *tls_ofld = &toep->tls;
	unsigned int len = plen + expn;

	txwr->op_to_immdlen = htobe32(V_WR_OP(FW_TLSTX_DATA_WR) |
	    V_FW_TLSTX_DATA_WR_COMPL(1) |
	    V_FW_TLSTX_DATA_WR_IMMDLEN(immdlen));
	txwr->flowid_len16 = htobe32(V_FW_TLSTX_DATA_WR_FLOWID(toep->tid) |
	    V_FW_TLSTX_DATA_WR_LEN16(credits));
	txwr->plen = htobe32(len);
	txwr->lsodisable_to_flags = htobe32(V_TX_ULP_MODE(ULP_MODE_TLS) |
	    V_TX_URG(0) | /* F_T6_TX_FORCE | */ V_TX_SHOVE(shove));
	txwr->ctxloc_to_exp = htobe32(V_FW_TLSTX_DATA_WR_NUMIVS(pdus) |
	    V_FW_TLSTX_DATA_WR_EXP(expn) |
	    V_FW_TLSTX_DATA_WR_CTXLOC(tls_ofld->key_location) |
	    V_FW_TLSTX_DATA_WR_IVDSGL(!imm_ivs) |
	    V_FW_TLSTX_DATA_WR_KEYSIZE(tls_ofld->k_ctx.tx_key_info_size >> 4));
	txwr->mfs = htobe16(tls_ofld->k_ctx.frag_size);
	txwr->adjustedplen_pkd = htobe16(
	    V_FW_TLSTX_DATA_WR_ADJUSTEDPLEN(tls_ofld->adjusted_plen));
	txwr->expinplenmax_pkd = htobe16(
	    V_FW_TLSTX_DATA_WR_EXPINPLENMAX(tls_ofld->expn_per_ulp));
	txwr->pdusinplenmax_pkd = htobe16(
	    V_FW_TLSTX_DATA_WR_PDUSINPLENMAX(tls_ofld->pdus_per_ulp));
}

static void
write_tlstx_cpl(struct cpl_tx_tls_sfo *cpl, struct toepcb *toep,
    struct tls_hdr *tls_hdr, unsigned int plen, unsigned int pdus)
{
	struct tls_ofld_info *tls_ofld = &toep->tls;
	int data_type, seglen;

	if (plen < tls_ofld->k_ctx.frag_size)
		seglen = plen;
	else
		seglen = tls_ofld->k_ctx.frag_size;
	data_type = tls_content_type(tls_hdr->type);
	cpl->op_to_seg_len = htobe32(V_CPL_TX_TLS_SFO_OPCODE(CPL_TX_TLS_SFO) |
	    V_CPL_TX_TLS_SFO_DATA_TYPE(data_type) |
	    V_CPL_TX_TLS_SFO_CPL_LEN(2) | V_CPL_TX_TLS_SFO_SEG_LEN(seglen));
	cpl->pld_len = htobe32(plen);
	if (data_type == CPL_TX_TLS_SFO_TYPE_HEARTBEAT)
		cpl->type_protover = htobe32(
		    V_CPL_TX_TLS_SFO_TYPE(tls_hdr->type));
	cpl->seqno_numivs = htobe32(tls_ofld->scmd0.seqno_numivs |
	    V_SCMD_NUM_IVS(pdus));
	cpl->ivgen_hdrlen = htobe32(tls_ofld->scmd0.ivgen_hdrlen);
	cpl->scmd1 = htobe64(tls_ofld->tx_seq_no);
	tls_ofld->tx_seq_no += pdus;
}

/*
 * Similar to write_tx_sgl() except that it accepts an optional
 * trailer buffer for IVs.
 */
static void
write_tlstx_sgl(void *dst, struct mbuf *start, int skip, int plen,
    void *iv_buffer, int iv_len, int nsegs, int n)
{
	struct mbuf *m;
	struct ulptx_sgl *usgl = dst;
	int i, j, rc;
	struct sglist sg;
	struct sglist_seg segs[n];

	KASSERT(nsegs > 0, ("%s: nsegs 0", __func__));

	sglist_init(&sg, n, segs);
	usgl->cmd_nsge = htobe32(V_ULPTX_CMD(ULP_TX_SC_DSGL) |
	    V_ULPTX_NSGE(nsegs));

	for (m = start; skip >= m->m_len; m = m->m_next)
		skip -= m->m_len;

	i = -1;
	for (m = start; plen > 0; m = m->m_next) {
		rc = sglist_append(&sg, mtod(m, char *) + skip,
		    m->m_len - skip);
		if (__predict_false(rc != 0))
			panic("%s: sglist_append %d", __func__, rc);
		plen -= m->m_len - skip;
		skip = 0;

		for (j = 0; j < sg.sg_nseg; i++, j++) {
			if (i < 0) {
				usgl->len0 = htobe32(segs[j].ss_len);
				usgl->addr0 = htobe64(segs[j].ss_paddr);
			} else {
				usgl->sge[i / 2].len[i & 1] =
				    htobe32(segs[j].ss_len);
				usgl->sge[i / 2].addr[i & 1] =
				    htobe64(segs[j].ss_paddr);
			}
#ifdef INVARIANTS
			nsegs--;
#endif
		}
		sglist_reset(&sg);
	}
	if (iv_buffer != NULL) {
		rc = sglist_append(&sg, iv_buffer, iv_len);
		if (__predict_false(rc != 0))
			panic("%s: sglist_append %d", __func__, rc);

		for (j = 0; j < sg.sg_nseg; i++, j++) {
			if (i < 0) {
				usgl->len0 = htobe32(segs[j].ss_len);
				usgl->addr0 = htobe64(segs[j].ss_paddr);
			} else {
				usgl->sge[i / 2].len[i & 1] =
				    htobe32(segs[j].ss_len);
				usgl->sge[i / 2].addr[i & 1] =
				    htobe64(segs[j].ss_paddr);
			}
#ifdef INVARIANTS
			nsegs--;
#endif
		}
	}
	if (i & 1)
		usgl->sge[i / 2].len[1] = htobe32(0);
	KASSERT(nsegs == 0, ("%s: nsegs %d, start %p, iv_buffer %p",
	    __func__, nsegs, start, iv_buffer));
}

/*
 * Similar to t4_push_frames() but handles TLS sockets when TLS offload
 * is enabled.  Rather than transmitting bulk data, the socket buffer
 * contains TLS records.  The work request requires a full TLS record,
 * so batch mbufs up until a full TLS record is seen.  This requires
 * reading the TLS header out of the start of each record to determine
 * its length.
 */
void
t4_push_tls_records(struct adapter *sc, struct toepcb *toep, int drop)
{
	struct tls_hdr thdr;
	struct mbuf *sndptr;
	struct fw_tlstx_data_wr *txwr;
	struct cpl_tx_tls_sfo *cpl;
	struct wrqe *wr;
	u_int plen, nsegs, credits, space, max_nsegs_1mbuf, wr_len;
	u_int expn_size, iv_len, pdus, sndptroff;
	struct tls_ofld_info *tls_ofld = &toep->tls;
	struct inpcb *inp = toep->inp;
	struct tcpcb *tp = intotcpcb(inp);
	struct socket *so = inp->inp_socket;
	struct sockbuf *sb = &so->so_snd;
	int tls_size, tx_credits, shove, /* compl,*/ sowwakeup;
	struct ofld_tx_sdesc *txsd;
	bool imm_ivs, imm_payload;
	void *iv_buffer, *iv_dst, *buf;

	INP_WLOCK_ASSERT(inp);
	KASSERT(toep->flags & TPF_FLOWC_WR_SENT,
	    ("%s: flowc_wr not sent for tid %u.", __func__, toep->tid));

	KASSERT(toep->ulp_mode == ULP_MODE_NONE ||
	    toep->ulp_mode == ULP_MODE_TCPDDP || toep->ulp_mode == ULP_MODE_TLS,
	    ("%s: ulp_mode %u for toep %p", __func__, toep->ulp_mode, toep));
	KASSERT(tls_tx_key(toep),
	    ("%s: TX key not set for toep %p", __func__, toep));

#ifdef VERBOSE_TRACES
	CTR4(KTR_CXGBE, "%s: tid %d toep flags %#x tp flags %#x drop %d",
	    __func__, toep->tid, toep->flags, tp->t_flags);
#endif
	if (__predict_false(toep->flags & TPF_ABORT_SHUTDOWN))
		return;

#ifdef RATELIMIT
	if (__predict_false(inp->inp_flags2 & INP_RATE_LIMIT_CHANGED) &&
	    (update_tx_rate_limit(sc, toep, so->so_max_pacing_rate) == 0)) {
		inp->inp_flags2 &= ~INP_RATE_LIMIT_CHANGED;
	}
#endif

	/*
	 * This function doesn't resume by itself.  Someone else must clear the
	 * flag and call this function.
	 */
	if (__predict_false(toep->flags & TPF_TX_SUSPENDED)) {
		KASSERT(drop == 0,
		    ("%s: drop (%d) != 0 but tx is suspended", __func__, drop));
		return;
	}

	txsd = &toep->txsd[toep->txsd_pidx];
	for (;;) {
		tx_credits = min(toep->tx_credits, MAX_OFLD_TX_CREDITS);
		space = max_imm_tls_space(tx_credits);
		wr_len = sizeof(struct fw_tlstx_data_wr) +
		    sizeof(struct cpl_tx_tls_sfo) + key_size(toep);
		if (wr_len + CIPHER_BLOCK_SIZE + 1 > space) {
#ifdef VERBOSE_TRACES
			CTR5(KTR_CXGBE,
			    "%s: tid %d tx_credits %d min_wr %d space %d",
			    __func__, toep->tid, tx_credits, wr_len +
			    CIPHER_BLOCK_SIZE + 1, space);
#endif
			return;
		}

		SOCKBUF_LOCK(sb);
		sowwakeup = drop;
		if (drop) {
			sbdrop_locked(sb, drop);
			MPASS(tls_ofld->sb_off >= drop);
			tls_ofld->sb_off -= drop;
			drop = 0;
		}

		/*
		 * Send a FIN if requested, but only if there's no
		 * more data to send.
		 */
		if (sbavail(sb) == tls_ofld->sb_off &&
		    toep->flags & TPF_SEND_FIN) {
			if (sowwakeup)
				sowwakeup_locked(so);
			else
				SOCKBUF_UNLOCK(sb);
			SOCKBUF_UNLOCK_ASSERT(sb);
			t4_close_conn(sc, toep);
			return;
		}

		if (sbavail(sb) < tls_ofld->sb_off + TLS_HEADER_LENGTH) {
			/*
			 * A full TLS header is not yet queued, stop
			 * for now until more data is added to the
			 * socket buffer.
			 */
#ifdef VERBOSE_TRACES
			CTR4(KTR_CXGBE, "%s: tid %d sbavail %d sb_off %d",
			    __func__, toep->tid, sbavail(sb), tls_ofld->sb_off);
#endif
			if (sowwakeup)
				sowwakeup_locked(so);
			else
				SOCKBUF_UNLOCK(sb);
			SOCKBUF_UNLOCK_ASSERT(sb);
			return;
		}

		/* Read the header of the next TLS record. */
		sndptr = sbsndmbuf(sb, tls_ofld->sb_off, &sndptroff);
		MPASS(!IS_AIOTX_MBUF(sndptr));
		m_copydata(sndptr, sndptroff, sizeof(thdr), (caddr_t)&thdr);
		tls_size = htons(thdr.length);
		plen = TLS_HEADER_LENGTH + tls_size;
		pdus = howmany(tls_size, tls_ofld->k_ctx.frag_size);
		iv_len = pdus * CIPHER_BLOCK_SIZE;

		if (sbavail(sb) < tls_ofld->sb_off + plen) {
			/*
			 * The full TLS record is not yet queued, stop
			 * for now until more data is added to the
			 * socket buffer.
			 */
#ifdef VERBOSE_TRACES
			CTR5(KTR_CXGBE,
			    "%s: tid %d sbavail %d sb_off %d plen %d",
			    __func__, toep->tid, sbavail(sb), tls_ofld->sb_off,
			    plen);
#endif
			if (sowwakeup)
				sowwakeup_locked(so);
			else
				SOCKBUF_UNLOCK(sb);
			SOCKBUF_UNLOCK_ASSERT(sb);
			return;
		}

		/* Shove if there is no additional data pending. */
		shove = (sbavail(sb) == tls_ofld->sb_off + plen) &&
		    !(tp->t_flags & TF_MORETOCOME);

		if (sb->sb_flags & SB_AUTOSIZE &&
		    V_tcp_do_autosndbuf &&
		    sb->sb_hiwat < V_tcp_autosndbuf_max &&
		    sbused(sb) >= sb->sb_hiwat * 7 / 8) {
			int newsize = min(sb->sb_hiwat + V_tcp_autosndbuf_inc,
			    V_tcp_autosndbuf_max);

			if (!sbreserve_locked(sb, newsize, so, NULL))
				sb->sb_flags &= ~SB_AUTOSIZE;
			else
				sowwakeup = 1;	/* room available */
		}
		if (sowwakeup)
			sowwakeup_locked(so);
		else
			SOCKBUF_UNLOCK(sb);
		SOCKBUF_UNLOCK_ASSERT(sb);

		if (__predict_false(toep->flags & TPF_FIN_SENT))
			panic("%s: excess tx.", __func__);

		/* Determine whether to use immediate vs SGL. */
		imm_payload = false;
		imm_ivs = false;
		if (wr_len + iv_len <= space) {
			imm_ivs = true;
			wr_len += iv_len;
			if (wr_len + tls_size <= space) {
				wr_len += tls_size;
				imm_payload = true;
			}
		}

		/* Allocate space for IVs if needed. */
		if (!imm_ivs) {
			iv_buffer = malloc(iv_len, M_CXGBE, M_NOWAIT);
			if (iv_buffer == NULL) {
				/*
				 * XXX: How to restart this?
				 */
				if (sowwakeup)
					sowwakeup_locked(so);
				else
					SOCKBUF_UNLOCK(sb);
				SOCKBUF_UNLOCK_ASSERT(sb);
				CTR3(KTR_CXGBE,
			    "%s: tid %d failed to alloc IV space len %d",
				    __func__, toep->tid, iv_len);
				return;
			}
		} else
			iv_buffer = NULL;

		/* Determine size of SGL. */
		nsegs = 0;
		max_nsegs_1mbuf = 0; /* max # of SGL segments in any one mbuf */
		if (!imm_payload) {
			nsegs = count_mbuf_segs(sndptr, sndptroff +
			    TLS_HEADER_LENGTH, tls_size, &max_nsegs_1mbuf);
			if (!imm_ivs) {
				int n = sglist_count(iv_buffer, iv_len);
				nsegs += n;
				if (n > max_nsegs_1mbuf)
					max_nsegs_1mbuf = n;
			}

			/* Account for SGL in work request length. */
			wr_len += sizeof(struct ulptx_sgl) +
			    ((3 * (nsegs - 1)) / 2 + ((nsegs - 1) & 1)) * 8;
		}

		wr = alloc_wrqe(roundup2(wr_len, 16), toep->ofld_txq);
		if (wr == NULL) {
			/* XXX: how will we recover from this? */
			toep->flags |= TPF_TX_SUSPENDED;
			return;
		}

#ifdef VERBOSE_TRACES
		CTR5(KTR_CXGBE, "%s: tid %d TLS record %d len %#x pdus %d",
		    __func__, toep->tid, thdr.type, tls_size, pdus);
#endif
		txwr = wrtod(wr);
		cpl = (struct cpl_tx_tls_sfo *)(txwr + 1);
		memset(txwr, 0, roundup2(wr_len, 16));
		credits = howmany(wr_len, 16);
		expn_size = tls_expansion_size(toep, tls_size, 0, NULL);
		write_tlstx_wr(txwr, toep, imm_payload ? tls_size : 0,
		    tls_size, expn_size, pdus, credits, shove, imm_ivs ? 1 : 0);
		write_tlstx_cpl(cpl, toep, &thdr, tls_size, pdus);
		tls_copy_tx_key(toep, cpl + 1);

		/* Generate random IVs */
		buf = (char *)(cpl + 1) + key_size(toep);
		if (imm_ivs) {
			MPASS(iv_buffer == NULL);
			iv_dst = buf;
			buf = (char *)iv_dst + iv_len;
		} else
			iv_dst = iv_buffer;
		arc4rand(iv_dst, iv_len, 0);

		if (imm_payload) {
			m_copydata(sndptr, sndptroff + TLS_HEADER_LENGTH,
			    tls_size, buf);
		} else {
			write_tlstx_sgl(buf, sndptr,
			    sndptroff + TLS_HEADER_LENGTH, tls_size, iv_buffer,
			    iv_len, nsegs, max_nsegs_1mbuf);
		}

		KASSERT(toep->tx_credits >= credits,
			("%s: not enough credits", __func__));

		toep->tx_credits -= credits;

		tp->snd_nxt += plen;
		tp->snd_max += plen;

		SOCKBUF_LOCK(sb);
		sbsndptr_adv(sb, sb->sb_sndptr, plen);
		tls_ofld->sb_off += plen;
		SOCKBUF_UNLOCK(sb);

		toep->flags |= TPF_TX_DATA_SENT;
		if (toep->tx_credits < MIN_OFLD_TLSTX_CREDITS(toep))
			toep->flags |= TPF_TX_SUSPENDED;

		KASSERT(toep->txsd_avail > 0, ("%s: no txsd", __func__));
		txsd->plen = plen;
		txsd->tx_credits = credits;
		txsd->iv_buffer = iv_buffer;
		txsd++;
		if (__predict_false(++toep->txsd_pidx == toep->txsd_total)) {
			toep->txsd_pidx = 0;
			txsd = &toep->txsd[0];
		}
		toep->txsd_avail--;

		atomic_add_long(&toep->vi->pi->tx_tls_records, 1);
		atomic_add_long(&toep->vi->pi->tx_tls_octets, plen);

		t4_l2t_send(sc, wr, toep->l2te);
	}
}

/*
 * For TLS data we place received mbufs received via CPL_TLS_DATA into
 * an mbufq in the TLS offload state.  When CPL_RX_TLS_CMP is
 * received, the completed PDUs are placed into the socket receive
 * buffer.
 *
 * The TLS code reuses the ulp_pdu_reclaimq to hold the pending mbufs.
 */
static int
do_tls_data(struct sge_iq *iq, const struct rss_header *rss, struct mbuf *m)
{
	struct adapter *sc = iq->adapter;
	const struct cpl_tls_data *cpl = mtod(m, const void *);
	unsigned int tid = GET_TID(cpl);
	struct toepcb *toep = lookup_tid(sc, tid);
	struct inpcb *inp = toep->inp;
	struct tcpcb *tp;
	int len;

	/* XXX: Should this match do_rx_data instead? */
	KASSERT(!(toep->flags & TPF_SYNQE),
	    ("%s: toep %p claims to be a synq entry", __func__, toep));

	KASSERT(toep->tid == tid, ("%s: toep tid/atid mismatch", __func__));

	/* strip off CPL header */
	m_adj(m, sizeof(*cpl));
	len = m->m_pkthdr.len;

	atomic_add_long(&toep->vi->pi->rx_tls_octets, len);

	KASSERT(len == G_CPL_TLS_DATA_LENGTH(be32toh(cpl->length_pkd)),
	    ("%s: payload length mismatch", __func__));

	INP_WLOCK(inp);
	if (inp->inp_flags & (INP_DROPPED | INP_TIMEWAIT)) {
		CTR4(KTR_CXGBE, "%s: tid %u, rx (%d bytes), inp_flags 0x%x",
		    __func__, tid, len, inp->inp_flags);
		INP_WUNLOCK(inp);
		m_freem(m);
		return (0);
	}

	/* Save TCP sequence number. */
	m->m_pkthdr.tls_tcp_seq = be32toh(cpl->seq);

	if (mbufq_enqueue(&toep->ulp_pdu_reclaimq, m)) {
#ifdef INVARIANTS
		panic("Failed to queue TLS data packet");
#else
		printf("%s: Failed to queue TLS data packet\n", __func__);
		INP_WUNLOCK(inp);
		m_freem(m);
		return (0);
#endif
	}

	tp = intotcpcb(inp);
	tp->t_rcvtime = ticks;

#ifdef VERBOSE_TRACES
	CTR4(KTR_CXGBE, "%s: tid %u len %d seq %u", __func__, tid, len,
	    be32toh(cpl->seq));
#endif

	INP_WUNLOCK(inp);
	return (0);
}

static int
do_rx_tls_cmp(struct sge_iq *iq, const struct rss_header *rss, struct mbuf *m)
{
	struct adapter *sc = iq->adapter;
	const struct cpl_rx_tls_cmp *cpl = mtod(m, const void *);
	struct tlsrx_hdr_pkt *tls_hdr_pkt;
	unsigned int tid = GET_TID(cpl);
	struct toepcb *toep = lookup_tid(sc, tid);
	struct inpcb *inp = toep->inp;
	struct tcpcb *tp;
	struct socket *so;
	struct sockbuf *sb;
	struct mbuf *tls_data;
	int len, pdu_length, pdu_overhead, sb_length;

	KASSERT(toep->tid == tid, ("%s: toep tid/atid mismatch", __func__));
	KASSERT(!(toep->flags & TPF_SYNQE),
	    ("%s: toep %p claims to be a synq entry", __func__, toep));

	/* strip off CPL header */
	m_adj(m, sizeof(*cpl));
	len = m->m_pkthdr.len;

	atomic_add_long(&toep->vi->pi->rx_tls_records, 1);

	KASSERT(len == G_CPL_RX_TLS_CMP_LENGTH(be32toh(cpl->pdulength_length)),
	    ("%s: payload length mismatch", __func__));

	INP_WLOCK(inp);
	if (inp->inp_flags & (INP_DROPPED | INP_TIMEWAIT)) {
		CTR4(KTR_CXGBE, "%s: tid %u, rx (%d bytes), inp_flags 0x%x",
		    __func__, tid, len, inp->inp_flags);
		INP_WUNLOCK(inp);
		m_freem(m);
		return (0);
	}

	pdu_length = G_CPL_RX_TLS_CMP_PDULENGTH(be32toh(cpl->pdulength_length));

	tp = intotcpcb(inp);

#ifdef VERBOSE_TRACES
	CTR6(KTR_CXGBE, "%s: tid %u PDU len %d len %d seq %u, rcv_nxt %u",
	    __func__, tid, pdu_length, len, be32toh(cpl->seq), tp->rcv_nxt);
#endif

	tp->rcv_nxt += pdu_length;
	if (tp->rcv_wnd < pdu_length) {
		toep->tls.rcv_over += pdu_length - tp->rcv_wnd;
		tp->rcv_wnd = 0;
	} else
		tp->rcv_wnd -= pdu_length;

	/* XXX: Not sure what to do about urgent data. */

	/*
	 * The payload of this CPL is the TLS header followed by
	 * additional fields.
	 */
	KASSERT(m->m_len >= sizeof(*tls_hdr_pkt),
	    ("%s: payload too small", __func__));
	tls_hdr_pkt = mtod(m, void *);

	/*
	 * Only the TLS header is sent to OpenSSL, so report errors by
	 * altering the record type.
	 */
	if ((tls_hdr_pkt->res_to_mac_error & M_TLSRX_HDR_PKT_ERROR) != 0)
		tls_hdr_pkt->type = CONTENT_TYPE_ERROR;

	/* Trim this CPL's mbuf to only include the TLS header. */
	KASSERT(m->m_len == len && m->m_next == NULL,
	    ("%s: CPL spans multiple mbufs", __func__));
	m->m_len = TLS_HEADER_LENGTH;
	m->m_pkthdr.len = TLS_HEADER_LENGTH;

	tls_data = mbufq_dequeue(&toep->ulp_pdu_reclaimq);
	if (tls_data != NULL) {
		KASSERT(be32toh(cpl->seq) == tls_data->m_pkthdr.tls_tcp_seq,
		    ("%s: sequence mismatch", __func__));

		/*
		 * Update the TLS header length to be the length of
		 * the payload data.
		 */
		tls_hdr_pkt->length = htobe16(tls_data->m_pkthdr.len);

		m->m_next = tls_data;
		m->m_pkthdr.len += tls_data->m_len;
	}

	so = inp_inpcbtosocket(inp);
	sb = &so->so_rcv;
	SOCKBUF_LOCK(sb);

	if (__predict_false(sb->sb_state & SBS_CANTRCVMORE)) {
		CTR3(KTR_CXGBE, "%s: tid %u, excess rx (%d bytes)",
		    __func__, tid, pdu_length);
		m_freem(m);
		SOCKBUF_UNLOCK(sb);
		INP_WUNLOCK(inp);

		CURVNET_SET(toep->vnet);
		INP_INFO_RLOCK(&V_tcbinfo);
		INP_WLOCK(inp);
		tp = tcp_drop(tp, ECONNRESET);
		if (tp)
			INP_WUNLOCK(inp);
		INP_INFO_RUNLOCK(&V_tcbinfo);
		CURVNET_RESTORE();

		return (0);
	}

	/*
	 * Not all of the bytes on the wire are included in the socket
	 * buffer (e.g. the MAC of the TLS record).  However, those
	 * bytes are included in the TCP sequence space.  To handle
	 * this, compute the delta for this TLS record in
	 * 'pdu_overhead' and treat those bytes as having already been
	 * "read" by the application for the purposes of expanding the
	 * window.  The meat of the TLS record passed to the
	 * application ('sb_length') will still not be counted as
	 * "read" until userland actually reads the bytes.
	 *
	 * XXX: Some of the calculations below are probably still not
	 * really correct.
	 */
	sb_length = m->m_pkthdr.len;
	pdu_overhead = pdu_length - sb_length;
	toep->rx_credits += pdu_overhead;
	tp->rcv_wnd += pdu_overhead;
	tp->rcv_adv += pdu_overhead;

	/* receive buffer autosize */
	MPASS(toep->vnet == so->so_vnet);
	CURVNET_SET(toep->vnet);
	if (sb->sb_flags & SB_AUTOSIZE &&
	    V_tcp_do_autorcvbuf &&
	    sb->sb_hiwat < V_tcp_autorcvbuf_max &&
	    sb_length > (sbspace(sb) / 8 * 7)) {
		unsigned int hiwat = sb->sb_hiwat;
		unsigned int newsize = min(hiwat + V_tcp_autorcvbuf_inc,
		    V_tcp_autorcvbuf_max);

		if (!sbreserve_locked(sb, newsize, so, NULL))
			sb->sb_flags &= ~SB_AUTOSIZE;
		else
			toep->rx_credits += newsize - hiwat;
	}

	KASSERT(toep->sb_cc >= sbused(sb),
	    ("%s: sb %p has more data (%d) than last time (%d).",
	    __func__, sb, sbused(sb), toep->sb_cc));
	toep->rx_credits += toep->sb_cc - sbused(sb);
	sbappendstream_locked(sb, m, 0);
	toep->sb_cc = sbused(sb);
#ifdef VERBOSE_TRACES
	CTR5(KTR_CXGBE, "%s: tid %u PDU overhead %d rx_credits %u rcv_wnd %u",
	    __func__, tid, pdu_overhead, toep->rx_credits, tp->rcv_wnd);
#endif
	if (toep->rx_credits > 0 && toep->sb_cc + tp->rcv_wnd < sb->sb_lowat) {
		int credits;

		credits = send_rx_credits(sc, toep, toep->rx_credits);
		toep->rx_credits -= credits;
		tp->rcv_wnd += credits;
		tp->rcv_adv += credits;
	}

	sorwakeup_locked(so);
	SOCKBUF_UNLOCK_ASSERT(sb);

	INP_WUNLOCK(inp);
	CURVNET_RESTORE();
	return (0);
}

#ifdef KERN_TLS
SYSCTL_NODE(_debug, OID_AUTO, t6tls, CTLFLAG_RD, 0, "T6 TLS");

static char tls_key[MAX_CIPHER_KSZ];
SYSCTL_OPAQUE(_debug_t6tls, OID_AUTO, key, CTLFLAG_RD, tls_key, sizeof(tls_key),
    "S", "Last cipher key");

static int tls_key_len;
SYSCTL_INT(_debug_t6tls, OID_AUTO, key_len, CTLFLAG_RD, &tls_key_len, 0,
    "Last cipher key length");

static char tls_iv[SALT_SIZE];
SYSCTL_OPAQUE(_debug_t6tls, OID_AUTO, iv, CTLFLAG_RD, tls_iv, sizeof(tls_iv),
    "S", "Last IV (salt)");

static char tls_record[16384];
SYSCTL_OPAQUE(_debug_t6tls, OID_AUTO, record, CTLFLAG_RD, tls_record,
    sizeof(tls_record), "S", "Last plaintext record ommitting trailer");

static int tls_record_len;
SYSCTL_INT(_debug_t6tls, OID_AUTO, record_len, CTLFLAG_RD, &tls_record_len, 0,
    "Last record length");

static struct protosw *tcp_protosw;

static int sbtls_parse_pkt(struct t6_sbtls_cipher *cipher, struct mbuf *m,
    int *nsegsp, int *len16p);
static int sbtls_write_wr(struct t6_sbtls_cipher *cipher, struct sge_txq *txq,
    void *wr, struct mbuf *m, u_int nsegs, u_int available);

static void
init_sbtls_k_ctx(struct tls_key_context *k_ctx, struct tls_so_enable *en,
    struct sbtls_info *tls)
{
	int mac_key_size;

	k_ctx->proto_ver = en->tls_vmajor << 8 | en->tls_vminor;
	k_ctx->mac_secret_size = en->hmac_key_len;
	k_ctx->cipher_secret_size = en->key_size;
	k_ctx->tx_key_info_size = sizeof(struct tx_keyctx_hdr) +
	    k_ctx->cipher_secret_size;
	if (en->crypt_algorithm == CRYPTO_AES_NIST_GCM_16) {
		k_ctx->state.auth_mode = CHSSL_GHASH;
		k_ctx->state.enc_mode = CH_EVP_CIPH_GCM_MODE;
		k_ctx->iv_size = 4;
		k_ctx->mac_first = 0;
		k_ctx->hmac_ctrl = 0;
		k_ctx->tx_key_info_size += GMAC_BLOCK_LEN;
	} else {
		switch (en->mac_algorthim) {
		case CRYPTO_SHA1_HMAC:
			mac_key_size = roundup2(SHA1_HASH_LEN, 16);
			k_ctx->state.auth_mode = CHSSL_SHA1;
			break;
		case CRYPTO_SHA2_256_HMAC:
			mac_key_size = SHA2_256_HASH_LEN;
			k_ctx->state.auth_mode = CHSSL_SHA256;
			break;
		case CRYPTO_SHA2_384_HMAC:
			mac_key_size = SHA2_512_HASH_LEN;
			k_ctx->state.auth_mode = CHSSL_SHA512_384;
			break;
		case CRYPTO_SHA2_512_HMAC:
			mac_key_size = SHA2_512_HASH_LEN;
			k_ctx->state.auth_mode = CHSSL_SHA512_512;
			break;
		}
		k_ctx->state.enc_mode = CH_EVP_CIPH_CBC_MODE;
		k_ctx->iv_size = 8; /* for CBC, iv is 16B, unit of 2B */
		k_ctx->mac_first = 1;
		k_ctx->tx_key_info_size += mac_key_size * 2;
	}

	k_ctx->iv_ctrl = 0;
	k_ctx->iv_algo = 0;
	k_ctx->frag_size = tls->sb_params.sb_maxlen;
}

static int
send_sbtls_act_open_req(struct adapter *sc, struct vi_info *vi,
    struct socket *so, struct toepcb *toep)
{
	struct cpl_t6_act_open_req *cpl6;
	struct cpl_act_open_req *cpl;
	struct inpcb *inp;
	struct wrqe *wr;
	uint64_t options;
	int error, mtu_idx, qid_atid;

	inp = so->so_pcb;
	toep->vnet = so->so_vnet;

	wr = alloc_wrqe(sizeof(*cpl6), toep->ctrlq);
	if (wr == NULL)
		return (ENOMEM);

	cpl6 = wrtod(wr);
	cpl = (struct cpl_act_open_req *)cpl6;
	INIT_TP_WR(cpl6, 0);
	mtu_idx = find_best_mtu_idx(sc, &inp->inp_inc, 0);
	qid_atid = V_TID_QID(sc->sge.fwq.abs_id) | V_TID_TID(toep->tid);
	OPCODE_TID(cpl) = htobe32(MK_OPCODE_TID(CPL_ACT_OPEN_REQ,
		qid_atid));
	inp_4tuple_get(inp, &cpl->local_ip, &cpl->local_port,
	    &cpl->peer_ip, &cpl->peer_port);

	options = F_TCAM_BYPASS | V_MSS_IDX(mtu_idx) |
	    V_ULP_MODE(ULP_MODE_NONE);
	options |= V_L2T_IDX(toep->l2te->idx);
	options |= V_SMAC_SEL(vi->smt_idx) | V_TX_CHAN(vi->pi->tx_chan);
	options |= F_NON_OFFLOAD;
#ifdef notsure
	options |= V_ACCEPT_MODE(2);
#endif
	cpl->opt0 = htobe64(options);

	cpl6->params = select_ntuple(vi, toep->l2te);

	options = V_TX_QUEUE(sc->params.tp.tx_modq[vi->pi->tx_chan]);
	cpl->opt2 = htobe32(options);

#ifdef notsure
	/* Code from t4_connect */
	cpl6->iss = htobe32(tp->iss);
#endif
	error = t4_l2t_send(sc, wr, toep->l2te);
	if (error == 0)
		toep->flags |= TPF_CPL_PENDING;
	else
		free_wrqe(wr);
	return (error);
};

void
sbtls_act_open_rpl(struct adapter *sc, struct toepcb *toep, u_int status,
    const struct cpl_act_open_rpl *cpl)
{
	struct inpcb *inp = toep->inp;

	free_atid(sc, toep->tid);
	if (status == 0)
		toep->tid = GET_TID(cpl);
	else
		toep->tid = -1;

	INP_WLOCK(inp);
	toep->flags &= ~TPF_CPL_PENDING;
	wakeup(toep);
	INP_WUNLOCK(inp);
}

static void
write_set_tcb_field(struct toepcb *toep, void *dst, uint16_t word,
    uint64_t mask, uint64_t val)
{
	struct cpl_set_tcb_field *cpl;

	cpl = dst;
	INIT_TP_WR_MIT_CPL(cpl, CPL_SET_TCB_FIELD, toep->tid);
	cpl->reply_ctrl = htobe16(F_NO_REPLY);
	cpl->word_cookie = htobe16(V_WORD(word));
	cpl->mask = htobe64(mask);
	cpl->val = htobe64(val);
}

static int
sbtls_set_tcb_field(struct toepcb *toep, struct sge_txq *txq, uint16_t word,
    uint64_t mask, uint64_t val)
{
	struct mbuf *m;
	void *items[1];
	int error;

	m = alloc_wr_mbuf(sizeof(struct cpl_set_tcb_field), M_NOWAIT);
	if (m == NULL)
		return (ENOMEM);
	write_set_tcb_field(toep, mtod(m, void *), word, mask, val);
	items[0] = m;
	error = mp_ring_enqueue(txq->r, items, 1, 1);
	if (error)
		m_free(m);
	return (error);
}

static int
t6_sbtls_try(struct socket *so, struct tls_so_enable *en, int *errorp)
{
	struct t6_sbtls_cipher *cipher;
	struct sbtls_info *tls;
	struct tls_key_context *k_ctx;
	struct tls_ofld_info *tls_ofld;
	struct mbuf *key_wr;
	struct sockaddr_in sin;
	struct sockaddr *nam;
	struct toepcb *toep;
	struct adapter *sc;
	struct vi_info *vi;
	struct ifnet *ifp;
	struct rtentry *rt;
	struct inpcb *inp;
	struct sge_txq *txq;
	int error, keyid, len, proto_ver;
	bool using_atid;

	/* Sanity check values in *en. */
	if (en->key_size != en->crypt_key_len)
		return (EINVAL);
	switch (en->crypt_algorithm) {
#ifdef notyet
	case CRYPTO_AES_CBC:
		/* XXX: Not sure if CBC uses a 4 byte IV for TLS? */
		if (en->iv_len != SALT_SIZE)
			return (EINVAL);
		switch (en->key_size) {
		case 128 / 8:
		case 192 / 8:
		case 256 / 8:
			break;
		default:
			return (EINVAL);
		}
		switch (en->mac_algorthim) {
		case CRYPTO_SHA1_HMAC:
		case CRYPTO_SHA2_256_HMAC:
		case CRYPTO_SHA2_384_HMAC:
		case CRYPTO_SHA2_512_HMAC:
			break;
		default:
			return (EPROTONOSUPPORT);
		}
		break;
#endif
	case CRYPTO_AES_NIST_GCM_16:
		if (en->iv_len != SALT_SIZE)
			return (EINVAL);
		switch (en->key_size) {
		case 128 / 8:
			if (en->mac_algorthim != CRYPTO_AES_128_NIST_GMAC)
				return (EINVAL);
			break;
		case 192 / 8:
			if (en->mac_algorthim != CRYPTO_AES_192_NIST_GMAC)
				return (EINVAL);
			break;
		case 256 / 8:
			if (en->mac_algorthim != CRYPTO_AES_256_NIST_GMAC)
				return (EINVAL);
			break;
		default:
			return (EINVAL);
		}
		break;
	default:
		return (EPROTONOSUPPORT);
	}
	proto_ver = en->tls_vmajor << 8 | en->tls_vminor;
	if (get_proto_ver(proto_ver) > DTLS_1_2_VERSION)
		return (EPROTONOSUPPORT);

	/*
	 * Perform routing lookup to find ifnet.  Reject if it is not
	 * on a T6 or on a T6 that doesn't support TLS.
	 *
	 * XXX: Only IPv4 currently.
	 */
	if (so->so_proto != tcp_protosw)
		return (EPROTONOSUPPORT);
	inp = so->so_pcb;
	INP_WLOCK_ASSERT(inp);
	rt = inp->inp_route.ro_rt;
	if (rt == NULL || rt->rt_ifp == NULL)
		return (ENXIO);

	/* XXX: Gross */
	ifp = rt->rt_ifp;
	if (ifp->if_get_counter != cxgbe_get_counter)
		return (ENXIO);
	vi = ifp->if_softc;
	sc = vi->pi->adapter;

	/*
	 * XXX: This requires TOE to be activated so that the atid table
	 * and TLS key map are initialized.
	 */
	if (!(sc->flags & KERN_TLS_OK) || !can_tls_offload(sc) ||
	    sc->tom_softc == NULL)
		return (ENXIO);

	toep = alloc_toepcb(vi, -1, -1, M_NOWAIT);
	if (toep == NULL)
		return (ENOMEM);
	toep->flags |= TPF_KERN_TLS;
	set_ulp_mode(toep, ULP_MODE_NONE);

	key_wr = NULL;
	using_atid = true;
	toep->tid = alloc_atid(sc, toep);
	if (toep->tid < 0) {
		error = ENOMEM;
		goto failed;
	}

	if (rt->rt_flags & RTF_GATEWAY)
		nam = rt->rt_gateway;
	else {
		nam = (struct sockaddr *)&sin;
		bzero(&sin, sizeof(sin));
		sin.sin_len = sizeof(sin);
		sin.sin_family = AF_INET;
		sin.sin_port = inp->inp_inc.inc_ie.ie_fport;
		sin.sin_addr = inp->inp_inc.inc_ie.ie_faddr;
	}
	toep->l2te = t4_l2t_get(vi->pi, ifp, nam);
	if (toep->l2te == NULL) {
		error = ENOMEM;
		goto failed;
	}

#ifdef KEY_IN_DDR
	keyid = get_new_keyid(toep);
	if (keyid < 0) {
		error = ENOMEM;
		goto failed;
	}
	tls_ofld = &toep->tls;
	tls_ofld->tx_key_addr = keyid;
	CTR3(KTR_CXGBE, "%s: atid %d allocated TX key addr %#x", __func__,
	    toep->tid, tls_ofld->tx_key_addr);
#else
	keyid = -1;
	tls_ofld = &toep->tls;
	CTR2(KTR_CXGBE, "%s: atid %d using immediate key ctx", __func__,
	    toep->tid);
	tls_ofld->key_location = TLS_SFO_WR_CONTEXTLOC_IMMEDIATE;
#endif

	toep->inp = inp;
	error = send_sbtls_act_open_req(sc, vi, so, toep);
	if (error)
		goto failed;

	/*
	 * Wait for reply to active open.
	 *
	 * XXX: What about rm lock?  Can't sleep while that is held.
	 *
	 * XXX: Probably need to recheck INP validity here and and in
	 * the try loop in sbtls_crypt_tls_enable().
	 */
	CTR2(KTR_CXGBE, "%s: atid %d sent CPL_ACT_OPEN_REQ", __func__,
	    toep->tid);
	while (toep->flags & TPF_CPL_PENDING) {
		/*
		 * XXX: PCATCH?  We would then have to discard the PCB
		 * when the completion CPL arrived.
		 */
		error = rw_sleep(toep, &inp->inp_lock, 0, "t6tlsop", 0);
	}

	using_atid = false;
	if (toep->tid < 0) {
		error = ENOMEM;
		goto failed;
	}

	txq = &sc->sge.txq[vi->first_txq];
	if (inp->inp_flowtype != M_HASHTYPE_NONE)
		txq += ((inp->inp_flowid % (vi->ntxq - vi->rsrv_noflowq)) +
		    vi->rsrv_noflowq);

	/* Clear TF_NON_OFFLOAD and set TF_CORE_BYPASS */
	error = sbtls_set_tcb_field(toep, txq, W_TCB_T_FLAGS,
	    V_TCB_T_FLAGS(V_TF_CORE_BYPASS(1) | V_TF_NON_OFFLOAD(1)),
	    V_TCB_T_FLAGS(V_TF_CORE_BYPASS(1)));
	if (error)
		goto failed;
#ifdef notsure
	error = sbtls_set_tcb_field(toep, txq, W_TCB_TX_MAX,
	    V_TCB_TX_MAX(M_TCB_TX_MAX), 0);
	if (error)
		goto failed;
#endif

	/*
	 * Preallocate a work request mbuf to hold the work request
	 * that programs the transmit key.  The work request isn't
	 * populated until the setup_cipher callback since the keys
	 * aren't available yet.  However, if that callback fails the
	 * socket won't fall back to software encryption, so the
	 * allocation is done here where failure can be handled more
	 * gracefully.
	 */
	len = roundup2(sizeof(struct tls_key_req), 16) +
	    roundup2(sizeof(struct tls_keyctx), 32);
#ifdef KEY_IN_DDR
	key_wr = alloc_wr_mbuf(len, M_NOWAIT);
	if (key_wr == NULL) {
		error = ENOMEM;
		goto failed;
	}
#endif

	tls = sbtls_init_sb_tls(so, en, sizeof(struct t6_sbtls_cipher));
	if (tls == NULL) {
		error = ENOMEM;
		goto failed;
	}
	cipher = tls->cipher;
	cipher->parse_pkt = sbtls_parse_pkt;
	cipher->write_tls_wr = sbtls_write_wr;
	cipher->sc = sc;
	cipher->toep = toep;
	cipher->txq = txq;
	cipher->key_wr = key_wr;

	k_ctx = &tls_ofld->k_ctx;
	init_sbtls_k_ctx(k_ctx, en, tls);

	tls_ofld->scmd0.seqno_numivs = htobe32(V_SCMD_SEQ_NO_CTRL(3) |
	    V_SCMD_PROTO_VERSION(get_proto_ver(k_ctx->proto_ver)) |
	    V_SCMD_ENC_DEC_CTRL(SCMD_ENCDECCTRL_ENCRYPT) |
	    V_SCMD_CIPH_AUTH_SEQ_CTRL((k_ctx->mac_first == 0)) |
	    V_SCMD_CIPH_MODE(k_ctx->state.enc_mode) |
	    V_SCMD_AUTH_MODE(k_ctx->state.auth_mode) |
	    V_SCMD_HMAC_CTRL(k_ctx->hmac_ctrl) |
	    V_SCMD_IV_SIZE(k_ctx->iv_size) | V_SCMD_NUM_IVS(1));

	tls_ofld->scmd0.ivgen_hdrlen = htobe32(
	    V_SCMD_IV_GEN_CTRL(k_ctx->iv_ctrl) |
#ifdef KEY_IN_DDR
	    V_SCMD_KEY_CTX_INLINE(0) |
#else
	    V_SCMD_KEY_CTX_INLINE(1) |
#endif
	    V_SCMD_TLS_FRAG_ENABLE(0));

	tls_ofld->mac_length = k_ctx->mac_secret_size;

	if (en->crypt_algorithm == CRYPTO_AES_NIST_GCM_16) {
		tls->sb_params.sb_tls_hlen = TLS_HEADER_LENGTH +
		    AEAD_EXPLICIT_DATA_SIZE;
		tls->sb_params.sb_tls_tlen = GCM_TAG_SIZE;
#ifdef notyet
	} else {
		tls->sb_params.sb_tls_hlen = TLS_HEADER_LENGTH +
		    CIPHER_BLOCK_SIZE;
		/* XXX: Padding */
		tls->sb_params.sb_tls_tlen = tls_ofld->mac_length;
#endif
	}
	tls->t_type = SBTLS_T_TYPE_CHELSIO;
	so->so_snd.sb_tls_flags |= SB_TLS_IFNET;
	return (0);

failed:
	if (key_wr != NULL)
		m_free(key_wr);
	if (toep->tid >= 0) {
		if (using_atid)
			free_atid(sc, toep->tid);
		else {
			remove_tid(sc, toep->tid, 1);
			release_tid(sc, toep->tid, toep->ctrlq);
		}
	}
	if (toep->l2te)
		t4_l2t_release(toep->l2te);
	free_toepcb(toep);
	return (error);
}

/* XXX: Should share this with ccr(4) eventually. */
static void
init_sbtls_gmac_hash(const char *key, int klen, char *ghash)
{
	static char zeroes[GMAC_BLOCK_LEN];
	uint32_t keysched[4 * (RIJNDAEL_MAXNR + 1)];
	int rounds;

	rounds = rijndaelKeySetupEnc(keysched, key, klen);
	rijndaelEncrypt(keysched, rounds, zeroes, ghash);
}

static void
t6_sbtls_setup_cipher(struct sbtls_info *tls, int *error)
{
	struct t6_sbtls_cipher *cipher = tls->cipher;
	struct toepcb *toep = cipher->toep;
	struct tls_ofld_info *tls_ofld = &toep->tls;
	struct tls_key_context *k_ctx;
#ifdef KEY_IN_DDR
	int keyid, kwrlen, kctxlen, len;
	struct tls_key_req *kwr;
	struct tls_keyctx *kctx;
	void *items[1];
#else
	struct tx_keyctx_hdr *khdr;
	unsigned int ck_size, mk_size;
#endif

	/* INP_WLOCK_ASSERT(inp); */

	/* Load keys into key context. */
	k_ctx = &tls_ofld->k_ctx;
	k_ctx->l_p_key = KEY_WRITE_TX;
	if (tls->sb_params.iv == NULL || tls->sb_params.crypt == NULL) {
		*error = EINVAL;
		return;
	}
#ifdef KEY_IN_DDR
	memcpy(k_ctx->tx.salt, tls->sb_params.iv, SALT_SIZE);
	memcpy(k_ctx->tx.key, tls->sb_params.crypt,
	    tls->sb_params.crypt_key_len);
	if (k_ctx->state.enc_mode == CH_EVP_CIPH_GCM_MODE) {
		init_sbtls_gmac_hash(tls->sb_params.crypt,
		    tls->sb_params.crypt_key_len * 8,
		    k_ctx->tx.key + tls->sb_params.crypt_key_len);
#ifdef notyet
	} else {
		/* Generate ipad and opad and append after key. */
		/*
		 * XXX: Probably want to share ccr_init_hmac_digest
		 * here rather than reimplementing.
		 */
#endif
	}
#else
	/*
	 * For inline keys, store the full key context inline on top of
	 * k_ctx->tx.  This is a bit of a gross hack, but safe because
	 * there is room in k_ctx->rx for the overflow.
	 */
	khdr = (void *)&k_ctx->tx;
	ck_size = k_ctx->cipher_secret_size;
	mk_size = k_ctx->mac_secret_size;

	khdr->ctxlen = (k_ctx->tx_key_info_size >> 4);
	khdr->dualck_to_txvalid = V_TLS_KEYCTX_TX_WR_SALT_PRESENT(1) |
	    V_TLS_KEYCTX_TX_WR_TXCK_SIZE(get_cipher_key_size(ck_size)) |
	    V_TLS_KEYCTX_TX_WR_TXMK_SIZE(get_mac_key_size(mk_size)) |
	    V_TLS_KEYCTX_TX_WR_TXVALID(1);
	if (k_ctx->state.enc_mode != CH_EVP_CIPH_GCM_MODE)
		khdr->dualck_to_txvalid |=
		    V_TLS_KEYCTX_TX_WR_TXOPAD_PRESENT(1);
	khdr->dualck_to_txvalid = htobe16(khdr->dualck_to_txvalid);
	memcpy(khdr->txsalt, tls->sb_params.iv, SALT_SIZE);
	memcpy(khdr + 1, tls->sb_params.crypt, tls->sb_params.crypt_key_len);
	if (k_ctx->state.enc_mode == CH_EVP_CIPH_GCM_MODE) {
		init_sbtls_gmac_hash(tls->sb_params.crypt,
		    tls->sb_params.crypt_key_len * 8,
		    (char *)(khdr + 1) + tls->sb_params.crypt_key_len);
#ifdef notyet
	} else {
		/* Generate ipad and opad and append after key. */
		/*
		 * XXX: Probably want to share ccr_init_hmac_digest
		 * here rather than reimplementing.
		 */
#endif
	}
#endif

#if 1
	memcpy(tls_key, tls->sb_params.crypt, tls->sb_params.crypt_key_len);
	tls_key_len = tls->sb_params.crypt_key_len;
	memcpy(tls_iv, tls->sb_params.iv, SALT_SIZE);
#endif

#ifdef KEY_IN_DDR
	keyid = tls_ofld->tx_key_addr;

	/* Populate key work request. */
	kwrlen = roundup2(sizeof(*kwr), 16);
	kctxlen = roundup2(sizeof(*kctx), 32);
	len = kwrlen + kctxlen;

	MPASS(cipher->key_wr->m_len == len);
	kwr = mtod(cipher->key_wr, void *);
	memset(kwr, 0, kwrlen);

	kwr->wr_hi = htobe32(V_FW_WR_OP(FW_ULPTX_WR) |
	    F_FW_WR_ATOMIC);
	kwr->wr_mid = htobe32(V_FW_WR_LEN16(DIV_ROUND_UP(len, 16)) |
	    V_FW_WR_FLOWID(toep->tid));
	kwr->protocol = get_proto_ver(k_ctx->proto_ver);
	kwr->mfs = htons(k_ctx->frag_size);
	kwr->reneg_to_write_rx = k_ctx->l_p_key;

	/* master command */
	kwr->cmd = htobe32(V_ULPTX_CMD(ULP_TX_MEM_WRITE) |
	    V_T5_ULP_MEMIO_ORDER(1) | V_T5_ULP_MEMIO_IMM(1));
	kwr->dlen = htobe32(V_ULP_MEMIO_DATA_LEN(kctxlen >> 5));
	kwr->len16 = htobe32((toep->tid << 8) |
	    DIV_ROUND_UP(len - sizeof(struct work_request_hdr), 16));
	kwr->kaddr = htobe32(V_ULP_MEMIO_ADDR(keyid >> 5));

	/* sub command */
	kwr->sc_more = htobe32(V_ULPTX_CMD(ULP_TX_SC_IMM));
	kwr->sc_len = htobe32(kctxlen);

	/* XXX: This assumes that kwrlen == sizeof(*kwr). */
	kctx = (struct tls_keyctx *)(kwr + 1);
	memset(kctx, 0, kctxlen);

	prepare_txkey_wr(kctx, k_ctx);

	/*
	 * Place the key work request in the transmit queue.  It
	 * should be sent to the NIC before any TLS packets using this
	 * session.
	 */
	items[0] = cipher->key_wr;
	*error = mp_ring_enqueue(cipher->txq->r, items, 1, 1);
	if (*error == 0) {
		cipher->key_wr = NULL;
		CTR2(KTR_CXGBE, "%s: tid %d sent key WR", __func__, toep->tid);
	}
#endif
}

static u_int
sbtls_base_wr_size(struct toepcb *toep)
{
	u_int wr_len;

	wr_len = sizeof(struct fw_ulptx_wr);	// 16
	wr_len += sizeof(struct ulp_txpkt);	// 8
	wr_len += sizeof(struct ulptx_idata);	// 8
	wr_len += sizeof(struct cpl_tx_sec_pdu);// 32
	wr_len += key_size(toep);		// 16
	wr_len += sizeof(struct cpl_tx_data);	// 16
	return (wr_len);
}

static u_int
sbtls_sgl_size(u_int nsegs)
{
	u_int wr_len;

	/* First segment is part of ulptx_sgl. */
	nsegs--;

	wr_len = sizeof(struct ulptx_sgl);
	wr_len += 8 * ((3 * nsegs) / 2 + (nsegs & 1));
	return (wr_len);
}

static int
sbtls_parse_pkt(struct t6_sbtls_cipher *cipher, struct mbuf *m, int *nsegsp,
    int *len16p)
{
	struct mbuf_ext_pgs *ext_pgs;
	struct tls_record_layer *hdr;
	struct ether_header *eh;
	struct ip *ip;
	struct tcphdr *tcp;
	struct mbuf *m_tls;
	u_int plen, wr_len;

	/*
	 * Locate headers in initial mbuf.
	 * XXX: This assumes all of the headers are in the initial mbuf.
	 * Could perhaps use m_advance() like parse_pkt() if that turns
	 * out to not be true.
	 */
	M_ASSERTPKTHDR(m);
	if (m->m_len <= sizeof(*eh) + sizeof(*ip)) {
		CTR2(KTR_CXGBE, "%s: tid %d header mbuf too short", __func__,
		    cipher->toep->tid);
		return (EINVAL);
	}
	eh = mtod(m, struct ether_header *);
	if (ntohs(eh->ether_type) != ETHERTYPE_IP) {
		CTR2(KTR_CXGBE, "%s: tid %d mbuf not ETHERTYPE_IP", __func__,
		    cipher->toep->tid);
		return (EINVAL);
	}
	m->m_pkthdr.l2hlen = sizeof(*eh);

	/* XXX: Reject unsupported IP options? */
	ip = (struct ip *)(eh + 1);
	if (ip->ip_p != IPPROTO_TCP) {
		CTR2(KTR_CXGBE, "%s: tid %d mbuf not IPPROTO_TCP", __func__,
		    cipher->toep->tid);
		return (EINVAL);
	}
	m->m_pkthdr.l3hlen = ip->ip_hl * 4;
	if (m->m_len <= m->m_pkthdr.l2hlen + m->m_pkthdr.l3hlen +
	    sizeof(*tcp)) {
		CTR2(KTR_CXGBE, "%s: tid %d header mbuf too short (2)",
		    __func__, cipher->toep->tid);
		return (EINVAL);
	}
	tcp = (struct tcphdr *)((char *)ip + m->m_pkthdr.l3hlen);
	m->m_pkthdr.l4hlen = tcp->th_off * 4;

	/* XXX: Reject unsupported TCP options? */

	/* Find the TLS record. */
	for (m_tls = m;; m_tls = m_tls->m_next)
		if (m_tls->m_flags & M_NOMAP)
			break;

	/* Assume all headers are in 'm' for now. */
	MPASS(m->m_next == m_tls);

	/*
	 * Determine the size of the entire TLS record payload
	 * excluding the trailer.
	 */
	MBUF_EXT_PGS_ASSERT(m_tls);
	ext_pgs = (void *)m_tls->m_ext.ext_buf;
	hdr = (void *)ext_pgs->hdr;
	plen = ext_pgs->hdr_len + ntohs(hdr->tls_length);

	/* Calculate the size of the work request. */
	wr_len = sbtls_base_wr_size(cipher->toep);
	/* XXX: immediate data eventually */
#ifdef notyet
	if (plen <= SGE_MAX_WR_LEN - wr_len) {
		*nsegsp = 0;
		wr_len += plen;
	} else {
#else
	{
#endif
		/*
		 * XXX: We can't modify the TLS header in ext_pgs as then
		 * the header length would be wrong for a retransmit.
		 * Instead, use the empty space in the header mbuf after
		 * the TCP header as a scratch area to write the
		 * TLS header.  This is a gross hack, but if we are able
		 * to write the TLS header as an immediate in the WR
		 * in the future we can remove it.  This trick will only
		 * work safely if we are always going to generate the
		 * same TLS header contents which is true for GCM at
		 * least.
		 */
		if (M_TRAILINGSPACE(m) < ext_pgs->hdr_len) {
			CTR3(KTR_CXGBE,
			    "%s: tid %d no room for TLS header (%u)",
			    __func__, cipher->toep->tid, M_TRAILINGSPACE(m));
			return (EINVAL);
		}
		*nsegsp = 1;

		/* XXX: eventually make tls_hdr always immediate? */
		*nsegsp += sglist_count_ext_pgs(ext_pgs, ext_pgs->hdr_len,
		    plen - ext_pgs->hdr_len);

		wr_len += sbtls_sgl_size(*nsegsp);
	}
	wr_len = roundup2(wr_len, 16);
	CTR4(KTR_CXGBE, "%s: tid %d wr_len %d nsegs %d", __func__,
	    cipher->toep->tid, wr_len, *nsegsp);
	if (wr_len > SGE_MAX_WR_LEN || *nsegsp > TX_SGL_SEGS)
		return (EFBIG);

	/*
	 * Include room for up to 5 SET_TCB requests before the real
	 * work request.
	 */
	wr_len += 5 * roundup2(sizeof(struct cpl_set_tcb_field), 16);
	*len16p = wr_len / 16;
	CTR3(KTR_CXGBE, "%s: tid %d len16 %d", __func__,
	    cipher->toep->tid, *len16p);
	return (0);
}

static void *
txq_advance(struct sge_txq *txq, void *wr, u_int len)
{
	struct sge_eq *eq = &txq->eq;
	uintptr_t ptr = (uintptr_t)wr;
	uintptr_t start = (uintptr_t)&eq->desc[0];
	uintptr_t end = (uintptr_t)&eq->desc[eq->sidx];

	MPASS(ptr >= start);
	MPASS(ptr < end);
	KASSERT(ptr + len <= end, ("%s: previous item overran txq", __func__));

	if (__predict_true(ptr + len < end))
		return ((void *)(ptr + len));
	else
		return ((void *)start);
}

static __be64
get_flit(struct sglist_seg *segs, int nsegs, int idx)
{
	int i = (idx / 3) * 2;

	switch (idx % 3) {
	case 0: {
		uint64_t rc;

		rc = (uint64_t)segs[i].ss_len << 32;
		if (i + 1 < nsegs)
			rc |= (uint64_t)(segs[i + 1].ss_len);

		return (htobe64(rc));
	}
	case 1:
		return (htobe64(segs[i].ss_paddr));
	case 2:
		return (htobe64(segs[i + 1].ss_paddr));
	}

	return (0);
}

/*
 * If the SGL ends on an address that is not 16 byte aligned, this function will
 * add a 0 filled flit at the end.
 */
static void
write_gl_to_txd(struct sge_txq *txq, caddr_t to)
{
	struct sge_eq *eq = &txq->eq;
	struct sglist *gl = txq->gl;
	struct sglist_seg *seg;
	__be64 *flitp, *wrap;
	struct ulptx_sgl *usgl;
	int i, nflits, nsegs;

	KASSERT(((uintptr_t)to & 0xf) == 0,
	    ("%s: SGL must start at a 16 byte boundary: %p", __func__, to));
	MPASS((uintptr_t)to >= (uintptr_t)&eq->desc[0]);
	MPASS((uintptr_t)to < (uintptr_t)&eq->desc[eq->sidx]);

	nsegs = gl->sg_nseg;
	MPASS(nsegs > 0);

	nflits = (3 * (nsegs - 1)) / 2 + ((nsegs - 1) & 1) + 2;
	flitp = (__be64 *)to;
	wrap = (__be64 *)(&eq->desc[eq->sidx]);
	seg = &gl->sg_segs[0];
	usgl = (void *)flitp;

	/*
	 * We start at a 16 byte boundary somewhere inside the tx descriptor
	 * ring, so we're at least 16 bytes away from the status page.  There is
	 * no chance of a wrap around in the middle of usgl (which is 16 bytes).
	 */

	usgl->cmd_nsge = htobe32(V_ULPTX_CMD(ULP_TX_SC_DSGL) |
	    V_ULPTX_NSGE(nsegs));
	usgl->len0 = htobe32(seg->ss_len);
	usgl->addr0 = htobe64(seg->ss_paddr);
	seg++;

	if ((uintptr_t)(flitp + nflits) <= (uintptr_t)wrap) {

		/* Won't wrap around at all */

		for (i = 0; i < nsegs - 1; i++, seg++) {
			usgl->sge[i / 2].len[i & 1] = htobe32(seg->ss_len);
			usgl->sge[i / 2].addr[i & 1] = htobe64(seg->ss_paddr);
		}
		if (i & 1)
			usgl->sge[i / 2].len[1] = htobe32(0);
		flitp += nflits;
	} else {

		/* Will wrap somewhere in the rest of the SGL */

		/* 2 flits already written, write the rest flit by flit */
		flitp = (void *)(usgl + 1);
		for (i = 0; i < nflits - 2; i++) {
			if (flitp == wrap)
				flitp = (void *)eq->desc;
			*flitp++ = get_flit(seg, nsegs - 1, i);
		}
	}

	if (nflits & 1) {
		MPASS(((uintptr_t)flitp) & 0xf);
		*flitp++ = 0;
	}

	MPASS((((uintptr_t)flitp) & 0xf) == 0);
}

_Static_assert(sizeof(struct cpl_set_tcb_field) <= EQ_ESIZE,
    "CPL_SET_TCB_FIELD must be smaller than a single TX descriptor");

static int
sbtls_write_wr(struct t6_sbtls_cipher *cipher, struct sge_txq *txq, void *dst,
    struct mbuf *m, u_int nsegs, u_int available)
{
	struct sge_eq *eq = &txq->eq;
	struct tx_sdesc *txsd;
	struct toepcb *toep;
	struct fw_ulptx_wr *wr;
	struct ulp_txpkt *txpkt;
	struct ulptx_idata *idata;
	struct cpl_tx_sec_pdu *sec_pdu;
	struct cpl_tx_data *tx_data;
	struct mbuf_ext_pgs *ext_pgs;
	struct tls_record_layer *hdr, *inhdr;
	struct tcphdr *tcp;
	struct mbuf *m_tls;
	u_int aad_start, aad_stop;
	u_int auth_start, auth_stop, auth_insert;
	u_int cipher_start, cipher_stop;
	u_int imm_len, mss, ndesc, plen, tcp_seqno, wr_len;

	ndesc = 0;
	toep = cipher->toep;
	MPASS(cipher->txq == txq);

	/* Find the TLS record. */
	for (m_tls = m;; m_tls = m_tls->m_next)
		if (m_tls->m_flags & M_NOMAP)
			break;

	/* Determine the TCP sequence number of the start of the record. */
	tcp = (struct tcphdr *)(mtod(m, char *) + m->m_pkthdr.l2hlen +
	    m->m_pkthdr.l3hlen);
	tcp_seqno = ntohl(tcp->th_seq) - mtod(m_tls, vm_offset_t);

	/* Flesh out TLS header */
	MBUF_EXT_PGS_ASSERT(m_tls);
	ext_pgs = (void *)m_tls->m_ext.ext_buf;
	inhdr = (void *)ext_pgs->hdr;
	hdr = mtodo(m, m->m_len);
	plen = ext_pgs->hdr_len + ntohs(inhdr->tls_length);
	hdr->tls_type = inhdr->tls_type;
	hdr->tls_vmajor = inhdr->tls_vmajor;
	hdr->tls_vminor = inhdr->tls_vminor;
	hdr->tls_length = htons(plen + ext_pgs->trail_len - TLS_HEADER_LENGTH);
	*(uint64_t *)(hdr + 1) = htobe64(ext_pgs->seqno);

	/* Update TCB fields. */
	write_set_tcb_field(toep, dst, W_TCB_TX_MAX, V_TCB_TX_MAX(M_TCB_TX_MAX),
	    V_TCB_TX_MAX(tcp_seqno));
	dst = txq_advance(txq, dst, EQ_ESIZE);
	ndesc++;
	write_set_tcb_field(toep, dst, W_TCB_SND_NXT_RAW,
	    V_TCB_SND_NXT_RAW(M_TCB_SND_NXT_RAW), V_TCB_SND_NXT_RAW(0));
	dst = txq_advance(txq, dst, EQ_ESIZE);
	ndesc++;
	write_set_tcb_field(toep, dst, W_TCB_SND_MAX_RAW,
	    V_TCB_SND_MAX_RAW(M_TCB_SND_MAX_RAW), V_TCB_SND_MAX_RAW(0));
	dst = txq_advance(txq, dst, EQ_ESIZE);
	ndesc++;
	write_set_tcb_field(toep, dst, W_TCB_SND_UNA_RAW,
	    V_TCB_SND_UNA_RAW(M_TCB_SND_UNA_RAW), V_TCB_SND_UNA_RAW(0));
	dst = txq_advance(txq, dst, EQ_ESIZE);
	ndesc++;
	write_set_tcb_field(toep, dst, W_TCB_RCV_NXT,
	    V_TCB_RCV_NXT(M_TCB_RCV_NXT), V_TCB_RCV_NXT(ntohl(tcp->th_ack)));
	dst = txq_advance(txq, dst, EQ_ESIZE);
	ndesc++;

	/* XXX: rcv_wnd? */
#if 1
	{
		int i, off, pgoff;

		memcpy(tls_record, hdr, ext_pgs->hdr_len);
		off = ext_pgs->hdr_len;
		pgoff = ext_pgs->first_pg_off;
		for (i = 0; i < ext_pgs->npgs; i++)
		{
			int pglen, tocopy;

			pglen = mbuf_ext_pg_len(ext_pgs, i, pgoff);
			tocopy = pglen;
			if (tocopy < sizeof(tls_record) - off)
				tocopy = sizeof(tls_record) - off;
			memcpy(tls_record + off,
			    (void *)(uintptr_t)(PHYS_TO_DMAP(ext_pgs->pa[i]) + pgoff),
			    tocopy);
			pgoff = 0;
			off += tocopy;
			if (off == sizeof(tls_record))
				break;
		}
		tls_record_len = off;
	}
#endif
	/* Calculate the size of the work request. */
	wr_len = sbtls_base_wr_size(cipher->toep);
	/* XXX: immediate data eventually */
#ifdef notyet
	if (nsegs == 0) {
		MPASS(plen <= SGE_MAX_WR_LEN - wr_len);
		wr_len += plen;
		imm_len = plen;
	} else {
#else
	{
#endif
		wr_len += sbtls_sgl_size(nsegs);
		imm_len = 0;
	}
	wr_len = roundup2(wr_len, 16);
	MPASS(ndesc + howmany(wr_len, EQ_ESIZE) <= available);

	/* FW_ULPTX_WR */
	wr = dst;
	wr->op_to_compl = htobe32(V_FW_WR_OP(FW_ULPTX_WR));
	wr->flowid_len16 = htobe32(F_FW_ULPTX_WR_DATA |
	    V_FW_WR_LEN16(wr_len / 16));
	wr->cookie = 0;

	/* ULP_TXPKT */
	txpkt = txq_advance(txq, wr, sizeof(*wr));
	txpkt->cmd_dest = htobe32(V_ULPTX_CMD(ULP_TX_PKT) |
	    V_ULP_TXPKT_DATAMODIFY(0) |
	    V_ULP_TXPKT_CHANNELID(toep->vi->pi->port_id) | V_ULP_TXPKT_DEST(0) |
	    V_ULP_TXPKT_FID(txq->eq.cntxt_id) | V_ULP_TXPKT_RO(1));
	txpkt->len = htobe32((wr_len - sizeof(*wr)) / 16);

	/* ULPTX_IDATA sub-command */
	idata = txq_advance(txq, txpkt, sizeof(*txpkt));
	idata->cmd_more = htobe32(V_ULPTX_CMD(ULP_TX_SC_IMM) |
	    V_ULP_TX_SC_MORE(imm_len != 0 ? 0 : 1));
	idata->len = htobe32(sizeof(struct cpl_tx_sec_pdu) + key_size(toep) +
	    sizeof(struct cpl_tx_data) + imm_len);

	/* CPL_TX_SEC_PDU */
	/* XXX: Not sure about TX_SEC_PDU_PLACEHOLDER */
	sec_pdu = txq_advance(txq, idata, sizeof(*idata));
	sec_pdu->op_ivinsrtofst = htobe32(
	    V_CPL_TX_SEC_PDU_OPCODE(CPL_TX_SEC_PDU) |
	    V_CPL_TX_SEC_PDU_CPLLEN(2) | V_CPL_TX_SEC_PDU_PLACEHOLDER(0) |
	    V_CPL_TX_SEC_PDU_IVINSRTOFST(TLS_HEADER_LENGTH + 1));

	sec_pdu->pldlen = htobe32(plen);

	/*
	 * AAD is TLS header.  IV is after AAD.  The cipher region
	 * starts after the IV.  See comments in ccr_authenc() and
	 * ccr_gmac() in t4_crypto.c regarding cipher and auth
	 * start/stop values.
	 */
	aad_start = 1;
	aad_stop = TLS_HEADER_LENGTH;
	cipher_start = ext_pgs->hdr_len + 1;
	if (toep->tls.k_ctx.state.enc_mode == CH_EVP_CIPH_GCM_MODE) {
		cipher_stop = 0;
		auth_start = cipher_start;
		auth_stop = 0;
		auth_insert = 0;
	} else {
		/* XXX: This might not be quite right due to padding. */
		cipher_stop = 0;
		auth_start = cipher_start;
		auth_stop = cipher_stop;
		auth_insert = 0;
	}
	sec_pdu->aadstart_cipherstop_hi = htobe32(
	    V_CPL_TX_SEC_PDU_AADSTART(aad_start) |
	    V_CPL_TX_SEC_PDU_AADSTOP(aad_stop) |
	    V_CPL_TX_SEC_PDU_CIPHERSTART(cipher_start) |
	    V_CPL_TX_SEC_PDU_CIPHERSTOP_HI(cipher_stop >> 4));
	sec_pdu->cipherstop_lo_authinsert = htobe32(
	    V_CPL_TX_SEC_PDU_CIPHERSTOP_LO(cipher_stop & 0xf) |
	    V_CPL_TX_SEC_PDU_AUTHSTART(auth_start) |
	    V_CPL_TX_SEC_PDU_AUTHSTOP(auth_stop) |
	    V_CPL_TX_SEC_PDU_AUTHINSERT(auth_insert));

	/* These two flits are actually a CPL_TLS_TX_SCMD_FMT. */
	sec_pdu->seqno_numivs = toep->tls.scmd0.seqno_numivs;
	sec_pdu->ivgen_hdrlen = toep->tls.scmd0.ivgen_hdrlen;

	/* XXX: Ok to reuse TLS sequence number? */
	sec_pdu->scmd1 = htobe64(ext_pgs->seqno);

	/* Key context */
	dst = txq_advance(txq, sec_pdu, sizeof(*sec_pdu));
	tls_copy_tx_key(toep, dst);

	/* CPL_TX_DATA */
	tx_data = txq_advance(txq, dst, key_size(toep));
	OPCODE_TID(tx_data) = htonl(MK_OPCODE_TID(CPL_TX_DATA, toep->tid));
	mss = toep->vi->ifp->if_mtu - (m->m_pkthdr.l3hlen + m->m_pkthdr.l4hlen);
	tx_data->len = V_TX_DATA_MSS(mss) |
	    V_TX_LENGTH(plen + ext_pgs->trail_len);
	tx_data->rsvd = htobe32(tcp_seqno);
	tx_data->flags = htobe32(F_TX_BYPASS);

	dst = txq_advance(txq, tx_data, sizeof(*tx_data));
	if (imm_len != 0) {
		panic("TODO: copy immediate data");
	} else {
		sglist_reset(txq->gl);
		if (sglist_append(txq->gl, hdr, ext_pgs->hdr_len) != 0 ||
		    sglist_append_ext_pgs(txq->gl, ext_pgs, ext_pgs->hdr_len,
		    plen - ext_pgs->hdr_len) != 0) {
#ifdef INVARIANTS
			panic("%s: failed to append sglist", __func__);
#endif
		}
		write_gl_to_txd(txq, dst);
	}

	ndesc += howmany(wr_len, EQ_ESIZE);
	MPASS(ndesc <= available);

	txsd = &txq->sdesc[eq->pidx];
	txsd->m = m;
	txsd->desc_used = ndesc;

	return (ndesc);
}

static void
t6_sbtls_clean_cipher(struct sbtls_info *tls, void *cipher_arg)
{
	struct t6_sbtls_cipher *cipher;
	struct adapter *sc;
	struct toepcb *toep;

	cipher = cipher_arg;
	sc = cipher->sc;
	toep = cipher->toep;

	CTR2(KTR_CXGBE, "%s: tid %d", __func__, toep->tid);

	/* free TID, L3/L4 */
	if (cipher->key_wr != NULL)
		m_free(cipher->key_wr);
	if (toep->l2te)
		t4_l2t_release(toep->l2te);
	if (toep->tid >= 0) {
		remove_tid(sc, toep->tid, 1);
		release_tid(sc, toep->tid, toep->ctrlq);
	}
	free_toepcb(toep);
}

struct sbtls_crypto_backend t6tls_backend = {
	.name = "Chelsio T6",
	.prio = 30,
	.api_version = SBTLS_API_VERSION,
	.try = t6_sbtls_try,
	.setup_cipher = t6_sbtls_setup_cipher,
	.clean_cipher = t6_sbtls_clean_cipher
};
#endif

void
t4_tls_mod_load(void)
{

	mtx_init(&tls_handshake_lock, "t4tls handshake", NULL, MTX_DEF);
	t4_register_cpl_handler(CPL_TLS_DATA, do_tls_data);
	t4_register_cpl_handler(CPL_RX_TLS_CMP, do_rx_tls_cmp);
#ifdef KERN_TLS
	tcp_protosw = pffindproto(PF_INET, IPPROTO_TCP, SOCK_STREAM);
	if (sbtls_crypto_backend_register(&t6tls_backend) != 0)
		printf("Failed to register Chelsio T6 SBTLS backend\n");
#endif
}

void
t4_tls_mod_unload(void)
{

#ifdef KERN_TLS
	sbtls_crypto_backend_deregister(&t6tls_backend);
#endif
	t4_register_cpl_handler(CPL_TLS_DATA, NULL);
	t4_register_cpl_handler(CPL_RX_TLS_CMP, NULL);
	mtx_destroy(&tls_handshake_lock);
}
#endif	/* TCP_OFFLOAD */
