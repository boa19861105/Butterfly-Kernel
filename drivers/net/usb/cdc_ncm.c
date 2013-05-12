/*
 * cdc_ncm.c
 *
 * Copyright (C) ST-Ericsson 2010-2012
 * Contact: Alexey Orishko <alexey.orishko@stericsson.com>
 * Original author: Hans Petter Selasky <hans.petter.selasky@stericsson.com>
 *
 * USB Host Driver for Network Control Model (NCM)
 * http://www.usb.org/developers/devclass_docs/NCM10.zip
 *
 * The NCM encoding, decoding and initialization logic
 * derives from FreeBSD 8.x. if_cdce.c and if_cdcereg.h
 *
 * This software is available to you under a choice of one of two
 * licenses. You may choose this file to be licensed under the terms
 * of the GNU General Public License (GPL) Version 2 or the 2-clause
 * BSD license listed below:
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

#include <linux/module.h>
#include <linux/init.h>
#include <linux/netdevice.h>
#include <linux/ctype.h>
#include <linux/ethtool.h>
#include <linux/workqueue.h>
#include <linux/mii.h>
#include <linux/crc32.h>
#include <linux/usb.h>
#include <linux/hrtimer.h>
#include <linux/atomic.h>
#include <linux/usb/usbnet.h>
#include <linux/usb/cdc.h>

#define	DRIVER_VERSION				"14-Mar-2012"

#define USB_CDC_NCM_NDP16_LENGTH_MIN		0x10

#define	CDC_NCM_NTB_MAX_SIZE_TX			32768	
#define	CDC_NCM_NTB_MAX_SIZE_RX			32768	

#define	CDC_NCM_MIN_DATAGRAM_SIZE		1514	

#define	CDC_NCM_MIN_TX_PKT			512	

#define	CDC_NCM_MAX_DATAGRAM_SIZE		8192	

#define	CDC_NCM_DPT_DATAGRAMS_MAX		40

#define	CDC_NCM_RESTART_TIMER_DATAGRAM_CNT	3
#define	CDC_NCM_TIMER_PENDING_CNT		2
#define CDC_NCM_TIMER_INTERVAL			(400UL * NSEC_PER_USEC)

#define	CDC_NCM_MIN_HDR_SIZE \
	(sizeof(struct usb_cdc_ncm_nth16) + sizeof(struct usb_cdc_ncm_ndp16) + \
	(CDC_NCM_DPT_DATAGRAMS_MAX + 1) * sizeof(struct usb_cdc_ncm_dpe16))

struct cdc_ncm_data {
	struct usb_cdc_ncm_nth16 nth16;
	struct usb_cdc_ncm_ndp16 ndp16;
	struct usb_cdc_ncm_dpe16 dpe16[CDC_NCM_DPT_DATAGRAMS_MAX + 1];
};

struct cdc_ncm_ctx {
	struct cdc_ncm_data tx_ncm;
	struct usb_cdc_ncm_ntb_parameters ncm_parm;
	struct hrtimer tx_timer;
	struct tasklet_struct bh;

	const struct usb_cdc_ncm_desc *func_desc;
	const struct usb_cdc_header_desc *header_desc;
	const struct usb_cdc_union_desc *union_desc;
	const struct usb_cdc_ether_desc *ether_desc;

	struct net_device *netdev;
	struct usb_device *udev;
	struct usb_host_endpoint *in_ep;
	struct usb_host_endpoint *out_ep;
	struct usb_host_endpoint *status_ep;
	struct usb_interface *intf;
	struct usb_interface *control;
	struct usb_interface *data;

	struct sk_buff *tx_curr_skb;
	struct sk_buff *tx_rem_skb;

	spinlock_t mtx;
	atomic_t stop;

	u32 tx_timer_pending;
	u32 tx_curr_offset;
	u32 tx_curr_last_offset;
	u32 tx_curr_frame_num;
	u32 rx_speed;
	u32 tx_speed;
	u32 rx_max;
	u32 tx_max;
	u32 max_datagram_size;
	u16 tx_max_datagrams;
	u16 tx_remainder;
	u16 tx_modulus;
	u16 tx_ndp_modulus;
	u16 tx_seq;
	u16 rx_seq;
	u16 connected;
};

static void cdc_ncm_txpath_bh(unsigned long param);
static void cdc_ncm_tx_timeout_start(struct cdc_ncm_ctx *ctx);
static enum hrtimer_restart cdc_ncm_tx_timer_cb(struct hrtimer *hr_timer);
static const struct driver_info cdc_ncm_info;
static struct usb_driver cdc_ncm_driver;
static const struct ethtool_ops cdc_ncm_ethtool_ops;

static const struct usb_device_id cdc_devs[] = {
	{ USB_INTERFACE_INFO(USB_CLASS_COMM,
		USB_CDC_SUBCLASS_NCM, USB_CDC_PROTO_NONE),
		.driver_info = (unsigned long)&cdc_ncm_info,
	},
	{
	},
};

MODULE_DEVICE_TABLE(usb, cdc_devs);

static void
cdc_ncm_get_drvinfo(struct net_device *net, struct ethtool_drvinfo *info)
{
	struct usbnet *dev = netdev_priv(net);

	strncpy(info->driver, dev->driver_name, sizeof(info->driver));
	strncpy(info->version, DRIVER_VERSION, sizeof(info->version));
	strncpy(info->fw_version, dev->driver_info->description,
		sizeof(info->fw_version));
	usb_make_path(dev->udev, info->bus_info, sizeof(info->bus_info));
}

static u8 cdc_ncm_setup(struct cdc_ncm_ctx *ctx)
{
	u32 val;
	u8 flags;
	u8 iface_no;
	int err;
	u16 ntb_fmt_supported;

	iface_no = ctx->control->cur_altsetting->desc.bInterfaceNumber;

	err = usb_control_msg(ctx->udev,
				usb_rcvctrlpipe(ctx->udev, 0),
				USB_CDC_GET_NTB_PARAMETERS,
				USB_TYPE_CLASS | USB_DIR_IN
				 | USB_RECIP_INTERFACE,
				0, iface_no, &ctx->ncm_parm,
				sizeof(ctx->ncm_parm), 10000);
	if (err < 0) {
		pr_debug("failed GET_NTB_PARAMETERS\n");
		return 1;
	}

	
	ctx->rx_max = le32_to_cpu(ctx->ncm_parm.dwNtbInMaxSize);
	ctx->tx_max = le32_to_cpu(ctx->ncm_parm.dwNtbOutMaxSize);
	ctx->tx_remainder = le16_to_cpu(ctx->ncm_parm.wNdpOutPayloadRemainder);
	ctx->tx_modulus = le16_to_cpu(ctx->ncm_parm.wNdpOutDivisor);
	ctx->tx_ndp_modulus = le16_to_cpu(ctx->ncm_parm.wNdpOutAlignment);
	
	ctx->tx_max_datagrams = le16_to_cpu(ctx->ncm_parm.wNtbOutMaxDatagrams);
	ntb_fmt_supported = le16_to_cpu(ctx->ncm_parm.bmNtbFormatsSupported);

	if (ctx->func_desc != NULL)
		flags = ctx->func_desc->bmNetworkCapabilities;
	else
		flags = 0;

	pr_debug("dwNtbInMaxSize=%u dwNtbOutMaxSize=%u "
		 "wNdpOutPayloadRemainder=%u wNdpOutDivisor=%u "
		 "wNdpOutAlignment=%u wNtbOutMaxDatagrams=%u flags=0x%x\n",
		 ctx->rx_max, ctx->tx_max, ctx->tx_remainder, ctx->tx_modulus,
		 ctx->tx_ndp_modulus, ctx->tx_max_datagrams, flags);

	
	if ((ctx->tx_max_datagrams == 0) ||
			(ctx->tx_max_datagrams > CDC_NCM_DPT_DATAGRAMS_MAX))
		ctx->tx_max_datagrams = CDC_NCM_DPT_DATAGRAMS_MAX;

	
	if (ctx->rx_max < USB_CDC_NCM_NTB_MIN_IN_SIZE) {
		pr_debug("Using min receive length=%d\n",
						USB_CDC_NCM_NTB_MIN_IN_SIZE);
		ctx->rx_max = USB_CDC_NCM_NTB_MIN_IN_SIZE;
	}

	if (ctx->rx_max > CDC_NCM_NTB_MAX_SIZE_RX) {
		pr_debug("Using default maximum receive length=%d\n",
						CDC_NCM_NTB_MAX_SIZE_RX);
		ctx->rx_max = CDC_NCM_NTB_MAX_SIZE_RX;
	}

	
	if (ctx->rx_max != le32_to_cpu(ctx->ncm_parm.dwNtbInMaxSize)) {

		if (flags & USB_CDC_NCM_NCAP_NTB_INPUT_SIZE) {
			struct usb_cdc_ncm_ndp_input_size *ndp_in_sz;

			ndp_in_sz = kzalloc(sizeof(*ndp_in_sz), GFP_KERNEL);
			if (!ndp_in_sz) {
				err = -ENOMEM;
				goto size_err;
			}

			err = usb_control_msg(ctx->udev,
					usb_sndctrlpipe(ctx->udev, 0),
					USB_CDC_SET_NTB_INPUT_SIZE,
					USB_TYPE_CLASS | USB_DIR_OUT
					 | USB_RECIP_INTERFACE,
					0, iface_no, ndp_in_sz, 8, 1000);
			kfree(ndp_in_sz);
		} else {
			__le32 *dwNtbInMaxSize;
			dwNtbInMaxSize = kzalloc(sizeof(*dwNtbInMaxSize),
					GFP_KERNEL);
			if (!dwNtbInMaxSize) {
				err = -ENOMEM;
				goto size_err;
			}
			*dwNtbInMaxSize = cpu_to_le32(ctx->rx_max);

			err = usb_control_msg(ctx->udev,
					usb_sndctrlpipe(ctx->udev, 0),
					USB_CDC_SET_NTB_INPUT_SIZE,
					USB_TYPE_CLASS | USB_DIR_OUT
					 | USB_RECIP_INTERFACE,
					0, iface_no, dwNtbInMaxSize, 4, 1000);
			kfree(dwNtbInMaxSize);
		}
size_err:
		if (err < 0)
			pr_debug("Setting NTB Input Size failed\n");
	}

	
	if ((ctx->tx_max <
	    (CDC_NCM_MIN_HDR_SIZE + CDC_NCM_MIN_DATAGRAM_SIZE)) ||
	    (ctx->tx_max > CDC_NCM_NTB_MAX_SIZE_TX)) {
		pr_debug("Using default maximum transmit length=%d\n",
						CDC_NCM_NTB_MAX_SIZE_TX);
		ctx->tx_max = CDC_NCM_NTB_MAX_SIZE_TX;
	}

	val = ctx->tx_ndp_modulus;

	if ((val < USB_CDC_NCM_NDP_ALIGN_MIN_SIZE) ||
	    (val != ((-val) & val)) || (val >= ctx->tx_max)) {
		pr_debug("Using default alignment: 4 bytes\n");
		ctx->tx_ndp_modulus = USB_CDC_NCM_NDP_ALIGN_MIN_SIZE;
	}

	val = ctx->tx_modulus;

	if ((val < USB_CDC_NCM_NDP_ALIGN_MIN_SIZE) ||
	    (val != ((-val) & val)) || (val >= ctx->tx_max)) {
		pr_debug("Using default transmit modulus: 4 bytes\n");
		ctx->tx_modulus = USB_CDC_NCM_NDP_ALIGN_MIN_SIZE;
	}

	
	if (ctx->tx_remainder >= ctx->tx_modulus) {
		pr_debug("Using default transmit remainder: 0 bytes\n");
		ctx->tx_remainder = 0;
	}

	
	ctx->tx_remainder = ((ctx->tx_remainder - ETH_HLEN) &
						(ctx->tx_modulus - 1));

	

	
	if (flags & USB_CDC_NCM_NCAP_CRC_MODE) {
		err = usb_control_msg(ctx->udev, usb_sndctrlpipe(ctx->udev, 0),
				USB_CDC_SET_CRC_MODE,
				USB_TYPE_CLASS | USB_DIR_OUT
				 | USB_RECIP_INTERFACE,
				USB_CDC_NCM_CRC_NOT_APPENDED,
				iface_no, NULL, 0, 1000);
		if (err < 0)
			pr_debug("Setting CRC mode off failed\n");
	}

	
	if (ntb_fmt_supported & USB_CDC_NCM_NTH32_SIGN) {
		err = usb_control_msg(ctx->udev, usb_sndctrlpipe(ctx->udev, 0),
				USB_CDC_SET_NTB_FORMAT, USB_TYPE_CLASS
				 | USB_DIR_OUT | USB_RECIP_INTERFACE,
				USB_CDC_NCM_NTB16_FORMAT,
				iface_no, NULL, 0, 1000);
		if (err < 0)
			pr_debug("Setting NTB format to 16-bit failed\n");
	}

	ctx->max_datagram_size = CDC_NCM_MIN_DATAGRAM_SIZE;

	
	if (flags & USB_CDC_NCM_NCAP_MAX_DATAGRAM_SIZE) {
		__le16 *max_datagram_size;
		u16 eth_max_sz = le16_to_cpu(ctx->ether_desc->wMaxSegmentSize);

		max_datagram_size = kzalloc(sizeof(*max_datagram_size),
				GFP_KERNEL);
		if (!max_datagram_size) {
			err = -ENOMEM;
			goto max_dgram_err;
		}

		err = usb_control_msg(ctx->udev, usb_rcvctrlpipe(ctx->udev, 0),
				USB_CDC_GET_MAX_DATAGRAM_SIZE,
				USB_TYPE_CLASS | USB_DIR_IN
				 | USB_RECIP_INTERFACE,
				0, iface_no, max_datagram_size,
				2, 1000);
		if (err < 0) {
			pr_debug("GET_MAX_DATAGRAM_SIZE failed, use size=%u\n",
						CDC_NCM_MIN_DATAGRAM_SIZE);
		} else {
			ctx->max_datagram_size =
				le16_to_cpu(*max_datagram_size);
			
			if (ctx->max_datagram_size > eth_max_sz)
					ctx->max_datagram_size = eth_max_sz;

			if (ctx->max_datagram_size > CDC_NCM_MAX_DATAGRAM_SIZE)
				ctx->max_datagram_size =
						CDC_NCM_MAX_DATAGRAM_SIZE;

			if (ctx->max_datagram_size < CDC_NCM_MIN_DATAGRAM_SIZE)
				ctx->max_datagram_size =
					CDC_NCM_MIN_DATAGRAM_SIZE;

			
			if (ctx->max_datagram_size !=
					le16_to_cpu(*max_datagram_size)) {
				err = usb_control_msg(ctx->udev,
						usb_sndctrlpipe(ctx->udev, 0),
						USB_CDC_SET_MAX_DATAGRAM_SIZE,
						USB_TYPE_CLASS | USB_DIR_OUT
						 | USB_RECIP_INTERFACE,
						0,
						iface_no, max_datagram_size,
						2, 1000);
				if (err < 0)
					pr_debug("SET_MAX_DGRAM_SIZE failed\n");
			}
		}
		kfree(max_datagram_size);
	}

max_dgram_err:
	if (ctx->netdev->mtu != (ctx->max_datagram_size - ETH_HLEN))
		ctx->netdev->mtu = ctx->max_datagram_size - ETH_HLEN;

	return 0;
}

static void
cdc_ncm_find_endpoints(struct cdc_ncm_ctx *ctx, struct usb_interface *intf)
{
	struct usb_host_endpoint *e;
	u8 ep;

	for (ep = 0; ep < intf->cur_altsetting->desc.bNumEndpoints; ep++) {

		e = intf->cur_altsetting->endpoint + ep;
		switch (e->desc.bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) {
		case USB_ENDPOINT_XFER_INT:
			if (usb_endpoint_dir_in(&e->desc)) {
				if (ctx->status_ep == NULL)
					ctx->status_ep = e;
			}
			break;

		case USB_ENDPOINT_XFER_BULK:
			if (usb_endpoint_dir_in(&e->desc)) {
				if (ctx->in_ep == NULL)
					ctx->in_ep = e;
			} else {
				if (ctx->out_ep == NULL)
					ctx->out_ep = e;
			}
			break;

		default:
			break;
		}
	}
}

static void cdc_ncm_free(struct cdc_ncm_ctx *ctx)
{
	if (ctx == NULL)
		return;

	if (ctx->tx_rem_skb != NULL) {
		dev_kfree_skb_any(ctx->tx_rem_skb);
		ctx->tx_rem_skb = NULL;
	}

	if (ctx->tx_curr_skb != NULL) {
		dev_kfree_skb_any(ctx->tx_curr_skb);
		ctx->tx_curr_skb = NULL;
	}

	kfree(ctx);
}

static int cdc_ncm_bind(struct usbnet *dev, struct usb_interface *intf)
{
	struct cdc_ncm_ctx *ctx;
	struct usb_driver *driver;
	u8 *buf;
	int len;
	int temp;
	u8 iface_no;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (ctx == NULL)
		return -ENODEV;

	hrtimer_init(&ctx->tx_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	ctx->tx_timer.function = &cdc_ncm_tx_timer_cb;
	ctx->bh.data = (unsigned long)ctx;
	ctx->bh.func = cdc_ncm_txpath_bh;
	atomic_set(&ctx->stop, 0);
	spin_lock_init(&ctx->mtx);
	ctx->netdev = dev->net;

	
	dev->data[0] = (unsigned long)ctx;

	
	driver = driver_of(intf);
	buf = intf->cur_altsetting->extra;
	len = intf->cur_altsetting->extralen;

	ctx->udev = dev->udev;
	ctx->intf = intf;

	
	while ((len > 0) && (buf[0] > 2) && (buf[0] <= len)) {

		if (buf[1] != USB_DT_CS_INTERFACE)
			goto advance;

		switch (buf[2]) {
		case USB_CDC_UNION_TYPE:
			if (buf[0] < sizeof(*(ctx->union_desc)))
				break;

			ctx->union_desc =
					(const struct usb_cdc_union_desc *)buf;

			ctx->control = usb_ifnum_to_if(dev->udev,
					ctx->union_desc->bMasterInterface0);
			ctx->data = usb_ifnum_to_if(dev->udev,
					ctx->union_desc->bSlaveInterface0);
			break;

		case USB_CDC_ETHERNET_TYPE:
			if (buf[0] < sizeof(*(ctx->ether_desc)))
				break;

			ctx->ether_desc =
					(const struct usb_cdc_ether_desc *)buf;
			dev->hard_mtu =
				le16_to_cpu(ctx->ether_desc->wMaxSegmentSize);

			if (dev->hard_mtu < CDC_NCM_MIN_DATAGRAM_SIZE)
				dev->hard_mtu =	CDC_NCM_MIN_DATAGRAM_SIZE;
			else if (dev->hard_mtu > CDC_NCM_MAX_DATAGRAM_SIZE)
				dev->hard_mtu =	CDC_NCM_MAX_DATAGRAM_SIZE;
			break;

		case USB_CDC_NCM_TYPE:
			if (buf[0] < sizeof(*(ctx->func_desc)))
				break;

			ctx->func_desc = (const struct usb_cdc_ncm_desc *)buf;
			break;

		default:
			break;
		}
advance:
		
		temp = buf[0];
		buf += temp;
		len -= temp;
	}

	
	if ((ctx->control == NULL) || (ctx->data == NULL) ||
	    (ctx->ether_desc == NULL) || (ctx->control != intf))
		goto error;

	
	temp = usb_driver_claim_interface(driver, ctx->data, dev);
	if (temp)
		goto error;

	iface_no = ctx->data->cur_altsetting->desc.bInterfaceNumber;

	
	temp = usb_set_interface(dev->udev, iface_no, 0);
	if (temp)
		goto error2;

	
	if (cdc_ncm_setup(ctx))
		goto error2;

	
	temp = usb_set_interface(dev->udev, iface_no, 1);
	if (temp)
		goto error2;

	cdc_ncm_find_endpoints(ctx, ctx->data);
	cdc_ncm_find_endpoints(ctx, ctx->control);

	if ((ctx->in_ep == NULL) || (ctx->out_ep == NULL) ||
	    (ctx->status_ep == NULL))
		goto error2;

	dev->net->ethtool_ops = &cdc_ncm_ethtool_ops;

	usb_set_intfdata(ctx->data, dev);
	usb_set_intfdata(ctx->control, dev);
	usb_set_intfdata(ctx->intf, dev);

	temp = usbnet_get_ethernet_addr(dev, ctx->ether_desc->iMACAddress);
	if (temp)
		goto error2;

	dev_info(&dev->udev->dev, "MAC-Address: %pM\n", dev->net->dev_addr);

	dev->in = usb_rcvbulkpipe(dev->udev,
		ctx->in_ep->desc.bEndpointAddress & USB_ENDPOINT_NUMBER_MASK);
	dev->out = usb_sndbulkpipe(dev->udev,
		ctx->out_ep->desc.bEndpointAddress & USB_ENDPOINT_NUMBER_MASK);
	dev->status = ctx->status_ep;
	dev->rx_urb_size = ctx->rx_max;

	netif_carrier_off(dev->net);
	ctx->tx_speed = ctx->rx_speed = 0;
	return 0;

error2:
	usb_set_intfdata(ctx->control, NULL);
	usb_set_intfdata(ctx->data, NULL);
	usb_driver_release_interface(driver, ctx->data);
error:
	cdc_ncm_free((struct cdc_ncm_ctx *)dev->data[0]);
	dev->data[0] = 0;
	dev_info(&dev->udev->dev, "bind() failure\n");
	return -ENODEV;
}

static void cdc_ncm_unbind(struct usbnet *dev, struct usb_interface *intf)
{
	struct cdc_ncm_ctx *ctx = (struct cdc_ncm_ctx *)dev->data[0];
	struct usb_driver *driver = driver_of(intf);

	if (ctx == NULL)
		return;		

	atomic_set(&ctx->stop, 1);

	if (hrtimer_active(&ctx->tx_timer))
		hrtimer_cancel(&ctx->tx_timer);

	tasklet_kill(&ctx->bh);

	
	if (intf == ctx->control && ctx->data) {
		usb_set_intfdata(ctx->data, NULL);
		usb_driver_release_interface(driver, ctx->data);
		ctx->data = NULL;

	} else if (intf == ctx->data && ctx->control) {
		usb_set_intfdata(ctx->control, NULL);
		usb_driver_release_interface(driver, ctx->control);
		ctx->control = NULL;
	}

	usb_set_intfdata(ctx->intf, NULL);
	cdc_ncm_free(ctx);
}

static void cdc_ncm_zero_fill(u8 *ptr, u32 first, u32 end, u32 max)
{
	if (first >= max)
		return;
	if (first >= end)
		return;
	if (end > max)
		end = max;
	memset(ptr + first, 0, end - first);
}

static struct sk_buff *
cdc_ncm_fill_tx_frame(struct cdc_ncm_ctx *ctx, struct sk_buff *skb)
{
	struct sk_buff *skb_out;
	u32 rem;
	u32 offset;
	u32 last_offset;
	u16 n = 0, index;
	u8 ready2send = 0;

	
	if (skb != NULL)
		swap(skb, ctx->tx_rem_skb);
	else
		ready2send = 1;


	
	if (ctx->tx_curr_skb != NULL) {
		
		skb_out = ctx->tx_curr_skb;
		offset = ctx->tx_curr_offset;
		last_offset = ctx->tx_curr_last_offset;
		n = ctx->tx_curr_frame_num;

	} else {
		
		skb_out = alloc_skb((ctx->tx_max + 1), GFP_ATOMIC);
		if (skb_out == NULL) {
			if (skb != NULL) {
				dev_kfree_skb_any(skb);
				ctx->netdev->stats.tx_dropped++;
			}
			goto exit_no_skb;
		}

		
		offset = ALIGN(sizeof(struct usb_cdc_ncm_nth16),
					ctx->tx_ndp_modulus) +
					sizeof(struct usb_cdc_ncm_ndp16) +
					(ctx->tx_max_datagrams + 1) *
					sizeof(struct usb_cdc_ncm_dpe16);

		
		last_offset = offset;
		
		offset = ALIGN(offset, ctx->tx_modulus) + ctx->tx_remainder;
		
		cdc_ncm_zero_fill(skb_out->data, 0, offset, offset);
		n = 0;
		ctx->tx_curr_frame_num = 0;
	}

	for (; n < ctx->tx_max_datagrams; n++) {
		
		if (offset >= ctx->tx_max) {
			ready2send = 1;
			break;
		}
		
		rem = ctx->tx_max - offset;

		if (skb == NULL) {
			skb = ctx->tx_rem_skb;
			ctx->tx_rem_skb = NULL;

			
			if (skb == NULL)
				break;
		}

		if (skb->len > rem) {
			if (n == 0) {
				
				dev_kfree_skb_any(skb);
				skb = NULL;
				ctx->netdev->stats.tx_dropped++;
			} else {
				
				if (ctx->tx_rem_skb != NULL) {
					dev_kfree_skb_any(ctx->tx_rem_skb);
					ctx->netdev->stats.tx_dropped++;
				}
				ctx->tx_rem_skb = skb;
				skb = NULL;
				ready2send = 1;
			}
			break;
		}

		memcpy(((u8 *)skb_out->data) + offset, skb->data, skb->len);

		ctx->tx_ncm.dpe16[n].wDatagramLength = cpu_to_le16(skb->len);
		ctx->tx_ncm.dpe16[n].wDatagramIndex = cpu_to_le16(offset);

		
		offset += skb->len;

		
		last_offset = offset;

		
		offset = ALIGN(offset, ctx->tx_modulus) + ctx->tx_remainder;

		
		cdc_ncm_zero_fill(skb_out->data, last_offset, offset,
								ctx->tx_max);
		dev_kfree_skb_any(skb);
		skb = NULL;
	}

	
	if (skb != NULL) {
		dev_kfree_skb_any(skb);
		skb = NULL;
		ctx->netdev->stats.tx_dropped++;
	}

	ctx->tx_curr_frame_num = n;

	if (n == 0) {
		
		
		ctx->tx_curr_skb = skb_out;
		ctx->tx_curr_offset = offset;
		ctx->tx_curr_last_offset = last_offset;
		goto exit_no_skb;

	} else if ((n < ctx->tx_max_datagrams) && (ready2send == 0)) {
		
		
		ctx->tx_curr_skb = skb_out;
		ctx->tx_curr_offset = offset;
		ctx->tx_curr_last_offset = last_offset;
		
		if (n < CDC_NCM_RESTART_TIMER_DATAGRAM_CNT)
			ctx->tx_timer_pending = CDC_NCM_TIMER_PENDING_CNT;
		goto exit_no_skb;

	} else {
		
		
	}

	
	if (last_offset > ctx->tx_max)
		last_offset = ctx->tx_max;

	
	offset = last_offset;

	if (offset > CDC_NCM_MIN_TX_PKT)
		offset = ctx->tx_max;

	
	cdc_ncm_zero_fill(skb_out->data, last_offset, offset, ctx->tx_max);

	
	last_offset = offset;

	if (((last_offset < ctx->tx_max) && ((last_offset %
			le16_to_cpu(ctx->out_ep->desc.wMaxPacketSize)) == 0)) ||
	    (((last_offset == ctx->tx_max) && ((ctx->tx_max %
		le16_to_cpu(ctx->out_ep->desc.wMaxPacketSize)) == 0)) &&
		(ctx->tx_max < le32_to_cpu(ctx->ncm_parm.dwNtbOutMaxSize)))) {
		
		*(((u8 *)skb_out->data) + last_offset) = 0;
		last_offset++;
	}

	
	for (; n <= CDC_NCM_DPT_DATAGRAMS_MAX; n++) {
		ctx->tx_ncm.dpe16[n].wDatagramLength = 0;
		ctx->tx_ncm.dpe16[n].wDatagramIndex = 0;
	}

	
	ctx->tx_ncm.nth16.dwSignature = cpu_to_le32(USB_CDC_NCM_NTH16_SIGN);
	ctx->tx_ncm.nth16.wHeaderLength =
					cpu_to_le16(sizeof(ctx->tx_ncm.nth16));
	ctx->tx_ncm.nth16.wSequence = cpu_to_le16(ctx->tx_seq);
	ctx->tx_ncm.nth16.wBlockLength = cpu_to_le16(last_offset);
	index = ALIGN(sizeof(struct usb_cdc_ncm_nth16), ctx->tx_ndp_modulus);
	ctx->tx_ncm.nth16.wNdpIndex = cpu_to_le16(index);

	memcpy(skb_out->data, &(ctx->tx_ncm.nth16), sizeof(ctx->tx_ncm.nth16));
	ctx->tx_seq++;

	
	ctx->tx_ncm.ndp16.dwSignature =
				cpu_to_le32(USB_CDC_NCM_NDP16_NOCRC_SIGN);
	rem = sizeof(ctx->tx_ncm.ndp16) + ((ctx->tx_curr_frame_num + 1) *
					sizeof(struct usb_cdc_ncm_dpe16));
	ctx->tx_ncm.ndp16.wLength = cpu_to_le16(rem);
	ctx->tx_ncm.ndp16.wNextNdpIndex = 0; 

	memcpy(((u8 *)skb_out->data) + index,
						&(ctx->tx_ncm.ndp16),
						sizeof(ctx->tx_ncm.ndp16));

	memcpy(((u8 *)skb_out->data) + index + sizeof(ctx->tx_ncm.ndp16),
					&(ctx->tx_ncm.dpe16),
					(ctx->tx_curr_frame_num + 1) *
					sizeof(struct usb_cdc_ncm_dpe16));

	
	skb_put(skb_out, last_offset);

	
	ctx->tx_curr_skb = NULL;
	ctx->netdev->stats.tx_packets += ctx->tx_curr_frame_num;
	return skb_out;

exit_no_skb:
	
	if (ctx->tx_curr_skb != NULL)
		cdc_ncm_tx_timeout_start(ctx);
	return NULL;
}

static void cdc_ncm_tx_timeout_start(struct cdc_ncm_ctx *ctx)
{
	
	if (!(hrtimer_active(&ctx->tx_timer) || atomic_read(&ctx->stop)))
		hrtimer_start(&ctx->tx_timer,
				ktime_set(0, CDC_NCM_TIMER_INTERVAL),
				HRTIMER_MODE_REL);
}

static enum hrtimer_restart cdc_ncm_tx_timer_cb(struct hrtimer *timer)
{
	struct cdc_ncm_ctx *ctx =
			container_of(timer, struct cdc_ncm_ctx, tx_timer);

	if (!atomic_read(&ctx->stop))
		tasklet_schedule(&ctx->bh);
	return HRTIMER_NORESTART;
}

static void cdc_ncm_txpath_bh(unsigned long param)
{
	struct cdc_ncm_ctx *ctx = (struct cdc_ncm_ctx *)param;

	spin_lock_bh(&ctx->mtx);
	if (ctx->tx_timer_pending != 0) {
		ctx->tx_timer_pending--;
		cdc_ncm_tx_timeout_start(ctx);
		spin_unlock_bh(&ctx->mtx);
	} else if (ctx->netdev != NULL) {
		spin_unlock_bh(&ctx->mtx);
		netif_tx_lock_bh(ctx->netdev);
		usbnet_start_xmit(NULL, ctx->netdev);
		netif_tx_unlock_bh(ctx->netdev);
	}
}

static struct sk_buff *
cdc_ncm_tx_fixup(struct usbnet *dev, struct sk_buff *skb, gfp_t flags)
{
	struct sk_buff *skb_out;
	struct cdc_ncm_ctx *ctx = (struct cdc_ncm_ctx *)dev->data[0];

	if (ctx == NULL)
		goto error;

	spin_lock_bh(&ctx->mtx);
	skb_out = cdc_ncm_fill_tx_frame(ctx, skb);
	spin_unlock_bh(&ctx->mtx);
	return skb_out;

error:
	if (skb != NULL)
		dev_kfree_skb_any(skb);

	return NULL;
}

static int cdc_ncm_rx_fixup(struct usbnet *dev, struct sk_buff *skb_in)
{
	struct sk_buff *skb;
	struct cdc_ncm_ctx *ctx = (struct cdc_ncm_ctx *)dev->data[0];
	int len;
	int nframes;
	int x;
	int offset;
	struct usb_cdc_ncm_nth16 *nth16;
	struct usb_cdc_ncm_ndp16 *ndp16;
	struct usb_cdc_ncm_dpe16 *dpe16;

	if (ctx == NULL)
		goto error;

	if (skb_in->len < (sizeof(struct usb_cdc_ncm_nth16) +
					sizeof(struct usb_cdc_ncm_ndp16))) {
		pr_debug("frame too short\n");
		goto error;
	}

	nth16 = (struct usb_cdc_ncm_nth16 *)skb_in->data;

	if (le32_to_cpu(nth16->dwSignature) != USB_CDC_NCM_NTH16_SIGN) {
		pr_debug("invalid NTH16 signature <%u>\n",
					le32_to_cpu(nth16->dwSignature));
		goto error;
	}

	len = le16_to_cpu(nth16->wBlockLength);
	if (len > ctx->rx_max) {
		pr_debug("unsupported NTB block length %u/%u\n", len,
								ctx->rx_max);
		goto error;
	}

	if ((ctx->rx_seq + 1) != le16_to_cpu(nth16->wSequence) &&
		(ctx->rx_seq || le16_to_cpu(nth16->wSequence)) &&
		!((ctx->rx_seq == 0xffff) && !le16_to_cpu(nth16->wSequence))) {
		pr_debug("sequence number glitch prev=%d curr=%d\n",
				ctx->rx_seq, le16_to_cpu(nth16->wSequence));
	}
	ctx->rx_seq = le16_to_cpu(nth16->wSequence);

	len = le16_to_cpu(nth16->wNdpIndex);
	if ((len + sizeof(struct usb_cdc_ncm_ndp16)) > skb_in->len) {
		pr_debug("invalid DPT16 index <%u>\n",
					le16_to_cpu(nth16->wNdpIndex));
		goto error;
	}

	ndp16 = (struct usb_cdc_ncm_ndp16 *)(((u8 *)skb_in->data) + len);

	if (le32_to_cpu(ndp16->dwSignature) != USB_CDC_NCM_NDP16_NOCRC_SIGN) {
		pr_debug("invalid DPT16 signature <%u>\n",
					le32_to_cpu(ndp16->dwSignature));
		goto error;
	}

	if (le16_to_cpu(ndp16->wLength) < USB_CDC_NCM_NDP16_LENGTH_MIN) {
		pr_debug("invalid DPT16 length <%u>\n",
					le32_to_cpu(ndp16->dwSignature));
		goto error;
	}

	nframes = ((le16_to_cpu(ndp16->wLength) -
					sizeof(struct usb_cdc_ncm_ndp16)) /
					sizeof(struct usb_cdc_ncm_dpe16));
	nframes--; 

	len += sizeof(struct usb_cdc_ncm_ndp16);

	if ((len + nframes * (sizeof(struct usb_cdc_ncm_dpe16))) >
								skb_in->len) {
		pr_debug("Invalid nframes = %d\n", nframes);
		goto error;
	}

	dpe16 = (struct usb_cdc_ncm_dpe16 *)(((u8 *)skb_in->data) + len);

	for (x = 0; x < nframes; x++, dpe16++) {
		offset = le16_to_cpu(dpe16->wDatagramIndex);
		len = le16_to_cpu(dpe16->wDatagramLength);

		if ((offset == 0) || (len == 0)) {
			if (!x)
				goto error; 
			break;
		}

		
		if (((offset + len) > skb_in->len) ||
				(len > ctx->rx_max) || (len < ETH_HLEN)) {
			pr_debug("invalid frame detected (ignored)"
					"offset[%u]=%u, length=%u, skb=%p\n",
					x, offset, len, skb_in);
			if (!x)
				goto error;
			break;

		} else {
			skb = skb_clone(skb_in, GFP_ATOMIC);
			if (!skb)
				goto error;
			skb->len = len;
			skb->data = ((u8 *)skb_in->data) + offset;
			skb_set_tail_pointer(skb, len);
			usbnet_skb_return(dev, skb);
		}
	}
	return 1;
error:
	return 0;
}

static void
cdc_ncm_speed_change(struct cdc_ncm_ctx *ctx,
		     struct usb_cdc_speed_change *data)
{
	uint32_t rx_speed = le32_to_cpu(data->DLBitRRate);
	uint32_t tx_speed = le32_to_cpu(data->ULBitRate);

	if ((tx_speed != ctx->tx_speed) || (rx_speed != ctx->rx_speed)) {
		ctx->tx_speed = tx_speed;
		ctx->rx_speed = rx_speed;

		if ((tx_speed > 1000000) && (rx_speed > 1000000)) {
			printk(KERN_INFO KBUILD_MODNAME
				": %s: %u mbit/s downlink "
				"%u mbit/s uplink\n",
				ctx->netdev->name,
				(unsigned int)(rx_speed / 1000000U),
				(unsigned int)(tx_speed / 1000000U));
		} else {
			printk(KERN_INFO KBUILD_MODNAME
				": %s: %u kbit/s downlink "
				"%u kbit/s uplink\n",
				ctx->netdev->name,
				(unsigned int)(rx_speed / 1000U),
				(unsigned int)(tx_speed / 1000U));
		}
	}
}

static void cdc_ncm_status(struct usbnet *dev, struct urb *urb)
{
	struct cdc_ncm_ctx *ctx;
	struct usb_cdc_notification *event;

	ctx = (struct cdc_ncm_ctx *)dev->data[0];

	if (urb->actual_length < sizeof(*event))
		return;

	
	if (test_and_clear_bit(EVENT_STS_SPLIT, &dev->flags)) {
		cdc_ncm_speed_change(ctx,
		      (struct usb_cdc_speed_change *)urb->transfer_buffer);
		return;
	}

	event = urb->transfer_buffer;

	switch (event->bNotificationType) {
	case USB_CDC_NOTIFY_NETWORK_CONNECTION:
		ctx->connected = event->wValue;

		printk(KERN_INFO KBUILD_MODNAME ": %s: network connection:"
			" %sconnected\n",
			ctx->netdev->name, ctx->connected ? "" : "dis");

		if (ctx->connected)
			netif_carrier_on(dev->net);
		else {
			netif_carrier_off(dev->net);
			ctx->tx_speed = ctx->rx_speed = 0;
		}
		break;

	case USB_CDC_NOTIFY_SPEED_CHANGE:
		if (urb->actual_length < (sizeof(*event) +
					sizeof(struct usb_cdc_speed_change)))
			set_bit(EVENT_STS_SPLIT, &dev->flags);
		else
			cdc_ncm_speed_change(ctx,
				(struct usb_cdc_speed_change *) &event[1]);
		break;

	default:
		dev_err(&dev->udev->dev, "NCM: unexpected "
			"notification 0x%02x!\n", event->bNotificationType);
		break;
	}
}

static int cdc_ncm_check_connect(struct usbnet *dev)
{
	struct cdc_ncm_ctx *ctx;

	ctx = (struct cdc_ncm_ctx *)dev->data[0];
	if (ctx == NULL)
		return 1;	

	return !ctx->connected;
}

static int
cdc_ncm_probe(struct usb_interface *udev, const struct usb_device_id *prod)
{
	return usbnet_probe(udev, prod);
}

static void cdc_ncm_disconnect(struct usb_interface *intf)
{
	struct usbnet *dev = usb_get_intfdata(intf);

	if (dev == NULL)
		return;		

	usbnet_disconnect(intf);
}

static int cdc_ncm_manage_power(struct usbnet *dev, int status)
{
	dev->intf->needs_remote_wakeup = status;
	return 0;
}

static const struct driver_info cdc_ncm_info = {
	.description = "CDC NCM",
	.flags = FLAG_POINTTOPOINT | FLAG_NO_SETINT | FLAG_MULTI_PACKET,
	.bind = cdc_ncm_bind,
	.unbind = cdc_ncm_unbind,
	.check_connect = cdc_ncm_check_connect,
	.manage_power = cdc_ncm_manage_power,
	.status = cdc_ncm_status,
	.rx_fixup = cdc_ncm_rx_fixup,
	.tx_fixup = cdc_ncm_tx_fixup,
};

static struct usb_driver cdc_ncm_driver = {
	.name = "cdc_ncm",
	.id_table = cdc_devs,
	.probe = cdc_ncm_probe,
	.disconnect = cdc_ncm_disconnect,
	.suspend = usbnet_suspend,
	.resume = usbnet_resume,
	.reset_resume =	usbnet_resume,
	.supports_autosuspend = 1,
};

static const struct ethtool_ops cdc_ncm_ethtool_ops = {
	.get_drvinfo = cdc_ncm_get_drvinfo,
	.get_link = usbnet_get_link,
	.get_msglevel = usbnet_get_msglevel,
	.set_msglevel = usbnet_set_msglevel,
	.get_settings = usbnet_get_settings,
	.set_settings = usbnet_set_settings,
	.nway_reset = usbnet_nway_reset,
};

module_usb_driver(cdc_ncm_driver);

MODULE_AUTHOR("Hans Petter Selasky");
MODULE_DESCRIPTION("USB CDC NCM host driver");
MODULE_LICENSE("Dual BSD/GPL");
