/*	$NetBSD: isakmp_inf.c,v 1.14.4.8 2007/08/01 11:52:20 vanhu Exp $	*/

/* Id: isakmp_inf.c,v 1.44 2006/05/06 20:45:52 manubsd Exp */

/*
 * Copyright (C) 1995, 1996, 1997, and 1998 WIDE Project.
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the project nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE PROJECT AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE PROJECT OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "config.h"

#include <sys/types.h>
#include <sys/param.h>
#include <sys/socket.h>

#include <System/net/pfkeyv2.h>
#include <netinet/in.h>
#include <sys/queue.h>
#ifndef HAVE_NETINET6_IPSEC
#include <netinet/ipsec.h>
#else
#include <netinet6/ipsec.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#if TIME_WITH_SYS_TIME
# include <sys/time.h>
# include <time.h>
#else
# if HAVE_SYS_TIME_H
#  include <sys/time.h>
# else
#  include <time.h>
# endif
#endif
#ifdef ENABLE_HYBRID
#include <resolv.h>
#endif

#include "libpfkey.h"

#include "var.h"
#include "vmbuf.h"
#include "schedule.h"
#include "str2val.h"
#include "misc.h"
#include "plog.h"
#include "debug.h"

#include "localconf.h"
#include "remoteconf.h"
#include "sockmisc.h"
#include "handler.h"
#include "policy.h"
#include "proposal.h"
#include "isakmp_var.h"
#include "evt.h"
#include "isakmp.h"
#ifdef ENABLE_HYBRID
#include "isakmp_xauth.h"
#include "isakmp_unity.h"
#include "isakmp_cfg.h" 
#endif
#include "isakmp_inf.h"
#include "oakley.h"
#include "ipsec_doi.h"
#include "crypto_openssl.h"
#include "pfkey.h"
#include "policy.h"
#include "algorithm.h"
#include "proposal.h"
#include "admin.h"
#include "strnames.h"
#ifdef ENABLE_NATT
#include "nattraversal.h"
#endif
#include "vpn_control_var.h"
#include "vpn_control.h"
#include "ike_session.h"
#include "ipsecSessionTracer.h"
#include "ipsecMessageTracer.h"

/* information exchange */
static int isakmp_info_recv_n (struct ph1handle *, struct isakmp_pl_n *, u_int32_t, int);
static int isakmp_info_recv_d (struct ph1handle *, struct isakmp_pl_d *, u_int32_t, int);

#ifdef ENABLE_DPD
static int isakmp_info_recv_r_u __P((struct ph1handle *,
	struct isakmp_pl_ru *, u_int32_t));
static int isakmp_info_recv_r_u_ack __P((struct ph1handle *,
	struct isakmp_pl_ru *, u_int32_t));
#endif

#ifdef ENABLE_VPNCONTROL_PORT
static int isakmp_info_recv_lb __P((struct ph1handle *, struct isakmp_pl_lb *lb, int));
#endif

//static void purge_isakmp_spi __P((int, isakmp_index *, size_t));
static void info_recv_initialcontact __P((struct ph1handle *));

static int
isakmp_ph1_responder_lifetime (struct ph1handle               *iph1,
                               struct isakmp_pl_resp_lifetime *notify)
{
    char *spi;

    if (ntohs(notify->h.len) < sizeof(*notify) + notify->spi_size) {
        plog(LLV_ERROR, LOCATION, iph1->remote,
             "invalid spi_size in notification payload.\n");
        return -1;
    }
    spi = val2str((char *)(notify + 1), notify->spi_size);

    plog(LLV_DEBUG, LOCATION, iph1->remote,
         "notification message ISAKMP-SA RESPONDER-LIFETIME, "
         "doi=%d proto_id=%d spi=%s(size=%d).\n",
         ntohl(notify->doi), notify->proto_id, spi, notify->spi_size);
    
    /* TODO */
    #if 0
        struct isakmp_pl_attr *attrpl;
        int                    len = ntohs(notify->h.len) - (sizeof(*notify) + notify->spi_size);

        attrpl = (struct isakmp_pl_attr *)((char *)(notify + 1) + notify->spi_size);
        while (len > 0) {
        }
    #endif

    racoon_free(spi);
    return 0;
}

static int
isakmp_ph2_responder_lifetime (struct ph2handle               *iph2,
                               struct isakmp_pl_resp_lifetime *notify)
{
    char *spi;

    if (ntohs(notify->h.len) < sizeof(*notify) + notify->spi_size) {
        plog(LLV_ERROR, LOCATION, iph2->dst,
             "invalid spi_size in notification payload.\n");
        return -1;
    }
    spi = val2str((char *)(notify + 1), notify->spi_size);
    
    plog(LLV_DEBUG, LOCATION, iph2->dst,
         "notification message IPSEC-SA RESPONDER-LIFETIME, "
         "doi=%d proto_id=%d spi=%s(size=%d).\n",
         ntohl(notify->doi), notify->proto_id, spi, notify->spi_size);
    
    /* TODO */
    
    racoon_free(spi);
    return 0;
}

/* %%%
 * Information Exchange
 */
/*
 * receive Information
 */
int
isakmp_info_recv(iph1, msg0)
	struct ph1handle *iph1;
	vchar_t *msg0;
{
	vchar_t *msg = NULL;
	vchar_t *pbuf = NULL;
	u_int32_t msgid = 0;
	int error = -1;
	struct isakmp *isakmp;
	struct isakmp_gen *gen;
    struct isakmp_parse_t *pa;
	void *p;
	vchar_t *hash, *payload;
	struct isakmp_gen *nd;
	u_int8_t np;
	int encrypted;
	int flag = 0;

	plog(LLV_DEBUG, LOCATION, NULL, "receive Information.\n");

	encrypted = ISSET(((struct isakmp *)msg0->v)->flags, ISAKMP_FLAG_E);
	msgid = ((struct isakmp *)msg0->v)->msgid;

	/* Use new IV to decrypt Informational message. */
	if (encrypted) {
		struct isakmp_ivm *ivm;

		if (iph1->ivm == NULL) {
			plog(LLV_ERROR, LOCATION, NULL, "iph1->ivm == NULL\n");
			IPSECSESSIONTRACEREVENT(iph1->parent_session,
									IPSECSESSIONEVENTCODE_IKE_PACKET_RX_FAIL,
									CONSTSTR("Information message"),
									CONSTSTR("Failed to process Information Message (no IV)"));
			return -1;
		}

		/* compute IV */
		ivm = oakley_newiv2(iph1, ((struct isakmp *)msg0->v)->msgid);
		if (ivm == NULL) {
			plog(LLV_ERROR, LOCATION, NULL,
				 "failed to compute IV\n");
			IPSECSESSIONTRACEREVENT(iph1->parent_session,
									IPSECSESSIONEVENTCODE_IKE_PACKET_RX_FAIL,
									CONSTSTR("Information message"),
									CONSTSTR("Failed to process Information Message (can't compute IV)"));
			return -1;
		}

		msg = oakley_do_decrypt(iph1, msg0, ivm->iv, ivm->ive);
		oakley_delivm(ivm);
		if (msg == NULL) {
			plog(LLV_ERROR, LOCATION, NULL,
				 "failed to decrypt packet\n");
			IPSECSESSIONTRACEREVENT(iph1->parent_session,
									IPSECSESSIONEVENTCODE_IKE_PACKET_RX_FAIL,
									CONSTSTR("Information message"),
									CONSTSTR("Failed to decrypt Information message"));
			return -1;
		}

	} else
		msg = vdup(msg0);

	/* Safety check */
	if (msg->l < sizeof(*isakmp) + sizeof(*gen)) {
		plog(LLV_ERROR, LOCATION, NULL, 
			"ignore information because the "
			"message is way too short\n");
		goto end;
	}

	isakmp = (struct isakmp *)msg->v;
	gen = (struct isakmp_gen *)((caddr_t)isakmp + sizeof(struct isakmp));
	np = gen->np;

	if (encrypted) {
		if (isakmp->np != ISAKMP_NPTYPE_HASH) {
			plog(LLV_ERROR, LOCATION, NULL,
			    "ignore information because the "
			    "message has no hash payload.\n");
			goto end;
		}

		if (iph1->status != PHASE1ST_ESTABLISHED &&
            (!iph1->approval || !iph1->skeyid_a)) {
			plog(LLV_ERROR, LOCATION, NULL,
			    "ignore information because ISAKMP-SA "
			    "has not been established yet.\n");
			goto end;
		}

		/* Safety check */
		if (msg->l < sizeof(*isakmp) + ntohs(gen->len) + sizeof(*nd)) {
			plog(LLV_ERROR, LOCATION, NULL, 
				"ignore information because the "
				"message is too short\n");
			goto end;
		}

		p = (caddr_t) gen + sizeof(struct isakmp_gen);
		nd = (struct isakmp_gen *) ((caddr_t) gen + ntohs(gen->len));

		/* nd length check */
		if (ntohs(nd->len) > msg->l - (sizeof(struct isakmp) +
		    ntohs(gen->len))) {
			plog(LLV_ERROR, LOCATION, NULL,
				 "too long payload length (broken message?)\n");
			goto end;
		}

		if (ntohs(nd->len) < sizeof(*nd)) {
			plog(LLV_ERROR, LOCATION, NULL,
				"too short payload length (broken message?)\n");
			goto end;
		}

		payload = vmalloc(ntohs(nd->len));
		if (payload == NULL) {
			plog(LLV_ERROR, LOCATION, NULL,
			    "cannot allocate memory\n");
			goto end;
		}

		memcpy(payload->v, (caddr_t) nd, ntohs(nd->len));

		/* compute HASH */
		hash = oakley_compute_hash1(iph1, isakmp->msgid, payload);
		if (hash == NULL) {
			plog(LLV_ERROR, LOCATION, NULL,
			    "cannot compute hash\n");

			vfree(payload);
			goto end;
		}
		
		if (ntohs(gen->len) - sizeof(struct isakmp_gen) != hash->l) {
			plog(LLV_ERROR, LOCATION, NULL,
			    "ignore information due to hash length mismatch\n");

			vfree(hash);
			vfree(payload);
			goto end;
		}

		if (memcmp(p, hash->v, hash->l) != 0) {
			plog(LLV_ERROR, LOCATION, NULL,
			    "ignore information due to hash mismatch\n");

			vfree(hash);
			vfree(payload);
			goto end;
		}

		plog(LLV_DEBUG, LOCATION, NULL, "hash validated.\n");

		vfree(hash);
		vfree(payload);
	} else {
		/* make sure the packet was encrypted after the beginning of phase 1. */
		switch (iph1->etype) {
		case ISAKMP_ETYPE_AGG:
		case ISAKMP_ETYPE_BASE:
		case ISAKMP_ETYPE_IDENT:
			if ((iph1->side == INITIATOR && iph1->status < PHASE1ST_MSG3SENT)
			 || (iph1->side == RESPONDER && iph1->status < PHASE1ST_MSG2SENT)) {
				break;
			}
			/*FALLTHRU*/
		default:
			plog(LLV_ERROR, LOCATION, iph1->remote,
				"%s message must be encrypted\n",
				s_isakmp_nptype(np));
			error = 0;
			goto end;
		}
	}

	if (!(pbuf = isakmp_parse(msg))) {
		plog(LLV_ERROR, LOCATION, NULL,
			 "failed to parse msg");
		error = -1;
		goto end;
	}

	error = 0;
	for (pa = ALIGNED_CAST(struct isakmp_parse_t *)pbuf->v; pa->type; pa++) {    // Wcast-align fix (void*) - aligned buffer of aligned (unpacked) structs
		switch (pa->type) {
		case ISAKMP_NPTYPE_HASH:
			/* Handled above */
			break;
		case ISAKMP_NPTYPE_N:
			error = isakmp_info_recv_n(iph1,
				(struct isakmp_pl_n *)pa->ptr,
				msgid, encrypted);
			break;
		case ISAKMP_NPTYPE_D:
			error = isakmp_info_recv_d(iph1,
				(struct isakmp_pl_d *)pa->ptr,
				msgid, encrypted);
			break;
		case ISAKMP_NPTYPE_NONCE:
			/* XXX to be 6.4.2 ike-01.txt */
			/* XXX IV is to be synchronized. */
			plog(LLV_ERROR, LOCATION, iph1->remote,
				"ignore Acknowledged Informational\n");
			break;
		default:
			/* don't send information, see isakmp_ident_r1() */
			error = 0;
			plog(LLV_ERROR, LOCATION, iph1->remote,
				"reject the packet, "
				"received unexpected payload type %s.\n",
				s_isakmp_nptype(gen->np));
		}
		if(error < 0) {
			break;
		} else {
			flag |= error;
		}
	}
	IPSECSESSIONTRACEREVENT(iph1->parent_session,
							IPSECSESSIONEVENTCODE_IKE_PACKET_RX_SUCC,
							CONSTSTR("Information message"),
							CONSTSTR(NULL));
	
end:
	if (error) {
		IPSECSESSIONTRACEREVENT(iph1->parent_session,
								IPSECSESSIONEVENTCODE_IKE_PACKET_RX_FAIL,
								CONSTSTR("Information message"),
								CONSTSTR("Failed to process Information Message"));
	}
	if (msg != NULL)
		vfree(msg);
	if (pbuf != NULL)
		vfree(pbuf);
	return error;
}

/*
 * handling of Notification payload
 */
static int
isakmp_info_recv_n(iph1, notify, msgid, encrypted)
	struct ph1handle *iph1;
	struct isakmp_pl_n *notify;
	u_int32_t msgid;
	int encrypted;
{
	u_int type;
	vchar_t *ndata;
	char *nraw;
	size_t l;
	char *spi;

	type = ntohs(notify->type);

	switch (type) {
	case ISAKMP_NTYPE_CONNECTED:
	case ISAKMP_NTYPE_REPLAY_STATUS:
#ifdef ENABLE_HYBRID
	case ISAKMP_NTYPE_UNITY_HEARTBEAT:
#endif
		/* do something */
		break;
    case ISAKMP_NTYPE_RESPONDER_LIFETIME:
        if (encrypted) {
            return(isakmp_ph1_responder_lifetime(iph1,
                                                 (struct isakmp_pl_resp_lifetime *)notify));
        }
        break;
	case ISAKMP_NTYPE_INITIAL_CONTACT:
		if (encrypted) {
			info_recv_initialcontact(iph1);
			return 0;
		}
		break;
#ifdef ENABLE_DPD
	case ISAKMP_NTYPE_R_U_THERE:
		if (encrypted)
			return isakmp_info_recv_r_u(iph1,
				(struct isakmp_pl_ru *)notify, msgid);
		break;
	case ISAKMP_NTYPE_R_U_THERE_ACK:
		if (encrypted)
			return isakmp_info_recv_r_u_ack(iph1,
				(struct isakmp_pl_ru *)notify, msgid);
		break;
#endif
#ifdef ENABLE_VPNCONTROL_PORT
	case ISAKMP_NTYPE_LOAD_BALANCE:
		isakmp_info_recv_lb(iph1, (struct isakmp_pl_lb *)notify, encrypted);
		break;
#endif

	default:
	    {
		/* XXX there is a potential of dos attack. */
		if(type >= ISAKMP_NTYPE_MINERROR &&
		   type <= ISAKMP_NTYPE_MAXERROR) {
			if (msgid == 0) {
				/* don't think this realy deletes ph1 ? */
				plog(LLV_ERROR, LOCATION, iph1->remote,
					"delete phase1 handle.\n");
				return -1;
			} else {
				if (getph2bymsgid(iph1, msgid) == NULL) {
					plog(LLV_ERROR, LOCATION, iph1->remote,
						"fatal %s notify messsage, "
						"phase1 should be deleted.\n",
						s_isakmp_notify_msg(type));
				} else {
					plog(LLV_ERROR, LOCATION, iph1->remote,
						"fatal %s notify messsage, "
						"phase2 should be deleted.\n",
						s_isakmp_notify_msg(type));
				}
			}
		} else {
			plog(LLV_ERROR, LOCATION, iph1->remote,
				"unhandled notify message %s, "
				"no phase2 handle found.\n",
				s_isakmp_notify_msg(type));
		}
	    }
	    break;
	}

	/* get spi if specified and allocate */
	if(notify->spi_size > 0) {
		if (ntohs(notify->h.len) < sizeof(*notify) + notify->spi_size) {
			plog(LLV_ERROR, LOCATION, iph1->remote,
				"invalid spi_size in notification payload.\n");
			return -1;
		}
		spi = val2str((char *)(notify + 1), notify->spi_size);

		plog(LLV_DEBUG, LOCATION, iph1->remote,
			"notification message %d:%s, "
			"doi=%d proto_id=%d spi=%s(size=%d).\n",
			type, s_isakmp_notify_msg(type),
			ntohl(notify->doi), notify->proto_id, spi, notify->spi_size);

		racoon_free(spi);
	}

	/* Send the message data to the logs */
	if(type >= ISAKMP_NTYPE_MINERROR &&
	   type <= ISAKMP_NTYPE_MAXERROR) {
		l = ntohs(notify->h.len) - sizeof(*notify) - notify->spi_size;
		if (l > 0) {
			nraw = (char*)notify;	
			nraw += sizeof(*notify) + notify->spi_size;
			if ((ndata = vmalloc(l)) != NULL) {
				memcpy(ndata->v, nraw, ndata->l);
				plog(LLV_ERROR, LOCATION, iph1->remote,
				    "Message: '%s'.\n", 
				    binsanitize(ndata->v, ndata->l));
				vfree(ndata);
			} else {
				plog(LLV_ERROR, LOCATION, iph1->remote,
				    "Cannot allocate memory\n");
			}
		}
	}
	return 0;
}

#ifdef ENABLE_VPNCONTROL_PORT
static
void
isakmp_info_vpncontrol_notify_ike_failed (struct ph1handle *iph1,
										  int               isakmp_info_initiator,
										  int               type,
										  vchar_t          *data)
{
	u_int32_t address;
	u_int32_t fail_reason;

	/* notify the API that we have received the delete */
	if (iph1->remote->ss_family == AF_INET)
		address = ((struct sockaddr_in *)(iph1->remote))->sin_addr.s_addr;
	else
		address = 0;
	
	if (isakmp_info_initiator == FROM_REMOTE) {
		int premature = oakley_find_status_in_certchain(iph1->cert, CERT_STATUS_PREMATURE);
		int expired = oakley_find_status_in_certchain(iph1->cert, CERT_STATUS_EXPIRED);

		if (premature) {
			fail_reason = VPNCTL_NTYPE_LOCAL_CERT_PREMATURE;
		} else if (expired) {
			fail_reason = VPNCTL_NTYPE_LOCAL_CERT_EXPIRED;
		} else {
			fail_reason = type;
		}
		vpncontrol_notify_ike_failed(fail_reason, isakmp_info_initiator, address, 0, NULL);
		return;
	} else {
		/* FROM_LOCAL */
		if (type == ISAKMP_INTERNAL_ERROR ||
			type <= ISAKMP_NTYPE_UNEQUAL_PAYLOAD_LENGTHS) {
			int premature = oakley_find_status_in_certchain(iph1->cert_p, CERT_STATUS_PREMATURE);
			int expired = oakley_find_status_in_certchain(iph1->cert_p, CERT_STATUS_EXPIRED);
			int subjname = oakley_find_status_in_certchain(iph1->cert_p, CERT_STATUS_INVALID_SUBJNAME);
			int subjaltname = oakley_find_status_in_certchain(iph1->cert_p, CERT_STATUS_INVALID_SUBJALTNAME);

			if (premature) {
				fail_reason = VPNCTL_NTYPE_PEER_CERT_PREMATURE;
			} else if (expired) {
				fail_reason = VPNCTL_NTYPE_PEER_CERT_EXPIRED;
			} else if (subjname) {
				fail_reason = VPNCTL_NTYPE_PEER_CERT_INVALID_SUBJNAME;
			} else if (subjaltname) {
				fail_reason = VPNCTL_NTYPE_PEER_CERT_INVALID_SUBJALTNAME;
			} else {
				fail_reason = type;
			}
			(void)vpncontrol_notify_ike_failed(fail_reason, isakmp_info_initiator, address,
											   (data ? data->l : 0), (u_int8_t *)(data ? data->v : NULL));
			return;
		}
	}
}
#endif /* ENABLE_VPNCONTROL_PORT */

/*
 * handling of Deletion payload
 */
static int
isakmp_info_recv_d(iph1, delete, msgid, encrypted)
	struct ph1handle *iph1;
	struct isakmp_pl_d *delete;
	u_int32_t msgid;
	int encrypted;
{
	int tlen, num_spi;
	struct ph1handle *del_ph1;
	union {
		u_int32_t spi32;
		u_int16_t spi16[2];
	} spi;

	if (ntohl(delete->doi) != IPSEC_DOI) {
		plog(LLV_ERROR, LOCATION, iph1->remote,
			"delete payload with invalid doi:%d.\n",
			ntohl(delete->doi));
#ifdef ENABLE_HYBRID
		/*
		 * At deconnexion time, Cisco VPN client does this
		 * with a zero DOI. Don't give up in that situation.
		 */
		if (((iph1->mode_cfg->flags &
		    ISAKMP_CFG_VENDORID_UNITY) == 0) || (delete->doi != 0))
			return 0;
#else
		return 0;
#endif
	}

	num_spi = ntohs(delete->num_spi);
	tlen = ntohs(delete->h.len) - sizeof(struct isakmp_pl_d);

	if (tlen != num_spi * delete->spi_size) {
		plog(LLV_ERROR, LOCATION, iph1->remote,
			"deletion payload with invalid length.\n");
		return 0;
	}

	plog(LLV_DEBUG, LOCATION, iph1->remote,
		"delete payload for protocol %s\n",
		s_ipsecdoi_proto(delete->proto_id));

	if(!iph1->rmconf->weak_phase1_check && !encrypted) {
		plog(LLV_WARNING, LOCATION, iph1->remote,
			"Ignoring unencrypted delete payload "
			"(check the weak_phase1_check option)\n");
		return 0;
	}

	switch (delete->proto_id) {
	case IPSECDOI_PROTO_ISAKMP:
		if (delete->spi_size != sizeof(isakmp_index)) {
			plog(LLV_ERROR, LOCATION, iph1->remote,
				"delete payload with strange spi "
				"size %d(proto_id:%d)\n",
				delete->spi_size, delete->proto_id);
			return 0;
		}

		del_ph1=getph1byindex((isakmp_index *)(delete + 1));
		if(del_ph1 != NULL){

            // hack: start a rekey now, if one was pending (only for client).
            if (del_ph1->sce_rekey &&
                del_ph1->parent_session &&
                del_ph1->parent_session->is_client &&
                del_ph1->parent_session->established) {
                isakmp_ph1rekeyexpire(del_ph1, FALSE);
            }
            
			EVT_PUSH(del_ph1->local, del_ph1->remote,
			EVTT_PEERPH1_NOPROP, NULL);
			if (del_ph1->scr)
				SCHED_KILL(del_ph1->scr);

			/*
			 * Do not delete IPsec SAs when receiving an IKE delete notification.
			 * Just delete the IKE SA.
			 */
#ifdef ENABLE_VPNCONTROL_PORT
			if (del_ph1->started_by_api || (del_ph1->is_rekey && del_ph1->parent_session && del_ph1->parent_session->is_client)) {
				if (islast_ph1(del_ph1)) {
					isakmp_info_vpncontrol_notify_ike_failed(del_ph1, FROM_REMOTE, VPNCTL_NTYPE_PH1_DELETE, NULL);
				}
			}
#endif
			isakmp_ph1expire(del_ph1);
		}
		break;

	case IPSECDOI_PROTO_IPSEC_AH:
	case IPSECDOI_PROTO_IPSEC_ESP:
		if (delete->spi_size != sizeof(u_int32_t)) {
			plog(LLV_ERROR, LOCATION, iph1->remote,
				"delete payload with strange spi "
				"size %d(proto_id:%d)\n",
				delete->spi_size, delete->proto_id);
			return 0;
		}
		EVT_PUSH(iph1->local, iph1->remote, 
		    EVTT_PEER_DELETE, NULL);
		purge_ipsec_spi(iph1->remote, delete->proto_id,
		    ALIGNED_CAST(u_int32_t *)(delete + 1), num_spi);     // Wcast-align fix (void*) - delete payload is aligned
		break;

	case IPSECDOI_PROTO_IPCOMP:
		/* need to handle both 16bit/32bit SPI */
		memset(&spi, 0, sizeof(spi));
		if (delete->spi_size == sizeof(spi.spi16[1])) {
			memcpy(&spi.spi16[1], delete + 1,
			    sizeof(spi.spi16[1]));
		} else if (delete->spi_size == sizeof(spi.spi32))
			memcpy(&spi.spi32, delete + 1, sizeof(spi.spi32));
		else {
			plog(LLV_ERROR, LOCATION, iph1->remote,
				"delete payload with strange spi "
				"size %d(proto_id:%d)\n",
				delete->spi_size, delete->proto_id);
			return 0;
		}
		purge_ipsec_spi(iph1->remote, delete->proto_id,
		    &spi.spi32, num_spi);
		break;

	default:
		plog(LLV_ERROR, LOCATION, iph1->remote,
			"deletion message received, "
			"invalid proto_id: %d\n",
			delete->proto_id);
		return 0;
	}

	plog(LLV_DEBUG, LOCATION, NULL, "purged SAs.\n");

	return 0;
}

/*
 * send Delete payload (for ISAKMP SA) in Informational exchange.
 */
int
isakmp_info_send_d1(iph1)
	struct ph1handle *iph1;
{
	struct isakmp_pl_d *d;
	vchar_t *payload = NULL;
	int tlen;
	int error = 0;

	if (iph1->status != PHASE2ST_ESTABLISHED)
		return 0;

	/* create delete payload */

	/* send SPIs of inbound SAs. */
	/* XXX should send outbound SAs's ? */
	tlen = sizeof(*d) + sizeof(isakmp_index);
	payload = vmalloc(tlen);
	if (payload == NULL) {
		plog(LLV_ERROR, LOCATION, NULL, 
			"failed to get buffer for payload.\n");
		return errno;
	}

	d = (struct isakmp_pl_d *)payload->v;
	d->h.np = ISAKMP_NPTYPE_NONE;
	d->h.len = htons(tlen);
	d->doi = htonl(IPSEC_DOI);
	d->proto_id = IPSECDOI_PROTO_ISAKMP;
	d->spi_size = sizeof(isakmp_index);
	d->num_spi = htons(1);
	memcpy(d + 1, &iph1->index, sizeof(isakmp_index));

	error = isakmp_info_send_common(iph1, payload,
					ISAKMP_NPTYPE_D, 0);
	vfree(payload);
	if (error) {
		IPSECSESSIONTRACEREVENT(iph1->parent_session,
								IPSECSESSIONEVENTCODE_IKEV1_INFO_NOTICE_TX_FAIL,
								CONSTSTR("Delete ISAKMP-SA"),
								CONSTSTR("Failed to transmit Delete-ISAKMP-SA message"));
	} else {
		IPSECSESSIONTRACEREVENT(iph1->parent_session,
								IPSECSESSIONEVENTCODE_IKEV1_INFO_NOTICE_TX_SUCC,
								CONSTSTR("Delete ISAKMP-SA"),
								CONSTSTR(NULL));
	}

	return error;
}

/*
 * send Delete payload (for IPsec SA) in Informational exchange, based on
 * pfkey msg.  It sends always single SPI.
 */
int
isakmp_info_send_d2(iph2)
	struct ph2handle *iph2;
{
	struct ph1handle *iph1;
	struct saproto *pr;
	struct isakmp_pl_d *d;
	vchar_t *payload = NULL;
	int tlen;
	int error = 0;
	u_int8_t *spi;

	if (iph2->status != PHASE2ST_ESTABLISHED)
		return 0;

	/*
	 * don't send delete information if there is no phase 1 handler.
	 * It's nonsensical to negotiate phase 1 to send the information.
	 */
    iph1 = ike_session_get_established_ph1(iph2->parent_session);
    if (!iph1) {
        iph1 = getph1byaddr(iph2->src, iph2->dst);
    }
	if (iph1 == NULL){
		IPSECSESSIONTRACEREVENT(iph2->parent_session,
								IPSECSESSIONEVENTCODE_IKE_PACKET_TX_FAIL,
								CONSTSTR("Information message"),
								CONSTSTR("Failed to transmit Information message"));
		IPSECSESSIONTRACEREVENT(iph2->parent_session,
								IPSECSESSIONEVENTCODE_IKEV1_INFO_NOTICE_TX_FAIL,
								CONSTSTR("Delete IPSEC-SA"),
								CONSTSTR("Failed to transmit Delete-IPSEC-SA message"));
		plog(LLV_DEBUG2, LOCATION, NULL,
			 "No ph1 handler found, could not send DELETE_SA\n");
		return 0;
	}

	/* create delete payload */
	for (pr = iph2->approval->head; pr != NULL; pr = pr->next) {

		/* send SPIs of inbound SAs. */
		/*
		 * XXX should I send outbound SAs's ?
		 * I send inbound SAs's SPI only at the moment because I can't
		 * decode any more if peer send encoded packet without aware of
		 * deletion of SA.  Outbound SAs don't come under the situation.
		 */
		tlen = sizeof(*d) + pr->spisize;
		payload = vmalloc(tlen);
		if (payload == NULL) {
			IPSECSESSIONTRACEREVENT(iph2->parent_session,
									IPSECSESSIONEVENTCODE_IKE_PACKET_TX_FAIL,
									CONSTSTR("Information message"),
									CONSTSTR("Failed to transmit Information message"));
			IPSECSESSIONTRACEREVENT(iph2->parent_session,
									IPSECSESSIONEVENTCODE_IKEV1_INFO_NOTICE_TX_FAIL,
									CONSTSTR("Delete IPSEC-SA"),
									CONSTSTR("Failed to transmit Delete-IPSEC-SA message"));
			plog(LLV_ERROR, LOCATION, NULL, 
				"failed to get buffer for payload.\n");
			return errno;
		}

		d = (struct isakmp_pl_d *)payload->v;
		d->h.np = ISAKMP_NPTYPE_NONE;
		d->h.len = htons(tlen);
		d->doi = htonl(IPSEC_DOI);
		d->proto_id = pr->proto_id;
		d->spi_size = pr->spisize;
		d->num_spi = htons(1);
		/*
		 * XXX SPI bits are left-filled, for use with IPComp.
		 * we should be switching to variable-length spi field...
		 */
		spi = (u_int8_t *)&pr->spi;
		spi += sizeof(pr->spi);
		spi -= pr->spisize;
		memcpy(d + 1, spi, pr->spisize);

		error = isakmp_info_send_common(iph1, payload,
						ISAKMP_NPTYPE_D, 0);
		vfree(payload);
		if (error) {
			IPSECSESSIONTRACEREVENT(iph2->parent_session,
									IPSECSESSIONEVENTCODE_IKEV1_INFO_NOTICE_TX_FAIL,
									CONSTSTR("Delete IPSEC-SA"),
									CONSTSTR("Failed to transmit Delete-IPSEC-SA"));
		} else {
			IPSECSESSIONTRACEREVENT(iph2->parent_session,
									IPSECSESSIONEVENTCODE_IKEV1_INFO_NOTICE_TX_SUCC,
									CONSTSTR("Delete IPSEC-SA"),
									CONSTSTR(NULL));
		}
	}

	return error;
}

/*
 * send Notification payload (for without ISAKMP SA) in Informational exchange
 */
int
isakmp_info_send_nx(isakmp, remote, local, type, data)
	struct isakmp *isakmp;
	struct sockaddr_storage *remote, *local;
	int type;
	vchar_t *data;
{
	struct ph1handle *iph1 = NULL;
	struct remoteconf *rmconf;
	vchar_t *payload = NULL;
	int tlen;
	int error = -1;
	struct isakmp_pl_n *n;
	int spisiz = 0;		/* see below */
	ike_session_t *sess = ike_session_get_session(local, remote, FALSE);

	/* search appropreate configuration */
	rmconf = getrmconf(remote);
	if (rmconf == NULL) {
		IPSECSESSIONTRACEREVENT(sess,
								IPSECSESSIONEVENTCODE_IKE_PACKET_TX_FAIL,
								CONSTSTR("Information message"),
								CONSTSTR("Failed to transmit Information message (no remote configuration)"));
		plog(LLV_ERROR, LOCATION, remote,
			"no configuration found for peer address.\n");
		goto end;
	}

	/* add new entry to isakmp status table. */
	iph1 = newph1();
	if (iph1 == NULL) {
		IPSECSESSIONTRACEREVENT(sess,
								IPSECSESSIONEVENTCODE_IKE_PACKET_TX_FAIL,
								CONSTSTR("Information message"),
								CONSTSTR("Failed to transmit Information message (no new phase1)"));
		plog(LLV_ERROR, LOCATION, NULL,
			 "failed to allocate ph1");
		return -1;
	}

	memcpy(&iph1->index.i_ck, &isakmp->i_ck, sizeof(cookie_t));
	isakmp_newcookie((char *)&iph1->index.r_ck, remote, local);
	iph1->status = PHASE1ST_START;
	iph1->rmconf = rmconf;
	if (link_rmconf_to_ph1(rmconf) < 0) {
		IPSECSESSIONTRACEREVENT(sess,
								IPSECSESSIONEVENTCODE_IKE_PACKET_TX_FAIL,
								CONSTSTR("Information message"),
								CONSTSTR("Failed to transmit Information message (can't link remote configuration to phase1)"));
		plog(LLV_ERROR, LOCATION, remote,
			 "couldn't link "
			 "configuration.\n");
		iph1->rmconf = NULL;
		error = -1;
		goto end;
	}
	iph1->side = INITIATOR;
	iph1->version = isakmp->v;
	iph1->flags = 0;
	iph1->msgid = 0;	/* XXX */
#ifdef ENABLE_HYBRID
	if ((iph1->mode_cfg = isakmp_cfg_mkstate()) == NULL) {
		error = -1;
		goto end;
	}
#endif
#ifdef ENABLE_FRAG
	iph1->frag = 0;
	iph1->frag_chain = NULL;
#endif

	/* copy remote address */
	if (copy_ph1addresses(iph1, rmconf, remote, local) < 0) {
		IPSECSESSIONTRACEREVENT(sess,
								IPSECSESSIONEVENTCODE_IKE_PACKET_TX_FAIL,
								CONSTSTR("Information message"),
								CONSTSTR("Failed to transmit Information Message (can't copy phase1 addresses)"));
		plog(LLV_ERROR, LOCATION, NULL,
			 "failed to copy ph1 addresses");
		error = -1;
		iph1 = NULL; /* deleted in copy_ph1addresses */
		goto end;
	}

	tlen = sizeof(*n) + spisiz;
	if (data)
		tlen += data->l;
	payload = vmalloc(tlen);
	if (payload == NULL) { 
		IPSECSESSIONTRACEREVENT(sess,
								IPSECSESSIONEVENTCODE_IKE_PACKET_TX_FAIL,
								CONSTSTR("Information message"),
								CONSTSTR("Failed to transmit Information Message (can't allocate payload)"));
		plog(LLV_ERROR, LOCATION, NULL,
			"failed to get buffer to send.\n");
		error = -1;
		goto end;
	}

	n = (struct isakmp_pl_n *)payload->v;
	n->h.np = ISAKMP_NPTYPE_NONE;
	n->h.len = htons(tlen);
	n->doi = htonl(IPSEC_DOI);
	n->proto_id = IPSECDOI_KEY_IKE;
	n->spi_size = spisiz;
	n->type = htons(type);
	if (spisiz)
		memset(n + 1, 0, spisiz);	/* XXX spisiz is always 0 */
	if (data)
		memcpy((caddr_t)(n + 1) + spisiz, data->v, data->l);

#ifdef ENABLE_VPNCONTROL_PORT
	isakmp_info_vpncontrol_notify_ike_failed(iph1, FROM_LOCAL, type, data);
#endif

	error = isakmp_info_send_common(iph1, payload, ISAKMP_NPTYPE_N, 0);
	vfree(payload);
	if (error) {
		IPSECSESSIONTRACEREVENT(sess,
								IPSECSESSIONEVENTCODE_IKEV1_INFO_NOTICE_TX_FAIL,
								CONSTSTR("Without ISAKMP-SA"),
								CONSTSTR("Failed to transmit Without-ISAKMP-SA message"));
	} else {
		IPSECSESSIONTRACEREVENT(sess,
								IPSECSESSIONEVENTCODE_IKEV1_INFO_NOTICE_TX_SUCC,
								CONSTSTR("Without ISAKMP-SA"),
								CONSTSTR(NULL));
	}
	
    end:
	if (iph1 != NULL)
		delph1(iph1);

	return error;
}

/*
 * send Notification payload (for ISAKMP SA) in Informational exchange
 */
int
isakmp_info_send_n1(iph1, type, data)
	struct ph1handle *iph1;
	int type;
	vchar_t *data;
{
	vchar_t *payload = NULL;
	int tlen;
	int error = 0;
	struct isakmp_pl_n *n;
	int spisiz;

	/*
	 * note on SPI size: which description is correct?  I have chosen
	 * this to be 0.
	 *
	 * RFC2408 3.1, 2nd paragraph says: ISAKMP SA is identified by
	 * Initiator/Responder cookie and SPI has no meaning, SPI size = 0.
	 * RFC2408 3.1, first paragraph on page 40: ISAKMP SA is identified
	 * by cookie and SPI has no meaning, 0 <= SPI size <= 16.
	 * RFC2407 4.6.3.3, INITIAL-CONTACT is required to set to 16.
	 */
	if (type == ISAKMP_NTYPE_INITIAL_CONTACT ||
			type == ISAKMP_NTYPE_LOAD_BALANCE)
		spisiz = sizeof(isakmp_index);
	else
		spisiz = 0;

	tlen = sizeof(*n) + spisiz;
	if (data)
		tlen += data->l;
	payload = vmalloc(tlen);
	if (payload == NULL) { 
		IPSECSESSIONTRACEREVENT(iph1->parent_session,
								IPSECSESSIONEVENTCODE_IKEV1_INFO_NOTICE_TX_FAIL,
								CONSTSTR("ISAKMP-SA"),
								CONSTSTR("Failed to transmit ISAKMP-SA message (can't allocate payload)"));
		plog(LLV_ERROR, LOCATION, NULL,
			"failed to get buffer to send.\n");
		return errno;
	}

	n = (struct isakmp_pl_n *)payload->v;
	n->h.np = ISAKMP_NPTYPE_NONE;
	n->h.len = htons(tlen);
	n->doi = htonl(iph1->rmconf->doitype);
	n->proto_id = IPSECDOI_PROTO_ISAKMP; /* XXX to be configurable ? */
	n->spi_size = spisiz;
	n->type = htons(type);
	if (spisiz)
		memcpy(n + 1, &iph1->index, sizeof(isakmp_index));
	if (data)
		memcpy((caddr_t)(n + 1) + spisiz, data->v, data->l);

#ifdef ENABLE_VPNCONTROL_PORT	
	isakmp_info_vpncontrol_notify_ike_failed(iph1, FROM_LOCAL, type, data);
#endif

	error = isakmp_info_send_common(iph1, payload, ISAKMP_NPTYPE_N, iph1->flags);
	vfree(payload);
	if (error) {
		IPSECSESSIONTRACEREVENT(iph1->parent_session,
								IPSECSESSIONEVENTCODE_IKEV1_INFO_NOTICE_TX_FAIL,
								CONSTSTR("ISAKMP-SA"),
								CONSTSTR("Can't transmit ISAKMP-SA message"));
	} else {
		IPSECSESSIONTRACEREVENT(iph1->parent_session,
								IPSECSESSIONEVENTCODE_IKEV1_INFO_NOTICE_TX_SUCC,
								CONSTSTR("ISAKMP-SA"),
								CONSTSTR(NULL));
	}

	return error;
}

/*
 * send Notification payload (for IPsec SA) in Informational exchange
 */
int
isakmp_info_send_n2(iph2, type, data)
	struct ph2handle *iph2;
	int type;
	vchar_t *data;
{
	struct ph1handle *iph1 = iph2->ph1;
	vchar_t *payload = NULL;
	int tlen;
	int error = 0;
	struct isakmp_pl_n *n;
	struct saproto *pr;

	if (!iph2->approval)
		return EINVAL;

	pr = iph2->approval->head;

	/* XXX must be get proper spi */
	tlen = sizeof(*n) + pr->spisize;
	if (data)
		tlen += data->l;
	payload = vmalloc(tlen);
	if (payload == NULL) { 
		IPSECSESSIONTRACEREVENT(iph2->parent_session,
								IPSECSESSIONEVENTCODE_IKEV1_INFO_NOTICE_TX_FAIL,
								CONSTSTR("IPSEC-SA"),
								CONSTSTR("Failed to transmit IPSEC-SA message (can't allocate payload)"));
		plog(LLV_ERROR, LOCATION, NULL,
			"failed to get buffer to send.\n");
		return errno;
	}

	n = (struct isakmp_pl_n *)payload->v;
	n->h.np = ISAKMP_NPTYPE_NONE;
	n->h.len = htons(tlen);
	n->doi = htonl(IPSEC_DOI);		/* IPSEC DOI (1) */
	n->proto_id = pr->proto_id;		/* IPSEC AH/ESP/whatever*/
	n->spi_size = pr->spisize;
	n->type = htons(type);
    memcpy(n + 1, &pr->spi, sizeof(u_int32_t));         // Wcast-align fix - copy instead of assign
	if (data)
		memcpy((caddr_t)(n + 1) + pr->spisize, data->v, data->l);

	iph2->flags |= ISAKMP_FLAG_E;	/* XXX Should we do FLAG_A ? */
	error = isakmp_info_send_common(iph1, payload, ISAKMP_NPTYPE_N, iph2->flags);
	vfree(payload);
	if (error) {
		IPSECSESSIONTRACEREVENT(iph2->parent_session,
								IPSECSESSIONEVENTCODE_IKEV1_INFO_NOTICE_TX_FAIL,
								CONSTSTR("IPSEC-SA"),
								CONSTSTR("Failed to transmit IPSEC-SA message"));
	} else {
		IPSECSESSIONTRACEREVENT(iph2->parent_session,
								IPSECSESSIONEVENTCODE_IKEV1_INFO_NOTICE_TX_SUCC,
								CONSTSTR("IPSEC-SA"),
								CONSTSTR(NULL));
	}
	
	return error;
}

/*
 * send Information
 * When ph1->skeyid_a == NULL, send message without encoding.
 */
int
isakmp_info_send_common(iph1, payload, np, flags)
	struct ph1handle *iph1;
	vchar_t *payload;
	u_int32_t np;
	int flags;
{
	struct ph2handle *iph2 = NULL;
	vchar_t *hash = NULL;
	struct isakmp *isakmp;
	struct isakmp_gen *gen;
	char *p;
	int tlen;
	int error = -1;

	/* add new entry to isakmp status table */
	iph2 = newph2();
	if (iph2 == NULL) {
		plog(LLV_ERROR, LOCATION, NULL,
			 "failed to allocate ph2");
		goto end;
	}

	iph2->dst = dupsaddr((struct sockaddr *)iph1->remote);
	if (iph2->dst == NULL) {
		plog(LLV_ERROR, LOCATION, NULL,
			 "failed to duplicate remote address");
		delph2(iph2);
		goto end;
	}
	iph2->src = dupsaddr((struct sockaddr *)iph1->local);
	if (iph2->src == NULL) {
		plog(LLV_ERROR, LOCATION, NULL,
			 "failed to duplicate local address");
		delph2(iph2);
		goto end;
	}
	switch (iph1->remote->ss_family) {
	case AF_INET:
#if (!defined(ENABLE_NATT)) || (defined(BROKEN_NATT))
		((struct sockaddr_in *)iph2->dst)->sin_port = 0;
		((struct sockaddr_in *)iph2->src)->sin_port = 0;
#endif
		break;
#ifdef INET6
	case AF_INET6:
#if (!defined(ENABLE_NATT)) || (defined(BROKEN_NATT))
		((struct sockaddr_in6 *)iph2->dst)->sin6_port = 0;
		((struct sockaddr_in6 *)iph2->src)->sin6_port = 0;
#endif
		break;
#endif
	default:
		plog(LLV_ERROR, LOCATION, NULL,
			"invalid family: %d\n", iph1->remote->ss_family);
		delph2(iph2);
		goto end;
	}
	iph2->ph1 = iph1;
	iph2->side = INITIATOR;
	iph2->status = PHASE2ST_START;
	iph2->msgid = isakmp_newmsgid2(iph1);

	/* get IV and HASH(1) if skeyid_a was generated. */
	if (iph1->skeyid_a != NULL) {
		iph2->ivm = oakley_newiv2(iph1, iph2->msgid);
		if (iph2->ivm == NULL) {
			plog(LLV_ERROR, LOCATION, NULL,
				 "failed to generate IV");
			delph2(iph2);
			goto end;
		}

		/* generate HASH(1) */
		hash = oakley_compute_hash1(iph2->ph1, iph2->msgid, payload);
		if (hash == NULL) {
			plog(LLV_ERROR, LOCATION, NULL,
				 "failed to generate HASH");
			delph2(iph2);
			goto end;
		}

		/* initialized total buffer length */
		tlen = hash->l;
		tlen += sizeof(*gen);
	} else {
		/* IKE-SA is not established */
		hash = NULL;

		/* initialized total buffer length */
		tlen = 0;
	}
	if ((flags & ISAKMP_FLAG_A) == 0)
		iph2->flags = (hash == NULL ? 0 : ISAKMP_FLAG_E);
	else
		iph2->flags = (hash == NULL ? 0 : ISAKMP_FLAG_A);

	insph2(iph2);
	bindph12(iph1, iph2);

	tlen += sizeof(*isakmp) + payload->l;

	/* create buffer for isakmp payload */
	iph2->sendbuf = vmalloc(tlen);
	if (iph2->sendbuf == NULL) { 
		plog(LLV_ERROR, LOCATION, NULL,
			"failed to get buffer to send.\n");
		goto err;
	}

	/* create isakmp header */
	isakmp = (struct isakmp *)iph2->sendbuf->v;
	memcpy(&isakmp->i_ck, &iph1->index.i_ck, sizeof(cookie_t));
	memcpy(&isakmp->r_ck, &iph1->index.r_ck, sizeof(cookie_t));
	isakmp->np = hash == NULL ? (np & 0xff) : ISAKMP_NPTYPE_HASH;
	isakmp->v = iph1->version;
	isakmp->etype = ISAKMP_ETYPE_INFO;
	isakmp->flags = iph2->flags;
	memcpy(&isakmp->msgid, &iph2->msgid, sizeof(isakmp->msgid));
	isakmp->len   = htonl(tlen);
	p = (char *)(isakmp + 1);

	/* create HASH payload */
	if (hash != NULL) {
		gen = (struct isakmp_gen *)p;
		gen->np = np & 0xff;
		gen->len = htons(sizeof(*gen) + hash->l);
		p += sizeof(*gen);
		memcpy(p, hash->v, hash->l);
		p += hash->l;
	}

	/* add payload */
	memcpy(p, payload->v, payload->l);
	p += payload->l;

#ifdef HAVE_PRINT_ISAKMP_C
	isakmp_printpacket(iph2->sendbuf, iph1->local, iph1->remote, 1);
#endif

	/* encoding */
	if (ISSET(isakmp->flags, ISAKMP_FLAG_E)) {
		vchar_t *tmp;

		tmp = oakley_do_encrypt(iph2->ph1, iph2->sendbuf, iph2->ivm->ive,
				iph2->ivm->iv);
		VPTRINIT(iph2->sendbuf);
		if (tmp == NULL) {
			plog(LLV_ERROR, LOCATION, NULL,
				 "failed to encrypt packet");
			goto err;
		}
		iph2->sendbuf = tmp;
	}

	/* HDR*, HASH(1), N */
	if (isakmp_send(iph2->ph1, iph2->sendbuf) < 0) {
		plog(LLV_ERROR, LOCATION, NULL,
			 "failed to send packet");
		VPTRINIT(iph2->sendbuf);
		goto err;
	}

	plog(LLV_DEBUG, LOCATION, NULL,
		"sendto Information %s.\n", s_isakmp_nptype(np));

	/*
	 * don't resend notify message because peer can use Acknowledged
	 * Informational if peer requires the reply of the notify message.
	 */

	/* XXX If Acknowledged Informational required, don't delete ph2handle */
	error = 0;
	VPTRINIT(iph2->sendbuf);
	IPSECSESSIONTRACEREVENT(iph1->parent_session,
							IPSECSESSIONEVENTCODE_IKE_PACKET_TX_SUCC,
							CONSTSTR("Information message"),
							CONSTSTR(NULL));
	
	goto err;	/* XXX */

end:
	if (error) {
		IPSECSESSIONTRACEREVENT(iph1->parent_session,
								IPSECSESSIONEVENTCODE_IKE_PACKET_TX_FAIL,
								CONSTSTR("Information message"),
								CONSTSTR("Failed to transmit Information message"));
	}
	if (hash)
		vfree(hash);
	return error;

err:
	unbindph12(iph2);
	remph2(iph2);
	delph2(iph2);
	goto end;
}

/*
 * add a notify payload to buffer by reallocating buffer.
 * If buf == NULL, the function only create a notify payload.
 *
 * XXX Which is SPI to be included, inbound or outbound ?
 */
vchar_t *
isakmp_add_pl_n(buf0, np_p, type, pr, data)
	vchar_t *buf0;
	u_int8_t **np_p;
	int type;
	struct saproto *pr;
	vchar_t *data;
{
	vchar_t *buf = NULL;
	struct isakmp_pl_n *n;
	int tlen;
	int oldlen = 0;

	if (*np_p)
		**np_p = ISAKMP_NPTYPE_N;

	tlen = sizeof(*n) + pr->spisize;

	if (data)
		tlen += data->l;
	if (buf0) {
		oldlen = buf0->l;
		buf = vrealloc(buf0, buf0->l + tlen);
	} else
		buf = vmalloc(tlen);
	if (!buf) {
		plog(LLV_ERROR, LOCATION, NULL,
			"failed to get a payload buffer.\n");
		return NULL;
	}

	n = (struct isakmp_pl_n *)(buf->v + oldlen);
	n->h.np = ISAKMP_NPTYPE_NONE;
	n->h.len = htons(tlen);
	n->doi = htonl(IPSEC_DOI);		/* IPSEC DOI (1) */
	n->proto_id = pr->proto_id;		/* IPSEC AH/ESP/whatever*/
	n->spi_size = pr->spisize;
	n->type = htons(type);
    memcpy(n + 1, &pr->spi, sizeof(u_int32_t));			// Wcast-align fix - copy instead of assign with cast
	if (data)
		memcpy((caddr_t)(n + 1) + pr->spisize, data->v, data->l);

	/* save the pointer of next payload type */
	*np_p = &n->h.np;

	return buf;
}

#if 0
static void
purge_isakmp_spi(proto, spi, n)
	int proto;
	isakmp_index *spi;	/*network byteorder*/
	size_t n;
{
	struct ph1handle *iph1;
	size_t i;

	for (i = 0; i < n; i++) {
		iph1 = getph1byindex(&spi[i]);
		if (!iph1 || iph1->is_dying || iph1->status == PHASE1ST_EXPIRED)
			continue;

		plog(LLV_INFO, LOCATION, NULL,
			"purged ISAKMP-SA proto_id=%s spi=%s.\n",
			s_ipsecdoi_proto(proto),
			isakmp_pindex(&spi[i], 0));

		SCHED_KILL(iph1->sce);
		SCHED_KILL(iph1->sce_rekey);
		iph1->status = PHASE1ST_EXPIRED;
		ike_session_update_ph1_ph2tree(iph1); // move unbind/rebind ph2s to from current ph1
		iph1->sce = sched_new(1, isakmp_ph1delete_stub, iph1);
	}
}
#endif


void
purge_ipsec_spi(dst0, proto, spi, n)
	struct sockaddr_storage *dst0;
	int proto;
	u_int32_t *spi;	/*network byteorder*/
	size_t n;
{
	vchar_t *buf = NULL;
	struct sadb_msg *msg, *next, *end;
	struct sadb_sa *sa;
	struct sadb_lifetime *lt;
	struct sockaddr_storage *src, *dst;
	struct ph2handle *iph2;
	u_int64_t created;
	size_t i;
	caddr_t mhp[SADB_EXT_MAX + 1];

	plog(LLV_DEBUG2, LOCATION, NULL,
		 "purge_ipsec_spi:\n");
	plog(LLV_DEBUG2, LOCATION, NULL, "dst0: %s\n", saddr2str((struct sockaddr *)dst0));
	plog(LLV_DEBUG2, LOCATION, NULL, "SPI: %08X\n", ntohl(spi[0]));
	plog(LLV_DEBUG2, LOCATION, NULL, "num SPI: %d\n", n);

	buf = pfkey_dump_sadb(ipsecdoi2pfkey_proto(proto));
	if (buf == NULL) {
		plog(LLV_DEBUG, LOCATION, NULL,
			"pfkey_dump_sadb returned nothing.\n");
		return;
	}

	msg = ALIGNED_CAST(struct sadb_msg *)buf->v;
	end = ALIGNED_CAST(struct sadb_msg *)(buf->v + buf->l);

	while (msg < end) {
		if ((msg->sadb_msg_len << 3) < sizeof(*msg))
			break;
		next = ALIGNED_CAST(struct sadb_msg *)((caddr_t)msg + (msg->sadb_msg_len << 3));
		if (msg->sadb_msg_type != SADB_DUMP) {
			msg = next;
			continue;
		}

		if (pfkey_align(msg, mhp) || pfkey_check(mhp)) {
			plog(LLV_ERROR, LOCATION, NULL,
				"pfkey_check (%s)\n", ipsec_strerror());
			msg = next;
			continue;
		}

		sa = ALIGNED_CAST(struct sadb_sa *)(mhp[SADB_EXT_SA]);       // Wcast-align fix (void*) - buffer of pointers to aligned structs
		if (!sa
		 || !mhp[SADB_EXT_ADDRESS_SRC]
		 || !mhp[SADB_EXT_ADDRESS_DST]) {
			msg = next;
			continue;
		}
		src =  ALIGNED_CAST(struct sockaddr_storage*)PFKEY_ADDR_SADDR(mhp[SADB_EXT_ADDRESS_SRC]);     // Wcast-align fix (void*) - buffer of pointers to aligned structs
		dst = ALIGNED_CAST(struct sockaddr_storage*)PFKEY_ADDR_SADDR(mhp[SADB_EXT_ADDRESS_DST]);
		lt = ALIGNED_CAST(struct sadb_lifetime*)mhp[SADB_EXT_LIFETIME_HARD];
		if(lt != NULL)
			created = lt->sadb_lifetime_addtime;
		else
			created = 0;

		if (sa->sadb_sa_state != SADB_SASTATE_MATURE
		 && sa->sadb_sa_state != SADB_SASTATE_DYING) {
			msg = next;
			continue;
		}
		plog(LLV_DEBUG2, LOCATION, NULL, "src: %s\n", saddr2str((struct sockaddr *)src));
		plog(LLV_DEBUG2, LOCATION, NULL, "dst: %s\n", saddr2str((struct sockaddr *)dst));



		/* XXX n^2 algorithm, inefficient */

		/* don't delete inbound SAs at the moment */
		/* XXX should we remove SAs with opposite direction as well? */
		if (CMPSADDR2(dst0, dst)) {
			plog(LLV_DEBUG2, LOCATION, NULL, "skipped dst: %s\n", saddr2str((struct sockaddr *)dst));
			msg = next;
			continue;
		}

		for (i = 0; i < n; i++) {
			plog(LLV_DEBUG, LOCATION, NULL,
				"check spi(packet)=%u spi(db)=%u.\n",
				ntohl(spi[i]), ntohl(sa->sadb_sa_spi));
			if (spi[i] != sa->sadb_sa_spi)
				continue;

			pfkey_send_delete(lcconf->sock_pfkey,
				msg->sadb_msg_satype,
				IPSEC_MODE_ANY,
				src, dst, sa->sadb_sa_spi);

			/*
			 * delete a relative phase 2 handler.
			 * continue to process if no relative phase 2 handler
			 * exists.
			 */
			iph2 = getph2bysaidx(src, dst, proto, spi[i]);
			if(iph2 != NULL){
				delete_spd(iph2);
				unbindph12(iph2);
				remph2(iph2);
				delph2(iph2);
			}

			plog(LLV_INFO, LOCATION, NULL,
				"purged IPsec-SA proto_id=%s spi=%u.\n",
				s_ipsecdoi_proto(proto),
				ntohl(spi[i]));
		}

		msg = next;
	}

	if (buf)
		vfree(buf);
}

/*
 * delete all phase2 sa relatived to the destination address.
 * Don't delete Phase 1 handlers on INITIAL-CONTACT, and don't ignore
 * an INITIAL-CONTACT if we have contacted the peer.  This matches the
 * Sun IKE behavior, and makes rekeying work much better when the peer
 * restarts.
 */
static void
info_recv_initialcontact(iph1)
	struct ph1handle *iph1;
{
	vchar_t *buf = NULL;
	struct sadb_msg *msg, *next, *end;
	struct sadb_sa *sa;
	struct sockaddr_storage *src, *dst;
	caddr_t mhp[SADB_EXT_MAX + 1];
	int proto_id, i;
	struct ph2handle *iph2;
#if 0
	char *loc, *rem;
#endif

	if (f_local)
		return;

	// TODO: make sure that is_rekey is cleared for this. and session indicates the same
#if 0
	loc = racoon_strdup(saddrwop2str(iph1->local));
	rem = racoon_strdup(saddrwop2str(iph1->remote));
	STRDUP_FATAL(loc);
	STRDUP_FATAL(rem);

	/*
	 * Purge all IPSEC-SAs for the peer.  We can do this
	 * the easy way (using a PF_KEY SADB_DELETE extension)
	 * or we can do it the hard way.
	 */
	for (i = 0; i < pfkey_nsatypes; i++) {
		proto_id = pfkey2ipsecdoi_proto(pfkey_satypes[i].ps_satype);

		plog(LLV_INFO, LOCATION, NULL,
		    "purging %s SAs for %s -> %s\n",
		    pfkey_satypes[i].ps_name, loc, rem);
		if (pfkey_send_delete_all(lcconf->sock_pfkey,
		    pfkey_satypes[i].ps_satype, IPSEC_MODE_ANY,
		    iph1->local, iph1->remote) == -1) {
			plog(LLV_ERROR, LOCATION, NULL,
			    "delete_all %s -> %s failed for %s (%s)\n",
			    loc, rem,
			    pfkey_satypes[i].ps_name, ipsec_strerror());
			goto the_hard_way;
		}

		deleteallph2(iph1->local, iph1->remote, proto_id);

		plog(LLV_INFO, LOCATION, NULL,
		    "purging %s SAs for %s -> %s\n",
		    pfkey_satypes[i].ps_name, rem, loc);
		if (pfkey_send_delete_all(lcconf->sock_pfkey,
		    pfkey_satypes[i].ps_satype, IPSEC_MODE_ANY,
		    iph1->remote, iph1->local) == -1) {
			plog(LLV_ERROR, LOCATION, NULL,
			    "delete_all %s -> %s failed for %s (%s)\n",
			    rem, loc,
			    pfkey_satypes[i].ps_name, ipsec_strerror());
			goto the_hard_way;
		}

		deleteallph2(iph1->remote, iph1->local, proto_id);
	}

	racoon_free(loc);
	racoon_free(rem);
	return;

 the_hard_way:
	racoon_free(loc);
	racoon_free(rem);
#endif

	buf = pfkey_dump_sadb(SADB_SATYPE_UNSPEC);
	if (buf == NULL) {
		plog(LLV_DEBUG, LOCATION, NULL,
			"pfkey_dump_sadb returned nothing.\n");
		return;
	}

	msg = ALIGNED_CAST(struct sadb_msg *)buf->v;
	end = ALIGNED_CAST(struct sadb_msg *)(buf->v + buf->l);

	while (msg < end) {
		if ((msg->sadb_msg_len << 3) < sizeof(*msg))
			break;
		next = ALIGNED_CAST(struct sadb_msg *)((caddr_t)msg + (msg->sadb_msg_len << 3));
		if (msg->sadb_msg_type != SADB_DUMP) {
			msg = next;
			continue;
		}

		if (pfkey_align(msg, mhp) || pfkey_check(mhp)) {
			plog(LLV_ERROR, LOCATION, NULL,
				"pfkey_check (%s)\n", ipsec_strerror());
			msg = next;
			continue;
		}

		if (mhp[SADB_EXT_SA] == NULL
		 || mhp[SADB_EXT_ADDRESS_SRC] == NULL
		 || mhp[SADB_EXT_ADDRESS_DST] == NULL) {
			msg = next;
			continue;
		}
		sa = ALIGNED_CAST(struct sadb_sa *)mhp[SADB_EXT_SA];                 // Wcast-align fix (void*) - buffer of pointers to aligned structs
		src = ALIGNED_CAST(struct sockaddr_storage *)PFKEY_ADDR_SADDR(mhp[SADB_EXT_ADDRESS_SRC]);
		dst = ALIGNED_CAST(struct sockaddr_storage *)PFKEY_ADDR_SADDR(mhp[SADB_EXT_ADDRESS_DST]);

		if (sa->sadb_sa_state != SADB_SASTATE_MATURE
		 && sa->sadb_sa_state != SADB_SASTATE_DYING) {
			msg = next;
			continue;
		}

		/*
		 * RFC2407 4.6.3.3 INITIAL-CONTACT is the message that
		 * announces the sender of the message was rebooted.
		 * it is interpreted to delete all SAs which source address
		 * is the sender of the message.
		 * racoon only deletes SA which is matched both the
		 * source address and the destination accress.
		 */
#ifdef ENABLE_NATT
		/* 
		 * XXX RFC 3947 says that whe MUST NOT use IP+port to find old SAs
		 * from this peer !
		 */
		if(iph1->natt_flags & NAT_DETECTED){
			if (CMPSADDR(iph1->local, src) == 0 &&
				CMPSADDR(iph1->remote, dst) == 0)
				;
			else if (CMPSADDR(iph1->remote, src) == 0 &&
					 CMPSADDR(iph1->local, dst) == 0)
				;
			else {
				msg = next;
				continue;
			}
		} else
#endif
		/* If there is no NAT-T, we don't have to check addr + port...
		 * XXX what about a configuration with a remote peers which is not
		 * NATed, but which NATs some other peers ?
		 * Here, the INITIAl-CONTACT would also flush all those NATed peers !!
		 */
		if (cmpsaddrwop(iph1->local, src) == 0 &&
		    cmpsaddrwop(iph1->remote, dst) == 0)
			;
		else if (cmpsaddrwop(iph1->remote, src) == 0 &&
		    cmpsaddrwop(iph1->local, dst) == 0)
			;
		else {
			msg = next;
			continue;
		}

		/*
		 * Make sure this is an SATYPE that we manage.
		 * This is gross; too bad we couldn't do it the
		 * easy way.
		 */
		for (i = 0; i < pfkey_nsatypes; i++) {
			if (pfkey_satypes[i].ps_satype ==
			    msg->sadb_msg_satype)
				break;
		}
		if (i == pfkey_nsatypes) {
			msg = next;
			continue;
		}

		plog(LLV_INFO, LOCATION, NULL,
			"purging spi=%u.\n", ntohl(sa->sadb_sa_spi));
		pfkey_send_delete(lcconf->sock_pfkey,
			msg->sadb_msg_satype,
			IPSEC_MODE_ANY, src, dst, sa->sadb_sa_spi);

		/*
		 * delete a relative phase 2 handler.
		 * continue to process if no relative phase 2 handler
		 * exists.
		 */
		proto_id = pfkey2ipsecdoi_proto(msg->sadb_msg_satype);
		iph2 = getph2bysaidx(src, dst, proto_id, sa->sadb_sa_spi);
		if (iph2) {
			delete_spd(iph2);
			unbindph12(iph2);
			remph2(iph2);
			delph2(iph2);
		}

		msg = next;
	}

	vfree(buf);
}

void
isakmp_check_notify(gen, iph1)
	struct isakmp_gen *gen;		/* points to Notify payload */
	struct ph1handle *iph1;
{
	struct isakmp_pl_n *notify = (struct isakmp_pl_n *)gen;

	plog(LLV_DEBUG, LOCATION, iph1->remote,
		"Notify Message received\n");

	switch (ntohs(notify->type)) {
	case ISAKMP_NTYPE_CONNECTED:
	case ISAKMP_NTYPE_RESPONDER_LIFETIME:
	case ISAKMP_NTYPE_REPLAY_STATUS:
	case ISAKMP_NTYPE_HEARTBEAT:
#ifdef ENABLE_HYBRID
	case ISAKMP_NTYPE_UNITY_HEARTBEAT:
#endif
		plog(LLV_WARNING, LOCATION, iph1->remote,
			"ignore %s notification.\n",
			s_isakmp_notify_msg(ntohs(notify->type)));
		break;
	case ISAKMP_NTYPE_INITIAL_CONTACT:
		plog(LLV_WARNING, LOCATION, iph1->remote,
			"ignore INITIAL-CONTACT notification, "
			"because it is only accepted after phase1.\n");
		break;
	case ISAKMP_NTYPE_LOAD_BALANCE:
		plog(LLV_WARNING, LOCATION, iph1->remote,
			"ignore LOAD-BALANCE notification, "
			"because it is only accepted after phase1.\n");
		break;
	default:
		isakmp_info_send_n1(iph1, ISAKMP_NTYPE_INVALID_PAYLOAD_TYPE, NULL);
		plog(LLV_ERROR, LOCATION, iph1->remote,
			"received unknown notification type %s.\n",
			s_isakmp_notify_msg(ntohs(notify->type)));
	}

	return;
}

void
isakmp_check_ph2_notify(gen, iph2)
struct isakmp_gen *gen;		/* points to Notify payload */
struct ph2handle *iph2;
{
	struct isakmp_pl_n *notify = (struct isakmp_pl_n *)gen;
    
	plog(LLV_DEBUG, LOCATION, iph2->dst,
         "Phase2 Notify Message received\n");
    
	switch (ntohs(notify->type)) {
        case ISAKMP_NTYPE_RESPONDER_LIFETIME:
            return((void)isakmp_ph2_responder_lifetime(iph2,
                                                       (struct isakmp_pl_resp_lifetime *)notify));
            break;
        case ISAKMP_NTYPE_CONNECTED:
        case ISAKMP_NTYPE_REPLAY_STATUS:
        case ISAKMP_NTYPE_HEARTBEAT:
#ifdef ENABLE_HYBRID
        case ISAKMP_NTYPE_UNITY_HEARTBEAT:
#endif
            plog(LLV_WARNING, LOCATION, iph2->dst,
                 "ignore %s notification.\n",
                 s_isakmp_notify_msg(ntohs(notify->type)));
            break;
        case ISAKMP_NTYPE_INITIAL_CONTACT:
            plog(LLV_WARNING, LOCATION, iph2->dst,
                 "ignore INITIAL-CONTACT notification, "
                 "because it is only accepted after phase1.\n");
            break;
        case ISAKMP_NTYPE_LOAD_BALANCE:
            plog(LLV_WARNING, LOCATION, iph2->dst,
                 "ignore LOAD-BALANCE notification, "
                 "because it is only accepted after phase1.\n");
            break;
        default:
            isakmp_info_send_n1(iph2->ph1, ISAKMP_NTYPE_INVALID_PAYLOAD_TYPE, NULL);
            plog(LLV_ERROR, LOCATION, iph2->dst,
                 "received unknown notification type %s.\n",
                 s_isakmp_notify_msg(ntohs(notify->type)));
	}
    
	return;
}

#ifdef ENABLE_VPNCONTROL_PORT
static int
isakmp_info_recv_lb(iph1, n, encrypted)
	struct ph1handle *iph1;
	struct isakmp_pl_lb *n;
	int encrypted;
{

	if (iph1->side != INITIATOR)
	{
		plog(LLV_DEBUG, LOCATION, NULL,
			"LOAD-BALANCE notification ignored - we are not the initiator.\n");
		return 0;
	}
	if (iph1->remote->ss_family != AF_INET) {
		plog(LLV_DEBUG, LOCATION, NULL,
			"LOAD-BALANCE notification ignored - only supported for IPv4.\n");
		return 0;
	}
	if (!encrypted) {
		plog(LLV_DEBUG, LOCATION, NULL,
			"LOAD-BALANCE notification ignored - not protected.\n");
		return 0;
	}
	if (ntohs(n->h.len) != sizeof(struct isakmp_pl_lb)) {
		plog(LLV_DEBUG, LOCATION, NULL,
			"Invalid length of payload\n");
		return -1;
	}	
	vpncontrol_notify_ike_failed(ISAKMP_NTYPE_LOAD_BALANCE, FROM_REMOTE,
		((struct sockaddr_in*)iph1->remote)->sin_addr.s_addr, 4, (u_int8_t*)(&(n->address)));
	
	plog(LLV_DEBUG, LOCATION, iph1->remote,
			"received LOAD_BALANCE notification - redirect address=%x.\n",
				ntohl(n->address));

    if (((struct sockaddr_in*)iph1->remote)->sin_addr.s_addr != ntohl(n->address)) {
        plog(LLV_DEBUG, LOCATION, iph1->remote,
             "deleting old phase1 because of LOAD_BALANCE notification - redirect address=%x.\n",
             ntohl(n->address));

        if (iph1->status == PHASE1ST_ESTABLISHED) {
            isakmp_info_send_d1(iph1);
        }
        isakmp_ph1expire(iph1);
    }

	return 0;
}
#endif

#ifdef ENABLE_DPD
static int
isakmp_info_recv_r_u (iph1, ru, msgid)
	struct ph1handle *iph1;
	struct isakmp_pl_ru *ru;
	u_int32_t msgid;
{
	struct isakmp_pl_ru *ru_ack;
	vchar_t *payload = NULL;
	int tlen;
	int error = 0;

	plog(LLV_DEBUG, LOCATION, iph1->remote,
		 "DPD R-U-There received\n");

	/* XXX should compare cookies with iph1->index?
	   Or is this already done by calling function?  */
	tlen = sizeof(*ru_ack);
	payload = vmalloc(tlen);
	if (payload == NULL) { 
		IPSECSESSIONTRACEREVENT(iph1->parent_session,
								IPSECSESSIONEVENTCODE_IKEV1_INFO_NOTICE_TX_FAIL,
								CONSTSTR("R-U-THERE? ACK"),
								CONSTSTR("Failed to transmit DPD response"));
		plog(LLV_ERROR, LOCATION, NULL,
			"failed to get buffer to send.\n");
		return errno;
	}

	ru_ack = (struct isakmp_pl_ru *)payload->v;
	ru_ack->h.np = ISAKMP_NPTYPE_NONE;
	ru_ack->h.len = htons(tlen);
	ru_ack->doi = htonl(IPSEC_DOI);
	ru_ack->type = htons(ISAKMP_NTYPE_R_U_THERE_ACK);
	ru_ack->proto_id = IPSECDOI_PROTO_ISAKMP; /* XXX ? */
	ru_ack->spi_size = sizeof(isakmp_index);
	memcpy(ru_ack->i_ck, ru->i_ck, sizeof(cookie_t));
	memcpy(ru_ack->r_ck, ru->r_ck, sizeof(cookie_t));	
	ru_ack->data = ru->data;

	/* XXX Should we do FLAG_A ?  */
	error = isakmp_info_send_common(iph1, payload, ISAKMP_NPTYPE_N,
					ISAKMP_FLAG_E);
	vfree(payload);
	if (error) {
		IPSECSESSIONTRACEREVENT(iph1->parent_session,
								IPSECSESSIONEVENTCODE_IKEV1_INFO_NOTICE_TX_FAIL,
								CONSTSTR("R-U-THERE? ACK"),
								CONSTSTR("Failed to transmit DPD ack"));
	} else {
		IPSECSESSIONTRACEREVENT(iph1->parent_session,
								IPSECSESSIONEVENTCODE_IKEV1_INFO_NOTICE_TX_SUCC,
								CONSTSTR("R-U-THERE? ACK"),
								CONSTSTR(NULL));
	}

	plog(LLV_DEBUG, LOCATION, NULL, "received a valid R-U-THERE, ACK sent\n");

	/* Should we mark tunnel as active ? */
	return error;
}

static int
isakmp_info_recv_r_u_ack (iph1, ru, msgid)
	struct ph1handle *iph1;
	struct isakmp_pl_ru *ru;
	u_int32_t msgid;
{

	plog(LLV_DEBUG, LOCATION, iph1->remote,
		 "DPD R-U-There-Ack received\n");

	/* XXX Maintain window of acceptable sequence numbers ?
	 * => ru->data <= iph2->dpd_seq &&
	 *    ru->data >= iph2->dpd_seq - iph2->dpd_fails ? */
	if (ntohl(ru->data) != iph1->dpd_seq) {
		plog(LLV_ERROR, LOCATION, iph1->remote,
			 "Wrong DPD sequence number (%d, %d expected).\n", 
			 ntohl(ru->data), iph1->dpd_seq);
		return 0;
	}

	if (memcmp(ru->i_ck, iph1->index.i_ck, sizeof(cookie_t)) ||
	    memcmp(ru->r_ck, iph1->index.r_ck, sizeof(cookie_t))) {
		plog(LLV_ERROR, LOCATION, iph1->remote,
			 "Cookie mismatch in DPD ACK!.\n");
		return 0;
	}

	iph1->dpd_fails = 0;

	iph1->dpd_seq++;

	/* Useless ??? */
	iph1->dpd_lastack = time(NULL);

	SCHED_KILL(iph1->dpd_r_u);

	isakmp_sched_r_u(iph1, 0);

	if (iph1->side == INITIATOR) {
		IPSECSESSIONTRACEREVENT(iph1->parent_session,
								IPSECSESSIONEVENTCODE_IKEV1_DPD_INIT_RESP,
								CONSTSTR("Initiator DPD Response"),
								CONSTSTR(NULL));
	} else {
		IPSECSESSIONTRACEREVENT(iph1->parent_session,
								IPSECSESSIONEVENTCODE_IKEV1_DPD_RESP_RESP,
								CONSTSTR("Responder DPD Response"),
								CONSTSTR(NULL));
	}
	plog(LLV_DEBUG, LOCATION, NULL, "received an R-U-THERE-ACK\n");

#ifdef ENABLE_VPNCONTROL_PORT
	vpncontrol_notify_peer_resp_ph1(1, iph1);
#endif /* ENABLE_VPNCONTROL_PORT */

	return 0;
}


/*
 * send Delete payload (for ISAKMP SA) in Informational exchange.
 */
void
isakmp_info_send_r_u(arg)
	void *arg;
{
	struct ph1handle *iph1 = arg;

	/* create R-U-THERE payload */
	struct isakmp_pl_ru *ru;
	vchar_t *payload = NULL;
	int tlen;
	int error = 0;

    if (iph1->status != PHASE1ST_ESTABLISHED) {
        plog(LLV_DEBUG, LOCATION, iph1->remote, "DPD r-u send aborted, invalid phase1 status %d....\n",
             iph1->status);
        return;
    }

	if (iph1->dpd_fails >= iph1->rmconf->dpd_maxfails) {
		u_int32_t address;

		IPSECSESSIONTRACEREVENT(iph1->parent_session,
								IPSECSESSIONEVENTCODE_IKEV1_DPD_MAX_RETRANSMIT,
								CONSTSTR("DPD maximum retransmits"),
								CONSTSTR("maxed-out of DPD requests without receiving an ack"));

		EVT_PUSH(iph1->local, iph1->remote, EVTT_DPD_TIMEOUT, NULL);
		if (iph1->remote->ss_family == AF_INET)
			address = ((struct sockaddr_in *)iph1->remote)->sin_addr.s_addr;
		else
			address = 0;
		(void)vpncontrol_notify_ike_failed(VPNCTL_NTYPE_PEER_DEAD, FROM_LOCAL, address, 0, NULL);

		purge_remote(iph1);
		plog(LLV_DEBUG, LOCATION, iph1->remote,
			 "DPD: remote seems to be dead\n");

		/* Do not reschedule here: phase1 is deleted,
		 * DPD will be reactivated when a new ph1 will be negociated
		 */
		return;
	}

	tlen = sizeof(*ru);
	payload = vmalloc(tlen);
	if (payload == NULL) {
		IPSECSESSIONTRACEREVENT(iph1->parent_session,
								IPSECSESSIONEVENTCODE_IKEV1_INFO_NOTICE_TX_FAIL,
								CONSTSTR("R-U-THERE?"),
								CONSTSTR("Failed to transmit DPD request"));
		plog(LLV_ERROR, LOCATION, NULL, 
			 "failed to get buffer for payload.\n");
		return;
	}
	ru = (struct isakmp_pl_ru *)payload->v;
	ru->h.np = ISAKMP_NPTYPE_NONE;
	ru->h.len = htons(tlen);
	ru->doi = htonl(IPSEC_DOI);
	ru->type = htons(ISAKMP_NTYPE_R_U_THERE);
	ru->proto_id = IPSECDOI_PROTO_ISAKMP; /* XXX ?*/
	ru->spi_size = sizeof(isakmp_index);

	memcpy(ru->i_ck, iph1->index.i_ck, sizeof(cookie_t));
	memcpy(ru->r_ck, iph1->index.r_ck, sizeof(cookie_t));

	if (iph1->dpd_seq == 0){
		/* generate a random seq which is not too big */
		srand(time(NULL));
		iph1->dpd_seq = rand() & 0x0fff;
	}

	ru->data = htonl(iph1->dpd_seq);

	error = isakmp_info_send_common(iph1, payload, ISAKMP_NPTYPE_N, 0);
	vfree(payload);
	if (error) {
		IPSECSESSIONTRACEREVENT(iph1->parent_session,
								IPSECSESSIONEVENTCODE_IKEV1_INFO_NOTICE_TX_FAIL,
								CONSTSTR("R-U-THERE?"),
								CONSTSTR("Failed to transmit DPD request"));
	} else {
		IPSECSESSIONTRACEREVENT(iph1->parent_session,
								IPSECSESSIONEVENTCODE_IKEV1_INFO_NOTICE_TX_SUCC,
								CONSTSTR("R-U-THERE?"),
								CONSTSTR(NULL));
	}

	if (iph1->side == INITIATOR) {
		IPSECSESSIONTRACEREVENT(iph1->parent_session,
								iph1->dpd_fails? IPSECSESSIONEVENTCODE_IKEV1_DPD_INIT_RETRANSMIT : IPSECSESSIONEVENTCODE_IKEV1_DPD_INIT_REQ,
								CONSTSTR("Initiator DPD Request"),
								CONSTSTR(NULL));
	} else {
		IPSECSESSIONTRACEREVENT(iph1->parent_session,
								iph1->dpd_fails? IPSECSESSIONEVENTCODE_IKEV1_DPD_RESP_RETRANSMIT : IPSECSESSIONEVENTCODE_IKEV1_DPD_RESP_REQ,
								CONSTSTR("Responder DPD Request"),
								CONSTSTR(NULL));
	}
	plog(LLV_DEBUG, LOCATION, iph1->remote,
		 "DPD R-U-There sent (%d)\n", error);

	/* will be decreased if ACK received... */
	iph1->dpd_fails++;

	/* Reschedule the r_u_there with a short delay,
	 * will be deleted/rescheduled if ACK received before */
	isakmp_sched_r_u(iph1, 1);

	plog(LLV_DEBUG, LOCATION, iph1->remote,
		 "rescheduling send_r_u (%d).\n", iph1->rmconf->dpd_retry);
}

/*
 * monitor DPD (ALGORITHM_INBOUND_DETECT) Informational exchange.
 */
static void
isakmp_info_monitor_r_u_algo_inbound_detect (struct ph1handle *iph1)
{
    if (iph1->status != PHASE1ST_ESTABLISHED) {
        plog(LLV_DEBUG, LOCATION, iph1->remote, "DPD monitoring (for ALGORITHM_INBOUND_DETECT) aborted, invalid phase1 status %d....\n",
             iph1->status);
        return;
    }

	plog(LLV_DEBUG, LOCATION, iph1->remote, "DPD monitoring (for ALGORITHM_INBOUND_DETECT) ....\n");
    
    // check phase1 for ike packets received from peer
    if (iph1->peer_sent_ike) {
        // yes, reshedule check
        iph1->peer_sent_ike = 0;
        
        /* ike packets received from peer... reschedule dpd */
        isakmp_sched_r_u(iph1, 0);
        
        plog(LLV_DEBUG, LOCATION, iph1->remote,
             "ike packets received from peer... reschedule monitor.\n");

        return;
    }

    // after ike packets, next we check if any data was received
    if (!iph1->parent_session->peer_sent_data_sc_dpd) {
        isakmp_info_send_r_u(iph1);
    } else {
        isakmp_sched_r_u(iph1, 0);
        
        plog(LLV_DEBUG, LOCATION, iph1->remote,
             "rescheduling DPD monitoring (for ALGORITHM_INBOUND_DETECT).\n");
    }
    iph1->parent_session->peer_sent_data_sc_dpd = 0;
}

/*
 * monitor DPD (ALGORITHM_BLACKHOLE_DETECT) Informational exchange.
 */
static void
isakmp_info_monitor_r_u_algo_blackhole_detect (struct ph1handle *iph1)
{
    if (iph1->status != PHASE1ST_ESTABLISHED) {
        plog(LLV_DEBUG, LOCATION, iph1->remote, "DPD monitoring (for ALGORITHM_BLACKHOLE_DETECT) aborted, invalid phase1 status %d....\n",
             iph1->status);
        return;
    }

	plog(LLV_DEBUG, LOCATION, iph1->remote, "DPD monitoring (for ALGORITHM_BLACKHOLE_DETECT) ....\n");

    // check if data was sent but none was received
    if (iph1->parent_session->i_sent_data_sc_dpd &&
        !iph1->parent_session->peer_sent_data_sc_dpd) {
        isakmp_info_send_r_u(iph1);
    } else {
        isakmp_sched_r_u(iph1, 0);
        
        plog(LLV_DEBUG, LOCATION, iph1->remote,
             "rescheduling DPD monitoring (for ALGORITHM_BLACKHOLE_DETECT) i = %d, peer %d.\n",
             iph1->parent_session->i_sent_data_sc_dpd,
             iph1->parent_session->peer_sent_data_sc_dpd);
    }
    iph1->parent_session->i_sent_data_sc_dpd = 0;
    iph1->parent_session->peer_sent_data_sc_dpd = 0;
}

/*
 * monitor DPD Informational exchange.
 */
static void
isakmp_info_monitor_r_u(arg)
void *arg;
{
	struct ph1handle *iph1 = arg;

    if (iph1 && iph1->rmconf) {
        if (iph1->rmconf->dpd_algo == DPD_ALGO_INBOUND_DETECT) {
            isakmp_info_monitor_r_u_algo_inbound_detect(iph1);
        } else if (iph1->rmconf->dpd_algo == DPD_ALGO_BLACKHOLE_DETECT) {
            isakmp_info_monitor_r_u_algo_blackhole_detect(iph1);
        } else {
            plog(LLV_DEBUG, LOCATION, iph1->remote, "DPD monitoring aborted, invalid algorithm %d....\n",
                 iph1->rmconf->dpd_algo);
        }
    }
}

/* Schedule a new R-U-THERE */
int
isakmp_sched_r_u(iph1, retry)
	struct ph1handle *iph1;
	int retry;
{
	if(iph1 == NULL ||
	   iph1->rmconf == NULL)
		return 1;


	if(iph1->dpd_support == 0 ||
	   iph1->rmconf->dpd_interval == 0)
		return 0;

	if(retry) {
		iph1->dpd_r_u = sched_new(iph1->rmconf->dpd_retry,
								  isakmp_info_send_r_u, iph1);
	} else {
        if (iph1->rmconf->dpd_algo == DPD_ALGO_INBOUND_DETECT ||
            iph1->rmconf->dpd_algo == DPD_ALGO_BLACKHOLE_DETECT) {
            iph1->dpd_r_u = sched_new(iph1->rmconf->dpd_interval,
                                      isakmp_info_monitor_r_u, iph1);
        } else {
            iph1->dpd_r_u = sched_new(iph1->rmconf->dpd_interval,
                                      isakmp_info_send_r_u, iph1);
        }
    }

	return 0;
}

/*
 * punts dpd for later because of some activity that:
 * 1) implicitly does dpd (e.g. phase2 exchanges), or
 * 2) indicates liveness (e.g. received ike packets).
 */
void
isakmp_reschedule_info_monitor_if_pending (struct ph1handle *iph1,
                                          char             *reason)
{
    if (!iph1 ||
        iph1->status != PHASE1ST_ESTABLISHED ||
        !iph1->dpd_support ||
        !iph1->rmconf->dpd_interval ||
        iph1->rmconf->dpd_algo == DPD_ALGO_DEFAULT) {
        return;
    }

    if (!iph1->peer_sent_ike) {
        SCHED_KILL(iph1->dpd_r_u);

        isakmp_sched_r_u(iph1, 0);

        plog(LLV_DEBUG, LOCATION, iph1->remote,
             "%s... rescheduling send_r_u.\n",
             reason);
    }
    iph1->peer_sent_ike++;
}
#endif
