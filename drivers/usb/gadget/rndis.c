/*
 * RNDIS MSG parser
 *
 * Authors:	Benedikt Spranger, Pengutronix
 *		Robert Schwebel, Pengutronix
 *
 *              This program is free software; you can redistribute it and/or
 *              modify it under the terms of the GNU General Public License
 *              version 2, as published by the Free Software Foundation.
 *
 *		This software was originally developed in conformance with
 *		Microsoft's Remote NDIS Specification License Agreement.
 *
 * 03/12/2004 Kai-Uwe Bloem <linux-development@auerswald.de>
 *		Fixed message length bug in init_response
 *
 * 03/25/2004 Kai-Uwe Bloem <linux-development@auerswald.de>
 *		Fixed rndis_rm_hdr length bug.
 *
 * Copyright (C) 2004 by David Brownell
 *		updates to merge with Linux 2.6, better match RNDIS spec
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/proc_fs.h>
#include <linux/slab.h>
#include <linux/seq_file.h>
#include <linux/netdevice.h>

#include <asm/io.h>
#include <asm/byteorder.h>
#include <asm/unaligned.h>


#undef	VERBOSE_DEBUG

#include "rndis.h"


/* The driver for your USB chip needs to support ep0 OUT to work with
 * RNDIS, plus all three CDC Ethernet endpoints (interrupt not optional).
 *
 * Windows hosts need an INF file like Documentation/usb/linux.inf
 * and will be happier if you provide the host_addr module parameter.
 */

#if 0
static int rndis_debug = 0;
module_param (rndis_debug, int, 0);
MODULE_PARM_DESC (rndis_debug, "enable debugging");
#else
#define rndis_debug		0
#endif

#define RNDIS_MAX_CONFIGS	1

int rndis_ul_max_pkt_per_xfer_rcvd;
module_param(rndis_ul_max_pkt_per_xfer_rcvd, int, S_IRUGO);
MODULE_PARM_DESC(rndis_ul_max_pkt_per_xfer_rcvd,
	"Max num of REMOTE_NDIS_PACKET_MSGs received in a single transfer");

int rndis_ul_max_xfer_size_rcvd;
module_param(rndis_ul_max_xfer_size_rcvd, int, S_IRUGO);
MODULE_PARM_DESC(rndis_ul_max_xfer_size_rcvd,
	"Max size of bus transfer received");


static rndis_params rndis_per_dev_params[RNDIS_MAX_CONFIGS];

/* Driver Version */
static const __le32 rndis_driver_version = cpu_to_le32(1);

/* Function Prototypes */
static rndis_resp_t *rndis_add_response(int configNr, u32 length);


/* supported OIDs */
static const u32 oid_supported_list[] =
{
	/* the general stuff */
	RNDIS_OID_GEN_SUPPORTED_LIST,
	RNDIS_OID_GEN_HARDWARE_STATUS,
	RNDIS_OID_GEN_MEDIA_SUPPORTED,
	RNDIS_OID_GEN_MEDIA_IN_USE,
	RNDIS_OID_GEN_MAXIMUM_FRAME_SIZE,
	RNDIS_OID_GEN_LINK_SPEED,
	RNDIS_OID_GEN_TRANSMIT_BLOCK_SIZE,
	RNDIS_OID_GEN_RECEIVE_BLOCK_SIZE,
	RNDIS_OID_GEN_VENDOR_ID,
	RNDIS_OID_GEN_VENDOR_DESCRIPTION,
	RNDIS_OID_GEN_VENDOR_DRIVER_VERSION,
	RNDIS_OID_GEN_CURRENT_PACKET_FILTER,
	RNDIS_OID_GEN_MAXIMUM_TOTAL_SIZE,
	RNDIS_OID_GEN_MEDIA_CONNECT_STATUS,
	RNDIS_OID_GEN_PHYSICAL_MEDIUM,

	/* the statistical stuff */
	RNDIS_OID_GEN_XMIT_OK,
	RNDIS_OID_GEN_RCV_OK,
	RNDIS_OID_GEN_XMIT_ERROR,
	RNDIS_OID_GEN_RCV_ERROR,
	RNDIS_OID_GEN_RCV_NO_BUFFER,
#ifdef	RNDIS_OPTIONAL_STATS
	RNDIS_OID_GEN_DIRECTED_BYTES_XMIT,
	RNDIS_OID_GEN_DIRECTED_FRAMES_XMIT,
	RNDIS_OID_GEN_MULTICAST_BYTES_XMIT,
	RNDIS_OID_GEN_MULTICAST_FRAMES_XMIT,
	RNDIS_OID_GEN_BROADCAST_BYTES_XMIT,
	RNDIS_OID_GEN_BROADCAST_FRAMES_XMIT,
	RNDIS_OID_GEN_DIRECTED_BYTES_RCV,
	RNDIS_OID_GEN_DIRECTED_FRAMES_RCV,
	RNDIS_OID_GEN_MULTICAST_BYTES_RCV,
	RNDIS_OID_GEN_MULTICAST_FRAMES_RCV,
	RNDIS_OID_GEN_BROADCAST_BYTES_RCV,
	RNDIS_OID_GEN_BROADCAST_FRAMES_RCV,
	RNDIS_OID_GEN_RCV_CRC_ERROR,
	RNDIS_OID_GEN_TRANSMIT_QUEUE_LENGTH,
#endif	/* RNDIS_OPTIONAL_STATS */

	/* mandatory 802.3 */
	/* the general stuff */
	RNDIS_OID_802_3_PERMANENT_ADDRESS,
	RNDIS_OID_802_3_CURRENT_ADDRESS,
	RNDIS_OID_802_3_MULTICAST_LIST,
	RNDIS_OID_802_3_MAC_OPTIONS,
	RNDIS_OID_802_3_MAXIMUM_LIST_SIZE,

	/* the statistical stuff */
	RNDIS_OID_802_3_RCV_ERROR_ALIGNMENT,
	RNDIS_OID_802_3_XMIT_ONE_COLLISION,
	RNDIS_OID_802_3_XMIT_MORE_COLLISIONS,
#ifdef	RNDIS_OPTIONAL_STATS
	RNDIS_OID_802_3_XMIT_DEFERRED,
	RNDIS_OID_802_3_XMIT_MAX_COLLISIONS,
	RNDIS_OID_802_3_RCV_OVERRUN,
	RNDIS_OID_802_3_XMIT_UNDERRUN,
	RNDIS_OID_802_3_XMIT_HEARTBEAT_FAILURE,
	RNDIS_OID_802_3_XMIT_TIMES_CRS_LOST,
	RNDIS_OID_802_3_XMIT_LATE_COLLISIONS,
#endif	/* RNDIS_OPTIONAL_STATS */

#ifdef	RNDIS_PM
	/* PM and wakeup are "mandatory" for USB, but the RNDIS specs
	 * don't say what they mean ... and the NDIS specs are often
	 * confusing and/or ambiguous in this context.  (That is, more
	 * so than their specs for the other OIDs.)
	 *
	 * FIXME someone who knows what these should do, please
	 * implement them!
	 */

	/* power management */
	OID_PNP_CAPABILITIES,
	OID_PNP_QUERY_POWER,
	OID_PNP_SET_POWER,

#ifdef	RNDIS_WAKEUP
	/* wake up host */
	OID_PNP_ENABLE_WAKE_UP,
	OID_PNP_ADD_WAKE_UP_PATTERN,
	OID_PNP_REMOVE_WAKE_UP_PATTERN,
#endif	/* RNDIS_WAKEUP */
#endif	/* RNDIS_PM */
};

unsigned long rndis_test_reset_msg_cnt = 0;
u32 rndis_test_last_msg_id = 0;
extern u32 rndis_test_last_resp_id;

extern unsigned long rndis_test_rx_usb_in;
extern unsigned long rndis_test_rx_net_out;
extern unsigned long rndis_test_rx_nomem;
extern unsigned long rndis_test_rx_error;

extern unsigned long rndis_test_tx_net_in;
extern unsigned long rndis_test_tx_busy;
extern unsigned long rndis_test_tx_usb_out;
extern unsigned long rndis_test_tx_complete;

/* NDIS Functions */
static int gen_ndis_query_resp(int configNr, u32 OID, u8 *buf,
			       unsigned buf_len, rndis_resp_t *r)
{
	int retval = -ENOTSUPP;
	u32 length = 4;	/* usually */
	__le32 *outbuf;
	int i, count;
	rndis_query_cmplt_type *resp;
	struct net_device *net;
	struct rtnl_link_stats64 temp;
	const struct rtnl_link_stats64 *stats;

	if (!r) return -ENOMEM;
	resp = (rndis_query_cmplt_type *)r->buf;

	if (!resp) return -ENOMEM;

	if (buf_len && rndis_debug > 1) {
		pr_debug("query OID %08x value, len %d:\n", OID, buf_len);
		for (i = 0; i < buf_len; i += 16) {
			pr_debug("%03d: %08x %08x %08x %08x\n", i,
				get_unaligned_le32(&buf[i]),
				get_unaligned_le32(&buf[i + 4]),
				get_unaligned_le32(&buf[i + 8]),
				get_unaligned_le32(&buf[i + 12]));
		}
	}

	/* response goes here, right after the header */
	outbuf = (__le32 *)&resp[1];
	resp->InformationBufferOffset = cpu_to_le32(16);

	net = rndis_per_dev_params[configNr].dev;
	stats = dev_get_stats(net, &temp);

	if (rndis_debug){
		pr_debug("gen_ndis_query_resp, OID is 0x%x, configNr is %d\n", OID, configNr) ;
	}

	switch (OID) {

	/* general oids (table 4-1) */

	/* mandatory */
	case RNDIS_OID_GEN_SUPPORTED_LIST:
		pr_debug("%s: RNDIS_OID_GEN_SUPPORTED_LIST\n", __func__);
		length = sizeof(oid_supported_list);
		count  = length / sizeof(u32);
		for (i = 0; i < count; i++)
			outbuf[i] = cpu_to_le32(oid_supported_list[i]);
		retval = 0;
		break;

	/* mandatory */
	case RNDIS_OID_GEN_HARDWARE_STATUS:
		pr_debug("%s: RNDIS_OID_GEN_HARDWARE_STATUS\n", __func__);
		/* Bogus question!
		 * Hardware must be ready to receive high level protocols.
		 * BTW:
		 * reddite ergo quae sunt Caesaris Caesari
		 * et quae sunt Dei Deo!
		 */
		*outbuf = cpu_to_le32(0);
		retval = 0;
		break;

	/* mandatory */
	case RNDIS_OID_GEN_MEDIA_SUPPORTED:
		pr_debug("%s: RNDIS_OID_GEN_MEDIA_SUPPORTED\n", __func__);
		*outbuf = cpu_to_le32(rndis_per_dev_params[configNr].medium);
		retval = 0;
		break;

	/* mandatory */
	case RNDIS_OID_GEN_MEDIA_IN_USE:
		pr_debug("%s: RNDIS_OID_GEN_MEDIA_IN_USE\n", __func__);
		/* one medium, one transport... (maybe you do it better) */
		*outbuf = cpu_to_le32(rndis_per_dev_params[configNr].medium);
		retval = 0;
		break;

	/* mandatory */
	case RNDIS_OID_GEN_MAXIMUM_FRAME_SIZE:
		pr_debug("%s: RNDIS_OID_GEN_MAXIMUM_FRAME_SIZE\n", __func__);
		if (rndis_per_dev_params[configNr].dev) {
			*outbuf = cpu_to_le32(
				rndis_per_dev_params[configNr].dev->mtu);
			retval = 0;
		}
		break;

	/* mandatory */
	case RNDIS_OID_GEN_LINK_SPEED:
		if (rndis_debug > 1)
			pr_debug("%s: RNDIS_OID_GEN_LINK_SPEED\n", __func__);
		if (rndis_per_dev_params[configNr].media_state
				== RNDIS_MEDIA_STATE_DISCONNECTED)
			*outbuf = cpu_to_le32(0);
		else
			*outbuf = cpu_to_le32(
				rndis_per_dev_params[configNr].speed);

		if (rndis_debug){
			pr_debug("OID_GEN_LINK_SPEED, speed %d. data: rx_in %lu, rx_out %lu, rx_nomem %lu, rx_error %lu, tx_in %lu, tx_out %lu, tx_busy %lu, tx_complete %lu, last_msg_id: 0x%x, last_resp_id: 0x%x, RNDIS reset cnt: 0x%lu, queue stopped? %d\n",
			rndis_per_dev_params[configNr].speed, rndis_test_rx_usb_in, rndis_test_rx_net_out, rndis_test_rx_nomem, rndis_test_rx_error, rndis_test_tx_net_in,
			rndis_test_tx_usb_out, rndis_test_tx_busy , rndis_test_tx_complete, rndis_test_last_msg_id, rndis_test_last_resp_id, rndis_test_reset_msg_cnt,
			netif_queue_stopped(rndis_per_dev_params[configNr].dev)) ;
		}

		retval = 0;
		break;

	/* mandatory */
	case RNDIS_OID_GEN_TRANSMIT_BLOCK_SIZE:
		pr_debug("%s: RNDIS_OID_GEN_TRANSMIT_BLOCK_SIZE\n", __func__);
		if (rndis_per_dev_params[configNr].dev) {
			*outbuf = cpu_to_le32(
				rndis_per_dev_params[configNr].dev->mtu);
			retval = 0;
		}
		break;

	/* mandatory */
	case RNDIS_OID_GEN_RECEIVE_BLOCK_SIZE:
		pr_debug("%s: RNDIS_OID_GEN_RECEIVE_BLOCK_SIZE\n", __func__);
		if (rndis_per_dev_params[configNr].dev) {
			*outbuf = cpu_to_le32(
				rndis_per_dev_params[configNr].dev->mtu);
			retval = 0;
		}
		break;

	/* mandatory */
	case RNDIS_OID_GEN_VENDOR_ID:
		pr_debug("%s: RNDIS_OID_GEN_VENDOR_ID\n", __func__);
		*outbuf = cpu_to_le32(
			rndis_per_dev_params[configNr].vendorID);
		retval = 0;
		break;

	/* mandatory */
	case RNDIS_OID_GEN_VENDOR_DESCRIPTION:
		pr_debug("%s: RNDIS_OID_GEN_VENDOR_DESCRIPTION\n", __func__);
		if (rndis_per_dev_params[configNr].vendorDescr) {
			length = strlen(rndis_per_dev_params[configNr].
					vendorDescr);
			memcpy(outbuf,
				rndis_per_dev_params[configNr].vendorDescr,
				length);
		} else {
			outbuf[0] = 0;
		}
		retval = 0;
		break;

	case RNDIS_OID_GEN_VENDOR_DRIVER_VERSION:
		pr_debug("%s: RNDIS_OID_GEN_VENDOR_DRIVER_VERSION\n", __func__);
		/* Created as LE */
		*outbuf = rndis_driver_version;
		retval = 0;
		break;

	/* mandatory */
	case RNDIS_OID_GEN_CURRENT_PACKET_FILTER:
		pr_debug("%s: RNDIS_OID_GEN_CURRENT_PACKET_FILTER\n", __func__);
		*outbuf = cpu_to_le32(*rndis_per_dev_params[configNr].filter);
		retval = 0;
		break;

	/* mandatory */
	case RNDIS_OID_GEN_MAXIMUM_TOTAL_SIZE:
		pr_debug("%s: RNDIS_OID_GEN_MAXIMUM_TOTAL_SIZE\n", __func__);
		*outbuf = cpu_to_le32(RNDIS_MAX_TOTAL_SIZE);
		retval = 0;
		break;

	/* mandatory */
	case RNDIS_OID_GEN_MEDIA_CONNECT_STATUS:
		if (rndis_debug > 1)
			pr_debug("%s: RNDIS_OID_GEN_MEDIA_CONNECT_STATUS\n", __func__);
		*outbuf = cpu_to_le32(rndis_per_dev_params[configNr]
						.media_state);
		if (rndis_debug){
			pr_debug("OID_GEN_MEDIA_CONNECT_STATUS, media_state %d\n", rndis_per_dev_params[configNr].media_state) ;
		}
		retval = 0;
		break;

	case RNDIS_OID_GEN_PHYSICAL_MEDIUM:
		pr_debug("%s: RNDIS_OID_GEN_PHYSICAL_MEDIUM\n", __func__);
		*outbuf = cpu_to_le32(0);
		retval = 0;
		break;

	/* The RNDIS specification is incomplete/wrong.   Some versions
	 * of MS-Windows expect OIDs that aren't specified there.  Other
	 * versions emit undefined RNDIS messages. DOCUMENT ALL THESE!
	 */
	case RNDIS_OID_GEN_MAC_OPTIONS:		/* from WinME */
		pr_debug("%s: RNDIS_OID_GEN_MAC_OPTIONS\n", __func__);
		*outbuf = cpu_to_le32(
			  RNDIS_MAC_OPTION_RECEIVE_SERIALIZED
			| RNDIS_MAC_OPTION_FULL_DUPLEX);
		retval = 0;
		break;

	/* statistics OIDs (table 4-2) */

	/* mandatory */
	case RNDIS_OID_GEN_XMIT_OK:
		if (rndis_debug > 1)
			pr_debug("%s: RNDIS_OID_GEN_XMIT_OK\n", __func__);
		if (stats) {
			*outbuf = cpu_to_le32(stats->tx_packets
				- stats->tx_errors - stats->tx_dropped);
			retval = 0;
		}
		break;

	/* mandatory */
	case RNDIS_OID_GEN_RCV_OK:
		if (rndis_debug > 1)
			pr_debug("%s: RNDIS_OID_GEN_RCV_OK\n", __func__);
		if (stats) {
			*outbuf = cpu_to_le32(stats->rx_packets
				- stats->rx_errors - stats->rx_dropped);
			retval = 0;
		}
		break;

	/* mandatory */
	case RNDIS_OID_GEN_XMIT_ERROR:
		if (rndis_debug > 1)
			pr_debug("%s: RNDIS_OID_GEN_XMIT_ERROR\n", __func__);
		if (stats) {
			*outbuf = cpu_to_le32(stats->tx_errors);
			if (rndis_debug){
				pr_err("OID_GEN_XMIT_ERROR, stats->tx_errors %llu\n", stats->tx_errors) ;
			}
			retval = 0;
		}
		break;

	/* mandatory */
	case RNDIS_OID_GEN_RCV_ERROR:
		if (rndis_debug > 1)
			pr_debug("%s: RNDIS_OID_GEN_RCV_ERROR\n", __func__);
		if (stats) {
			*outbuf = cpu_to_le32(stats->rx_errors);
			if (rndis_debug){
				pr_err("OID_GEN_RCV_ERROR, stats->rx_errors %llu\n", stats->rx_errors) ;
			}
			retval = 0;
		}
		break;

	/* mandatory */
	case RNDIS_OID_GEN_RCV_NO_BUFFER:
		pr_debug("%s: RNDIS_OID_GEN_RCV_NO_BUFFER\n", __func__);
		if (stats) {
			*outbuf = cpu_to_le32(stats->rx_dropped);
			if (rndis_debug){
				pr_err("OID_GEN_RCV_NO_BUFFER, stats->rx_dropped %llu\n", stats->rx_dropped) ;
			}
			retval = 0;
		}
		break;

	/* ieee802.3 OIDs (table 4-3) */

	/* mandatory */
	case RNDIS_OID_802_3_PERMANENT_ADDRESS:
		pr_debug("%s: RNDIS_OID_802_3_PERMANENT_ADDRESS\n", __func__);
		if (rndis_per_dev_params[configNr].dev) {
			length = ETH_ALEN;
			memcpy(outbuf,
				rndis_per_dev_params[configNr].host_mac,
				length);
			retval = 0;
			if (rndis_debug){
				pr_debug("OID_802_3_PERMANENT_ADDRESS, host_mac is %x %x  ....\n",
				rndis_per_dev_params [configNr].host_mac[0],
				rndis_per_dev_params [configNr].host_mac[1]) ;
			}
		}
		break;

	/* mandatory */
	case RNDIS_OID_802_3_CURRENT_ADDRESS:
		pr_debug("%s: RNDIS_OID_802_3_CURRENT_ADDRESS\n", __func__);
		if (rndis_per_dev_params[configNr].dev) {
			length = ETH_ALEN;
			memcpy(outbuf,
				rndis_per_dev_params [configNr].host_mac,
				length);

			if (rndis_debug){
				pr_debug("OID_802_3_CURRENT_ADDRESS, host_mac is %x %x  ....\n",
					rndis_per_dev_params [configNr].host_mac[0],
					rndis_per_dev_params [configNr].host_mac[1]) ;
			}

			retval = 0;
		}
		break;

	/* mandatory */
	case RNDIS_OID_802_3_MULTICAST_LIST:
		pr_debug("%s: RNDIS_OID_802_3_MULTICAST_LIST\n", __func__);
		/* Multicast base address only */
		*outbuf = cpu_to_le32(0xE0000000);
		retval = 0;
		break;

	/* mandatory */
	case RNDIS_OID_802_3_MAXIMUM_LIST_SIZE:
		pr_debug("%s: RNDIS_OID_802_3_MAXIMUM_LIST_SIZE\n", __func__);
		/* Multicast base address only */
		*outbuf = cpu_to_le32(1);
		retval = 0;
		break;

	case RNDIS_OID_802_3_MAC_OPTIONS:
		pr_debug("%s: RNDIS_OID_802_3_MAC_OPTIONS\n", __func__);
		*outbuf = cpu_to_le32(0);
		retval = 0;
		break;

	/* ieee802.3 statistics OIDs (table 4-4) */

	/* mandatory */
	case RNDIS_OID_802_3_RCV_ERROR_ALIGNMENT:
		pr_debug("%s: RNDIS_OID_802_3_RCV_ERROR_ALIGNMENT\n", __func__);
		if (stats) {
			if (rndis_debug){
				pr_debug("OID_802_3_RCV_ERROR_ALIGNMENT, stats->rx_frame_errors is %llu\n", stats->rx_frame_errors) ;
			}
			*outbuf = cpu_to_le32(stats->rx_frame_errors);
			retval = 0;
		}
		break;

	/* mandatory */
	case RNDIS_OID_802_3_XMIT_ONE_COLLISION:
		pr_debug("%s: RNDIS_OID_802_3_XMIT_ONE_COLLISION\n", __func__);
		*outbuf = cpu_to_le32(0);
		retval = 0;
		break;

	/* mandatory */
	case RNDIS_OID_802_3_XMIT_MORE_COLLISIONS:
		pr_debug("%s: RNDIS_OID_802_3_XMIT_MORE_COLLISIONS\n", __func__);
		*outbuf = cpu_to_le32(0);
		retval = 0;
		break;

	default:
		pr_warning("%s: query unknown OID 0x%08X\n",
			 __func__, OID);
	}
	if (retval < 0){
		length = 0;
		pr_warning("%s: bad gen_ndis_query_resp, retval is 0x%d\n", __func__, retval) ;
	}

	resp->InformationBufferLength = cpu_to_le32(length);
	r->length = length + sizeof(*resp);
	resp->MessageLength = cpu_to_le32(r->length);
	return retval;
}

static int gen_ndis_set_resp(u8 configNr, u32 OID, u8 *buf, u32 buf_len,
			     rndis_resp_t *r)
{
	rndis_set_cmplt_type *resp;
	int i, retval = -ENOTSUPP;
	struct rndis_params *params;

	if (!r)
		return -ENOMEM;
	resp = (rndis_set_cmplt_type *)r->buf;
	if (!resp)
		return -ENOMEM;

	if (buf_len && rndis_debug > 1) {
		pr_debug("set OID %08x value, len %d:\n", OID, buf_len);
		for (i = 0; i < buf_len; i += 16) {
			pr_debug("%03d: %08x %08x %08x %08x\n", i,
				get_unaligned_le32(&buf[i]),
				get_unaligned_le32(&buf[i + 4]),
				get_unaligned_le32(&buf[i + 8]),
				get_unaligned_le32(&buf[i + 12]));
		}
	}

	if (rndis_debug){
		pr_debug("gen_ndis_set_resp, OID is 0x%x\n", OID) ;
	}

	params = &rndis_per_dev_params[configNr];
	switch (OID) {
	case RNDIS_OID_GEN_CURRENT_PACKET_FILTER:

		/* these NDIS_PACKET_TYPE_* bitflags are shared with
		 * cdc_filter; it's not RNDIS-specific
		 * NDIS_PACKET_TYPE_x == USB_CDC_PACKET_TYPE_x for x in:
		 *	PROMISCUOUS, DIRECTED,
		 *	MULTICAST, ALL_MULTICAST, BROADCAST
		 */
		*params->filter = (u16)get_unaligned_le32(buf);
		pr_debug("%s: RNDIS_OID_GEN_CURRENT_PACKET_FILTER %08x\n",
			__func__, *params->filter);

		/* this call has a significant side effect:  it's
		 * what makes the packet flow start and stop, like
		 * activating the CDC Ethernet altsetting.
		 */
		retval = 0;
		if (*params->filter) {
			params->state = RNDIS_DATA_INITIALIZED;
			netif_carrier_on(params->dev);
			if (netif_running(params->dev))
				netif_wake_queue(params->dev);
		} else {
			params->state = RNDIS_INITIALIZED;
			netif_carrier_off(params->dev);
			netif_stop_queue(params->dev);
		}
		break;

	case RNDIS_OID_802_3_MULTICAST_LIST:
		/* I think we can ignore this */
		pr_debug("%s: RNDIS_OID_802_3_MULTICAST_LIST\n", __func__);
		retval = 0;
		break;

	default:
		pr_warning("%s: set unknown OID 0x%08X, size %d\n",
			 __func__, OID, buf_len);
	}

	if (retval){
		pr_debug("gen_ndis_set_resp, <==== retval is 0x%d\n", retval) ;
	}

	return retval;
}

/*
 * Response Functions
 */

static int rndis_init_response(int configNr, rndis_init_msg_type *buf)
{
	rndis_init_cmplt_type *resp;
	rndis_resp_t *r;
	struct rndis_params *params = rndis_per_dev_params + configNr;

	if (!params->dev)
		return -ENOTSUPP;

	r = rndis_add_response(configNr, sizeof(rndis_init_cmplt_type));
	if (!r)
		return -ENOMEM;
	resp = (rndis_init_cmplt_type *)r->buf;

	resp->MessageType = cpu_to_le32(RNDIS_MSG_INIT_C);
	resp->MessageLength = cpu_to_le32(52);
	resp->RequestID = buf->RequestID; /* Still LE in msg buffer */
	resp->Status = cpu_to_le32(RNDIS_STATUS_SUCCESS);
	resp->MajorVersion = cpu_to_le32(RNDIS_MAJOR_VERSION);
	resp->MinorVersion = cpu_to_le32(RNDIS_MINOR_VERSION);
	resp->DeviceFlags = cpu_to_le32(RNDIS_DF_CONNECTIONLESS);
	resp->Medium = cpu_to_le32(RNDIS_MEDIUM_802_3);
	resp->MaxPacketsPerTransfer = cpu_to_le32(params->max_pkt_per_xfer);
	resp->MaxTransferSize = cpu_to_le32(params->max_pkt_per_xfer *
		(params->dev->mtu
		+ sizeof(struct ethhdr)
		+ sizeof(struct rndis_packet_msg_type)
		+ 22));
	resp->PacketAlignmentFactor = cpu_to_le32(0);
	resp->AFListOffset = cpu_to_le32(0);
	resp->AFListSize = cpu_to_le32(0);

	params->resp_avail(params->v);
	return 0;
}

static int rndis_query_response(int configNr, rndis_query_msg_type *buf)
{
	rndis_query_cmplt_type *resp;
	rndis_resp_t *r;
	struct rndis_params *params = rndis_per_dev_params + configNr;

	/* pr_debug("%s: OID = %08X\n", __func__, cpu_to_le32(buf->OID)); */
	if (!params->dev)
		return -ENOTSUPP;

	/*
	 * we need more memory:
	 * gen_ndis_query_resp expects enough space for
	 * rndis_query_cmplt_type followed by data.
	 * oid_supported_list is the largest data reply
	 */
	r = rndis_add_response(configNr,
		sizeof(oid_supported_list) + sizeof(rndis_query_cmplt_type));

	if (!r){
		pr_err("rndis_query_response, rndis_add_response return NULL\n") ;
		return -ENOMEM;
	}
	resp = (rndis_query_cmplt_type *)r->buf;

	resp->MessageType = cpu_to_le32(RNDIS_MSG_QUERY_C);
	resp->RequestID = buf->RequestID; /* Still LE in msg buffer */

	if (gen_ndis_query_resp(configNr, le32_to_cpu(buf->OID),
			le32_to_cpu(buf->InformationBufferOffset)
					+ 8 + (u8 *)buf,
			le32_to_cpu(buf->InformationBufferLength),
			r)) {
		/* OID not supported */
		resp->Status = cpu_to_le32(RNDIS_STATUS_NOT_SUPPORTED);
		resp->MessageLength = cpu_to_le32(sizeof *resp);
		resp->InformationBufferLength = cpu_to_le32(0);
		resp->InformationBufferOffset = cpu_to_le32(0);
	} else
		resp->Status = cpu_to_le32(RNDIS_STATUS_SUCCESS);

	params->resp_avail(params->v);
	return 0;
}

static int rndis_set_response(int configNr, rndis_set_msg_type *buf)
{
	u32 BufLength, BufOffset;
	rndis_set_cmplt_type *resp;
	rndis_resp_t *r;
	struct rndis_params *params = rndis_per_dev_params + configNr;

	r = rndis_add_response(configNr, sizeof(rndis_set_cmplt_type));

	if (!r){
		pr_err("rndis_set_response, rndis_add_response return NULL\n") ;
		return -ENOMEM;
	}
	resp = (rndis_set_cmplt_type *)r->buf;

	BufLength = le32_to_cpu(buf->InformationBufferLength);
	BufOffset = le32_to_cpu(buf->InformationBufferOffset);

#ifdef	VERBOSE_DEBUG
	pr_debug("%s: Length: %d\n", __func__, BufLength);
	pr_debug("%s: Offset: %d\n", __func__, BufOffset);
	pr_debug("%s: InfoBuffer: ", __func__);

	for (i = 0; i < BufLength; i++) {
		pr_debug("%02x ", *(((u8 *) buf) + i + 8 + BufOffset));
	}

	pr_debug("\n");
#endif

	resp->MessageType = cpu_to_le32(RNDIS_MSG_SET_C);
	resp->MessageLength = cpu_to_le32(16);
	resp->RequestID = buf->RequestID; /* Still LE in msg buffer */
	if (gen_ndis_set_resp(configNr, le32_to_cpu(buf->OID),
			((u8 *)buf) + 8 + BufOffset, BufLength, r))
		resp->Status = cpu_to_le32(RNDIS_STATUS_NOT_SUPPORTED);
	else
		resp->Status = cpu_to_le32(RNDIS_STATUS_SUCCESS);

	params->resp_avail(params->v);
	return 0;
}

static int rndis_reset_response(int configNr, rndis_reset_msg_type *buf)
{
	rndis_reset_cmplt_type *resp;
	rndis_resp_t *r;
	struct rndis_params *params = rndis_per_dev_params + configNr;

	/* patch for RNDIS reset msg : clear the previous response ~*/
	u8 *buf_legacy;
	u32 length;

	pr_debug("%s: rndis reset !! clean the old resposes...\n", __func__);
	while ((buf_legacy = rndis_get_next_response(configNr, &length)))
		rndis_free_response(configNr, buf_legacy);

	rndis_test_reset_msg_cnt ++ ;
	/* patch for RNDIS reset msg */

	r = rndis_add_response(configNr, sizeof(rndis_reset_cmplt_type));
	if (!r)
		return -ENOMEM;
	resp = (rndis_reset_cmplt_type *)r->buf;

	resp->MessageType = cpu_to_le32(RNDIS_MSG_RESET_C);
	resp->MessageLength = cpu_to_le32(16);
	resp->Status = cpu_to_le32(RNDIS_STATUS_SUCCESS);
	/* resent information */
	resp->AddressingReset = cpu_to_le32(1);

	params->resp_avail(params->v);
	return 0;
}

static int rndis_keepalive_response(int configNr,
				    rndis_keepalive_msg_type *buf)
{
	rndis_keepalive_cmplt_type *resp;
	rndis_resp_t *r;
	struct rndis_params *params = rndis_per_dev_params + configNr;

	/* host "should" check only in RNDIS_DATA_INITIALIZED state */

	r = rndis_add_response(configNr, sizeof(rndis_keepalive_cmplt_type));

	if (!r){
		pr_err("rndis_keepalive_response, rndis_add_response return NULL\n") ;
		return -ENOMEM;
	}

	resp = (rndis_keepalive_cmplt_type *)r->buf;

	resp->MessageType = cpu_to_le32(RNDIS_MSG_KEEPALIVE_C);
	resp->MessageLength = cpu_to_le32(16);
	resp->RequestID = buf->RequestID; /* Still LE in msg buffer */
	resp->Status = cpu_to_le32(RNDIS_STATUS_SUCCESS);

	params->resp_avail(params->v);

	if (rndis_debug){
		pr_debug("rndis_keepalive_response, data: rx_in %lu, rx_out %lu, rx_nomem %lu, rx_error %lu, tx_in %lu, tx_out %lu, tx_busy %lu, tx_complete %lu, last_msg_id: 0x%x, last_resp_id: 0x%x, RNDIS reset cnt: 0x%lu, queue stopped? %d\n",
		rndis_test_rx_usb_in, rndis_test_rx_net_out, rndis_test_rx_nomem, rndis_test_rx_error, rndis_test_tx_net_in,
		rndis_test_tx_usb_out, rndis_test_tx_busy , rndis_test_tx_complete, rndis_test_last_msg_id, rndis_test_last_resp_id, rndis_test_reset_msg_cnt,
		netif_queue_stopped(params->dev)) ;
	}

	return 0;
}


/*
 * Device to Host Comunication
 */
static int rndis_indicate_status_msg(int configNr, u32 status)
{
	rndis_indicate_status_msg_type *resp;
	rndis_resp_t *r;
	struct rndis_params *params = rndis_per_dev_params + configNr;

	if (params->state == RNDIS_UNINITIALIZED)
		return -ENOTSUPP;

	r = rndis_add_response(configNr,
				sizeof(rndis_indicate_status_msg_type));
	if (!r)
		return -ENOMEM;
	resp = (rndis_indicate_status_msg_type *)r->buf;

	resp->MessageType = cpu_to_le32(RNDIS_MSG_INDICATE);
	resp->MessageLength = cpu_to_le32(20);
	resp->Status = cpu_to_le32(status);
	resp->StatusBufferLength = cpu_to_le32(0);
	resp->StatusBufferOffset = cpu_to_le32(0);

	params->resp_avail(params->v);
	return 0;
}

int rndis_signal_connect(int configNr)
{
	rndis_per_dev_params[configNr].media_state
			= RNDIS_MEDIA_STATE_CONNECTED;
	return rndis_indicate_status_msg(configNr,
					  RNDIS_STATUS_MEDIA_CONNECT);
}

int rndis_signal_disconnect(int configNr)
{
	rndis_per_dev_params[configNr].media_state
			= RNDIS_MEDIA_STATE_DISCONNECTED;
	return rndis_indicate_status_msg(configNr,
					  RNDIS_STATUS_MEDIA_DISCONNECT);
}

void rndis_uninit(int configNr)
{
	u8 *buf;
	u32 length;

	if (configNr >= RNDIS_MAX_CONFIGS)
		return;
	rndis_per_dev_params[configNr].state = RNDIS_UNINITIALIZED;

	/* drain the response queue */
	while ((buf = rndis_get_next_response(configNr, &length)))
		rndis_free_response(configNr, buf);
}

void rndis_set_host_mac(int configNr, const u8 *addr)
{
	rndis_per_dev_params[configNr].host_mac = addr;
}

/*
 * Message Parser
 */
int rndis_msg_parser(u8 configNr, u8 *buf)
{
	u32 MsgType, MsgLength, MsgID;
	__le32 *tmp;
	struct rndis_params *params;

	if (!buf)
		return -ENOMEM;

	tmp = (__le32 *)buf;
	MsgType   = get_unaligned_le32(tmp++);
	MsgLength = get_unaligned_le32(tmp++);
	MsgID = get_unaligned_le32(tmp++);
	if (rndis_debug){
		pr_debug("rndis_msg_parser, MsgType is %d,configNr is %d, RequestID is 0x%x\n", MsgType ,configNr, MsgID) ;
	}
	rndis_test_last_msg_id = MsgID ;

	if (configNr >= RNDIS_MAX_CONFIGS)
		return -ENOTSUPP;
	params = &rndis_per_dev_params[configNr];

	/* NOTE: RNDIS is *EXTREMELY* chatty ... Windows constantly polls for
	 * rx/tx statistics and link status, in addition to KEEPALIVE traffic
	 * and normal HC level polling to see if there's any IN traffic.
	 */

	/* For USB: responses may take up to 10 seconds */
	switch (MsgType) {
	case RNDIS_MSG_INIT:
		pr_debug("%s: RNDIS_MSG_INIT\n",
			__func__);
		params->state = RNDIS_INITIALIZED;
		return rndis_init_response(configNr,
					(rndis_init_msg_type *)buf);

	case RNDIS_MSG_HALT:
		pr_debug("%s: RNDIS_MSG_HALT\n",
			__func__);
		params->state = RNDIS_UNINITIALIZED;
		if (params->dev) {
			netif_carrier_off(params->dev);
			netif_stop_queue(params->dev);
		}
		return 0;

	case RNDIS_MSG_QUERY:
		return rndis_query_response(configNr,
					(rndis_query_msg_type *)buf);

	case RNDIS_MSG_SET:
		return rndis_set_response(configNr,
					(rndis_set_msg_type *)buf);

	case RNDIS_MSG_RESET:
		pr_debug("%s: RNDIS_MSG_RESET\n",
			__func__);
		return rndis_reset_response(configNr,
					(rndis_reset_msg_type *)buf);

	case RNDIS_MSG_KEEPALIVE:
		/* For USB: host does this every 5 seconds */
		if (rndis_debug > 1)
			pr_debug("%s: RNDIS_MSG_KEEPALIVE\n",
				__func__);
		return rndis_keepalive_response(configNr,
						 (rndis_keepalive_msg_type *)
						 buf);

	default:
		/* At least Windows XP emits some undefined RNDIS messages.
		 * In one case those messages seemed to relate to the host
		 * suspending itself.
		 */
		pr_warning("%s: unknown RNDIS message 0x%08X len %d\n",
			__func__, MsgType, MsgLength);

		{
			unsigned i;
			for (i = 0; i < MsgLength; i += 16) {
				pr_debug("%03d: "
					" %02x %02x %02x %02x"
					" %02x %02x %02x %02x"
					" %02x %02x %02x %02x"
					" %02x %02x %02x %02x"
					"\n",
					i,
					buf[i], buf [i+1],
						buf[i+2], buf[i+3],
					buf[i+4], buf [i+5],
						buf[i+6], buf[i+7],
					buf[i+8], buf [i+9],
						buf[i+10], buf[i+11],
					buf[i+12], buf [i+13],
						buf[i+14], buf[i+15]);
			}
		}
		//print_hex_dump_bytes(__func__, DUMP_PREFIX_OFFSET,
		//		     buf, MsgLength);
		break;
	}

	return -ENOTSUPP;
}

int rndis_register(void (*resp_avail)(void *v), void *v)
{
	u8 i;

	if (!resp_avail)
		return -EINVAL;

	for (i = 0; i < RNDIS_MAX_CONFIGS; i++) {
		if (!rndis_per_dev_params[i].used) {
			rndis_per_dev_params[i].used = 1;
			rndis_per_dev_params[i].resp_avail = resp_avail;
			rndis_per_dev_params[i].v = v;
			rndis_per_dev_params[i].max_pkt_per_xfer = 1;
			pr_debug("%s: configNr = %d\n", __func__, i);
			return i;
		}
	}
	pr_debug("failed\n");

	return -ENODEV;
}

void rndis_deregister(int configNr)
{
	pr_debug("%s:\n", __func__);

	if (configNr >= RNDIS_MAX_CONFIGS) return;
	rndis_per_dev_params[configNr].used = 0;
}

int rndis_set_param_dev(u8 configNr, struct net_device *dev, u16 *cdc_filter)
{
	pr_debug("%s:\n", __func__);
	if (!dev)
		return -EINVAL;
	if (configNr >= RNDIS_MAX_CONFIGS) return -1;

	rndis_per_dev_params[configNr].dev = dev;
	rndis_per_dev_params[configNr].filter = cdc_filter;

	/* reset aggregation stats for every set_alt */
	rndis_ul_max_xfer_size_rcvd = 0;
	rndis_ul_max_pkt_per_xfer_rcvd = 0;

	return 0;
}

int rndis_set_param_vendor(u8 configNr, u32 vendorID, const char *vendorDescr)
{
	pr_debug("%s:\n", __func__);
	if (!vendorDescr) return -1;
	if (configNr >= RNDIS_MAX_CONFIGS) return -1;

	rndis_per_dev_params[configNr].vendorID = vendorID;
	rndis_per_dev_params[configNr].vendorDescr = vendorDescr;

	return 0;
}

int rndis_set_param_medium(u8 configNr, u32 medium, u32 speed)
{
	pr_debug("%s: %u %u\n", __func__, medium, speed);
	if (configNr >= RNDIS_MAX_CONFIGS) return -1;

	rndis_per_dev_params[configNr].medium = medium;
	rndis_per_dev_params[configNr].speed = speed;

	return 0;
}

void rndis_set_max_pkt_xfer(u8 configNr, u8 max_pkt_per_xfer)
{
	pr_debug("%s:\n", __func__);

	rndis_per_dev_params[configNr].max_pkt_per_xfer = max_pkt_per_xfer;
}

void rndis_add_hdr(struct sk_buff *skb)
{
	struct rndis_packet_msg_type *header;

	if (!skb)
		return;
	header = (void *)skb_push(skb, sizeof(*header));
	memset(header, 0, sizeof *header);
	header->MessageType = cpu_to_le32(RNDIS_MSG_PACKET);
	header->MessageLength = cpu_to_le32(skb->len);
	header->DataOffset = cpu_to_le32(36);
	header->DataLength = cpu_to_le32(skb->len - sizeof(*header));
}

void rndis_free_response(int configNr, u8 *buf)
{
	rndis_resp_t *r;
	struct list_head *act, *tmp;

	list_for_each_safe(act, tmp,
			&(rndis_per_dev_params[configNr].resp_queue))
	{
		if (!act)
			continue;

		r = list_entry(act, rndis_resp_t, list);
		if (r && r->buf == buf) {
			list_del(&r->list);
			kfree(r);
		}
	}
}

u8 *rndis_get_next_response(int configNr, u32 *length)
{
	rndis_resp_t *r;
	struct list_head *act, *tmp;

	if (!length) return NULL;

	list_for_each_safe(act, tmp,
			&(rndis_per_dev_params[configNr].resp_queue))
	{
		r = list_entry(act, rndis_resp_t, list);
		if (!r->send) {
			r->send = 1;
			*length = r->length;
			return r->buf;
		}
	}

	return NULL;
}

static rndis_resp_t *rndis_add_response(int configNr, u32 length)
{
	rndis_resp_t *r;

	/* NOTE: this gets copied into ether.c USB_BUFSIZ bytes ... */
	r = kmalloc(sizeof(rndis_resp_t) + length, GFP_ATOMIC);
	if (!r) return NULL;

	r->buf = (u8 *)(r + 1);
	r->length = length;
	r->send = 0;

	list_add_tail(&r->list,
		&(rndis_per_dev_params[configNr].resp_queue));
	return r;
}

int rndis_rm_hdr(struct gether *port,
			struct sk_buff *skb,
			struct sk_buff_head *list)
{
	int num_pkts = 0;

	if (skb->len > rndis_ul_max_xfer_size_rcvd)
		rndis_ul_max_xfer_size_rcvd = skb->len;

	while (skb->len) {
		struct rndis_packet_msg_type *hdr;
		struct sk_buff		*skb2;
		u32		msg_len, data_offset, data_len;

		if (skb->len < sizeof *hdr) {
			pr_err("invalid rndis pkt: skblen:%u hdr_len:%zu",
					skb->len, sizeof *hdr);
			dev_kfree_skb_any(skb);
			return -EINVAL;
		}

		hdr = (void *)skb->data;
		msg_len = le32_to_cpu(hdr->MessageLength);
		data_offset = le32_to_cpu(hdr->DataOffset);
		data_len = le32_to_cpu(hdr->DataLength);

		if (skb->len < msg_len ||
			((data_offset + data_len + 8) > msg_len)) {
			pr_err("invalid rndis message: %d/%d/%d/%d, len:%d\n",
				le32_to_cpu(hdr->MessageType),
				msg_len, data_offset, data_len, skb->len);
			dev_kfree_skb_any(skb);
			return -EOVERFLOW;
		}

		if (le32_to_cpu(hdr->MessageType) != RNDIS_MSG_PACKET) {
			pr_err("invalid rndis message: %d/%d/%d/%d, len:%d\n",
				le32_to_cpu(hdr->MessageType),
				msg_len, data_offset, data_len, skb->len);
			dev_kfree_skb_any(skb);
			return -EINVAL;
		}

		num_pkts++;

		skb_pull(skb, data_offset + 8);

		if (data_len == skb->len ||
				data_len == (skb->len - 1)) {
			skb_trim(skb, data_len);
			break;
		}

		skb2 = skb_clone(skb, GFP_ATOMIC);
		if (!skb2) {
			pr_err("%s:skb clone failed\n", __func__);
			dev_kfree_skb_any(skb);
			return -ENOMEM;
		}

		skb_pull(skb, msg_len - sizeof *hdr);
		skb_trim(skb2, data_len);
		skb_queue_tail(list, skb2);
	}

	if (num_pkts > rndis_ul_max_pkt_per_xfer_rcvd)
		rndis_ul_max_pkt_per_xfer_rcvd = num_pkts;

	skb_queue_tail(list, skb);

	return 0;
}

#ifdef CONFIG_USB_GADGET_DEBUG_FILES

static int rndis_proc_show(struct seq_file *m, void *v)
{
	rndis_params *param = m->private;

	seq_printf(m,
			 "Config Nr. %d\n"
			 "used      : %s\n"
			 "state     : %s\n"
			 "medium    : 0x%08X\n"
			 "speed     : %d\n"
			 "cable     : %s\n"
			 "vendor ID : 0x%08X\n"
			 "vendor    : %s\n"
			 "ul-max-xfer-size:%d max-xfer-size-rcvd: %d\n"
			 "ul-max-pkts-per-xfer:%d max-pkts-per-xfer-rcvd:%d\n",
			 param->confignr, (param->used) ? "y" : "n",
			 ({ char *s = "?";
			 switch (param->state) {
			 case RNDIS_UNINITIALIZED:
				s = "RNDIS_UNINITIALIZED"; break;
			 case RNDIS_INITIALIZED:
				s = "RNDIS_INITIALIZED"; break;
			 case RNDIS_DATA_INITIALIZED:
				s = "RNDIS_DATA_INITIALIZED"; break;
			}; s; }),
			 param->medium,
			 (param->media_state) ? 0 : param->speed*100,
			 (param->media_state) ? "disconnected" : "connected",
			 param->vendorID, param->vendorDescr,
			 param->max_pkt_per_xfer *
				 (param->dev->mtu + sizeof(struct ethhdr) +
				  sizeof(struct rndis_packet_msg_type) + 22),
			 rndis_ul_max_xfer_size_rcvd,
			 param->max_pkt_per_xfer,
			 rndis_ul_max_pkt_per_xfer_rcvd);
	return 0;
}

static ssize_t rndis_proc_write(struct file *file, const char __user *buffer,
				size_t count, loff_t *ppos)
{
	rndis_params *p = PDE_DATA(file_inode(file));
	u32 speed = 0;
	int i, fl_speed = 0;

	for (i = 0; i < count; i++) {
		char c;
		if (get_user(c, buffer))
			return -EFAULT;
		switch (c) {
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
			fl_speed = 1;
			speed = speed * 10 + c - '0';
			break;
		case 'C':
		case 'c':
			rndis_signal_connect(p->confignr);
			break;
		case 'D':
		case 'd':
			rndis_signal_disconnect(p->confignr);
			break;
		default:
			if (fl_speed) p->speed = speed;
			else pr_debug("%c is not valid\n", c);
			break;
		}

		buffer++;
	}

	return count;
}

static int rndis_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, rndis_proc_show, PDE_DATA(inode));
}

static const struct file_operations rndis_proc_fops = {
	.owner		= THIS_MODULE,
	.open		= rndis_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
	.write		= rndis_proc_write,
};

#define	NAME_TEMPLATE "driver/rndis-%03d"

static struct proc_dir_entry *rndis_connect_state [RNDIS_MAX_CONFIGS];

#endif /* CONFIG_USB_GADGET_DEBUG_FILES */

static int rndis_test_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "[RNDIS] rx_in %lu, rx_out %lu, rx_nomem %lu, rx_error %lu, tx_in %lu, tx_out %lu, tx_busy %lu, tx_complete %lu, last_msg_id: 0x%x, last_resp_id: 0x%x, RNDIS reset cnt: %lu, queue stopped? %d\n",
	rndis_test_rx_usb_in, rndis_test_rx_net_out, rndis_test_rx_nomem, rndis_test_rx_error, rndis_test_tx_net_in,
	rndis_test_tx_usb_out, rndis_test_tx_busy , rndis_test_tx_complete, rndis_test_last_msg_id, rndis_test_last_resp_id, rndis_test_reset_msg_cnt,
	netif_queue_stopped(rndis_per_dev_params[0].dev)) ;

	return 0;
}

static int rndis_test_proc_open(struct inode *inode, struct file *file)
{
	//return single_open(file, rndis_test_proc_show, inode->i_private);
	return single_open(file, rndis_test_proc_show, inode->i_private);
}

static ssize_t rndis_test_proc_write(struct file *file, const char __user *buf, size_t length, loff_t *ppos)
{
	char msg[32];

	if (length >= sizeof(msg)) {
		return -EINVAL;
	}
	if (copy_from_user(msg, buf, length))
		return -EFAULT;

	msg[length] = 0 ;

	return length;
}

static bool rndis_initialized;

static const struct file_operations rndis_test_proc_fops = {
	.owner		= THIS_MODULE,
	.open		= rndis_test_proc_open,
	.write = rndis_test_proc_write,
	.read		= seq_read,
	.llseek		= seq_lseek,

};


static struct proc_dir_entry *rndis_test_proc ;

int rndis_init(void)
{
	u8 i;

	if (rndis_initialized)
		return 0;

	rndis_test_proc = proc_create("rndis_test_proc", 0444, NULL,
					&rndis_test_proc_fops);

	for (i = 0; i < RNDIS_MAX_CONFIGS; i++) {
#ifdef	CONFIG_USB_GADGET_DEBUG_FILES
		char name [20];

		sprintf(name, NAME_TEMPLATE, i);
		rndis_connect_state[i] = proc_create_data(name, 0660, NULL,
					&rndis_proc_fops,
					(void *)(rndis_per_dev_params + i));
		if (!rndis_connect_state[i]) {
			pr_debug("%s: remove entries", __func__);
			while (i) {
				sprintf(name, NAME_TEMPLATE, --i);
				remove_proc_entry(name, NULL);
			}
			pr_debug("\n");
			return -EIO;
		}
#endif
		rndis_per_dev_params[i].confignr = i;
		rndis_per_dev_params[i].used = 0;
		rndis_per_dev_params[i].state = RNDIS_UNINITIALIZED;
		rndis_per_dev_params[i].media_state
				= RNDIS_MEDIA_STATE_DISCONNECTED;
		INIT_LIST_HEAD(&(rndis_per_dev_params[i].resp_queue));
	}

	rndis_initialized = true;
	return 0;
}

void rndis_exit(void)
{
#ifdef CONFIG_USB_GADGET_DEBUG_FILES
	u8 i;
	char name[20];
#endif

	if (!rndis_initialized)
		return;
	rndis_initialized = false;

	remove_proc_entry("rndis_test_proc", NULL);

#ifdef CONFIG_USB_GADGET_DEBUG_FILES
	for (i = 0; i < RNDIS_MAX_CONFIGS; i++) {
		sprintf(name, NAME_TEMPLATE, i);
		remove_proc_entry(name, NULL);
	}
#endif
}
