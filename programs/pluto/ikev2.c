/* demultiplex incoming IKE messages
 *
 * Copyright (C) 1997 Angelos D. Keromytis.
 * Copyright (C) 1998-2010,2013-2017 D. Hugh Redelmeier <hugh@mimosa.com>
 * Copyright (C) 2007-2008 Michael Richardson <mcr@xelerance.com>
 * Copyright (C) 2009 David McCullough <david_mccullough@securecomputing.com>
 * Copyright (C) 2008-2011 Paul Wouters <paul@xelerance.com>
 * Copyright (C) 2010 Simon Deziel <simon@xelerance.com>
 * Copyright (C) 2010 Tuomo Soini <tis@foobar.fi>
 * Copyright (C) 2011-2012 Avesh Agarwal <avagarwa@redhat.com>
 * Copyright (C) 2012 Paul Wouters <paul@libreswan.org>
 * Copyright (C) 2012-2019 Paul Wouters <pwouters@redhat.com>
 * Copyright (C) 2013 Matt Rogers <mrogers@redhat.com>
 * Copyright (C) 2015-2019 Andrew Cagney
 * Copyright (C) 2016-2018 Antony Antony <appu@phenome.org>
 * Copyright (C) 2017 Sahana Prasad <sahana.prasad07@gmail.com>
 * Copyright (C) 2020 Yulia Kuzovkova <ukuzovkova@gmail.com>
 * Copyright (C) 2021 Paul Wouters <paul.wouters@aiven.io>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <https://www.gnu.org/licenses/gpl2.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 */

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>


#include "sysdep.h"
#include "constants.h"

#include "defs.h"
#include "id.h"
#include "x509.h"
#include "pluto_x509.h"
#include "certs.h"
#include "connections.h"        /* needs id.h */
#include "state.h"
#include "packet.h"
#include "crypto.h"
#include "crypt_symkey.h"
#include "ike_alg.h"
#include "log.h"
#include "demux.h"      /* needs packet.h */
#include "ikev2.h"
#include "ipsec_doi.h"  /* needs demux.h and state.h */
#include "timer.h"
#include "whack.h"      /* requires connections.h */
#include "server.h"
#include "nat_traversal.h"
#include "vendor.h"
#include "ip_address.h"
#include "ikev2_send.h"
#include "state_db.h"
#include "ietf_constants.h"
#include "ikev2_cookie.h"
#include "plutoalg.h" /* for default_ike_groups */
#include "ikev2_message.h"	/* for ikev2_decrypt_msg() */
#include "pluto_stats.h"
#include "keywords.h"
#include "ikev2_msgid.h"
#include "ikev2_redirect.h"
#include "ikev2_states.h"
#include "ip_endpoint.h"
#include "host_pair.h"		/* for find_v2_host_connection() */
#include "kernel.h"
#include "iface.h"
#include "ikev2_notify.h"
#include "unpack.h"
#include "pending.h"		/* for release_pending_whacks() */
#include "ikev2_host_pair.h"
#include "ikev2_ike_sa_init.h"
#include "ikev2_informational.h"
#include "ikev2_create_child_sa.h"
#include "ikev2_ike_intermediate.h"
#include "ikev2_ike_auth.h"
#include "ikev2_delete.h"		/* for record_v2_delete() */
#include "ikev2_child.h"		/* for jam_v2_child_sa_details() */

/*
 * IKEv2 has slightly different states than IKEv1.
 *
 * IKEv2 puts all the responsibility for retransmission on the end that
 * wants to do something, usually, that the initiator. (But, not always
 * the original initiator, of the responder decides it needs to rekey first)
 *
 * Each exchange has a bit that indicates if it is an Initiator message,
 * or if it is a response.  The Responder never retransmits its messages
 * except in response to an Initiator retransmission.
 *
 * The message ID is *NOT* used in the cryptographic state at all, but instead
 * serves the role of a sequence number.  This makes the state machine far
 * simpler, and there really are no exceptions.
 *
 * The upper level state machine is therefore much simpler.
 * The lower level takes care of retransmissions, and the upper layer state
 * machine just has to worry about whether it needs to go into cookie mode,
 * etc.
 *
 * Like IKEv1, IKEv2 can have multiple child SAs.  Like IKEv1, each one of
 * the child SAs ("Phase 2") will get their own state. Unlike IKEv1,
 * an implementation may negotiate multiple CHILD_SAs at the same time
 * using different MessageIDs.  This is enabled by an option (a notify)
 * that the responder sends to the initiator.  The initiator may only
 * do concurrent negotiations if it sees the notify.
 *
 * XXX This implementation does not support concurrency, but it shouldn't be
 *     that hard to do.  The most difficult part will be to map the message IDs
 *     to the right state. Some CHILD_SAs may take multiple round trips,
 *     and each one will have to be mapped to the same state.
 *
 * The IKEv2 state values are chosen from the same state space as IKEv1.
 *
 */

/*
 * From RFC 5996 syntax: [optional] and {encrypted}
 *
 * Initiator                         Responder
 * -------------------------------------------------------------------
 *
 * IKE_SA_INIT exchange (initial exchange):
 *
 * HDR, SAi1, KEi, Ni            -->
 *                                 <--  HDR, SAr1, KEr, Nr, [CERTREQ]
 *
 * IKE_AUTH exchange (after IKE_SA_INIT exchange):
 *
 * HDR, SK {IDi, [CERT,] [CERTREQ,]
 *        [IDr,] AUTH, SAi2,
 *        TSi, TSr}              -->
 *                                 <--  HDR, SK {IDr, [CERT,] AUTH,
 *                                           SAr2, TSi, TSr}
 * [Parent SA (SAx1) established. Child SA (SAx2) may have been established]
 *
 *
 * Extended IKE_AUTH (see RFC 5996bis 2.6):
 *
 * HDR(A,0), SAi1, KEi, Ni  -->
 *                              <--  HDR(A,0), N(COOKIE)
 * HDR(A,0), N(COOKIE), SAi1,
 *     KEi, Ni  -->
 *                              <--  HDR(A,B), SAr1, KEr,
 *                                       Nr, [CERTREQ]
 * HDR(A,B), SK {IDi, [CERT,]
 *     [CERTREQ,] [IDr,] AUTH,
 *     SAi2, TSi, TSr}  -->
 *                              <--  HDR(A,B), SK {IDr, [CERT,]
 *                                       AUTH, SAr2, TSi, TSr}
 * [Parent SA (SAx1) established. Child SA (SAx2) may have been established]
 *
 *
 * CREATE_CHILD_SA Exchange (new child variant RFC 5996 1.3.1):
 *
 * HDR, SK {SA, Ni, [KEi],
 *            TSi, TSr}  -->
 *                              <--  HDR, SK {SA, Nr, [KEr],
 *                                       TSi, TSr}
 *
 *
 * CREATE_CHILD_SA Exchange (rekey child variant RFC 5996 1.3.3):
 *
 * HDR, SK {N(REKEY_SA), SA, Ni, [KEi],
 *     TSi, TSr}   -->
 *                    <--  HDR, SK {SA, Nr, [KEr],
 *                             TSi, TSr}
 *
 *
 * CREATE_CHILD_SA Exchange (rekey parent SA variant RFC 5996 1.3.2):
 *
 * HDR, SK {SA, Ni, KEi} -->
 *                            <--  HDR, SK {SA, Nr, KEr}
 */

/* Short forms for building payload type sets */

#define P(N) LELEM(ISAKMP_NEXT_v2##N)

/*
 * IKEv2 State transitions (aka microcodes).
 *
 * This table contains all possible state transitions, some of which
 * involve a message.
 *
 * During initialization this table parsed populating the
 * corresponding IKEv2 finite states.  While not the most efficient,
 * it seems to work.
 */

static /*const*/ struct v2_state_transition v2_state_transition_table[] = {

#define req_clear_payloads message_payloads.required   /* required unencrypted payloads (allows just one) for received packet */
#define opt_clear_payloads message_payloads.optional   /* optional unencrypted payloads (none or one) for received packet */
#define req_enc_payloads   encrypted_payloads.required /* required encrypted payloads (allows just one) for received packet */
#define opt_enc_payloads   encrypted_payloads.optional /* optional encrypted payloads (none or one) for received packet */

	/* no state:   --> I1
	 * HDR, SAi1, KEi, Ni -->
	 */
	{ .story      = "initiate IKE_SA_INIT",
	  .state      = STATE_PARENT_I0,
	  .next_state = STATE_PARENT_I1,
	  .send       = MESSAGE_REQUEST,
	  .processor  = NULL,
	  .timeout_event = EVENT_RETRANSMIT, },

	/* STATE_PARENT_I1: R1B --> I1B
	 *                     <--  HDR, N
	 * HDR, N, SAi1, KEi, Ni -->
	 */

	{ .story      = "received anti-DDOS COOKIE notify response; resending IKE_SA_INIT request with cookie payload added",
	  .state      = STATE_PARENT_I1,
	  .next_state = STATE_PARENT_I0,
	  .flags = SMF2_SUPPRESS_SUCCESS_LOG,
	  .send       = NO_MESSAGE,
	  .message_payloads = { .required = P(N), .notification = v2N_COOKIE, },
	  .processor = ikev2_in_IKE_SA_INIT_R_v2N_COOKIE,
	  .recv_role  = MESSAGE_RESPONSE,
	  .recv_type  = ISAKMP_v2_IKE_SA_INIT,
	  .timeout_event = EVENT_SA_DISCARD, },

	{ .story      = "received IKE_SA_INIT INVALID_KE_PAYLOAD notify response; resending IKE_SA_INIT with new KE payload",
	  .state      = STATE_PARENT_I1,
	  .next_state = STATE_PARENT_I0,
	  .flags = SMF2_SUPPRESS_SUCCESS_LOG,
	  .send       = NO_MESSAGE,
	  .message_payloads = { .required = P(N), .notification = v2N_INVALID_KE_PAYLOAD, },
	  .processor = process_v2_IKE_SA_INIT_response_v2N_INVALID_KE_PAYLOAD,
	  .recv_role  = MESSAGE_RESPONSE,
	  .recv_type  = ISAKMP_v2_IKE_SA_INIT,
	  .timeout_event = EVENT_SA_DISCARD, },

	{ .story      = "received REDIRECT notify response; resending IKE_SA_INIT request to new destination",
	  .state      = STATE_PARENT_I1,
	  .next_state = STATE_IKESA_DEL,
	  .flags = SMF2_SUPPRESS_SUCCESS_LOG,
	  .send       = NO_MESSAGE,
	  .message_payloads = { .required = P(N), .notification = v2N_REDIRECT, },
	  .processor = ikev2_in_IKE_SA_INIT_R_v2N_REDIRECT,
	  .recv_role  = MESSAGE_RESPONSE,
	  .recv_type  = ISAKMP_v2_IKE_SA_INIT,
	  /* XXX: this is an instant timeout */
	  .timeout_event = EVENT_v2_REDIRECT,
	},

	/* STATE_PARENT_I1: R1 --> I2
	 *                     <--  HDR, SAr1, KEr, Nr, [CERTREQ]
	 * HDR, SK {IDi, [CERT,] [CERTREQ,]
	 *      [IDr,] AUTH, SAi2,
	 *      TSi, TSr}      -->
	 */
	{ .story      = "Initiator: process IKE_SA_INIT reply, initiate IKE_AUTH or IKE_INTERMEDIATE",
	  .state      = STATE_PARENT_I1,
	  .next_state = STATE_PARENT_I2,
	  .send       = MESSAGE_REQUEST,
	  .req_clear_payloads = P(SA) | P(KE) | P(Nr),
	  .opt_clear_payloads = P(CERTREQ),
	  .processor  = process_v2_IKE_SA_INIT_response,
	  .recv_role  = MESSAGE_RESPONSE,
	  .recv_type  = ISAKMP_v2_IKE_SA_INIT,
	  .timeout_event = EVENT_RETRANSMIT, },

	{ .story      = "Initiator: process IKE_INTERMEDIATE reply, initiate IKE_AUTH or IKE_INTERMEDIATE",
	  .state      = STATE_PARENT_I2,
	  .next_state = STATE_PARENT_I2,
	  .flags = MESSAGE_RESPONSE,
	  .send = MESSAGE_REQUEST,
	  .req_clear_payloads = P(SK),
	  .opt_clear_payloads = LEMPTY,
	  .processor  = process_v2_IKE_INTERMEDIATE_response,
	  .recv_role  = MESSAGE_RESPONSE,
	  .recv_type  = ISAKMP_v2_IKE_INTERMEDIATE,
	  .timeout_event = EVENT_RETRANSMIT, },

	/* STATE_PARENT_I2: R2 -->
	 *                     <--  HDR, SK {IDr, [CERT,] AUTH,
	 *                               SAr2, TSi, TSr}
	 * [Parent SA established]
	 */

	/*
	 * This pair of state transitions should be merged?
	 */
	{ .story      = "Initiator: process IKE_AUTH response",
	  .state      = STATE_PARENT_I2,
	  .next_state = STATE_V2_ESTABLISHED_IKE_SA,
	  /* logged mid transition */
	  .flags      = SMF2_SUPPRESS_SUCCESS_LOG|SMF2_RELEASE_WHACK,
	  .req_clear_payloads = P(SK),
	  .req_enc_payloads = P(IDr) | P(AUTH),
	  .opt_enc_payloads = P(CERT) | P(CP) | P(SA) | P(TSi) | P(TSr),
	  .processor  = process_v2_IKE_AUTH_response,
	  .recv_role  = MESSAGE_RESPONSE,
	  .recv_type  = ISAKMP_v2_IKE_AUTH,
	  .timeout_event = EVENT_SA_REPLACE,
	},

	{ .story      = "Initiator: processing IKE_AUTH failure response",
	  .state      = STATE_PARENT_I2,
	  .next_state = STATE_PARENT_I2,
	  .message_payloads = { .required = P(SK), },
	  /* .encrypted_payloads = { .required = P(N), }, */
	  .processor  = process_v2_IKE_AUTH_failure_response,
	  .recv_role  = MESSAGE_RESPONSE,
	  .recv_type  = ISAKMP_v2_IKE_AUTH,
	},

	/* no state: none I1 --> R1
	 *                <-- HDR, SAi1, KEi, Ni
	 * HDR, SAr1, KEr, Nr, [CERTREQ] -->
	 */
	{ .story      = "Respond to IKE_SA_INIT",
	  .state      = STATE_PARENT_R0,
	  .next_state = STATE_PARENT_R1,
	  .send       = MESSAGE_RESPONSE,
	  .req_clear_payloads = P(SA) | P(KE) | P(Ni),
	  .processor  = process_v2_IKE_SA_INIT_request,
	  .recv_role  = MESSAGE_REQUEST,
	  .recv_type  = ISAKMP_v2_IKE_SA_INIT,
	  .timeout_event = EVENT_SA_DISCARD, },

	/* STATE_PARENT_R1: I2 --> R2
	 *                  <-- HDR, SK {IDi, [CERT,] [CERTREQ,]
	 *                             [IDr,] AUTH, SAi2,
	 *                             TSi, TSr}
	 * HDR, SK {IDr, [CERT,] AUTH,
	 *      SAr2, TSi, TSr} -->
	 *
	 * [Parent SA established]
	 */
	{ .story      = "Responder: process IKE_AUTH request (no SKEYSEED)",
	  .state      = STATE_PARENT_R1,
	  .next_state = STATE_PARENT_R1,
	  .flags = SMF2_NO_SKEYSEED,
	  .send       = MESSAGE_RESPONSE,
	  .req_clear_payloads = P(SK),
	  .req_enc_payloads = LEMPTY,
	  .opt_enc_payloads = LEMPTY,
	  .processor  = process_v2_IKE_AUTH_request_no_skeyseed,
	  .recv_role  = MESSAGE_REQUEST,
	  .recv_type  = ISAKMP_v2_IKE_AUTH,
	  .timeout_event = EVENT_SA_REPLACE, },

	{ .story      = "Responder: process IKE_INTERMEDIATE request (no SKEYSEED)",
	  .state      = STATE_PARENT_R1,
	  .next_state = STATE_PARENT_R1,
	  .flags = MESSAGE_REQUEST | SMF2_NO_SKEYSEED,
	  .send = MESSAGE_RESPONSE,
	  .req_clear_payloads = P(SK),
	  .req_enc_payloads = LEMPTY,
	  .opt_enc_payloads = LEMPTY,
	  .processor  = process_v2_IKE_INTERMEDIATE_request_no_skeyseed,
	  .recv_role  = MESSAGE_REQUEST,
	  .recv_type  = ISAKMP_v2_IKE_INTERMEDIATE,
	  .timeout_event = EVENT_SA_REPLACE, },

	{ .story      = "Responder: process IKE_INTERMEDIATE request (with SKEYSEED)",
	  .state      = STATE_PARENT_R1,
	  .next_state = STATE_PARENT_R1,
	  .flags = MESSAGE_REQUEST,
	  .send = MESSAGE_RESPONSE,
	  .req_clear_payloads = P(SK),
	  .req_enc_payloads = LEMPTY,
	  .opt_enc_payloads = LEMPTY,
	  .processor  = process_v2_IKE_INTERMEDIATE_request,
	  .recv_role  = MESSAGE_REQUEST,
	  .recv_type  = ISAKMP_v2_IKE_INTERMEDIATE,
	  .timeout_event = EVENT_SA_REPLACE, },

	/*
	 * These two transitions should be merged; the no-child
	 * variant is just so that the code can be hobbled.
	 */

	{ .story      = "Responder: process IKE_AUTH request",
	  .state      = STATE_PARENT_R1,
	  .next_state = STATE_V2_ESTABLISHED_IKE_SA,
	  .flags      = SMF2_SUPPRESS_SUCCESS_LOG|SMF2_RELEASE_WHACK,
	  .send       = MESSAGE_RESPONSE,
	  .req_clear_payloads = P(SK),
	  .req_enc_payloads = P(IDi) | P(AUTH),
	  .opt_enc_payloads = P(CERT) | P(CERTREQ) | P(IDr) | P(CP) | P(SA) | P(TSi) | P(TSr),
	  .processor  = process_v2_IKE_AUTH_request,
	  .recv_role  = MESSAGE_REQUEST,
	  .recv_type  = ISAKMP_v2_IKE_AUTH,
	  .timeout_event = EVENT_SA_REPLACE, },

	/*
	 * There are three different CREATE_CHILD_SA's invocations,
	 * this is the combined write up (not in RFC). See above for
	 * individual cases from RFC.
	 *
	 * The order that they rekey here matters.
	 *
	 * HDR, SK {SA, Ni, [KEi], [N(REKEY_SA)], [TSi, TSr]} -->
	 *                <-- HDR, SK {N}
	 *                <-- HDR, SK {SA, Nr, [KEr], [TSi, TSr]}
	 */

	/*
	 * CREATE_CHILD_SA exchange to rekey IKE SA.
	 *
	 * In the responder, note the lack of TS (traffic selectors)
	 * payload.  Since rekey and create child exchanges contain TS
	 * they won't match.
	 */

	/* no state:   --> CREATE_CHILD IKE Rekey Request
	 * HDR, SAi, KEi, Ni -->
	 */

	{ .story      = "initiate rekey IKE_SA (CREATE_CHILD_SA)",
	  .state      = STATE_V2_REKEY_IKE_I0,
	  .next_state = STATE_V2_REKEY_IKE_I1,
	  .send       = MESSAGE_REQUEST,
	  .processor  = initiate_v2_CREATE_CHILD_SA_rekey_ike_request,
	  .timeout_event = EVENT_RETRANSMIT, },

	{ .story      = "process rekey IKE SA request (CREATE_CHILD_SA)",
	  .state      = STATE_V2_REKEY_IKE_R0,
	  .next_state = STATE_V2_ESTABLISHED_IKE_SA,
	  .send       = MESSAGE_RESPONSE,
	  .req_clear_payloads = P(SK),
	  .req_enc_payloads = P(SA) | P(Ni) | P(KE),
	  .opt_enc_payloads = P(N),
	  .processor  = process_v2_CREATE_CHILD_SA_rekey_ike_request,
	  .recv_role  = MESSAGE_REQUEST,
	  .recv_type  = ISAKMP_v2_CREATE_CHILD_SA,
	  .timeout_event = EVENT_SA_REPLACE },

	{ .story      = "process rekey IKE SA response (CREATE_CHILD_SA)",
	  .state      = STATE_V2_REKEY_IKE_I1,
	  .next_state = STATE_V2_ESTABLISHED_IKE_SA,
	  .req_clear_payloads = P(SK),
	  .req_enc_payloads = P(SA) | P(Ni) |  P(KE),
	  .opt_enc_payloads = P(N),
	  .processor  = process_v2_CREATE_CHILD_SA_rekey_ike_response,
	  .recv_role  = MESSAGE_RESPONSE,
	  .recv_type  = ISAKMP_v2_CREATE_CHILD_SA,
	  .timeout_event = EVENT_SA_REPLACE, },

	{ .story      = "process rekey IKE SA failure response (CREATE_CHILD_SA)",
	  .state      = STATE_V2_REKEY_IKE_I1,
	  .next_state = STATE_CHILDSA_DEL, /* never reached */
	  .flags      = SMF2_RELEASE_WHACK,
	  .message_payloads = { .required = P(SK), },
	  .processor  = process_v2_CREATE_CHILD_SA_failure_response,
	  .recv_role  = MESSAGE_RESPONSE,
	  .recv_type  = ISAKMP_v2_CREATE_CHILD_SA,
	  .timeout_event = EVENT_RETAIN, /* no timeout really */
	},

	/*
	 * CREATE_CHILD_SA exchange to rekey existing child.
	 *
	 * In the responder, note the presence of both TS (traffic
	 * selectors) payload and REKEY_SA notification.  Since a
	 * create child does not include the REKEY_SA notify it won't
	 * match.
	 */

	{ .story      = "initiate rekey Child SA (CREATE_CHILD_SA)",
	  .state      = STATE_V2_REKEY_CHILD_I0,
	  .next_state = STATE_V2_REKEY_CHILD_I1,
	  .send       = MESSAGE_REQUEST,
	  .processor  = initiate_v2_CREATE_CHILD_SA_rekey_child_request,
	  .timeout_event = EVENT_RETRANSMIT, },

	{ .story      = "process rekey Child SA request (CREATE_CHILD_SA)",
	  .state      = STATE_V2_REKEY_CHILD_R0,
	  .next_state = STATE_V2_ESTABLISHED_CHILD_SA,
	  .flags      = SMF2_RELEASE_WHACK,
	  .send       = MESSAGE_RESPONSE,
	  .message_payloads.required = P(SK),
	  .encrypted_payloads.required = P(SA) | P(Ni) | P(TSi) | P(TSr),
	  .encrypted_payloads.optional = P(KE) | P(N) | P(CP),
	  .encrypted_payloads.notification = v2N_REKEY_SA,
	  .processor  = process_v2_CREATE_CHILD_SA_rekey_child_request,
	  .recv_role  = MESSAGE_REQUEST,
	  .recv_type  = ISAKMP_v2_CREATE_CHILD_SA,
	  .timeout_event = EVENT_SA_REPLACE, },

	{ .story      = "process rekey Child SA response (CREATE_CHILD_SA)",
	  .state      = STATE_V2_REKEY_CHILD_I1,
	  .next_state = STATE_V2_ESTABLISHED_CHILD_SA,
	  .flags      = SMF2_RELEASE_WHACK,
	  .message_payloads.required = P(SK),
	  .encrypted_payloads.required = P(SA) | P(Ni) | P(TSi) | P(TSr),
	  .encrypted_payloads.optional = P(KE) | P(N) | P(CP),
	  .processor  = process_v2_CREATE_CHILD_SA_rekey_child_response,
	  .recv_role  = MESSAGE_RESPONSE,
	  .recv_type  = ISAKMP_v2_CREATE_CHILD_SA,
	  .timeout_event = EVENT_SA_REPLACE, },

	{ .story      = "process rekey Child SA failure response (CREATE_CHILD_SA)",
	  .state      = STATE_V2_REKEY_CHILD_I1,
	  .next_state = STATE_CHILDSA_DEL, /* never reached */
	  .flags      = SMF2_RELEASE_WHACK,
	  .message_payloads = { .required = P(SK), },
	  .processor  = process_v2_CREATE_CHILD_SA_failure_response,
	  .recv_role  = MESSAGE_RESPONSE,
	  .recv_type  = ISAKMP_v2_CREATE_CHILD_SA,
	  .timeout_event = EVENT_RETAIN, /* no timeout really */
	},

	/*
	 * CREATE_CHILD_SA exchange to create a new child.
	 *
	 * In the responder note the presence of just TS (traffic
	 * selectors) payloads.  Earlier rules will have weeded out
	 * both rekey IKE (no TS payload) and rekey Child (has
	 * REKEY_SA notify) leaving just create new child.
	 */

	{ .story      = "initiate create Child SA (CREATE_CHILD_SA)",
	  .state      = STATE_V2_NEW_CHILD_I0,
	  .next_state = STATE_V2_NEW_CHILD_I1,
	  .send       = MESSAGE_REQUEST,
	  .processor  = initiate_v2_CREATE_CHILD_SA_new_child_request,
	  .timeout_event = EVENT_RETRANSMIT, },

	{ .story      = "process create Child SA request (CREATE_CHILD_SA)",
	  .state      = STATE_V2_NEW_CHILD_R0,
	  .next_state = STATE_V2_ESTABLISHED_CHILD_SA,
	  .flags      = SMF2_RELEASE_WHACK,
	  .send       = MESSAGE_RESPONSE,
	  .req_clear_payloads = P(SK),
	  .req_enc_payloads = P(SA) | P(Ni) | P(TSi) | P(TSr),
	  .opt_enc_payloads = P(KE) | P(N) | P(CP),
	  .processor  = process_v2_CREATE_CHILD_SA_new_child_request,
	  .recv_role  = MESSAGE_REQUEST,
	  .recv_type  = ISAKMP_v2_CREATE_CHILD_SA,
	  .timeout_event = EVENT_SA_REPLACE, },

	{ .story      = "process create Child SA response (CREATE_CHILD_SA)",
	  .state      = STATE_V2_NEW_CHILD_I1,
	  .next_state = STATE_V2_ESTABLISHED_CHILD_SA,
	  .flags      = SMF2_RELEASE_WHACK,
	  .req_clear_payloads = P(SK),
	  .req_enc_payloads = P(SA) | P(Ni) | P(TSi) | P(TSr),
	  .opt_enc_payloads = P(KE) | P(N) | P(CP),
	  .processor  = process_v2_CREATE_CHILD_SA_new_child_response,
	  .recv_role  = MESSAGE_RESPONSE,
	  .recv_type  = ISAKMP_v2_CREATE_CHILD_SA,
	  .timeout_event = EVENT_SA_REPLACE, },

	{ .story      = "process create Child SA failure response (CREATE_CHILD_SA)",
	  .state      = STATE_V2_NEW_CHILD_I1,
	  .next_state = STATE_CHILDSA_DEL, /* never reached */
	  .flags      = SMF2_RELEASE_WHACK,
	  .message_payloads = { .required = P(SK), },
	  .processor  = process_v2_CREATE_CHILD_SA_failure_response,
	  .recv_role  = MESSAGE_RESPONSE,
	  .recv_type  = ISAKMP_v2_CREATE_CHILD_SA,
	  .timeout_event = EVENT_RETAIN, /* no timeout really */
	},

	/* Informational Exchange */

	/* RFC 5996 1.4 "The INFORMATIONAL Exchange"
	 *
	 * HDR, SK {[N,] [D,] [CP,] ...}  -->
	 *   <--  HDR, SK {[N,] [D,] [CP], ...}
	*
	 * A liveness exchange is a special empty message.
	 *
	 * XXX: since these just generate an empty response, they
	 * might as well have a dedicated liveness function.
	 *
	 * XXX: rather than all this transition duplication, the
	 * established states should share common transition stored
	 * outside of this table.
	 */

	{ .story      = "Informational Request (liveness probe)",
	  .state      = STATE_V2_ESTABLISHED_IKE_SA,
	  .next_state = STATE_V2_ESTABLISHED_IKE_SA,
	  .flags      = SMF2_SUPPRESS_SUCCESS_LOG,
	  .send       = MESSAGE_RESPONSE,
	  .message_payloads.required = P(SK),
	  .processor  = process_v2_INFORMATIONAL_request,
	  .recv_role  = MESSAGE_REQUEST,
	  .recv_type  = ISAKMP_v2_INFORMATIONAL,
	  .timeout_event = EVENT_RETAIN, },

	{ .story      = "Informational Response (liveness probe)",
	  .state      = STATE_V2_ESTABLISHED_IKE_SA,
	  .next_state = STATE_V2_ESTABLISHED_IKE_SA,
	  .flags      = SMF2_SUPPRESS_SUCCESS_LOG|SMF2_RELEASE_WHACK,
	  .message_payloads.required = P(SK),
	  .processor  = process_v2_INFORMATIONAL_response,
	  .recv_role  = MESSAGE_RESPONSE,
	  .recv_type  = ISAKMP_v2_INFORMATIONAL,
	  .timeout_event = EVENT_RETAIN, },

	{ .story      = "Informational Request",
	  .state      = STATE_V2_ESTABLISHED_IKE_SA,
	  .next_state = STATE_V2_ESTABLISHED_IKE_SA,
	  .send       = MESSAGE_RESPONSE,
	  .req_clear_payloads = P(SK),
	  .opt_enc_payloads = P(N) | P(D) | P(CP),
	  .processor  = process_v2_INFORMATIONAL_request,
	  .recv_role  = MESSAGE_REQUEST,
	  .recv_type  = ISAKMP_v2_INFORMATIONAL,
	  .timeout_event = EVENT_RETAIN, },

	{ .story      = "Informational Response",
	  .state      = STATE_V2_ESTABLISHED_IKE_SA,
	  .recv_type  = ISAKMP_v2_INFORMATIONAL,
	  .req_clear_payloads = P(SK),
	  .opt_enc_payloads = P(N) | P(D) | P(CP),
	  .processor  = process_v2_INFORMATIONAL_response,
	  .next_state = STATE_V2_ESTABLISHED_IKE_SA,
	  .recv_role  = MESSAGE_RESPONSE,
	  .timeout_event = EVENT_RETAIN, },

	{ .story      = "IKE_SA_DEL: process INFORMATIONAL response",
	  .state      = STATE_IKESA_DEL,
	  .next_state = STATE_IKESA_DEL,
	  .flags      = 0,
	  .req_clear_payloads = P(SK),
	  .opt_enc_payloads = P(N) | P(D) | P(CP),
	  .processor  = IKE_SA_DEL_process_v2_INFORMATIONAL_response,
	  .recv_role  = MESSAGE_RESPONSE,
	  .recv_type  = ISAKMP_v2_INFORMATIONAL,
	  .timeout_event = EVENT_RETAIN, },

	/* last entry */
	{ .story      = "roof",
	  .state      = STATE_IKEv2_ROOF }

#undef req_clear_payloads
#undef opt_clear_payloads
#undef req_enc_payloads
#undef opt_enc_payloads

};

void init_ikev2(void)
{
	dbg("checking IKEv2 state table");

	/*
	 * Fill in FINITE_STATES[].
	 *
	 * This is a hack until each finite-state is a separate object
	 * with corresponding edges (aka microcodes).
	 *
	 * XXX: Long term goal is to have a constant FINITE_STATES[]
	 * contain constant pointers and this static writeable array
	 * to just go away.
	 */
	for (enum state_kind kind = STATE_IKEv2_FLOOR; kind < STATE_IKEv2_ROOF; kind++) {
		/* fill in using static struct */
		const struct finite_state *fs = &v2_states[kind - STATE_IKEv2_FLOOR];
		passert(fs->kind == kind);
		passert(finite_states[kind] == NULL);
		finite_states[kind] = fs;
	}

	/*
	 * Iterate over the state transitions filling in missing bits
	 * and checking for consistency.
	 */
	for (struct v2_state_transition *t = v2_state_transition_table;
	     t->state < STATE_IKEv2_ROOF; t++) {

		passert(t->state >= STATE_IKEv2_FLOOR);
		passert(t->state < STATE_IKEv2_ROOF);
		struct finite_state *from = &v2_states[t->state - STATE_IKEv2_FLOOR];

		passert(t->next_state >= STATE_IKEv2_FLOOR);
		passert(t->next_state < STATE_IKEv2_ROOF);
		const struct finite_state *to = finite_states[t->next_state];
		passert(to != NULL);

		if (DBGP(DBG_BASE)) {
			if (from->nr_transitions == 0) {
				LSWLOG_DEBUG(buf) {
					jam(buf, "  ");
					lswlog_finite_state(buf, from);
					jam(buf, ":");
				}
			}
			const char *send;
			switch (t->send) {
			case NO_MESSAGE: send = ""; break;
			case MESSAGE_REQUEST: send = " send-request"; break;
			case MESSAGE_RESPONSE: send = " send-response"; break;
			default: bad_case(t->send);
			}
			DBG_log("    -> %s %s%s (%s)", to->short_name,
				enum_name_short(&timer_event_names,
						t->timeout_event),
				send, t->story);
		}

		/*
		 * Check that the NOTIFY -> PBS -> MD.pbs[]!=NULL will work.
		 */
		if (t->message_payloads.notification != v2N_NOTHING_WRONG) {
			pexpect(v2_pd_from_notification(t->message_payloads.notification) != PD_v2_INVALID);
		}
		if (t->encrypted_payloads.notification != v2N_NOTHING_WRONG) {
			pexpect(v2_pd_from_notification(t->encrypted_payloads.notification) != PD_v2_INVALID);
		}

		/*
		 * Check recv:MESSAGE_REQUEST->send:MESSAGE_RESPONSE.
		 */
		pexpect(t->recv_role == MESSAGE_REQUEST ? t->send = MESSAGE_RESPONSE : true);

		/*
		 * Check recv_type && recv_role
		 */
		pexpect(t->recv_role == NO_MESSAGE ? t->recv_type == 0 : t->recv_type != 0);

		/*
		 * Point .fs_v2_microcode at the first transition for
		 * the from state.  All other transitions for the from
		 * state should follow immediately after (or to put it
		 * another way, previous should match).
		 */
		if (from->v2_transitions == NULL) {
			/* start of the next state */
			passert(from->nr_transitions == 0);
			from->v2_transitions = t;
		} else {
			passert(t[-1].state == t->state);
		}
		from->nr_transitions++;
	}
}

/*
 * split an incoming message into payloads
 */
struct payload_summary ikev2_decode_payloads(struct logger *log,
					     struct msg_digest *md,
					     pb_stream *in_pbs,
					     enum next_payload_types_ikev2 np)
{
	struct payload_summary summary = {
		.parsed = true,
		.n = v2N_NOTHING_WRONG,
	};

	/*
	 * ??? zero out the digest descriptors -- might nuke
	 * ISAKMP_NEXT_v2SK digest!
	 *
	 * XXX: and v2SKF? Safer to leave them as is and just use new
	 * ones - always add to MD, never take away.
	 */

	/*
	 * XXX: Currently, when a message containing an SK payload is
	 * decoded, the encrypted payloads get appended to the
	 * previously decoded non-encrypted payloads.  For instance,
	 * given a message containing two notifications:
	 *
	 *     N(1), SK{ N(2) }
	 *
	 * The notification digest would contain both the unencrypted
	 * N(1) and encrypted N(2).  Since the unencrypted value is
	 * protected, while not very good, isn't really dangerous.
	 */

	while (np != ISAKMP_NEXT_v2NONE) {
		esb_buf b;
		dbg("Now let's proceed with payload (%s)",
		    enum_show(&ikev2_payload_names, np, &b));

		if (md->digest_roof >= elemsof(md->digest)) {
			llog(RC_LOG_SERIOUS, log,
				    "more than %zu payloads in message; ignored",
				    elemsof(md->digest));
			summary.n = v2N_INVALID_SYNTAX;
			break;
		}

		/*
		 * *pd is the payload digest for this payload.
		 * It has three fields:
		 *	pbs is filled in by in_struct
		 *	payload is filled in by in_struct
		 *	next is filled in by list linking logic
		 */
		struct payload_digest *const pd = md->digest + md->digest_roof;

		/*
		 * map the payload onto its payload descriptor which
		 * describes how to decode it
		 */
		const struct_desc *sd = v2_payload_desc(np);

		if (sd == NULL) {
			/*
			 * This payload is unknown to us.  RFCs 4306
			 * and 5996 2.5 say that if the payload has
			 * the Critical Bit, we should be upset but if
			 * it does not, we should just ignore it.
			 */
			diag_t d = pbs_in_struct(in_pbs, &ikev2_generic_desc,
						 &pd->payload, sizeof(pd->payload), &pd->pbs);
			if (d != NULL) {
				llog_diag(RC_LOG_SERIOUS, log, &d,
					 "malformed payload in packet");
				summary.n = v2N_INVALID_SYNTAX;
				break;
			}
			if (pd->payload.v2gen.isag_critical & ISAKMP_PAYLOAD_CRITICAL) {
				/*
				 * It was critical.  See RFC 5996 1.5
				 * "Version Numbers and Forward
				 * Compatibility"
				 */
				const char *role;
				switch (v2_msg_role(md)) {
				case MESSAGE_REQUEST:
					role = "request";
					break;
				case MESSAGE_RESPONSE:
					role = "response";
					break;
				default:
					bad_case(v2_msg_role(md));
				}
				esb_buf b;
				llog(RC_LOG_SERIOUS, log,
				     "message %s contained an unknown critical payload type (%s)",
				     role, enum_show(&ikev2_payload_names, np, &b));
				summary.n = v2N_UNSUPPORTED_CRITICAL_PAYLOAD;
				summary.data[0] = np;
				summary.data_size = 1;
				break;
			}
			esb_buf eb;
			llog(RC_COMMENT, log,
			     "non-critical payload ignored because it contains an unknown or unexpected payload type (%s) at the outermost level",
			     enum_show(&ikev2_payload_names, np, &eb));
			np = pd->payload.generic.isag_np;
			continue;
		}

		if (np >= LELEM_ROOF) {
			dbg("huge next-payload %u", np);
			summary.n = v2N_INVALID_SYNTAX;
			break;
		}
		summary.repeated |= (summary.present & LELEM(np));
		summary.present |= LELEM(np);

		/*
		 * Read in the payload recording what type it should
		 * be.
		 */
		pd->payload_type = np;
		diag_t d = pbs_in_struct(in_pbs, sd,
					 &pd->payload, sizeof(pd->payload),
					 &pd->pbs);
		if (d != NULL) {
			llog_diag(RC_LOG_SERIOUS, log, &d,
				 "malformed payload in packet");
			summary.n = v2N_INVALID_SYNTAX;
			break;
		}

		dbg("processing payload: %s (len=%zu)",
		    enum_show(&ikev2_payload_names, np, &b),
		    pbs_left(&pd->pbs));

		/*
		 * Place payload at the end of the chain for this
		 * type.
		 */
		if (md->last[np] == NULL) {
			/* first */
			md->chain[np] = md->last[np] = pd;
			pd->next = NULL;
		} else {
			/* append */
			md->last[np]->next = pd;
			md->last[np] = pd;
			pd->next = NULL;
		}

		/*
		 * Go deeper:
		 *
		 * XXX: should this do 'deeper' analysis of packets.
		 * For instance checking the SPI of a notification
		 * payload?  Probably not as the value may be ignored.
		 *
		 * The exception is seems to be v2N - both cookie and
		 * redirect code happen early and use the values.
		 */

		switch (np) {
		case ISAKMP_NEXT_v2N:
			decode_v2N_payload(log, md, pd);
			break;
		default:
			break;
		}

		/*
		 * Determine the next payload.
		 *
		 * SK and SKF are special - their next-payload field
		 * is for the first embedded payload - so force it to
		 * NONE:
		 *
		 * RFC 5996 2.14 "Encrypted Payload":
		 *
		 * Next Payload - The payload type of the first
		 * embedded payload.  Note that this is an exception
		 * in the standard header format, since the Encrypted
		 * payload is the last payload in the message and
		 * therefore the Next Payload field would normally be
		 * zero.  But because the content of this payload is
		 * embedded payloads and there was no natural place to
		 * put the type of the first one, that type is placed
		 * here.
		 */
		switch (np) {
		case ISAKMP_NEXT_v2SK:
		case ISAKMP_NEXT_v2SKF:
			/* special */
			np = ISAKMP_NEXT_v2NONE;
			break;
		default:
			np = pd->payload.generic.isag_np;
			break;
		}

		md->digest_roof++;
	}

	return summary;
}

static struct child_sa *process_v2_child_ix(struct ike_sa *ike,
					    const struct v2_state_transition *svm)
{
	/*
	 * XXX: Still a mess.  Should call processor with the IKE SA.
	 * The processor can then create a nested state.
	 */
	enum sa_type sa_type;
	switch (svm->state) {
	case STATE_V2_NEW_CHILD_R0:
	case STATE_V2_REKEY_CHILD_R0:
		sa_type = IPSEC_SA;
		break;
	default:
		pexpect(svm->state == STATE_V2_REKEY_IKE_R0);
		sa_type = IKE_SA;
	}

	struct child_sa *child = new_v2_child_state(ike->sa.st_connection,
						    ike, sa_type,
						    SA_RESPONDER,
						    svm->state,
						    null_fd);

	connection_buf ibuf;
	connection_buf cbuf;
	dbg(PRI_CONNECTION" #%lu received %s CREATE_CHILD_SA Child "PRI_CONNECTION" #%lu in %s will process it further",
	    pri_connection(ike->sa.st_connection, &ibuf),
	    ike->sa.st_serialno, svm->story,
	    pri_connection(child->sa.st_connection, &cbuf),
	    child->sa.st_serialno, child->sa.st_state->name);

	return child;
}

/*
 * Find the SA (IKE or CHILD), within IKE's family, that is initiated
 * or is responding to Message ID.
 *
 * XXX: There's overlap between this and the is_duplicate_*() code.
 * For instance, there's little point in looking for a state when the
 * IKE SA's window shows it too old (at least if we ignore
 * record'n'send bugs).
 */

static struct state *find_v2_sa_by_initiator_wip(struct ike_sa *ike, const msgid_t msgid)
{
	/*
	 * XXX: Would a linked list of CHILD SAs work better, would
	 * mean reference counting?  Should this also check that MSGID
	 * is within the IKE SA's window?
	 */
	struct state *st;
	if (ike->sa.st_v2_msgid_wip.initiator == msgid) {
		/* short-cut */
		st = &ike->sa;
	} else {
		struct state_query query = {
			.where = HERE,
			.ike_version = IKEv2,
			.ike_spis = &ike->sa.st_ike_spis,
		};
		for (st = next_state(NULL, &query); st != NULL; st = next_state(st, &query)) {
			if (st->st_v2_msgid_wip.initiator == msgid) {
				break;
			}
		}
	}
	pexpect(st == NULL ||
		st->st_clonedfrom == SOS_NOBODY ||
		st->st_clonedfrom == ike->sa.st_serialno);
	return st;
}

static struct state *find_v2_sa_by_responder_wip(struct ike_sa *ike, const msgid_t msgid)
{
	/*
	 * XXX: Would a linked list of CHILD SAs work better, would
	 * mean reference counting?  Should this also check that MSGID
	 * is within the IKE SA's window?
	 */
	struct state *st;
	if (ike->sa.st_v2_msgid_wip.responder == msgid) {
		/* short-cut */
		st = &ike->sa;
	} else {
		struct state_query query = {
			.where = HERE,
			.ike_version = IKEv2,
			.ike_spis = &ike->sa.st_ike_spis,
		};
		for (st = next_state(NULL, &query); st != NULL; st = next_state(st, &query)) {
			if (st->st_v2_msgid_wip.responder == msgid) {
				break;
			}
		}
	}
	pexpect(st == NULL ||
		st->st_clonedfrom == SOS_NOBODY ||
		st->st_clonedfrom == ike->sa.st_serialno);
	return st;
}

/*
 * Is this a duplicate message?
 *
 * XXX:
 *
 * record'n'send bypassing the send queue can result in pluto having
 * more outstanding messages then the negotiated window size.
 *
 * This and the find_v2_sa_by_*_wip() have some overlap.  For
 * instance, little point in searching for a state when the IKE SA's
 * window shows the Message ID is too old (only record'n'send breakage
 * means it might still have a message, argh!).
 */

/*
 * A duplicate request could be:
 *
 * - the request still being processed (for instance waiting on
 *   crypto), which can be tossed
 *
 * - the request last processed, which should trigger a retransmit of
 *   the response
 *
 * - an older request which can be tossed
 *
 * But if it is a fragment, much of this is skipped.
 */
static bool is_duplicate_request(struct ike_sa *ike,
				 struct msg_digest *md)
{
	passert(v2_msg_role(md) == MESSAGE_REQUEST);
	intmax_t msgid = md->hdr.isa_msgid;

	/* lie to keep test results happy */
	dbg("#%lu st.st_msgid_lastrecv %jd md.hdr.isa_msgid %08jx",
	    ike->sa.st_serialno, ike->sa.st_v2_msgid_windows.responder.recv, msgid);

	/* the sliding window is really small?!? */
	pexpect(ike->sa.st_v2_msgid_windows.responder.recv ==
		ike->sa.st_v2_msgid_windows.responder.sent);

	if (msgid < ike->sa.st_v2_msgid_windows.responder.sent) {
		/*
		 * this is an OLD retransmit and out sliding window
		 * holds only the most recent response. we can't do
		 * anything
		 */
		log_state(RC_LOG, &ike->sa,
			  "received too old retransmit: %jd < %jd",
			  msgid, ike->sa.st_v2_msgid_windows.responder.sent);
		return true;
	} else if (msgid == ike->sa.st_v2_msgid_windows.responder.sent) {
		/*
		 * This was the last request processed and,
		 * presumably, a response was sent.  Retransmit the
		 * saved response (the response was saved right?).
		 */
		if (ike->sa.st_v2_outgoing[MESSAGE_RESPONSE] == NULL) {
			FAIL_V2_MSGID(ike, &ike->sa,
				      "retransmission for message %jd exchange %s failed responder.sent %jd - there is no stored message or fragments to retransmit",
				      msgid, enum_name(&ikev2_exchange_names, md->hdr.isa_xchg),
				      ike->sa.st_v2_msgid_windows.responder.sent);
			return true;
		}
		/*
		 * If things are fragmented, only respond to the first
		 * fragment.
		 */
		unsigned fragment = 0;
		if (md->hdr.isa_np == ISAKMP_NEXT_v2SKF) {
			struct ikev2_skf skf;
			pb_stream in_pbs = md->message_pbs; /* copy */
			pb_stream ignored;
			diag_t d = pbs_in_struct(&in_pbs, &ikev2_skf_desc,
						 &skf, sizeof(skf), &ignored);
			if (d != NULL) {
				llog_diag(RC_LOG, ike->sa.st_logger, &d, "%s", "");
				return true;
			}
			fragment = skf.isaskf_number;
		}
		if (fragment == 0) {
			log_state(RC_LOG, &ike->sa,
				  "received duplicate %s message request (Message ID %jd); retransmitting response",
				  enum_name_short(&ikev2_exchange_names, md->hdr.isa_xchg),
				  msgid);
			send_recorded_v2_message(ike, "ikev2-responder-retransmit",
						 MESSAGE_RESPONSE);
		} else if (fragment == 1) {
			log_state(RC_LOG, &ike->sa,
				  "received duplicate %s message request (Message ID %jd, fragment %u); retransmitting response",
				  enum_name_short(&ikev2_exchange_names, md->hdr.isa_xchg),
				  msgid, fragment);
			send_recorded_v2_message(ike, "ikev2-responder-retransmt (fragment 1)",
						 MESSAGE_RESPONSE);
		} else {
			dbg_v2_msgid(ike, &ike->sa,
				     "received duplicate %s message request (Message ID %jd, fragment %u); discarded as not fragment 1",
				     enum_name_short(&ikev2_exchange_names, md->hdr.isa_xchg),
				     msgid, fragment);
		}
		return true;
	} else {
		/* all that is left */
		pexpect(msgid > ike->sa.st_v2_msgid_windows.responder.sent);
	}

	/*
	 * Is something already processing this request?
	 *
	 * Processing only starts for real once a responder has
	 * accumulated all fragments and obtained KEYMAT.
	 */
	{
		struct state *responder = find_v2_sa_by_responder_wip(ike, md->hdr.isa_msgid);
		/* only a true responder */
		pexpect(responder == NULL ||
			responder->st_v2_msgid_wip.responder == msgid);
		if (responder != NULL) {
			/* this generates the log message */
			pexpect(verbose_state_busy(responder));
			return true;
		}
	}

	/*
	 * The IKE SA responder, having accumulated all the fragments
	 * for the IKE_AUTH request, is computing the SKEYSEED.  When
	 * SKEYSEED finishes .st_v2_rfrags is wiped and the Message
	 * IDs updated to flag that the message as work-in-progress
	 * (so above check will have succeeded).
	 */
	if (state_is_busy(&ike->sa)) {
		/*
		 * To keep tests happy, try to output text matching
		 * verbose_state_busy(); but with some extra detail.
		 */
#if 0
		/*
		 * XXX: hang onto this code for now - it shows how to
		 * lightly unpack fragments.  Will be useful when
		 * fragmentation code is moved out of the state lookup
		 * code.
		 */
		unsigned fragment = 0;
		if (md->hdr.isa_np == ISAKMP_NEXT_v2SKF) {
			struct ikev2_skf skf;
			pb_stream in_pbs = md->message_pbs; /* copy */
			pb_stream ignored;
			diag_t d = pbs_in_struct(&skf, &ikev2_skf_desc, &in_pbs, &ignored);
			if (d != NULL) {
				llog_diag(RC_LOG, st->st_logger, &d, "%s", "");
				return true;
			}
			fragment = skf.isaskf_number;
		}
		if (fragment == 0) {
			log_state(RC_LOG, &ike->sa,
				  "discarding packet received during asynchronous work (DNS or crypto) in %s",
				  ike->sa.st_state->name);
		} else if (fragment == 1) {
			log_state(RC_LOG, &ike->sa,
				  "discarding fragments received during asynchronous work (DNS or crypto) in %s",
				  ike->sa.st_state->name);
		} else {
			dbg_v2_msgid(ike, &ike->sa,
				     "discarding fragment %u received during asynchronous work (DNS or crypto) in %s",
				     fragment, ike->sa.st_state->name);
		}
#else
		log_state(RC_LOG, &ike->sa,
			  "discarding packet received during asynchronous work (DNS or crypto) in %s",
			  ike->sa.st_state->name);
#endif
		return true;
	}

	struct v2_incoming_fragments *frags = ike->sa.st_v2_incoming[MESSAGE_REQUEST];
	if (frags != NULL) {
		pexpect(frags->count < frags->total);
		dbg_v2_msgid(ike, &ike->sa,
			     "not a duplicate - responder is accumulating fragments for message request %jd",
			     msgid);
	} else {
		dbg_v2_msgid(ike, &ike->sa,
			     "not a duplicate - message request %jd is new",
			     msgid);
	}

	return false;
}

/*
 * A duplicate response could be:
 *
 * - for an old request where there's no longer an initiator waiting,
 *   and can be dropped
 *
 * - the initiator is busy, presumably because this response is a
 *   duplicate and the initiator is waiting on crypto to complete so
 *   it can decrypt the response
 */
static bool is_duplicate_response(struct ike_sa *ike,
				  struct state *initiator,
				  struct msg_digest *md)
{
	passert(v2_msg_role(md) == MESSAGE_RESPONSE);
	intmax_t msgid = md->hdr.isa_msgid;

	/* only a true initiator */
	pexpect(initiator == NULL ||
		initiator->st_v2_msgid_wip.initiator == msgid);

	/* the sliding window is really small?!? */
	pexpect(ike->sa.st_v2_msgid_windows.responder.recv ==
		ike->sa.st_v2_msgid_windows.responder.sent);

	if (msgid <= ike->sa.st_v2_msgid_windows.initiator.recv) {
		/*
		 * Processing of the response was completed so drop as
		 * too old.
		 *
		 * XXX: Should be rate_log() but that shows up in the
		 * whack output.  While "correct" it messes with test
		 * output.  The old log line didn't show up because
		 * current-state wasn't set.
		 *
		 * Here's roughly why INITIATOR can be non-NULL:
		 *
		 * - west.#8 needs a rekey, so west.#11 is created and
		 * it sends a CREATE_CHILD_SA with Message ID 3.
		 *
		 * - west.#8 gives up on the re-key so it forces a
		 * delete request (aka record'n'send), sending a
		 * second message with ID 4
		 *
		 * West has two outstanding messages yet its window
		 * size of 1!
		 *
		 * - east receives the rekey with ID 3, creates
		 * east.#11 and and sends it off for further
		 * processing
		 *
		 * - east receives the delete with ID 4, forces a
		 * message ID update and sends an ID 4 response
		 * confirming the delete
		 *
		 * - east.#11 finishes its crypto so east sends back
		 * its response with Message ID 3 for a re-keyed SA it
		 * just deleted?!?!
		 *
		 * East has responded with two out-of-order messages
		 * (if the window size was 2 this would be ok but it
		 * isn't).
		 *
		 * - west receives the ID 4 response, tries to delete
		 * the IKE SA but can't because west.#11 is lurking;
		 * but regardless the ID window is forced 2->4
		 *
		 * - west receives the ID 3 response, which is clearly
		 * to-old so doesn't expect there to be a matching
		 * initiator, arrg
		 */
		if (initiator != NULL) {
			dbg_v2_msgid(ike, initiator, "XXX: expecting initiator==NULL - suspect record'n'send with an out-of-order wrong packet response; discarding packet");
		} else {
			dbg_v2_msgid(ike, initiator, "already processed response %jd (%s); discarding packet",
				     msgid, enum_name_short(&ikev2_exchange_names, md->hdr.isa_xchg));
		}
		return true;
	}

	if (initiator == NULL) {
		/*
		 * While there's an IKE SA matching the IKE SPIs,
		 * there's no corresponding initiator for the message.
		 *
		 * XXX: rate_log() sends to whack which, while making
		 * sense, but churns the test output.
		 */
		log_state(RC_LOG, &ike->sa,
			  "%s message response with Message ID %jd has no matching SA",
			  enum_name(&ikev2_exchange_names, md->hdr.isa_xchg), msgid);
		return true;
	}

	/*
	 * Sanity check the MSGID and initiator against the IKE SA
	 * Message ID window.
	 */

	if (msgid > ike->sa.st_v2_msgid_windows.initiator.sent) {
		/*
		 * There was an initiator waiting for a message that,
		 * according to the IKE SA, has yet to be sent?!?
		 */
		FAIL_V2_MSGID(ike, initiator,
			      "dropping response with Message ID %jd which is from the future - last request sent was %jd",
			      msgid, ike->sa.st_v2_msgid_windows.initiator.sent);
		return true;
	}

	/*
	 * If the state is busy, presumably doing something like
	 * crypto, skip further processing.
	 *
	 * For fragments, things only go busy once all fragments have
	 * been received (and re-transmitted fragments are ignored).
	 * If this changes then a lot more than this code will need to
	 * be moved.
	 *
	 * XXX: Is there a better way to handle this?
	 */
	if (verbose_state_busy(initiator)) {
		return true;
	}

	return false;
}

/*
 * process an input packet, possibly generating a reply.
 *
 * If all goes well, this routine eventually calls a state-specific
 * transition function.
 *
 * This routine will not release_any_md(mdp).  It is expected that its
 * caller will do this.  In fact, it will zap *mdp to NULL if it thinks
 * **mdp should not be freed.  So the caller should be prepared for
 * *mdp being set to NULL.
 *
 * Start by looking for (or creating) the IKE SA responsible for the
 * IKE SPIs group .....
 */

static void ike_process_packet(struct msg_digest *mdp, struct ike_sa *ike);

void ikev2_process_packet(struct msg_digest *md)
{
	/* Look for an state that matches the various things we know:
	 *
	 * 1) exchange type received?
	 * 2) is it initiator or not?
	 */
	const enum isakmp_xchg_type ix = md->hdr.isa_xchg;

	/*
	 * If the IKE SA initiator sent the message then this end is
	 * looking for the IKE SA responder (and vice versa).
	 */
	enum sa_role expected_local_ike_role = (md->hdr.isa_flags & ISAKMP_FLAGS_v2_IKE_I) ? SA_RESPONDER : SA_INITIATOR;

	/*
	 * Dump what the message says, once a state has been found
	 * this can be checked against what is.
	 */
	LSWDBGP(DBG_BASE, buf) {
		switch (expected_local_ike_role) {
		case SA_RESPONDER:
			jam(buf, "I am the IKE SA Original Responder");
			break;
		case SA_INITIATOR:
			jam(buf, "I am the IKE SA Original Initiator");
			break;
		default:
			bad_case(expected_local_ike_role);
		}
		jam(buf, " receiving an IKEv2 ");
		jam_enum_short(buf, &ikev2_exchange_names, ix);
		switch (v2_msg_role(md)) {
		case MESSAGE_RESPONSE:
			jam(buf, " response ");
			break;
		case MESSAGE_REQUEST:
			jam(buf, " request ");
			break;
		default:
			bad_case(v2_msg_role(md));
		}
	}

	/*
	 * If the message is a new IKE_SA_INIT request (or previously
	 * discarded request due to cookies) then there is no IKE SA,
	 * and no point trying to use the state machinery.
	 *
	 * If the message is an IKE_SA_INIT response, then there might
	 * be an IKE SA; but the number of checks required means that
	 * it is also a good idea to just avoid the state machinery.
	 */

	if (ix == ISAKMP_v2_IKE_SA_INIT) {
		process_v2_IKE_SA_INIT(md);
		return;
	}

	passert(v2_msg_role(md) == MESSAGE_REQUEST ||
		v2_msg_role(md) == MESSAGE_RESPONSE);

	/*
	 * Find the IKE SA with matching SPIs.
	 *
	 * The IKE SA's Message IDs can then be used to determine if
	 * the message fits in the message window (new request,
	 * expected response, or old message).
	 */
	struct ike_sa *ike = find_v2_ike_sa(&md->hdr.isa_ike_spis,
					    expected_local_ike_role);
	if (ike == NULL) {
		esb_buf ixb;
		rate_log(md, "%s message %s has no corresponding IKE SA",
			 enum_show_short(&ikev2_exchange_names, ix, &ixb),
			 v2_msg_role(md) == MESSAGE_REQUEST ? "request" : "response");
		return;
	}

	/*
	 * There's at least an IKE SA, and possibly ST willing to
	 * process the message.
	 */
	passert(ike != NULL);

	/*
	 * Re-check ST's IKE SA's role against the I(Initiator) flag
	 * in the headers.  Since above searches will only find an IKE
	 * SA when the IKE SA's role is correct, this should always
	 * work.
	 */
	if (!pexpect(ike->sa.st_sa_role == expected_local_ike_role)) {
		return;
	}

	/*
	 * Since there's an IKE SA start billing and logging against
	 * it.
	 */
	statetime_t start = statetime_backdate(&ike->sa, &md->md_inception);
	ike_process_packet(md, ike);
	statetime_stop(&start, "%s()", __func__);
}

/*
 * The IKE SA for the message has been found (or created).  Continue
 * verification, and identify the state (ST) that the message should
 * be sent to.
 */

static void ike_process_packet(struct msg_digest *md, struct ike_sa *ike)
{
	/*
	 * Deal with duplicate messages and busy states.
	 */
	struct state *st;
	switch (v2_msg_role(md)) {
	case MESSAGE_REQUEST:
		/*
		 * The IKE SA always processes requests.
		 *
		 * XXX: except further down where the code creates a
		 * new state when CREATE_CHILD_SA and switches to
		 * that.
		 *
		 * The other quirk is with fragments; but the only
		 * case that matters it when the IKE SA accumulating
		 * them.
		 */
		if (md->fake_clone) {
			log_state(RC_LOG, &ike->sa, "IMPAIR: processing a fake (cloned) message");
		}
		/*
		 * Is this duplicate?
		 *
		 * If MD is a fragment then it isn't considered a
		 * duplicate.
		 */
		if (is_duplicate_request(ike, md)) {
			return;
		}
		st = &ike->sa;
		break;
	case MESSAGE_RESPONSE:
		/*
		 * This is the response to an earlier request; use the
		 * IKE SA to find the state that initiated the
		 * exchange (sent that request).
		 *
		 * If the response is a fragment then ST will be
		 * non-NULL; is_duplicate_state() gets to figure out
		 * if the fragments are complete or need to wait
		 * longer.
		 */
		st = find_v2_sa_by_initiator_wip(ike, md->hdr.isa_msgid);
		if (md->fake_clone) {
			log_state(RC_LOG, &ike->sa, "IMPAIR: processing a fake (cloned) message");
		}
		if (is_duplicate_response(ike, st, md)) {
			return;
		}
		pexpect(st != NULL);
		break;
	default:
		bad_case(v2_msg_role(md));
	}

	/*
	 * Now that the state that is to process the message has been
	 * selected, switch logging to it.
	 *
	 * XXX: why the need to constantly pick a single winner and
	 * switch to it?  Because tests expect messages to be logged
	 * against a specific state.  It would be better of that code
	 * specified that state as a parameter.
	 */
	passert(st != NULL);
	/* XXX: debug-logging this is redundant */

	/*
	 * Have a state an and IKE SA, time to decode the payloads.
	 */
	dbg("unpacking clear payload");
	passert(!md->message_payloads.parsed);
	pexpect(v2_msg_role(md) == MESSAGE_RESPONSE ||
		md->hdr.isa_xchg != ISAKMP_v2_IKE_SA_INIT);
	md->message_payloads =
		ikev2_decode_payloads(ike->sa.st_logger, md,
				      &md->message_pbs,
				      md->hdr.isa_np);
	if (md->message_payloads.n != v2N_NOTHING_WRONG) {
		/*
		 * Should only respond when the message is an
		 * IKE_SA_INIT request.  But that was handled above
		 * when dealing with cookies so here, there's zero
		 * reason to respond.
		 *
		 * decode calls packet code and that logs errors on
		 * the spot
		 */
		/* already logged */
		return;
	}

	ikev2_process_state_packet(ike, st, md);
}

/*
 * XXX: Hack to find the transition that would have been run if the
 * packet was ok, so it can be 'failed'.
 *
 * This is largely astetic.  It could use the first transition but
 * often a later transition reads better.  Perhaps the last transition
 * since, presumably, that is the most generic?
 */

static void hack_error_transition(struct state *st, struct msg_digest *md)
{
	passert(md != NULL);
	const struct v2_state_transition *transition;
	const struct finite_state *state = st->st_state;
	switch (state->kind) {
	case STATE_PARENT_R1:
		/*
		 * Responding to either an IKE_INTERMEDIATE or
		 * IKE_AUTH request: look for the NOSKEYSEED
		 * transitions (and prefer IKE_AUTH).
		 *
		 * Once SKEYSEED is off-loaded and STATE_PARENT_I1 has
		 * only one (ok, two) transition, this is no longer a
		 * hack.
		 */
		pexpect(state->nr_transitions == 4);
		if (md->hdr.isa_xchg == ISAKMP_v2_IKE_INTERMEDIATE) {
			transition = &state->v2_transitions[2];
			pexpect(transition->recv_type == ISAKMP_v2_IKE_INTERMEDIATE);
			pexpect(transition->next_state == STATE_PARENT_R1);
		} else {
			transition = &state->v2_transitions[3];
			pexpect(transition->recv_type == ISAKMP_v2_IKE_AUTH);
			pexpect(transition->next_state == STATE_V2_ESTABLISHED_IKE_SA);
		}
		pexpect((transition->flags & SMF2_NO_SKEYSEED) == 0);
		pexpect(transition->state == STATE_PARENT_R1);
		break;
	case STATE_PARENT_I2:
	{
		/*
		 * Receiving IKE_AUTH response: it is buried deep
		 * down; would adding an extra transition that always
		 * matches be better?
		 */
		unsigned transition_nr = 1;
		pexpect(state->nr_transitions > transition_nr);
		transition = &state->v2_transitions[transition_nr];
		pexpect(transition->state == STATE_PARENT_I2);
		pexpect(transition->next_state == STATE_V2_ESTABLISHED_IKE_SA);
		break;
	}
	default:
		if (/*pexpect*/(state->nr_transitions > 0)) {
			transition = &state->v2_transitions[state->nr_transitions - 1];
		} else {
			static const struct v2_state_transition undefined_transition = {
				.story = "suspect message",
				.state = STATE_UNDEFINED,
				.next_state = STATE_UNDEFINED,
			};
			transition = &undefined_transition;
		}
		break;
	}
	/*pexpect(st->st_v2_transition == NULL);*/
	st->st_v2_transition = transition;
}

/*
 * The SA the message is intended for has also been identified.
 * Continue ...
 *
 * XXX: Well except for a CREATE_CHILD_SA request where, after further
 * processing the SA may get created.  Should this message instead be
 * sent to the IKE SA, which can then create a WIP child?
 */

void ikev2_process_state_packet(struct ike_sa *ike, struct state *st,
				struct msg_digest *md)
{
	passert(ike != NULL);
	passert(st != NULL);

	/*
	 * There is no "struct state" object if-and-only-if we're
	 * responding to a shiny new SA_INIT message.  The start-state
	 * transition will (probably) create the object.
	 *
	 * But what about when pluto, as the initial responder, is
	 * fending of an attack attack by sending back and requiring
	 * cookies - won't the cookie need a "struct state"?
	 * According to the RFC: no.  Instead a small table of
	 * constants can be used to generate cookies on the fly.
	 */
	const struct finite_state *from_state = st->st_state;
	dbg("#%lu in state %s: %s", st->st_serialno,
	    from_state->short_name, from_state->story);

	struct ikev2_payload_errors message_payload_status = { .bad = false };
	struct ikev2_payload_errors encrypted_payload_status = { .bad = false };

	const enum isakmp_xchg_type ix = md->hdr.isa_xchg;

	/*
	 * XXX: Unlike find_v2_state_transition(), the below scans
	 * every single state transition and then, in the case of a
	 * CREATE_CHILD_SA, ignores the "from" state.
	 *
	 * XXX: Unlike find_v2_state_transition(), this code detects
	 * and decrypts packets and fragments in the middle of the
	 * lookup.  Being more aggressive with decrypting fragments
	 * will likely force that logic to be moved to before this
	 * lookup.
	 */

	const struct v2_state_transition *svm;
	for (svm = v2_state_transition_table; svm->state != STATE_IKEv2_ROOF;
	     svm++) {

		/*
		 * Does the message match the state transition?
		 */
		if (svm->recv_type != ix) {
			continue;
		}
		if (svm->recv_role != v2_msg_role(md)) {
			continue;
		}

		/*
		 * Does the from state match the SA's current state?
		 *
		 * XXX: Eventually this part won't be needed as each
		 * state will have its own list of transitions.  See
		 * find_v2_state_transition() which is currently only
		 * used by IKES_SA_INIT.
		 *
		 * Unfortunately ...
		 *
		 * For CREATE_CHILD_SA, the responder ignores the
		 * .from_state and instead applies some other magic
		 * (the .from_state ends up being used when creating
		 * the Child SA).
		 */
		if (ix == ISAKMP_v2_CREATE_CHILD_SA &&
		    /* matched above */ svm->recv_role == MESSAGE_REQUEST) {
			dbg("potential CREATE_CHILD_SA responder stumbling through to process_v2_child_ix()");
		} else if (svm->state != from_state->kind) {
			continue;
		}

		/*
		 * Check the message payloads are as expected.
		 */
		pexpect(md->message_payloads.parsed);
		struct ikev2_payload_errors message_payload_errors
			= ikev2_verify_payloads(md, &md->message_payloads,
						&svm->message_payloads);
		if (message_payload_errors.bad) {
			/* Save this failure for later logging. */
			message_payload_status = message_payload_errors;
			continue;
		}

		/*
		 * If there is no SK (or SKF) payload then checking is
		 * complete and things have matched.
		 *
		 * (.seen&(P(SK)|P(SKF))!=0 is equivalent.
		 */
		if (!(svm->message_payloads.required & P(SK))) {
			break;
		}

		/*
		 * Since the encrypted payload appears plausible, deal
		 * with fragmentation.
		 */
		if (!md->encrypted_payloads.parsed) {
			/*
			 * Deal with fragmentation.  The function
			 * returns FALSE either when there are more
			 * fragments, the fragment is corrupt, the
			 * fragment is a duplicate, or the fragment
			 * count changed (it also drops all
			 * fragments).  Either way stop processing.
			 *
			 * Only upon _first_ arrival of the last
			 * fragment, does the function return TRUE.
			 * The the processing flow below can then
			 * continue to the SKEYSEED check.
			 *
			 * However, if SKEYSEED (g^{xy}) needed to be
			 * computed then this code will be re-entered
			 * with all fragments present (so "the"
			 * function should not be called).
			 */
			struct v2_incoming_fragments *frags =
				ike->sa.st_v2_incoming[v2_msg_role(md)];
			bool have_all_fragments =
				(frags != NULL && frags->count == frags->total);
			/*
			 * XXX: Because fragments are only checked
			 * all-at-once after they have all arrived, a
			 * single corrupt fragment will cause all
			 * fragments being thrown away, and the entire
			 * process re-start (Is this tested?)
			 *
			 * XXX: This code should instead check
			 * fragments as they arrive.  That means
			 * kicking off the g^{xy} calculation in the
			 * background (if it were in the foreground,
			 * the fragments would be dropped).  Later.
			 */
			if (md->message_payloads.present & P(SKF)) {
				if (have_all_fragments) {
					dbg("already have all fragments, skipping fragment collection");
				} else if (!ikev2_collect_fragment(md, ike)) {
					return;
				}
			}
			/*
			 * For this state transition, does it only
			 * apply when there's no SKEYSEED?  If so, and
			 * SKEYSEED is missing, then things match; else
			 * things can't match.
			 */
			if (svm->flags & SMF2_NO_SKEYSEED) {
				if (ike->sa.hidden_variables.st_skeyid_calculated) {
					continue;
				} else {
					break;
				}
			}
			/*
			 * XXX: Shouldn't reach this point without
			 * SKEYSEED so bail if somehow that hasn't
			 * happened.  No point in even calling
			 * ikev2_decrypt_msg() (it will also fail).
			 *
			 * Suspect it would be cleaner if the state
			 * machine included an explicit SMF2_SKEYSEED
			 * flag and all states requiring integrity
			 * were marked with that. Currently P(SK) and
			 * P(SKF) imply this.
			 */
			if (!pexpect(ike->sa.hidden_variables.st_skeyid_calculated)) {
				return;
			}
			/*
			 * Decrypt the packet, checking it for
			 * integrity.  Anything lacking integrity is
			 * dropped.
			 */
			if (!ikev2_decrypt_msg(ike, md)) {
				log_state(RC_LOG, &ike->sa,
					  "encrypted payload seems to be corrupt; dropping packet");
				return;
			}
			/*
			 * The message is protected - the integrity
			 * check passed - so it was definitely sent by
			 * the other end of the secured IKE SA.
			 *
			 * However, for an AUTH packet, the other end
			 * hasn't yet been authenticated (and an
			 * INFORMATIONAL exchange immediately
			 * following AUTH be due to failed
			 * authentication).
			 *
			 * If there's something wrong with the message
			 * contents, then the IKE SA gets abandoned,
			 * but a new new one may be initiated.
			 *
			 * See "2.21.2.  Error Handling in IKE_AUTH"
			 * and "2.21.3.  Error Handling after IKE SA
			 * is Authenticated".
			 *
			 * For UNSUPPORTED_CRITICAL_PAYLOAD, while the
			 * RFC clearly states that for the initial
			 * exchanges and an INFORMATIONAL exchange
			 * immediately following, the notification
			 * causes a delete, it says nothing for
			 * exchanges that follow.
			 *
			 * For moment treat it the same.  Given the
			 * PAYLOAD ID that should identify the problem
			 * isn't being returned this is the least of
			 * our problems.
			 */
			struct payload_digest *sk = md->chain[ISAKMP_NEXT_v2SK];
			md->encrypted_payloads = ikev2_decode_payloads(ike->sa.st_logger, md, &sk->pbs,
								       sk->payload.generic.isag_np);
			if (md->encrypted_payloads.n != v2N_NOTHING_WRONG) {
				/*
				 * XXX: Hack to get the
				 * transition that would have
				 * been run so it can be
				 * 'failed'.
				 */
				hack_error_transition(st, md);
				switch (v2_msg_role(md)) {
				case MESSAGE_REQUEST:
					/*
					 * Send back a protected error
					 * response.  Need to first
					 * put the IKE SA into
					 * responder mode.
					 */
					v2_msgid_start_responder(ike, st, md);
					chunk_t data = chunk2(md->encrypted_payloads.data,
							      md->encrypted_payloads.data_size);
					record_v2N_response(st->st_logger, ike, md,
							    md->encrypted_payloads.n, &data,
							    ENCRYPTED_PAYLOAD);
					break;
				case MESSAGE_RESPONSE:
					/*
					 * Can't respond so kill the
					 * IKE SA.  The secured
					 * message contained crap so
					 * there's little that can be
					 * done.
					 */
					break;
				default:
					bad_case(v2_msg_role(md));
				}
				complete_v2_state_transition(st, md, STF_FATAL);
				return;
			}
		} /* else { go ahead } */
		struct ikev2_payload_errors encrypted_payload_errors
			= ikev2_verify_payloads(md, &md->encrypted_payloads,
						&svm->encrypted_payloads);
		if (encrypted_payload_errors.bad) {
			/* Save this failure for later logging. */
			encrypted_payload_status = encrypted_payload_errors;
			continue;
		}

		if (svm->state != from_state->kind && ix == ISAKMP_v2_CREATE_CHILD_SA) {
			/*
			 * The IKE SA is receiving a CREATE_CHILD_SA
			 * request.  Unlike STATE_PARENT_R0 (and the
			 * initial responder) the R0 state isn't
			 * obvious - rekey IKE SA, rekey CHILD SA, and
			 * create CHILD SA are all slightly different.
			 *
			 * The code deals with this by ignoring the
			 * from_state, and then later, forcing MD's
			 * from state to values in the table.
			 */
			dbg("state #%lu forced to match CREATE_CHILD_SA from %s->%s by ignoring from state",
			    st->st_serialno,
			    finite_states[svm->state]->name,
			    finite_states[svm->next_state]->name);
		}

		/* must be the right state machine entry */
		break;
	}

	dbg("selected state microcode %s", svm->story);

	/* no useful state microcode entry? */
	if (svm->state == STATE_IKEv2_ROOF) {
		/* count all the error notifications */
		for (struct payload_digest *ntfy = md->chain[ISAKMP_NEXT_v2N];
		     ntfy != NULL; ntfy = ntfy->next) {
			pstat(ikev2_recv_notifies_e, ntfy->payload.v2n.isan_type);
		}
		/*
		 * All branches: log error, [complete transition]
		 * (why), return so first error wins.
		 */
		if (message_payload_status.bad) {
			/*
			 * A very messed up message - none of the
			 * state transitions recognized it!.
			 */
			log_v2_payload_errors(st->st_logger, md,
					      &message_payload_status);
			return;
		}
		if (encrypted_payload_status.bad) {
			/*
			 * Payload decrypted and integrity was ok but
			 * contents weren't valid.
			 *
			 * XXX: According to "2.21.2.  Error Handling
			 * in IKE_AUTH" and "2.21.3.  Error Handling
			 * after IKE SA is Authenticated" this should
			 * be fatal, killing the IKE SA.  Oops.
			 *
			 * XXX: how can one complete a state
			 * transition on something that was never
			 * started?  Since this is fatal, the state
			 * needs to be deleted.
			 *
			 * XXX: an alternative would be to treat this
			 * like some new but as-of-yet not supported
			 * message combination so just ignore it (but
			 * update Message IDs).
			 */
			log_v2_payload_errors(st->st_logger, md,
					      &encrypted_payload_status);
			/*
			 * XXX: Hack to get the transition
			 * that would have been run so it can
			 * be 'failed'.
			 */
			hack_error_transition(st, md);
			switch (v2_msg_role(md)) {
			case MESSAGE_REQUEST:
				/*
				 * Send back a protected error
				 * response.  Need to first put the
				 * IKE SA into responder mode.
				 */
				v2_msgid_start_responder(ike, st, md);
				record_v2N_response(st->st_logger, ike, md,
						    v2N_INVALID_SYNTAX, NULL,
						    ENCRYPTED_PAYLOAD);
				break;
			case MESSAGE_RESPONSE:
				/*
				 * Can't respond so kill the IKE SA -
				 * the secured message contained crap
				 * so there's little that can be done.
				 */
				break;
			default:
				bad_case(v2_msg_role(md));
			}
			/* XXX: calls delete_state() */
			complete_v2_state_transition(st, md, STF_FATAL);
			return;
		}
		/*
		 * Presumably things are pretty messed up.  While
		 * there might be a state there probably isn't an
		 * established IKE SA (so don't even consider trying
		 * to send an encrypted response), for instance:
		 *
		 * - instead of an IKE_AUTH request, the initiator
		 * sends something totally unexpected (such as an
		 * informational) and things end up here
		 *
		 * - when an IKE_AUTH request's IKE SA succeeeds but
		 * CHILD SA fails (and pluto screws up the IKE SA by
		 * updating its state but not its Message ID and not
		 * responding), the re-transmitted IKE_AUTH ends up
		 * here
		 *
		 * If a request, should it send an un-encrypted
		 * v2N_INVALID_SYNTAX?
		 */
		log_state(RC_LOG, &ike->sa, "no useful state microcode entry found for incoming packet");
		/* "dropping message with no matching microcode" */
		return;
	}

	if (ix == ISAKMP_v2_CREATE_CHILD_SA) {
		/*
		 * XXX: This code was embedded in the end of the FSM
		 * search loop.  Since it was always executed when the
		 * state matches, move it out of the loop.  Suspect
		 * this, and the code below, really belong in the
		 * state transition function proper.
		 */
		/* going to switch to child st. before that update parent */
		if (!LHAS(ike->sa.hidden_variables.st_nat_traversal, NATED_HOST))
			update_ike_endpoints(ike, md);

		/* bit further processing of create CREATE_CHILD_SA exchange */

		/*
		 * let's get a child state either new or existing to
		 * proceed
		 */
		struct child_sa *child;
		if (v2_msg_role(md) == MESSAGE_RESPONSE) {
			child = pexpect_child_sa(st);
		} else {
			pexpect(IS_IKE_SA(st));
			child = process_v2_child_ix(ike, svm);
		}

		/*
		 * Switch to child state (possibly from the same child
		 * state, see above)
		 */
		dbg("forcing ST #%lu to CHILD #%lu.#%lu in FSM processor",
		    st->st_serialno, ike->sa.st_serialno, child->sa.st_serialno);
		st = &child->sa;
	}

	v2_dispatch(ike, st, md, svm);
}

void v2_dispatch(struct ike_sa *ike, struct state *st,
		 struct msg_digest *md,
		 const struct v2_state_transition *svm)
{
	md->svm = svm;

	/*
	 * For the responder, update the work-in-progress Message ID
	 * window (since work has commenced).
	 *
	 * Exclude the SKEYSEED calculation - the message has yet to
	 * be decrypted so true work on the message is yet to comence.
	 */
	if (v2_msg_role(md) == MESSAGE_REQUEST &&
	    !(svm->flags & SMF2_NO_SKEYSEED)) {
		v2_msgid_start_responder(ike, st, md);
	}

	if (DBGP(DBG_BASE)) {
		if (pbs_left(&md->message_pbs) != 0)
			DBG_log("removing %d bytes of padding",
				(int) pbs_left(&md->message_pbs));
	}

	md->message_pbs.roof = md->message_pbs.cur;	/* trim padding (not actually legit) */

	dbg("calling processor %s", svm->story);

	/*
	 * XXX: for now pass in the possibly NULL child; suspect a
	 * better model is to drop the child and instead have the IKE
	 * SA run a nested state machine for the child.
	 *
	 * For instance, when a CREATE_CHILD_SA request arrives, pass
	 * that to the IKE SA and then let it do all the create child
	 * magic.
	 */
	so_serial_t old_st = st->st_serialno;

	statetime_t start = statetime_start(st);
	struct child_sa *child = IS_CHILD_SA(st) ? pexpect_child_sa(st) : NULL;
	/* danger: st may not be valid */
	stf_status e = svm->processor(ike, child, md);

	if (e == STF_SKIP_COMPLETE_STATE_TRANSITION) {
		/*
		 * Danger! Processor did something dodgy like free ST!
		 */
		dbg("processor '%s' for #%lu suppresed complete st_v2_transition",
		    svm->story, old_st);
	} else {
		complete_v2_state_transition(st, md, e);
	}

	statetime_stop(&start, "processing: %s in %s()", svm->story, __func__);
	/* our caller with release_any_md(mdp) */
}

/*
 * This logs to the main log the authentication and encryption keys
 * for an IKEv2 SA.  This is done in a format that is compatible with
 * tcpdump 4.0's -E option.
 *
 * The log message will require that a cut command is used to remove
 * the initial text.
 *
 * DANGER: this intentionally leaks cryptographic secrets.
 */
void ikev2_log_parentSA(const struct state *st)
{
	if (DBGP(DBG_PRIVATE)) {
		if (st->st_oakley.ta_integ == NULL ||
		    st->st_oakley.ta_encrypt == NULL)
			return;

		/* format initiator SPI */
		char tispi[3 + 2*IKE_SA_SPI_SIZE];
		(void)datatot(st->st_ike_spis.initiator.bytes, sizeof(st->st_ike_spis.initiator.bytes),
			'x',
			tispi, sizeof(tispi));

		/* format responder SPI */
		char trspi[3 + 2*IKE_SA_SPI_SIZE];
		(void)datatot(st->st_ike_spis.responder.bytes, sizeof(st->st_ike_spis.responder.bytes),
			'x',
			trspi, sizeof(trspi));

		const char *authalgo = st->st_oakley.ta_integ->integ_tcpdump_name;
		const char *encalgo = st->st_oakley.ta_encrypt->encrypt_tcpdump_name;

		/*
		 * Text of encryption key length (suffix for encalgo).
		 * No more than 3 digits, but compiler fears it might be 5.
		 */
		char tekl[6] = "";
		if (st->st_oakley.enckeylen != 0)
			snprintf(tekl, sizeof(tekl), "%u",
				 st->st_oakley.enckeylen);

		/* v2 IKE authentication key for initiator (256 bit bound) */
		chunk_t ai = chunk_from_symkey("ai", st->st_skey_ai_nss,
					       st->st_logger);
		char tai[3 + 2 * BYTES_FOR_BITS(256)] = "";
		(void)datatot(ai.ptr, ai.len, 'x', tai, sizeof(tai));
		free_chunk_content(&ai);

		/* v2 IKE encryption key for initiator (256 bit bound) */
		chunk_t ei = chunk_from_symkey("ei", st->st_skey_ei_nss,
					       st->st_logger);
		char tei[3 + 2 * BYTES_FOR_BITS(256)] = "";
		(void)datatot(ei.ptr, ei.len, 'x', tei, sizeof(tei));
		free_chunk_content(&ei);

		DBG_log("ikev2 I %s %s %s:%s %s%s:%s",
			tispi, trspi,
			authalgo, tai,
			encalgo, tekl, tei);

		/* v2 IKE authentication key for responder (256 bit bound) */
		chunk_t ar = chunk_from_symkey("ar", st->st_skey_ar_nss,
					       st->st_logger);
		char tar[3 + 2 * BYTES_FOR_BITS(256)] = "";
		(void)datatot(ar.ptr, ar.len, 'x', tar, sizeof(tar));
		free_chunk_content(&ar);

		/* v2 IKE encryption key for responder (256 bit bound) */
		chunk_t er = chunk_from_symkey("er", st->st_skey_er_nss,
					       st->st_logger);
		char ter[3 + 2 * BYTES_FOR_BITS(256)] = "";
		(void)datatot(er.ptr, er.len, 'x', ter, sizeof(ter));
		free_chunk_content(&er);

		DBG_log("ikev2 R %s %s %s:%s %s%s:%s",
			tispi, trspi,
			authalgo, tar,
			encalgo, tekl, ter);
	}
}

static void ikev2_child_emancipate(struct ike_sa *from, struct child_sa *to,
				   const struct v2_state_transition *transition)
{
	/* initialize the the new IKE SA. reset and message ID */
	to->sa.st_clonedfrom = SOS_NOBODY;
	v2_msgid_init_ike(pexpect_ike_sa(&to->sa));

	/* Switch to the new IKE SPIs */
	to->sa.st_ike_spis = to->sa.st_ike_rekey_spis;
	rehash_state_cookies_in_db(&to->sa);

	/* TO has correct IKE_SPI so can migrate */
	v2_migrate_children(from, to);

	/* child is now a parent */
	ikev2_ike_sa_established(pexpect_ike_sa(&to->sa),
				 transition, transition->next_state);
}

static void jam_v2_ike_details(struct jambuf *buf, struct state *st)
{
	jam_parent_sa_details(buf, st);
}

static void success_v2_state_transition(struct state *st, struct msg_digest *md,
					const struct v2_state_transition *transition)
{
	/*
	 * XXX: the transition's from state can lie - it may be
	 * different to the ST's state!
	 */
	enum state_kind from_state = transition->state;
	struct connection *c = st->st_connection;
	struct ike_sa *ike = ike_sa(st, HERE);

	if (from_state != transition->next_state) {
		dbg("transitioning from state %s to state %s",
		    finite_states[from_state]->name,
		    finite_states[transition->next_state]->name);
	}

	/*
	 * Update counters, and if part of the transition, send the
	 * new message.
	 */

	dbg("Message ID: updating counters for #%lu", st->st_serialno);
	v2_msgid_update_recv(ike, st, md);
	v2_msgid_update_sent(ike, st, md, transition->send);
	v2_msgid_schedule_next_initiator(ike);

	bool established_before = (IS_IKE_SA_ESTABLISHED(st) ||
				   IS_CHILD_SA_ESTABLISHED(st));

	if (from_state == STATE_V2_REKEY_IKE_R0 ||
	    from_state == STATE_V2_REKEY_IKE_I1) {
		ikev2_child_emancipate(ike, pexpect_child_sa(st),
				       transition);
	} else {
		change_state(st, transition->next_state);
	}
	passert(st->st_state->kind >= STATE_IKEv2_FLOOR);
	passert(st->st_state->kind <  STATE_IKEv2_ROOF);

	bool established_after = (IS_IKE_SA_ESTABLISHED(st) ||
				  IS_CHILD_SA_ESTABLISHED(st));

	bool just_established = (!established_before && established_after);
	if (just_established) {
		/*
		 * Count successful transition into an established
		 * state.
		 */
		pstat_sa_established(st);
	}

	/*
	 * Tell whack and logs our progress.
	 *
	 * If it's OE or a state transition we're not telling anyone
	 * about, then be quiet.
	 */

	dbg("announcing the state transition");
	enum rc_type w;
	void (*jam_details)(struct jambuf *buf, struct state *st);
	bool suppress_log = ((transition->flags & SMF2_SUPPRESS_SUCCESS_LOG) ||
			     (c != NULL && (c->policy & POLICY_OPPORTUNISTIC)));
	const char *sep = " ";
	if (transition->state == transition->next_state) {
		/*
		 * HACK for seemingly going around in circles
		 */
		jam_details = NULL;
		w = RC_NEW_V2_STATE + st->st_state->kind;
	} else if (IS_CHILD_SA(st) && just_established) {
		jam_details = jam_v2_child_details;
		suppress_log = false;
		sep = "; ";
		w = RC_SUCCESS; /* also triggers detach */
	} else if (IS_IKE_SA(st) && just_established) {
		/* ike_sa_established() called elsewhere */
		jam_details = jam_v2_ike_details;
		w = RC_SUCCESS; /* also triggers detach */
	} else if (transition->state == STATE_PARENT_I1 &&
		   transition->next_state == STATE_PARENT_I2) {
		jam_details = jam_v2_ike_details;
		w = RC_NEW_V2_STATE + st->st_state->kind;
	} else if (st->st_state->kind == STATE_PARENT_R1) {
		jam_details = jam_v2_ike_details;
		w = RC_NEW_V2_STATE + st->st_state->kind;
	} else {
		jam_details = NULL;
		w = RC_NEW_V2_STATE + st->st_state->kind;
	}

        if (suppress_log) {
		LSWDBGP(DBG_BASE, buf) {
			jam(buf, "%s", st->st_state->story);
			/* document SA details for admin's pleasure */
			if (jam_details != NULL) {
				jam_string(buf, sep);
				jam_details(buf, st);
			}
		}
	} else {
		LLOG_JAMBUF(w, st->st_logger, buf) {
			jam(buf, "%s", st->st_state->story);
			/* document SA details for admin's pleasure */
			if (jam_details != NULL) {
				jam_string(buf, sep);
				jam_details(buf, st);
			}
		}
	}

	/*
	 * 2.23.  NAT Traversal
	 *
	 * There are cases where a NAT box decides to remove mappings
	 * that are still alive (for example, the keepalive interval
	 * is too long, or the NAT box is rebooted).  This will be
	 * apparent to a host if it receives a packet whose integrity
	 * protection validates, but has a different port, address, or
	 * both from the one that was associated with the SA in the
	 * validated packet.  When such a validated packet is found, a
	 * host that does not support other methods of recovery such
	 * as IKEv2 Mobility and Multihoming (MOBIKE) [MOBIKE], and
	 * that is not behind a NAT, SHOULD send all packets
	 * (including retransmission packets) to the IP address and
	 * port in the validated packet, and SHOULD store this as the
	 * new address and port combination for the SA (that is, they
	 * SHOULD dynamically update the address).  A host behind a
	 * NAT SHOULD NOT do this type of dynamic address update if a
	 * validated packet has different port and/or address values
	 * because it opens a possible DoS attack (such as allowing an
	 * attacker to break the connection with a single packet).
	 * Also, dynamic address update should only be done in
	 * response to a new packet; otherwise, an attacker can revert
	 * the addresses with old replayed packets.  Because of this,
	 * dynamic updates can only be done safely if replay
	 * protection is enabled.  When IKEv2 is used with MOBIKE,
	 * dynamically updating the addresses described above
	 * interferes with MOBIKE's way of recovering from the same
	 * situation.  See Section 3.8 of [MOBIKE] for more
	 * information.
	 *
	 * XXX: so ....
	 *
	 * - replay protection?
	 *
	 * - IKE_AUTH exchange? Already handled? Exclude?
	 */
	if (nat_traversal_enabled &&
	    /*
	     * Only when responding ...
	     */
	    transition->send == MESSAGE_RESPONSE &&
	    pexpect(v2_msg_role(md) == MESSAGE_REQUEST) &&
	    /*
	     * Only when the request changes the remote's endpoint ...
	     */
	    !endpoint_eq_endpoint(ike->sa.st_remote_endpoint, md->sender) &&
	    /*
	     * Only when the request was protected and passes
	     * integrity ...
	     *
	     * Once keymat is present, only encrypted messessages with
	     * valid integrity can successfully complete a transaction
	     * with STF_OK.  True?  True.
	     *
	     * IS_IKE_SA_ESTABLISHED() better?  False.  IKE_AUTH
	     * messages meet the above requirements.
	     */
	    ike->sa.hidden_variables.st_skeyid_calculated &&
	    md->encrypted_payloads.parsed &&
	    md->encrypted_payloads.n == v2N_NOTHING_WRONG &&
	    /*
	     * Only when the local IKE SA isn't behind NAT but the
	     * remote IKE SA is ...
	     */
	    !LHAS(ike->sa.hidden_variables.st_nat_traversal, NATED_HOST) &&
	    LHAS(ike->sa.hidden_variables.st_nat_traversal, NATED_PEER)) {
		/*
		 * Things are looking plausible.
		 */
		if (ike->sa.st_ike_sent_v2n_mobike_supported &&
		    ike->sa.st_ike_seen_v2n_mobike_supported &&
		    md->hdr.isa_xchg == ISAKMP_v2_INFORMATIONAL) {
			/*
			 * Only when MOBIKE has not been negotiated ...
			 */
			endpoint_buf sb, mb;
			dbg("NAT: MOBIKE: skipping remote sender from %s -> %s as MOBIKE delt with it in informational code(?)",
			    str_endpoint(&ike->sa.st_remote_endpoint, &sb),
			    str_endpoint(&md->sender, &mb));
		} else {
			endpoint_buf sb, mb;
			dbg("NAT: MOBKIE: updating remote sender from %s -> %s as non-MOBIKE or non-INFORMATIONAL",
			    str_endpoint(&ike->sa.st_remote_endpoint, &sb),
			    str_endpoint(&md->sender, &mb));
			ike->sa.st_remote_endpoint = md->sender;
		}
	}

	/* if requested, send the new reply packet */
	switch (transition->send) {
	case MESSAGE_REQUEST:
	case MESSAGE_RESPONSE:
		send_recorded_v2_message(ike, finite_states[from_state]->name,
					 transition->send);
		break;
	case NO_MESSAGE:
		break;
	default:
		bad_case(transition->send);;
	}

	if (just_established) {
		release_any_whack(st, HERE, "IKEv2 transitions finished");

		/* XXX should call unpend again on parent SA */
		if (IS_CHILD_SA(st)) {
			/* with failed child sa, we end up here with an orphan?? */
			dbg("unpending #%lu's IKE SA #%lu", st->st_serialno,
			    ike->sa.st_serialno);
			/* a better call unpend in ikev2_ike_sa_established? */
			unpend(ike, c);

			/*
			 * If this was an OE connection, check for removing a potential
			 * matching bare shunt entry - bare shunts are always a %pass or
			 * %hold SPI but are found regardless of whether we passed in
			 * SPI_PASS or SPI_HOLD ?
			 */
			if (LIN(POLICY_OPPORTUNISTIC, c->policy)) {
				struct spd_route *sr = &c->spd;
				struct bare_shunt **bs = bare_shunt_ptr(&sr->this.client,
									&sr->that.client,
									sr->this.protocol,
									"old bare shunt to delete");

				if (bs != NULL) {
					dbg("deleting old bare shunt");
					if (!delete_bare_shunt(&c->spd.this.host_addr,
							       &c->spd.that.host_addr,
							       c->spd.this.protocol,
							       SPI_PASS /* else its not bare */,
							       /*skip_xfrm_policy_delete?*/true,
							       "installed IPsec SA replaced old bare shunt",
							       st->st_logger)) {
						log_state(RC_LOG_SERIOUS, &ike->sa,
							  "Failed to delete old bare shunt");
					}
				}
			}
			release_any_whack(&ike->sa, HERE, "IKEv2 transitions finished so releaseing IKE SA");
		}
	} else if (transition->flags & SMF2_RELEASE_WHACK) {
		dbg("releasing whack");
		release_any_whack(st, HERE, "ST per transition");
		if (st != &ike->sa) {
			release_any_whack(&ike->sa, HERE, "IKE per transition");
		}
	}

	/* Schedule for whatever timeout is specified */
	{
		enum event_type kind = transition->timeout_event;
		struct connection *c = st->st_connection;

		switch (kind) {

		case EVENT_RETRANSMIT:
			/*
			 * Event retransmit is really a secret code to
			 * indicate that a request is being sent and a
			 * retransmit should already be scheduled.
			 */
			dbg("checking that a retransmit timeout_event was already");
			delete_event(st); /* relying on retransmit */
			pexpect(ike->sa.st_retransmit_event != NULL);
			pexpect(transition->send == MESSAGE_REQUEST);
			break;

		case EVENT_SA_REPLACE: /* IKE or Child SA replacement event */
			v2_schedule_replace_event(st);
			break;

		case EVENT_SA_DISCARD:
			delete_event(st);
			event_schedule(kind, MAXIMUM_RESPONDER_WAIT_DELAY, st);
			break;

		case EVENT_NULL:
			/*
			 * Is there really no case where we want to
			 * set no timer?  more likely an accident?
			 */
			pexpect_fail(st->st_logger, HERE,
				     "V2 microcode entry (%s) has unspecified timeout_event",
				     transition->story);
			break;

		case EVENT_v2_REDIRECT:
			event_delete(EVENT_v2_REDIRECT, st);
			event_schedule(EVENT_v2_REDIRECT, deltatime(0), st);
			break;

		case EVENT_RETAIN:
			/* the previous event is retained */
			dbg("#%lu is retaining %s with is previously set timeout",
			    st->st_serialno, (st->st_event == NULL ? "<no-event>" :
					      enum_name(&timer_event_names, st->st_event->ev_type)));
			break;

		default:
			bad_case(kind);
			break;
		}
		/*
		 * start liveness checks if set, making sure we only
		 * schedule once when moving from I2->I3 or R1->R2
		 */
		if (st->st_state->kind != from_state &&
			st->st_state->kind != STATE_UNDEFINED &&
			IS_CHILD_SA_ESTABLISHED(st) &&
			dpd_active_locally(st)) {
			dbg("dpd enabled, scheduling ikev2 liveness checks");
			deltatime_t delay = deltatime_max(c->dpd_delay, deltatime(MIN_LIVENESS));
			event_schedule(EVENT_v2_LIVENESS, delay, st);
		}
	}
}

/*
 * Dependent on RESULT, either complete, suspend, abandon, or abort
 * (delete state) the state transition started by the state-specific
 * state transition function.
 *
 * Since this is function is meaningless without a state, ST really
 * should be non-NULL.
 *
 * XXX: A broken exception is when responding to an IKE_SA_INIT
 * request - the state machine calls the state transition function
 * with no state (trusting that the transition function will do the
 * job, but that isn't always true).  The fix is to create the state
 * before calling the state transition function (like is done for the
 * CHILD_SA code).
 *
 * Since, when initiating an exchange there is no message, code can't
 * assume that (*MDP) is non-NULL.
 *
 * XXX: Some state transition functions switch state part way (see
 * AUTH child code) and then tunnel the new state to this code via
 * (*MDP)->st and some callers passing in (*MDP)->st).  The fix is for
 * the AUTH code to handle the CHILD SA as a nested or separate
 * transition.
 *
 * XXX: The state transition structure (microcode) is stored in (*MDP)
 * forcing that structure to be created.  The fix is to store the
 * state's transition in the state.  As a bonus this makes determining
 * if a state is busy really really easy - if there's a
 * state-transition then it must be.
 *
 * This routine does not free (*MDP) (using release_any_md(mdp)).
 * However, when suspending a state transition, it will save it in ST
 * and zap (*MDP) so that the caller can't free it.  Hence, the caller
 * must be prepared for (*MDP) being set to NULL.
 *
 * XXX: At some point (*MDP) was being used for:
 *
 * - find st
 * - success_v2_state_transition(st, md);
 *   - for svm:
 *     - svm->next_state,
 *     - svm->flags & SMF2_SEND,
 *     - svm->timeout_event,
 *     -svm->flags, story
 *   - find from_state (st might be gone)
 *   - ikev2_update_msgid_counters(md);
 *   - nat_traversal_change_port_lookup(md, st)
 * - !(md->hdr.isa_flags & ISAKMP_FLAGS_v2_MSG_R) to gate Notify payloads/exchanges [WRONG]
 * - find note for STF_INTERNAL_ERROR
 * - find note for STF_FAIL (might not be part of result (STF_FAIL+note))
 *
 * We don't use these but complete_v1_state_transition does:
 * - record md->event_already_set
 * - remember_received_packet(st, md);
 * - fragvid, dpd, nortel
 */
void complete_v2_state_transition(struct state *st,
				  struct msg_digest *md,
				  stf_status result)
{
	passert(st != NULL);
	struct ike_sa *ike = ike_sa(st, HERE);
	/* struct child_sa *child = IS_CHILD_SA(st) ? pexpect_child_sa(st) : NULL; */

	/* statistics */
	/* this really depends on the type of error whether it is an IKE or IPsec fail */
	if (result > STF_FAIL) {
		pstats(ike_stf, STF_FAIL);
	} else {
		pstats(ike_stf, result);
	}

	/*
	 * Try to get the transition that is being completed ...
	 *
	 * For the moment this comes from the (presumably non-NULL)
	 * MD.SVM.
	 *
	 * XXX: However, when a packet is bad and no transition is
	 * selected, this code is still called:
	 *
	 * STF_IGNORE: to undo the v2_msgid_start_responder() call;
	 * better would probably be to move that call to after a
	 * transition has been found (but fragmentation makes this
	 * messy).
	 *
	 * STF_FATAL: to discard a state in response to a bad exchange
	 * (for instance a protected packet's contents are bogus).
	 *
	 * Long term, this value should be extracted from the state
	 * and .st_v2_state_transition - it just isn't possible to
	 * squeeze both the IKE and CHILD transitions into MD.ST.
	 */
#if 0
	const struct v2_state_transition *transition = st->st_v2_transition;
	if (!pexpect(transition != NULL) && md != NULL) {
		transition = md->svm;
	}
#else
	const struct v2_state_transition *transition = (md != NULL && md->svm != NULL ? md->svm :
						       st->st_v2_transition);
#endif
	static const struct v2_state_transition undefined_transition = {
		.story = "suspect message",
		.state = STATE_UNDEFINED,
		.next_state = STATE_UNDEFINED,
	};
	/* double negative */
	if (!pexpect(transition != NULL)) {
		transition = &undefined_transition;
	}

	LSWDBGP(DBG_BASE, buf) {
		const struct finite_state *transition_from = finite_states[transition->state];

		jam(buf, "#%lu complete_v2_state_transition()", st->st_serialno);
		if (st->st_state != transition_from) {
			jam(buf, " in state %s", st->st_state->short_name);
		}
		jam(buf, " ");
		jam_v2_transition(buf, transition);
		jam(buf, " with status ");
		jam_v2_stf_status(buf, result);
		/* does MD.SVM diverge? */
		if (md != NULL && transition != md->svm) {
			jam(buf, "; md.svm=");
			jam_v2_transition(buf, md->svm);
		}
		/* does ST.ST_V2_TRANSITION diverge? */
		if (transition != st->st_v2_transition) {
			jam(buf, "; .st_v2_transition=");
			jam_v2_transition(buf, st->st_v2_transition);
		}
	}

	/* audit log failures - success is audit logged in ikev2_ike_sa_established() */
	if (result > STF_OK) {
		linux_audit_conn(st, IS_IKE_SA_ESTABLISHED(st) ? LAK_CHILD_FAIL : LAK_PARENT_FAIL);
	}

	switch (result) {

	case STF_SUSPEND:
		/*
		 * If this transition was triggered by an
		 * incoming packet, save it.
		 *
		 * XXX: some initiator code creates a fake MD
		 * (there isn't a real one); save that as
		 * well.
		 *
		 * XXX: should the helper code be responsible for
		 * saving an MD reference?
		 */
		suspend_any_md(st, md);
		return;

	case STF_IGNORE:
		/*
		 * logged above
		 *
		 * XXX: really?  Suspect this means to say logged
		 * where STF_IGNORE is returned.
		 *
		 * XXX: even when a packet is invalid and no
		 * transition is selected (TRANSITION==NULL) this code
		 * is executed - caller needs to cancel the responder
		 * processing the message.
		 */
		if (v2_msg_role(md) == MESSAGE_REQUEST) {
			v2_msgid_cancel_responder(ike, st, md);
		}
		return;

	case STF_OK:
		/* advance the state */
		success_v2_state_transition(st, md, transition);
		break;

	case STF_INTERNAL_ERROR:
		log_state(RC_INTERNALERR, st, "state transition function for %s had internal error",
			  st->st_state->name);
		release_pending_whacks(st, "internal error");
		break;

	case STF_V2_DELETE_IKE_AUTH_INITIATOR:
		/*
		 * XXX: this is a hack so that IKE_AUTH initiator can
		 * both delete the IKE SA and send an IKE SA delete
		 * notification because the IKE_AUTH response was
		 * rejected.
		 */
		/*
		 * Initiator processing response, finish current
		 * exchange ready for delete request.
		 */
		dbg("Message ID: forcing a response received update (deleting IKE_AUTH initiator)");
		pexpect(st->st_state->kind == STATE_PARENT_I2);
		pexpect(v2_msg_role(md) == MESSAGE_RESPONSE);
		v2_msgid_update_recv(ike, &ike->sa, md);
		/*
		 * All done, now delete the IKE family sending out a
		 * last gasp delete.
		 *
		 * XXX: this call will fire and forget.  It should
		 * call v2_msgid_queue_initiator() with high priority
		 * so this is performed as a separate transition?
		 */
		delete_ike_family(ike, DO_SEND_DELETE);
		/* get out of here -- everything is invalid */
		ike = NULL;
		st = NULL;
		return;

	case STF_FATAL:
		/*
		 * XXX: even when a packet is invalid and no
		 * transition is selected (TRANSITION==NULL) this code
		 * is executed - caller needs to kill the state.
		 */
		log_state(RC_FATAL, st, "encountered fatal error in state %s",
			  st->st_state->name);
		switch (v2_msg_role(md)) {
		case MESSAGE_RESPONSE:
			dbg("Message ID: forcing a response received update");
			v2_msgid_update_recv(ike, NULL, md);
			break;
		case MESSAGE_REQUEST:
			dbg("Message ID: responding with recorded fatal error");
			pexpect(transition->send == MESSAGE_RESPONSE);
			if (ike->sa.st_v2_outgoing[MESSAGE_RESPONSE] != NULL) {
				v2_msgid_update_recv(ike, st, md);
				v2_msgid_update_sent(ike, st, md, transition->send);
				send_recorded_v2_message(ike, "STF_FATAL",
							 MESSAGE_RESPONSE);
			} else {
				dbg("Message ID: exchange zombie as no response?");
			}
			break;
		case NO_MESSAGE:
			break;
		}

		/* if this was a child fail, don't destroy the IKE SA */
		if (IS_IKE_SA(st)) {
			delete_ike_family(ike, DONT_SEND_DELETE);
			ike = NULL;
		}

		/* kill all st pointers */
		st = NULL;
		break;

	case STF_FAIL:
		log_state(RC_NOTIFICATION, st, "state transition '%s' failed",
			  transition->story);
		switch (v2_msg_role(md)) {
		case MESSAGE_RESPONSE:
			v2_msgid_schedule_next_initiator(ike);
			v2_msgid_update_recv(ike, st, md);
			break;
		case MESSAGE_REQUEST:
			dbg("Message ID: responding with recorded error");
			pexpect(transition->send == MESSAGE_RESPONSE);
			v2_msgid_update_recv(ike, st, md);
			v2_msgid_update_sent(ike, st, md, transition->send);
			send_recorded_v2_message(ike, "STF_FAIL", MESSAGE_RESPONSE);
			break;
		case NO_MESSAGE:
			break;
		}
		release_pending_whacks(st, "fatal error"); /* fatal != STF_FATAL  :) */
		delete_state(st);

		/* kill all st pointers */
		st = NULL;
		ike = NULL;
		break;

	default: /* STF_FAIL+notification */
		passert(result > STF_FAIL);
		/*
		 * XXX: For IKEv2, this code path isn't sufficient - a
		 * message request can result in a response that
		 * contains both a success and a fail.  Better to
		 * record the responses and and then return
		 * STF_ZOMBIFY signaling both that the message should
		 * be sent and the state deleted.
		 */
		v2_notification_t notification = result - STF_FAIL;
		/* Only the responder sends a notification */
		if (v2_msg_role(md) == MESSAGE_REQUEST) {
			dbg("sending a notification reply");
			v2_msgid_update_recv(ike, st, md);
			record_v2N_response(st->st_logger, ike, md,
					    notification, NULL/*no data*/,
					    ENCRYPTED_PAYLOAD);
			v2_msgid_update_sent(ike, st, md, transition->send);
			send_recorded_v2_message(ike, "STF_FAIL",
						 MESSAGE_RESPONSE);
			/*
			 * XXX: is this always false; if true above
			 * record would pexpect()?
			 */
			if (md->hdr.isa_xchg == ISAKMP_v2_IKE_SA_INIT) {
				delete_state(st);
				/* kill all st pointers */
				st = NULL; ike = NULL;
			} else {
				dbg("forcing #%lu to a discard event",
				    st->st_serialno);
				delete_event(st);
				event_schedule(EVENT_SA_DISCARD,
					       MAXIMUM_RESPONDER_WAIT_DELAY,
					       st);
			}
		} else {
			log_state(RC_NOTIFICATION+notification, st,
				  "state transition '%s' failed with %s",
				  transition->story, enum_name(&ikev2_notify_names, notification));
		}
		break;
	}
}

void jam_v2_stf_status(struct jambuf *buf, unsigned status)
{
	if (status <= STF_FAIL) {
		jam_enum(buf, &stf_status_names, status);
	} else {
		jam(buf, "STF_FAIL+");
		jam_enum(buf, &ikev2_notify_names, status - STF_FAIL);
	}
}

static void reinitiate_ike_sa_init(struct state *st, void *arg)
{
	if (st == NULL) {
		dbg("re-initiate lost state");
		return;
	}
	struct ike_sa *ike = ike_sa(st, HERE);
	if (ike == NULL) {
		/* already logged */
		return;
	}
	stf_status (*resume)(struct ike_sa *ike) = arg;

	/*
	 * Need to wind back the Message ID counters so that the send
	 * code things it is creating Message 0.
	 */
	v2_msgid_init_ike(ike);

	/*
	 * Pretend to be running the initiate state.
	 */
	set_v2_transition(&ike->sa, finite_states[STATE_PARENT_I0]->v2_transitions, HERE); /* first */
	complete_v2_state_transition(&ike->sa, NULL/*no-MD*/, resume(ike));
}

void schedule_reinitiate_v2_ike_sa_init(struct ike_sa *ike,
					stf_status (*resume)(struct ike_sa *ike))
{
	schedule_callback("reinitiating IKE_SA_INIT", ike->sa.st_serialno,
			  reinitiate_ike_sa_init, resume);
}

bool v2_notification_fatal(v2_notification_t n)
{
	return (n == v2N_INVALID_SYNTAX ||
		n == v2N_AUTHENTICATION_FAILED ||
		n == v2N_UNSUPPORTED_CRITICAL_PAYLOAD);
}

stf_status stf_status_from_v2_notification(v2_notification_t n)
{
	return (n == v2N_NOTHING_WRONG ? STF_OK :
		v2_notification_fatal(n) ? STF_FATAL :
		STF_FAIL);
}

bool already_has_larval_v2_child(struct ike_sa *ike, const struct connection *c)
{
	const lset_t pending_states = (LELEM(STATE_V2_NEW_CHILD_I1) |
				       LELEM(STATE_V2_NEW_CHILD_I0) |
				       LELEM(STATE_V2_NEW_CHILD_R0));

	struct state_query query = {
		.where = HERE,
		.ike_version = IKEv2,
		.ike_spis = &ike->sa.st_ike_spis,
		/* only children */
		.ike = ike,
	};

	for (struct state *st = next_state(NULL, &query);
	     st != NULL; st = next_state(st, &query)) {
		/* larval child state? */
		if (!LHAS(pending_states, st->st_state->kind)) {
			continue;
		}
		/* not an instance, but a connection? */
		if (!streq(st->st_connection->name, c->name)) {
			continue;
		}
		llog(RC_LOG, c->logger, "connection already has the pending Child SA negotiation #%lu using IKE SA #%lu",
		     st->st_serialno, ike->sa.st_serialno);
		return true;
	}

	return false;
}
