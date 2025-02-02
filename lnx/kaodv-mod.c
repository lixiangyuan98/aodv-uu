/*****************************************************************************
 *
 * Copyright (C) 2001 Uppsala University and Ericsson AB.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Author: Erik Nordstr�m, <erik.nordstrom@it.uu.se>
 * 
 *****************************************************************************/
#include <linux/version.h>
#include <linux/moduleparam.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/if.h>
#include <linux/skbuff.h>
#include <linux/netdevice.h>
#include <linux/inetdevice.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <linux/tcp.h>
#include <net/tcp.h>
#include <net/route.h>

#include "kaodv-mod.h"
#include "kaodv-expl.h"
#include "kaodv-netlink.h"
#include "kaodv-queue.h"
#include "kaodv-ipenc.h"
#include "kaodv-debug.h"
#include "kaodv.h"

#define ACTIVE_ROUTE_TIMEOUT active_route_timeout
#define MAX_INTERFACES 10

static int qual = 0;
static unsigned long pkts_dropped = 0;
int qual_th = 0;
int is_gateway = 1;
int active_route_timeout = 3000;
//static unsigned int loindex = 0;

#define ADDR_HOST 1
#define ADDR_BROADCAST 2

void kaodv_update_route_timeouts(int hooknum, const struct net_device *dev,
								 struct iphdr *iph)
{
	struct expl_entry e;
	struct in_addr bcaddr;
	int res;

	bcaddr.s_addr = 0; /* Stop compiler from complaining about
			    * uninitialized bcaddr */

	res = if_info_from_ifindex(NULL, &bcaddr, dev->ifindex);

	if (res < 0)
		return;

	if (hooknum == NF_INET_PRE_ROUTING)
		kaodv_netlink_send_rt_update_msg(PKT_INBOUND, iph->saddr,
										 iph->daddr, dev->ifindex);
	else if (iph->daddr != INADDR_BROADCAST && iph->daddr != bcaddr.s_addr)
		kaodv_netlink_send_rt_update_msg(PKT_OUTBOUND, iph->saddr,
										 iph->daddr, dev->ifindex);

	/* First update forward route and next hop */
	if (kaodv_expl_get(iph->daddr, &e))
	{

		kaodv_expl_update(e.daddr, e.nhop, ACTIVE_ROUTE_TIMEOUT,
						  e.flags, dev->ifindex);

		if (e.nhop != e.daddr && kaodv_expl_get(e.nhop, &e))
			kaodv_expl_update(e.daddr, e.nhop, ACTIVE_ROUTE_TIMEOUT,
							  e.flags, dev->ifindex);
	}
	/* Update reverse route */
	if (kaodv_expl_get(iph->saddr, &e))
	{

		kaodv_expl_update(e.daddr, e.nhop, ACTIVE_ROUTE_TIMEOUT,
						  e.flags, dev->ifindex);

		if (e.nhop != e.daddr && kaodv_expl_get(e.nhop, &e))
			kaodv_expl_update(e.daddr, e.nhop, ACTIVE_ROUTE_TIMEOUT,
							  e.flags, dev->ifindex);
	}
}

static unsigned int kaodv_hook(void *priv,
							   struct sk_buff *skb,
							   const struct nf_hook_state *state)
{
	unsigned int hooknum = state->hook;
	const struct net_device *in = state->in;
	const struct net_device *out = state->out;
	int (*okfn)(struct net *, struct sock *, struct sk_buff *) = state->okfn;
	struct iphdr *iph = SKB_NETWORK_HDR_IPH(skb);
	struct expl_entry e;
	struct in_addr ifaddr, bcaddr;
	int res = 0;

	memset(&ifaddr, 0, sizeof(struct in_addr));
	memset(&bcaddr, 0, sizeof(struct in_addr));

	/* We are only interested in IP packets */
	if (iph == NULL)
		return NF_ACCEPT;

	/* We want AODV control messages to go through directly to the
	 * AODV socket.... */
	if (iph && iph->protocol == IPPROTO_UDP)
	{
		struct udphdr *udph;

		udph = (struct udphdr *)((char *)iph + (iph->ihl << 2));

		if (ntohs(udph->dest) == AODV_PORT ||
			ntohs(udph->source) == AODV_PORT)
		{

#ifdef CONFIG_QUAL_THRESHOLD
		qual = (skb)->iwq.qual;
		if (qual_th && hooknum == NF_INET_PRE_ROUTING)
		{
			if (qual && qual < qual_th)
			{
				pkts_dropped++;
				return NF_DROP;
			}
		}
#endif /* CONFIG_QUAL_THRESHOLD */
			if (hooknum == NF_INET_PRE_ROUTING && in)
				kaodv_update_route_timeouts(hooknum, in, iph);

			return NF_ACCEPT;
		}
	}

	if (hooknum == NF_INET_PRE_ROUTING)
		res = if_info_from_ifindex(&ifaddr, &bcaddr, in->ifindex);
	else
		res = if_info_from_ifindex(&ifaddr, &bcaddr, out->ifindex);

	if (res < 0)
		return NF_ACCEPT;

	/* Ignore broadcast and multicast packets */
	if (iph->daddr == INADDR_BROADCAST ||
		IN_MULTICAST(ntohl(iph->daddr)) ||
		iph->daddr == bcaddr.s_addr)
		return NF_ACCEPT;

	/* Check which hook the packet is on... */
	switch (hooknum)
	{
	case NF_INET_PRE_ROUTING:
		kaodv_update_route_timeouts(hooknum, in, iph);

		/* If we are a gateway maybe we need to decapsulate? */
		if (is_gateway && iph->protocol == IPPROTO_MIPE &&
			iph->daddr == ifaddr.s_addr)
		{
			ip_pkt_decapsulate(skb);
			iph = SKB_NETWORK_HDR_IPH(skb);
			return NF_ACCEPT;
		}
		/* Ignore packets generated locally or that are for this
		 * node. */
		if (iph->saddr == ifaddr.s_addr ||
			iph->daddr == ifaddr.s_addr)
		{
			return NF_ACCEPT;
		}
		/* Check for unsolicited data packets */
		else if (!kaodv_expl_get(iph->daddr, &e))
		{
			kaodv_netlink_send_rerr_msg(PKT_INBOUND, iph->saddr,
										iph->daddr, in->ifindex);
			return NF_DROP;
		}
		/* Check if we should repair the route */
		else if (e.flags & KAODV_RT_REPAIR)
		{

			kaodv_netlink_send_rt_msg(KAODVM_REPAIR, iph->saddr,
									  iph->daddr);

			kaodv_queue_enqueue_packet(skb, okfn);

			return NF_STOLEN;
		}
		break;
	case NF_INET_LOCAL_OUT:

		if (!kaodv_expl_get(iph->daddr, &e) ||
			(e.flags & KAODV_RT_REPAIR))
		{

			if (!kaodv_queue_find(iph->daddr))
				kaodv_netlink_send_rt_msg(KAODVM_ROUTE_REQ,
										  0,
										  iph->daddr);

			kaodv_queue_enqueue_packet(skb, okfn);

			return NF_STOLEN;
		}
		else if (e.flags & KAODV_RT_GW_ENCAP)
		{
			/* Make sure that also the virtual Internet
			 * dest entry is refreshed */
			kaodv_update_route_timeouts(hooknum, out, iph);

			skb = ip_pkt_encapsulate(skb, e.nhop);

			if (!skb)
				return NF_STOLEN;

			ip_route_me_harder(&init_net, skb, RTN_LOCAL);
		}
		break;
	case NF_INET_POST_ROUTING:
		kaodv_update_route_timeouts(hooknum, out, iph);
	}
	return NF_ACCEPT;
}

/*
 * Called when the module is inserted in the kernel.
 */
static char *ifname[MAX_INTERFACES] = {"eth0"};
static int num_parms = 0;
module_param_array(ifname, charp, &num_parms, 0444);
module_param(qual_th, int, 0);

static struct nf_hook_ops kaodv_ops[] = {
	{
		.hook = kaodv_hook,
		.pf = PF_INET,
		.hooknum = NF_INET_PRE_ROUTING,
		.priority = NF_IP_PRI_FIRST,
	},
	{
		.hook = kaodv_hook,
		.pf = PF_INET,
		.hooknum = NF_INET_LOCAL_OUT,
		.priority = NF_IP_PRI_FILTER,
	},
	{
		.hook = kaodv_hook,
		.pf = PF_INET,
		.hooknum = NF_INET_POST_ROUTING,
		.priority = NF_IP_PRI_FILTER,
	},
};

// NOTE: 不确定正确性
static ssize_t kaodv_read_proc(struct file* p_file, char* p_buf, size_t p_count, loff_t* p_offset) {
	sprintf(p_buf,
		"qual threshold=%d\npkts dropped=%lu\nlast qual=%d\ngateway_mode=%d\n",
		qual_th, pkts_dropped, qual, is_gateway);
	return 0;
}

// static int kaodv_read_proc(char *buff, char **start, off_t off, int count,
// 						   int *eof, void *data)
// {
// 	int len;

// 	len = sprintf(buff,
// 				  "qual threshold=%d\npkts dropped=%lu\nlast qual=%d\ngateway_mode=%d\n",
// 				  qual_th, pkts_dropped, qual, is_gateway);

// 	*start = buff + off;
// 	len -= off;
// 	if (len > count)
// 		len = count;
// 	else if (len < 0)
// 		len = 0;
// 	return len;
// }

static const struct file_operations kaodv_proc_fops = {
	read: kaodv_read_proc
};

static int __init kaodv_init(void)
{
	struct net_device *dev = NULL;
	int i, ret = -ENOMEM;

	kaodv_expl_init();

	ret = kaodv_queue_init();

	if (ret < 0)
		return ret;

	ret = kaodv_netlink_init();

	if (ret < 0)
		goto cleanup_queue;

	ret = nf_register_net_hook(&init_net, &kaodv_ops[0]);

	if (ret < 0)
		goto cleanup_netlink;

	ret = nf_register_net_hook(&init_net, &kaodv_ops[1]);

	if (ret < 0)
		goto cleanup_hook0;

	ret = nf_register_net_hook(&init_net, &kaodv_ops[2]);

	if (ret < 0)
		goto cleanup_hook1;

	/* Prefetch network device info (ip, broadcast address, ifindex). */
	for (i = 0; i < MAX_INTERFACES; i++)
	{
		if (!ifname[i])
			break;

		dev = dev_get_by_name(&init_net, ifname[i]);

		if (!dev)
		{
			printk("No device %s available, ignoring!\n",
				   ifname[i]);
			continue;
		}
		if_info_add(dev);

		dev_put(dev);
	}

	if (!proc_create("kaodv", 0, init_net.proc_net, &kaodv_proc_fops))
		KAODV_DEBUG("Could not create kaodv proc entry");
	KAODV_DEBUG("Module init OK");

	return ret;

cleanup_hook1:
	nf_unregister_net_hook(&init_net, &kaodv_ops[1]);
cleanup_hook0:
	nf_unregister_net_hook(&init_net, &kaodv_ops[0]);
cleanup_netlink:
	kaodv_netlink_fini();
cleanup_queue:
	kaodv_queue_fini();

	return ret;
}

/*
 * Called when removing the module from memory... 
 */
static void __exit kaodv_exit(void)
{
	unsigned int i;

	if_info_purge();

	for (i = 0; i < sizeof(kaodv_ops) / sizeof(struct nf_hook_ops); i++)
		nf_unregister_net_hook(&init_net, &kaodv_ops[i]);
	remove_proc_entry("kaodv", init_net.proc_net);
	kaodv_queue_fini();
	kaodv_expl_fini();
	kaodv_netlink_fini();
}

module_init(kaodv_init);
module_exit(kaodv_exit);
