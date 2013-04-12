/*
 * Copyright (c) 2009, 2010, 2011, 2012, 2013 Nicira, Inc.
 * Copyright (c) 2013 Simon Horman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>
#include <linux/openvswitch.h>
#include <stdlib.h>
#include <string.h>

#include "execute-actions.h"
#include "netlink.h"
#include "packets.h"
#include "util.h"

static void
eth_set_src_and_dst(struct ofpbuf *packet,
                    const struct ovs_key_ethernet *eth_key)
{
    struct eth_header *eh = packet->l2;

    memcpy(eh->eth_src, eth_key->eth_src, sizeof eh->eth_src);
    memcpy(eh->eth_dst, eth_key->eth_dst, sizeof eh->eth_dst);
}

static void
execute_set_action(struct ofpbuf *packet, const struct nlattr *a)
{
    enum ovs_key_attr type = nl_attr_type(a);
    const struct ovs_key_ipv4 *ipv4_key;
    const struct ovs_key_ipv6 *ipv6_key;
    const struct ovs_key_tcp *tcp_key;
    const struct ovs_key_udp *udp_key;

    switch (type) {
    case OVS_KEY_ATTR_PRIORITY:
    case OVS_KEY_ATTR_TUNNEL:
    case OVS_KEY_ATTR_SKB_MARK:
        /* not implemented */
        break;

    case OVS_KEY_ATTR_ETHERNET:
        eth_set_src_and_dst(packet,
                   nl_attr_get_unspec(a, sizeof(struct ovs_key_ethernet)));
        break;

    case OVS_KEY_ATTR_IPV4:
        ipv4_key = nl_attr_get_unspec(a, sizeof(struct ovs_key_ipv4));
        packet_set_ipv4(packet, ipv4_key->ipv4_src, ipv4_key->ipv4_dst,
                        ipv4_key->ipv4_tos, ipv4_key->ipv4_ttl);
        break;

    case OVS_KEY_ATTR_IPV6:
        ipv6_key = nl_attr_get_unspec(a, sizeof(struct ovs_key_ipv6));
        packet_set_ipv6(packet, ipv6_key->ipv6_proto, ipv6_key->ipv6_src,
                        ipv6_key->ipv6_dst, ipv6_key->ipv6_tclass,
                        ipv6_key->ipv6_label, ipv6_key->ipv6_hlimit);
        break;

    case OVS_KEY_ATTR_TCP:
        tcp_key = nl_attr_get_unspec(a, sizeof(struct ovs_key_tcp));
        packet_set_tcp_port(packet, tcp_key->tcp_src, tcp_key->tcp_dst);
        break;

     case OVS_KEY_ATTR_UDP:
        udp_key = nl_attr_get_unspec(a, sizeof(struct ovs_key_udp));
        packet_set_udp_port(packet, udp_key->udp_src, udp_key->udp_dst);
        break;

     case OVS_KEY_ATTR_MPLS:
         set_mpls_lse(packet, nl_attr_get_be32(a));
         break;

     case OVS_KEY_ATTR_UNSPEC:
     case OVS_KEY_ATTR_ENCAP:
     case OVS_KEY_ATTR_ETHERTYPE:
     case OVS_KEY_ATTR_IN_PORT:
     case OVS_KEY_ATTR_VLAN:
     case OVS_KEY_ATTR_ICMP:
     case OVS_KEY_ATTR_ICMPV6:
     case OVS_KEY_ATTR_ARP:
     case OVS_KEY_ATTR_ND:
     case __OVS_KEY_ATTR_MAX:
     default:
        NOT_REACHED();
    }
}

static void
execute_sample(void *dp, struct ofpbuf *packet, struct flow *key,
               const struct nlattr *action,
               void (*output)(void *dp, struct ofpbuf *packet,
                              uint32_t out_port),
               void (*userspace)(void *dp, struct ofpbuf *packet,
                                 struct flow *key, const struct nlattr *a))
{
    const struct nlattr *subactions = NULL;
    const struct nlattr *a;
    size_t left;

    NL_NESTED_FOR_EACH_UNSAFE (a, left, action) {
        int type = nl_attr_type(a);

        switch ((enum ovs_sample_attr) type) {
        case OVS_SAMPLE_ATTR_PROBABILITY:
            if (random_uint32() >= nl_attr_get_u32(a)) {
                return;
            }
            break;

        case OVS_SAMPLE_ATTR_ACTIONS:
            subactions = a;
            break;

        case OVS_SAMPLE_ATTR_UNSPEC:
        case __OVS_SAMPLE_ATTR_MAX:
        default:
            NOT_REACHED();
        }
    }

    execute_actions(dp, packet, key, nl_attr_get(subactions),
                    nl_attr_get_size(subactions), output, userspace);
}

void
execute_actions(void *dp, struct ofpbuf *packet, struct flow *key,
                const struct nlattr *actions, size_t actions_len,
                void (*output)(void *dp, struct ofpbuf *packet,
                               uint32_t out_port),
                void (*userspace)(void *dp, struct ofpbuf *packet,
                                  struct flow *key, const struct nlattr *a))
{
    const struct nlattr *a;
    unsigned int left;

    NL_ATTR_FOR_EACH_UNSAFE (a, left, actions, actions_len) {
        int type = nl_attr_type(a);

        switch ((enum ovs_action_attr) type) {
        case OVS_ACTION_ATTR_OUTPUT:
            ovs_assert(dp && output);
            output(dp, packet, nl_attr_get_u32(a));
            break;

        case OVS_ACTION_ATTR_USERSPACE:
            ovs_assert(dp && userspace && key);
            userspace(dp, packet, key, a);
            break;

        case OVS_ACTION_ATTR_PUSH_VLAN: {
            const struct ovs_action_push_vlan *vlan = nl_attr_get(a);
            eth_push_vlan(packet, vlan->vlan_tci);
            break;
        }

        case OVS_ACTION_ATTR_POP_VLAN:
            eth_pop_vlan(packet);
            break;

        case OVS_ACTION_ATTR_PUSH_MPLS: {
            const struct ovs_action_push_mpls *mpls = nl_attr_get(a);
            push_mpls(packet, mpls->mpls_ethertype, mpls->mpls_lse);
            break;
         }

        case OVS_ACTION_ATTR_POP_MPLS:
            pop_mpls(packet, nl_attr_get_be16(a));
            break;

        case OVS_ACTION_ATTR_SET:
            execute_set_action(packet, nl_attr_get(a));
            break;

        case OVS_ACTION_ATTR_SAMPLE:
            execute_sample(dp, packet, key, a, output, userspace);
            break;

        case OVS_ACTION_ATTR_UNSPEC:
        case __OVS_ACTION_ATTR_MAX:
            NOT_REACHED();
        }
    }
}
