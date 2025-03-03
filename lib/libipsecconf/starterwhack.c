/*
 * Libreswan whack functions to communicate with pluto (whack.c)
 *
 * Copyright (C) 2001-2002 Mathieu Lafon - Arkoon Network Security
 * Copyright (C) 2004-2006 Michael Richardson <mcr@xelerance.com>
 * Copyright (C) 2010-2019 D. Hugh Redelmeier <hugh@mimosa.com>
 * Copyright (C) 2011 Mattias Walström <lazzer@vmlinux.org>
 * Copyright (C) 2012-2017 Paul Wouters <paul@libreswan.org>
 * Copyright (C) 2012 Philippe Vouters <Philippe.Vouters@laposte.net>
 * Copyright (C) 2013 Antony Antony <antony@phenome.org>
 * Copyright (C) 2013 Matt Rogers <mrogers@redhat.com>
 * Copyright (C) 2016, Andrew Cagney <cagney@gnu.org>
 * Copyright (C) 2017 Paul Wouters <pwouters@redhat.com>
 * Copyright (C) 2017 Mayank Totale <mtotale@gmail.com>
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
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#include "sysdep.h"

#include "ipsecconf/starterwhack.h"
#include "ipsecconf/confread.h"
#include "ipsecconf/starterlog.h"

#include "socketwrapper.h"

#ifndef _LIBRESWAN_H
#include <libreswan.h>	/* FIXME: ugly include lines */
#include "constants.h"
#endif

#include "lswalloc.h"
#include "lswlog.h"
#include "whack.h"
#include "id.h"
#include "ip_address.h"
#include "ip_info.h"

static int send_reply(int sock, char *buf, ssize_t len)
{
	/* send the secret to pluto */
	if (write(sock, buf, len) != len) {
		int e = errno;

		starter_log(LOG_LEVEL_ERR, "whack: write() failed (%d %s)",
			e, strerror(e));
		return RC_WHACK_PROBLEM;
	}
	return 0;
}

static int starter_whack_read_reply(int sock,
				char xauthusername[MAX_XAUTH_USERNAME_LEN],
				char xauthpass[XAUTH_MAX_PASS_LENGTH],
				int usernamelen,
				int xauthpasslen)
{
	char buf[4097]; /* arbitrary limit on log line length */
	char *be = buf;
	int ret = 0;

	for (;; ) {
		char *ls = buf;
		ssize_t rl = read(sock, be, (buf + sizeof(buf) - 1) - be);

		if (rl < 0) {
			int e = errno;

			fprintf(stderr, "whack: read() failed (%d %s)\n", e,
				strerror(e));
			return RC_WHACK_PROBLEM;
		}
		if (rl == 0) {
			if (be != buf)
				fprintf(stderr,
					"whack: last line from pluto too long or unterminated\n");


			break;
		}

		be += rl;
		*be = '\0';

		for (;; ) {
			char *le = strchr(ls, '\n');

			if (le == NULL) {
				/* move last, partial line to start of buffer */
				memmove(buf, ls, be - ls);
				be -= ls - buf;
				break;
			}

			le++;	/* include NL in line */
			if (isatty(STDOUT_FILENO) &&
				write(STDOUT_FILENO, ls, le - ls) == -1) {
				int e = errno;
				starter_log(LOG_LEVEL_ERR,
					"whack: write() starterwhack.c:124 failed (%d %s), and ignored.",
					e, strerror(e));
			}
			/*
			 * figure out prefix number and how it should affect
			 * our exit status
			 */
			{
				/*
				 * we don't generally use strtoul but
				 * in this case, its failure mode
				 * (0 for nonsense) is probably OK.
				 */
				unsigned long s = strtoul(ls, NULL, 10);

				switch (s) {
				case RC_COMMENT:
				case RC_LOG:
					/* ignore */
					break;

				case RC_SUCCESS:
					/* be happy */
					ret = 0;
					break;

				case RC_ENTERSECRET:
					if (xauthpasslen == 0) {
						xauthpasslen =
							whack_get_secret(
								xauthpass,
								XAUTH_MAX_PASS_LENGTH);
					}
					if (xauthpasslen >
						XAUTH_MAX_PASS_LENGTH) {
						/*
						 * for input >= 128,
						 * xauthpasslen would be 129
						 */
						xauthpasslen =
							XAUTH_MAX_PASS_LENGTH;
						starter_log(LOG_LEVEL_ERR,
							"xauth password cannot be >= %d chars",
							XAUTH_MAX_PASS_LENGTH);
					}
					ret = send_reply(sock, xauthpass,
							xauthpasslen);
					if (ret != 0)
						return ret;

					break;

				case RC_USERPROMPT:
					if (usernamelen == 0) {
						usernamelen = whack_get_value(
							xauthusername,
							MAX_XAUTH_USERNAME_LEN);
					}
					if (usernamelen >
						MAX_XAUTH_USERNAME_LEN) {
						/*
						 * for input >= 128,
						 * useramelen would be 129
						 */
						usernamelen =
							MAX_XAUTH_USERNAME_LEN;
						starter_log(LOG_LEVEL_ERR,
							"username cannot be >= %d chars",
							MAX_XAUTH_USERNAME_LEN);
					}
					ret = send_reply(sock, xauthusername,
							usernamelen);
					if (ret != 0)
						return ret;

					break;

				default:
					/* pass through */
					ret = s;
					break;
				}
			}
			ls = le;
		}
	}
	return ret;
}

static int send_whack_msg(struct whack_message *msg, char *ctlsocket)
{
	struct sockaddr_un ctl_addr = { .sun_family = AF_UNIX };
	int sock;
	ssize_t len;
	struct whackpacker wp;
	err_t ugh;
	int ret;

	/* copy socket location */
	fill_and_terminate(ctl_addr.sun_path, ctlsocket, sizeof(ctl_addr.sun_path));

	/*  Pack strings */
	wp.msg = msg;
	wp.str_next = (unsigned char *)msg->string;
	wp.str_roof = (unsigned char *)&msg->string[sizeof(msg->string)];

	ugh = pack_whack_msg(&wp);

	if (ugh != NULL) {
		starter_log(LOG_LEVEL_ERR,
			"send_wack_msg(): can't pack strings: %s", ugh);
		return -1;
	}

	len = wp.str_next - (unsigned char *)msg;

	/* Connect to pluto ctl */
	sock = safe_socket(AF_UNIX, SOCK_STREAM, 0);
	if (sock < 0) {
		starter_log(LOG_LEVEL_ERR, "socket() failed: %s",
			strerror(errno));
		return -1;
	}
	if (connect(sock, (struct sockaddr *)&ctl_addr,
			offsetof(struct sockaddr_un, sun_path) +
				strlen(ctl_addr.sun_path)) <
		0)
	{
		starter_log(LOG_LEVEL_ERR, "connect(pluto_ctl) failed: %s",
			strerror(errno));
		close(sock);
		return -1;
	}

	/* Send message */
	if (write(sock, msg, len) != len) {
		starter_log(LOG_LEVEL_ERR, "write(pluto_ctl) failed: %s",
			strerror(errno));
		close(sock);
		return -1;
	}

	/* read reply */
	{
		char xauthusername[MAX_XAUTH_USERNAME_LEN];
		char xauthpass[XAUTH_MAX_PASS_LENGTH];

		ret = starter_whack_read_reply(sock, xauthusername, xauthpass, 0,
					0);
		close(sock);
	}

	return ret;
}

static const struct whack_message empty_whack_message = {
	.magic = WHACK_MAGIC,
};

/* NOT RE-ENTRANT: uses a static buffer */
static char *connection_name(const struct starter_conn *conn)
{
	/* If connection name is '%auto', create a new name like conn_xxxxx */
	static char buf[32];

	if (streq(conn->name, "%auto")) {
		snprintf(buf, sizeof(buf), "conn_%ld", conn->id);
		return buf;
	} else {
		return conn->name;
	}
}

static bool set_whack_end(char *lr,
			struct whack_end *w,
			const struct starter_end *l)
{
	w->leftright = lr;
	w->id = l->id;
	w->host_type = l->addrtype;

	switch (l->addrtype) {
	case KH_IPADDR:
	case KH_IFACE:
		w->host_addr = l->addr;
		break;

	case KH_DEFAULTROUTE:
	case KH_IPHOSTNAME:
		/* note: we always copy the name string below */
		w->host_addr = l->host_family->address.any;
		break;

	case KH_OPPO:
	case KH_GROUP:
	case KH_OPPOGROUP:
		/* policy should have been set to OPPO */
		w->host_addr = l->host_family->address.any;
		break;

	case KH_ANY:
		w->host_addr = l->host_family->address.any;
		break;

	default:
		printf("Failed to load connection %s= is not set\n", lr);
		return false;
	}
	w->host_addr_name = l->strings[KSCF_IP];

	switch (l->nexttype) {
	case KH_IPADDR:
		w->host_nexthop = l->nexthop;
		break;

	case KH_DEFAULTROUTE: /* acceptable to set nexthop to %defaultroute */
	case KH_NOTSET:	/* acceptable to not set nexthop */
		/*
		 * but, get the family set up right
		 * XXX the nexthop type has to get into the whack message!
		 */
		w->host_nexthop = l->host_family->address.any;
		break;

	default:
		printf("%s: do something with nexthop case: %d\n", lr,
			l->nexttype);
		break;
	}

	if (address_is_specified(l->sourceip))
		w->host_srcip = l->sourceip;

	if (cidr_is_specified(l->vti_ip))
		w->host_vtiip = l->vti_ip;

	if (cidr_is_specified(l->ifaceip))
		w->ifaceip = l->ifaceip;

	w->has_client = l->has_client;
	if (l->has_client) {
		w->client = l->subnet;
	} else {
		w->client = l->host_family->subnet.all;
	}

	w->host_ikeport = l->options[KNCF_IKEPORT];
	w->protoport = l->protoport;

	if (l->certx != NULL) {
		w->cert = l->certx;
	}
	if (l->ckaid != NULL) {
		w->ckaid = l->ckaid;
	}
	if (l->rsasigkey_type == PUBKEY_PREEXCHANGED) {
		/*
		 * Only send over raw (prexchanged) rsapubkeys (i.e.,
		 * not %cert et.a.)
		 *
		 * XXX: but what is with the two rsasigkeys?  Whack seems
		 * to be willing to send pluto two raw pubkeys under
		 * the same ID.  Just assume that the first key should
		 * be used for the CKAID.
		 */
		passert(l->rsasigkey != NULL);
		w->rsasigkey = l->rsasigkey;
	}
	w->ca = l->ca;
	if (l->options_set[KNCF_SENDCERT])
		w->sendcert = l->options[KNCF_SENDCERT];
	else
		w->sendcert = CERT_ALWAYSSEND;

	if (l->options_set[KNCF_AUTH])
		w->authby = l->options[KNCF_AUTH];

	w->updown = l->updown;
	w->virt = NULL;
	w->virt = l->virt;
	w->key_from_DNS_on_demand = l->key_from_DNS_on_demand;

	if (l->options_set[KNCF_XAUTHSERVER])
		w->xauth_server = l->options[KNCF_XAUTHSERVER];
	if (l->options_set[KNCF_XAUTHCLIENT])
		w->xauth_client = l->options[KNCF_XAUTHCLIENT];
	if (l->strings_set[KSCF_USERNAME])
		w->xauth_username = l->strings[KSCF_USERNAME];

	if (l->options_set[KNCF_MODECONFIGSERVER])
		w->modecfg_server = l->options[KNCF_MODECONFIGSERVER];
	if (l->options_set[KNCF_MODECONFIGCLIENT])
		w->modecfg_client = l->options[KNCF_MODECONFIGCLIENT];
	if (l->options_set[KNCF_CAT])
		w->cat = l->options[KNCF_CAT];
	w->pool_range = l->pool_range;
	return true;
}

static int starter_whack_add_pubkey(struct starter_config *cfg,
				const struct starter_conn *conn,
				const struct starter_end *end, const char *lr)
{
	const char *err;
	char err_buf[TTODATAV_BUF];
	char keyspace[1024 + 4];
	int ret = 0;

	struct whack_message msg = empty_whack_message;
	msg.whack_key = true;
	msg.pubkey_alg = PUBKEY_ALG_RSA;
	if (end->id && end->rsasigkey) {
		msg.keyid = end->id;

		switch (end->rsasigkey_type) {
		case PUBKEY_DNSONDEMAND:
			starter_log(LOG_LEVEL_DEBUG,
				"conn %s/%s has key from DNS",
				connection_name(conn), lr);
			break;

		case PUBKEY_CERTIFICATE:
			starter_log(LOG_LEVEL_DEBUG,
				"conn %s/%s has key from certificate",
				connection_name(conn), lr);
			break;

		case PUBKEY_NOTSET:
			break;

		case PUBKEY_PREEXCHANGED:
			err = ttodatav(end->rsasigkey, 0, 0, keyspace,
				sizeof(keyspace),
				&msg.keyval.len,
				err_buf, sizeof(err_buf), 0);
			if (err) {
				starter_log(LOG_LEVEL_ERR,
					"conn %s/%s: rsasigkey malformed [%s]",
					connection_name(conn), lr, err);
				return 1;
			} else {
				starter_log(LOG_LEVEL_DEBUG,
					    "\tsending %s %srsasigkey=%s",
					    connection_name(conn), lr, end->rsasigkey);
				msg.keyval.ptr = (unsigned char *)keyspace;
				ret = send_whack_msg(&msg, cfg->ctlsocket);
			}
		}
	}

	if (ret < 0)
		return ret;

	return 0;
}

static void conn_log_val(const struct starter_conn *conn,
			 const char *name, const char *value)
{
	starter_log(LOG_LEVEL_DEBUG, "conn: \"%s\" %s=%s",
		    conn->name, name, value == NULL ? "<unset>" : value);
}

static int starter_whack_basic_add_conn(struct starter_config *cfg,
					const struct starter_conn *conn)
{
	struct whack_message msg = empty_whack_message;
	msg.whack_connection = true;
	msg.whack_delete = true;	/* always do replace for now */
	msg.name = connection_name(conn);

	msg.tunnel_addr_family = conn->left.host_family->af;

	if (conn->right.addrtype == KH_IPHOSTNAME)
		msg.dnshostname = conn->right.strings[KSCF_IP];

	msg.nic_offload = conn->options[KNCF_NIC_OFFLOAD];
	msg.sa_ike_life_seconds = deltatime(conn->options[KNCF_IKELIFETIME]);
	msg.sa_ipsec_life_seconds = deltatime(conn->options[KNCF_SALIFETIME]);
	msg.sa_rekey_margin = deltatime(conn->options[KNCF_REKEYMARGIN]);
	msg.sa_rekey_fuzz = conn->options[KNCF_REKEYFUZZ];
	msg.sa_keying_tries = conn->options[KNCF_KEYINGTRIES];
	msg.sa_replay_window = conn->options[KNCF_REPLAY_WINDOW];
	msg.xfrm_if_id = conn->options[KNCF_XFRM_IF_ID];

	msg.r_interval = deltatime_ms(conn->options[KNCF_RETRANSMIT_INTERVAL_MS]);
	msg.r_timeout = deltatime(conn->options[KNCF_RETRANSMIT_TIMEOUT]);

	msg.ike_version = conn->ike_version;
	msg.policy = conn->policy;
	msg.sighash_policy = conn->sighash_policy;

	msg.connalias = conn->connalias;

	msg.metric = conn->options[KNCF_METRIC];

	if (conn->options_set[KNCF_CONNMTU])
		msg.connmtu = conn->options[KNCF_CONNMTU];
	if (conn->options_set[KNCF_PRIORITY])
		msg.sa_priority = conn->options[KNCF_PRIORITY];
	if (conn->options_set[KNCF_TFCPAD])
		msg.sa_tfcpad = conn->options[KNCF_TFCPAD];
	if (conn->options_set[KNCF_NO_ESP_TFC])
		msg.send_no_esp_tfc = conn->options[KNCF_NO_ESP_TFC];
	if (conn->options_set[KNCF_NFLOG_CONN])
		msg.nflog_group = conn->options[KNCF_NFLOG_CONN];

	if (conn->options_set[KNCF_REQID]) {
		if (conn->options[KNCF_REQID] <= 0 ||
		    conn->options[KNCF_REQID] > IPSEC_MANUAL_REQID_MAX) {
			starter_log(LOG_LEVEL_ERR,
				"Ignoring reqid value - range must be 1-%u",
				IPSEC_MANUAL_REQID_MAX);
		} else {
			msg.sa_reqid = conn->options[KNCF_REQID];
		}
	}

	if (conn->options_set[KNCF_REMOTE_TCPPORT]) {
		msg.remote_tcpport = conn->options[KNCF_REMOTE_TCPPORT];
	} else {
		msg.remote_tcpport = NAT_IKE_UDP_PORT;
	}

	if (conn->options_set[KNCF_TCP]) {
		msg.iketcp = conn->options[KNCF_TCP];
	} else {
		msg.iketcp = IKE_TCP_NO;
	}

	/* default to HOLD */
	msg.dpd_action = DPD_ACTION_HOLD;
	if (conn->options_set[KNCF_DPDDELAY] &&
		conn->options_set[KNCF_DPDTIMEOUT]) {
		msg.dpd_delay = deltatime(conn->options[KNCF_DPDDELAY]);
		msg.dpd_timeout = deltatime(conn->options[KNCF_DPDTIMEOUT]);
		if (conn->options_set[KNCF_DPDACTION])
			msg.dpd_action = conn->options[KNCF_DPDACTION];

		if (conn->options_set[KNCF_REKEY] && !conn->options[KNCF_REKEY]) {
			if (conn->options[KNCF_DPDACTION] ==
				DPD_ACTION_RESTART) {
				starter_log(LOG_LEVEL_ERR,
					"conn: \"%s\" warning dpdaction cannot be 'restart'  when rekey=no - defaulting to 'hold'",
					conn->name);
				msg.dpd_action = DPD_ACTION_HOLD;
			}
		}
	} else {
		if (conn->options_set[KNCF_DPDDELAY]  ||
			conn->options_set[KNCF_DPDTIMEOUT] ||
			conn->options_set[KNCF_DPDACTION]) {
			starter_log(LOG_LEVEL_ERR,
				"conn: \"%s\" warning dpd settings are ignored unless both dpdtimeout= and dpddelay= are set",
				conn->name);
		}
	}

	if (conn->options_set[KNCF_SEND_CA])
		msg.send_ca = conn->options[KNCF_SEND_CA];
	else
		msg.send_ca = CA_SEND_NONE;


	if (conn->options_set[KNCF_ENCAPS])
		msg.encaps = conn->options[KNCF_ENCAPS];
	else
		msg.encaps = yna_auto;

	if (conn->options_set[KNCF_NAT_KEEPALIVE])
		msg.nat_keepalive = conn->options[KNCF_NAT_KEEPALIVE];
	else
		msg.nat_keepalive = TRUE;

	if (conn->options_set[KNCF_IKEV1_NATT])
		msg.ikev1_natt = conn->options[KNCF_IKEV1_NATT];
	else
		msg.ikev1_natt = NATT_BOTH;


	/* Activate sending out own vendorid */
	if (conn->options_set[KNCF_SEND_VENDORID])
		msg.send_vendorid = conn->options[KNCF_SEND_VENDORID];

	/* Activate Cisco quircky behaviour not replacing old IPsec SA's */
	if (conn->options_set[KNCF_INITIAL_CONTACT])
		msg.initial_contact = conn->options[KNCF_INITIAL_CONTACT];

	/* Activate their quircky behaviour - rumored to be needed for ModeCfg and RSA */
	if (conn->options_set[KNCF_CISCO_UNITY])
		msg.cisco_unity = conn->options[KNCF_CISCO_UNITY];

	if (conn->options_set[KNCF_VID_STRONGSWAN])
		msg.fake_strongswan = conn->options[KNCF_VID_STRONGSWAN];

	/* Active our Cisco interop code if set */
	if (conn->options_set[KNCF_REMOTEPEERTYPE])
		msg.remotepeertype = conn->options[KNCF_REMOTEPEERTYPE];

#ifdef HAVE_NM
	/* Network Manager support */
	if (conn->options_set[KNCF_NMCONFIGURED])
		msg.nmconfigured = conn->options[KNCF_NMCONFIGURED];

#endif

	if (conn->strings_set[KSCF_SA_SEC_LABEL]) {
		msg.sec_label = conn->sec_label;
		starter_log(LOG_LEVEL_DEBUG, "conn: \"%s\" sec_label=%s",
			conn->name, msg.sec_label);
	}

	msg.modecfg_dns = conn->modecfg_dns;
	conn_log_val(conn, "modecfgdns", msg.modecfg_dns);
	msg.modecfg_domains = conn->modecfg_domains;
	conn_log_val(conn, "modecfgdomains", msg.modecfg_domains);
	msg.modecfg_banner = conn->modecfg_banner;
	conn_log_val(conn, "modecfgbanner", msg.modecfg_banner);

	msg.conn_mark_both = conn->conn_mark_both;
	conn_log_val(conn, "mark", msg.conn_mark_both);
	msg.conn_mark_in = conn->conn_mark_in;
	conn_log_val(conn, "mark-in", msg.conn_mark_in);
	msg.conn_mark_out = conn->conn_mark_out;
	conn_log_val(conn, "mark-out", msg.conn_mark_out);

	msg.vti_iface = conn->vti_iface;
	conn_log_val(conn, "vti_iface", msg.vti_iface);
	if (conn->options_set[KNCF_VTI_ROUTING])
		msg.vti_routing = conn->options[KNCF_VTI_ROUTING];
	if (conn->options_set[KNCF_VTI_SHARED])
		msg.vti_shared = conn->options[KNCF_VTI_SHARED];

	msg.redirect_to = conn->redirect_to;
	conn_log_val(conn, "redirect-to", msg.redirect_to);
	msg.accept_redirect_to = conn->accept_redirect_to;
	conn_log_val(conn, "accept-redirect-to", msg.accept_redirect_to);

	if (conn->options_set[KNCF_XAUTHBY])
		msg.xauthby = conn->options[KNCF_XAUTHBY];
	if (conn->options_set[KNCF_XAUTHFAIL])
		msg.xauthfail = conn->options[KNCF_XAUTHFAIL];

	if (!set_whack_end("left",  &msg.left, &conn->left))
		return -1;
	if (!set_whack_end("right", &msg.right, &conn->right))
		return -1;

	msg.esp = conn->esp;
	conn_log_val(conn, "esp", msg.esp);
	msg.ike = conn->ike_crypto;
	conn_log_val(conn, "ike", msg.ike);

	int r = send_whack_msg(&msg, cfg->ctlsocket);
	if (r != 0)
		return r;

	if (conn->left.rsasigkey != NULL) {
		r = starter_whack_add_pubkey(cfg, conn, &conn->left,  "left");
		if (r != 0)
			return r;
	}
	if (conn->right.rsasigkey != NULL) {
		r = starter_whack_add_pubkey(cfg, conn, &conn->right,  "right");
		if (r != 0)
			return r;
	}

	return 0;
}

static bool one_subnet_from_string(const struct starter_conn *conn,
				   char **psubnets,
				   const struct ip_info *afi,
				   ip_subnet *sn,
				   char *lr, struct logger *logger)
{
	char *eln;
	char *subnets = *psubnets;
	err_t e;

	if (subnets == NULL)
		return FALSE;

	/* find first non-space item */
	while (*subnets != '\0' && (char_isspace(*subnets) || *subnets == ','))
		subnets++;

	/* did we find something? */
	if (*subnets == '\0')
		return FALSE;	/* no */

	eln = subnets;

	/* find end of this item */
	while (*subnets != '\0' && !(char_isspace(*subnets) || *subnets == ','))
		subnets++;

	e = ttosubnet(shunk2(eln, subnets - eln), afi, '6', sn, logger);
	if (e != NULL) {
		starter_log(LOG_LEVEL_ERR,
			"conn: \"%s\" warning '%s' is not a subnet declaration. (%ssubnets)",
			conn->name,
			eln, lr);
	}

	*psubnets = subnets;
	return TRUE;
}

/*
 * permutate_conns - generate all combinations of subnets={}
 *
 * @operation - the function to apply to each generated conn
 * @cfg       - the base configuration
 * @conn      - the conn to permute
 *
 * This function goes through the set of N x M combinations of the subnets
 * defined in conn's "subnets=" declarations and synthesizes conns with
 * the proper left/right subnet settings, and then calls operation(),
 * (which is usually add/delete/route/etc.)
 *
 */
static int starter_permutate_conns(int
				   (*operation)(struct starter_config *cfg,
						const struct starter_conn *conn),
				   struct starter_config *cfg,
				   const struct starter_conn *conn,
				   struct logger *logger)
{
	struct starter_conn sc;
	int lc, rc;
	char *leftnets, *rightnets;
	char tmpconnname[256];
	ip_subnet lnet, rnet;

	leftnets = "";
	if (conn->left.strings_set[KSCF_SUBNETS])
		leftnets = conn->left.strings[KSCF_SUBNETS];

	rightnets = "";
	if (conn->right.strings_set[KSCF_SUBNETS])
		rightnets = conn->right.strings[KSCF_SUBNETS];

	/*
	 * the first combination is the current leftsubnet/rightsubnet
	 * value, and then each iteration of rightsubnets, and then
	 * each permutation of leftsubnets X rightsubnets.
	 *
	 * If both subnet= is set and subnets=, then it is as if an extra
	 * element of subnets= has been added, so subnets= for only one
	 * side will do the right thing, as will some combinations of also=
	 *
	 */

	if (conn->left.strings_set[KSCF_SUBNET]) {
		lnet = conn->left.subnet;
		lc = 0;
	} else {
		one_subnet_from_string(conn, &leftnets,
				       conn->left.host_family,
				       &lnet, "left", logger);
		lc = 1;
	}

	if (conn->right.strings_set[KSCF_SUBNET]) {
		rnet = conn->right.subnet;
		rc = 0;
	} else {
		one_subnet_from_string(conn, &rightnets,
				       conn->right.host_family,
				       &rnet, "right", logger);
		rc = 1;
	}

	for (;;) {
		int success;

		/* copy conn  --- we can borrow all pointers, since this
		 * is a temporary copy */
		sc = *conn;

		/* fix up leftsubnet/rightsubnet properly, make sure
		 * that has_client is set.
		 */
		sc.left.subnet = lnet;
		sc.left.has_client = TRUE;

		sc.right.subnet = rnet;
		sc.right.has_client = TRUE;

		snprintf(tmpconnname, sizeof(tmpconnname), "%s/%ux%u",
			conn->name, lc, rc);
		sc.name = tmpconnname;

		sc.connalias = conn->name;

		success = (*operation)(cfg, &sc);
		if (success != 0) {
			/* Fail at first failure?  I think so. */
			return success;
		}

		/*
		 * okay, advance right first, and if it is out, then do
		 * left.
		 */
		rc++;
		if (!one_subnet_from_string(conn, &rightnets,
					    conn->right.host_family,
					    &rnet, "right", logger)) {
			/* reset right, and advance left! */
			rightnets = "";
			if (conn->right.strings_set[KSCF_SUBNETS])
				rightnets = conn->right.strings[KSCF_SUBNETS];

			/* should rightsubnet= be the first item ? */
			if (conn->right.strings_set[KSCF_SUBNET]) {
				rnet = conn->right.subnet;
				rc = 0;
			} else {
				one_subnet_from_string(conn, &rightnets,
						       conn->right.host_family,
						       &rnet, "right", logger);
				rc = 1;
			}

			/* left */
			lc++;
			if (!one_subnet_from_string(conn, &leftnets,
						    conn->left.host_family,
						    &lnet, "left", logger))
				break;
		}
	}

	return 0;	/* success. */
}

int starter_whack_add_conn(struct starter_config *cfg,
			   const struct starter_conn *conn,
			   struct logger *logger)
{
	/* basic case, nothing special to synthize! */
	if (!conn->left.strings_set[KSCF_SUBNETS] &&
	    !conn->right.strings_set[KSCF_SUBNETS])
		return starter_whack_basic_add_conn(cfg, conn);

	return starter_permutate_conns(starter_whack_basic_add_conn,
				       cfg, conn, logger);
}

static int starter_whack_basic_route_conn(struct starter_config *cfg,
					const struct starter_conn *conn)
{
	struct whack_message msg = empty_whack_message;
	msg.whack_route = true;
	msg.name = connection_name(conn);
	return send_whack_msg(&msg, cfg->ctlsocket);
}

int starter_whack_route_conn(struct starter_config *cfg,
			     struct starter_conn *conn,
			     struct logger *logger)
{
	/* basic case, nothing special to synthize! */
	if (!conn->left.strings_set[KSCF_SUBNETS] &&
	    !conn->right.strings_set[KSCF_SUBNETS])
		return starter_whack_basic_route_conn(cfg, conn);

	return starter_permutate_conns(starter_whack_basic_route_conn,
				       cfg, conn, logger);
}

int starter_whack_initiate_conn(struct starter_config *cfg,
				struct starter_conn *conn)
{
	struct whack_message msg = empty_whack_message;
	msg.whack_initiate = true;
	msg.whack_async = true;
	msg.name = connection_name(conn);
	return send_whack_msg(&msg, cfg->ctlsocket);
}

int starter_whack_listen(struct starter_config *cfg)
{
	struct whack_message msg = empty_whack_message;
	msg.whack_listen = true;
	return send_whack_msg(&msg, cfg->ctlsocket);
}
