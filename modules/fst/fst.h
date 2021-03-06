/*
** Copyright (C) 2018-2018 Cisco and/or its affiliates. All rights reserved.
** Author: Michael R. Altizer <mialtize@cisco.com>
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License Version 2 as
** published by the Free Software Foundation.  You may not use, modify or
** distribute this program under any other version of the GNU General
** Public License.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#ifndef _FST_H
#define _FST_H

#include <deque>
#include <list>
#include <memory>
#include <unordered_map>

#include "daq_common.h"
#include "decode.h"
#include "PMurHash.h"

struct FstKey
{
    union
    {
        struct in_addr ip4;
        struct in6_addr ip6;
    } ip_l;
    union
    {
        struct in_addr ip4;
        struct in6_addr ip6;
    } ip_h;
    uint16_t l4_port_l;
    uint16_t l4_port_h;
    uint16_t addr_space_id;
    uint16_t vlan_tag;
    uint8_t protocol;
    uint8_t ipver;

    bool populate(const DAQ_PktHdr_t *, const DecodeData *);
    bool operator==(const FstKey&) const;
};

struct FstKeyHash
{
    std::size_t operator()(FstKey const& key) const noexcept
    {
        return PMurHash32(0, &key, sizeof(key));
    }
};

struct FstEntry
{
    FstEntry(const DAQ_PktHdr_t *pkthdr, const FstKey &key, uint32_t id, bool swapped);
    ~FstEntry() { delete[] ha_state; }
    void update_stats(const DAQ_PktHdr_t *pkthdr, bool swapped);

    Flow_Stats_t flow_stats = { };
    uint8_t *ha_state = nullptr;
    uint32_t ha_state_len = 0;
    uint32_t flow_id;
#define FST_ENTRY_FLAG_SWAPPED      0x1
#define FST_ENTRY_FLAG_WHITELISTED  0x2
#define FST_ENTRY_FLAG_BLACKLISTED  0x4
#define FST_ENTRY_FLAG_OPAQUE_SET   0x8
    uint32_t flags = 0;
};

struct FstNode
{
    const FstKey *key;
    std::shared_ptr<FstEntry> entry;

    std::list<FstNode*>::iterator lru_iter;
    std::list<FstNode*>::iterator timeout_iter;
    struct FstTimeoutList *timeout_list;
};

struct FstTimeoutList
{
    enum ID : uint8_t
    {
        TCP_SHORT = 0,
        TCP_LONG,
        UDP,
        ICMP,
        OTHER,
        MAX
    };

    FstTimeoutList(ID tol_id, time_t sec) : list_id(tol_id), timeout(sec) { }

    std::list<FstNode*> list;
    ID list_id;
    time_t timeout;
};

class FlowStateTable
{
public:
    FstNode *find(const FstKey &key);
    FstNode *insert(const FstKey &key, std::shared_ptr<FstEntry> entry);
    size_t size() const { return flow_table.size(); }
    void clear();

    void set_max_size(size_t size);
    size_t get_max_size() const { return max_size; }

    void move_node_to_timeout_list(FstNode *node, FstTimeoutList::ID tol_id);
    unsigned process_timeouts(const struct timeval *curr_time);

    bool purgatory_empty() { return purgatory.empty(); }
    std::shared_ptr<FstEntry> get_lost_soul();

private:
    void extract_node(FstNode *node);
    void prune_lru();

    std::unordered_map<FstKey, FstNode*, FstKeyHash> flow_table;
    std::list<FstNode*> lru_list;
    std::deque<std::shared_ptr<FstEntry>> purgatory;
    FstTimeoutList timeout_lists[FstTimeoutList::ID::MAX] = {
        { FstTimeoutList::ID::TCP_SHORT, 30 },
        { FstTimeoutList::ID::TCP_LONG, 3600 },
        { FstTimeoutList::ID::UDP, 180 },
        { FstTimeoutList::ID::ICMP, 30 },
        { FstTimeoutList::ID::OTHER, 60 }
    };
    size_t max_size = 0;
};

FstEntry::FstEntry(const DAQ_PktHdr_t *pkthdr, const FstKey &key, uint32_t id, bool swapped)
{
    flow_stats.ingressZone = pkthdr->ingress_group;
    flow_stats.egressZone = pkthdr->egress_group;
    flow_stats.ingressIntf = pkthdr->ingress_index;
    flow_stats.egressIntf = pkthdr->egress_index;

    if (key.ipver == 4)
    {
        struct in6_addr *initiatorIp = (struct in6_addr *) &flow_stats.initiatorIp;
        struct in6_addr *responderIp = (struct in6_addr *) &flow_stats.responderIp;
        initiatorIp->s6_addr16[5] = 0xFFFF;
        responderIp->s6_addr16[5] = 0xFFFF;
        if (!swapped)
        {
            initiatorIp->s6_addr32[3] = key.ip_l.ip4.s_addr;
            responderIp->s6_addr32[3] = key.ip_h.ip4.s_addr;
        }
        else
        {
            initiatorIp->s6_addr32[3] = key.ip_h.ip4.s_addr;
            responderIp->s6_addr32[3] = key.ip_l.ip4.s_addr;
        }
    }
    else if (key.ipver == 6)
    {
        if (!swapped)
        {
            memcpy(&flow_stats.initiatorIp, key.ip_l.ip6.s6_addr, sizeof(flow_stats.initiatorIp));
            memcpy(&flow_stats.responderIp, key.ip_h.ip6.s6_addr, sizeof(flow_stats.responderIp));
        }
        else
        {
            memcpy(&flow_stats.initiatorIp, key.ip_h.ip6.s6_addr, sizeof(flow_stats.initiatorIp));
            memcpy(&flow_stats.responderIp, key.ip_l.ip6.s6_addr, sizeof(flow_stats.responderIp));
        }
    }
    if (!swapped)
    {
        flow_stats.initiatorPort = key.l4_port_l;
        flow_stats.responderPort = key.l4_port_h;
    }
    else
    {
        flow_stats.initiatorPort = key.l4_port_h;
        flow_stats.responderPort = key.l4_port_l;
    }

    flow_stats.sof_timestamp = pkthdr->ts;

    flow_stats.vlan_tag = key.vlan_tag;
    flow_stats.address_space_id = key.addr_space_id;
    flow_stats.protocol = key.protocol;

    flow_id = id;
    if (swapped)
        flags |= FST_ENTRY_FLAG_SWAPPED;
}

void FstEntry::update_stats(const DAQ_PktHdr_t *pkthdr, bool swapped)
{
    if (!swapped == !(flags & FST_ENTRY_FLAG_SWAPPED))
    {
        flow_stats.initiatorPkts++;
        flow_stats.initiatorBytes += pkthdr->pktlen;
    }
    else
    {
        flow_stats.responderPkts++;
        flow_stats.responderBytes += pkthdr->pktlen;
    }
    flow_stats.eof_timestamp = pkthdr->ts;
}

void FlowStateTable::extract_node(FstNode *node)
{
    if (node->timeout_list)
        node->timeout_list->list.erase(node->timeout_iter);
    lru_list.erase(node->lru_iter);
    flow_table.erase(*node->key);
    purgatory.push_back(node->entry);
    delete node;
}

void FlowStateTable::prune_lru()
{
    FstNode *node = lru_list.back();
    extract_node(node);
}

FstNode *FlowStateTable::find(const FstKey &key)
{
    auto result = flow_table.find(key);
    if (result == flow_table.end())
        return nullptr;

    FstNode *node = result->second;
    if (node->lru_iter != lru_list.begin())
        lru_list.splice(lru_list.begin(), lru_list, node->lru_iter);

    return node;
}

FstNode *FlowStateTable::insert(const FstKey &key, std::shared_ptr<FstEntry> entry)
{
    if (flow_table.find(key) != flow_table.end())
        return nullptr;

    // Size limit reached? If so, prune the LRU entry
    if (max_size > 0 && flow_table.size() >= max_size)
    {
        while (flow_table.size() >= max_size)
            prune_lru();
    }

    FstNode *node = new FstNode();
    node->entry = entry;
    auto result = flow_table.insert({ key, node });
    if (!result.second)
    {
        delete node;
        return nullptr;
    }
    auto it = result.first;
    node->key = &it->first;
    lru_list.push_front(node);
    node->lru_iter = lru_list.begin();

    return node;
}

void FlowStateTable::clear()
{
    while (flow_table.size() > 0)
        prune_lru();
}

void FlowStateTable::set_max_size(size_t size)
{
    max_size = size;
    if (max_size > 0 && flow_table.size() > max_size)
    {
        while (flow_table.size() > max_size)
            prune_lru();
    }
}

void FlowStateTable::move_node_to_timeout_list(FstNode *node, FstTimeoutList::ID tol_id)
{
    if (node->timeout_list)
        node->timeout_list->list.erase(node->timeout_iter);
    node->timeout_list = &timeout_lists[tol_id];
    node->timeout_list->list.push_front(node);
    node->timeout_iter = node->timeout_list->list.begin();
}

unsigned FlowStateTable::process_timeouts(const struct timeval *curr_time)
{
    unsigned timeout_count = 0;
    for (FstTimeoutList &to_list : timeout_lists)
    {
        while (!to_list.list.empty())
        {
            FstNode *node = to_list.list.back();
            struct timeval target_time = node->entry->flow_stats.eof_timestamp;
            target_time.tv_sec += to_list.timeout;
            if (curr_time->tv_sec < target_time.tv_sec ||
                (curr_time->tv_sec == target_time.tv_sec && curr_time->tv_usec < target_time.tv_usec))
                break;
            extract_node(node);
            timeout_count++;
        }
    }
    return timeout_count;
}

std::shared_ptr<FstEntry> FlowStateTable::get_lost_soul()
{
    if (purgatory.empty())
        return nullptr;
    std::shared_ptr<FstEntry> entry = purgatory.front();
    purgatory.pop_front();
    return entry;
}

static inline int ip6_cmp(const struct in6_addr *ip1, const struct in6_addr *ip2)
{
    if (ip1->s6_addr32[0] < ip2->s6_addr32[0])
        return -1;

    if (ip1->s6_addr32[0] > ip2->s6_addr32[0])
        return 1;

    if (ip1->s6_addr32[1] < ip2->s6_addr32[1])
        return -1;

    if (ip1->s6_addr32[1] > ip2->s6_addr32[1])
        return 1;

    if (ip1->s6_addr32[2] < ip2->s6_addr32[2])
        return -1;

    if (ip1->s6_addr32[2] > ip2->s6_addr32[2])
        return 1;

    if (ip1->s6_addr32[3] < ip2->s6_addr32[3])
        return -1;

    if (ip1->s6_addr32[3] > ip2->s6_addr32[4])
        return 1;

    return 0;
}

static inline void ip6_copy(struct in6_addr *dst, const struct in6_addr *src)
{
    dst->s6_addr32[0] = src->s6_addr32[0];
    dst->s6_addr32[1] = src->s6_addr32[1];
    dst->s6_addr32[2] = src->s6_addr32[2];
    dst->s6_addr32[3] = src->s6_addr32[3];
}

bool FstKey::operator==(const FstKey &other) const
{
    if (vlan_tag != other.vlan_tag)
        return false;
    if (memcmp(&ip_l, &other.ip_l, sizeof(ip_l)) || memcmp(&ip_h, &other.ip_h, sizeof(ip_h)))
        return false;
    if (protocol != other.protocol)
        return false;
    if (l4_port_l != other.l4_port_l || l4_port_h != other.l4_port_h)
        return false;
    return true;
}

bool FstKey::populate(const DAQ_PktHdr_t *pkthdr, const DecodeData *dd)
{
    bool swapped = false;

    addr_space_id = pkthdr->address_space_id;

    /* L2 */
    if (dd->vlan)
        vlan_tag = VTH_VLAN(dd->vlan);

    /* L3 */
    int ip_cmp;
    if (dd->ip)
    {
        uint32_t src_addr = dd->ip->saddr;
        uint32_t dst_addr = dd->ip->daddr;
        if (src_addr < dst_addr)
        {
            ip_l.ip4.s_addr = src_addr;
            ip_h.ip4.s_addr = dst_addr;
            ip_cmp = -1;
        }
        else if (src_addr > dst_addr)
        {
            ip_l.ip4.s_addr = dst_addr;
            ip_h.ip4.s_addr = src_addr;
            ip_cmp = 1;
            swapped = true;
        }
        else
        {
            ip_l.ip4.s_addr = src_addr;
            ip_h.ip4.s_addr = dst_addr;
            ip_cmp = 0;
        }
        ipver = 4;
    }
    else if (dd->ip6)
    {
        ip_cmp = ip6_cmp(&dd->ip6->ip6_src, &dd->ip6->ip6_dst);
        if (ip_cmp <= 0)
        {
            ip6_copy(&ip_l.ip6, &dd->ip6->ip6_src);
            ip6_copy(&ip_h.ip6, &dd->ip6->ip6_dst);
        }
        else
        {
            ip6_copy(&ip_l.ip6, &dd->ip6->ip6_dst);
            ip6_copy(&ip_h.ip6, &dd->ip6->ip6_src);
            swapped = true;
        }
        ipver = 6;
    }
    else
    {
        ipver = 0;
        return swapped;
    }

    /* L4 */
    uint16_t src_port, dst_port;
    if (dd->tcp)
    {
        protocol = IPPROTO_TCP;
        src_port = dd->tcp->th_sport;
        dst_port = dd->tcp->th_dport;
    }
    else if (dd->udp)
    {
        protocol = IPPROTO_UDP;
        src_port = dd->udp->uh_sport;
        dst_port = dd->udp->uh_dport;
    }
    else if (dd->icmp)
    {
        protocol = IPPROTO_ICMP;
        if (dd->icmp->type == ICMP_ECHOREPLY)
        {
            src_port = 0;
            dst_port = ICMP_ECHO;
        }
        else
        {
            src_port = dd->icmp->type;
            dst_port = 0;
        }
    }
    else if (dd->icmp6)
    {
        protocol = IPPROTO_ICMPV6;
        if (dd->icmp6->icmp6_type == ICMP6_ECHO_REPLY)
        {
            src_port = 0;
            dst_port = ICMP6_ECHO_REQUEST;
        }
        else
        {
            src_port = dd->icmp6->icmp6_type;
            dst_port = 0;
        }
    }
    else
    {
        src_port = 0;
        dst_port = 0;
    }

    if (ip_cmp < 0 || (ip_cmp == 0 && src_port < dst_port))
    {
        l4_port_l = src_port;
        l4_port_h = dst_port;
    }
    else
    {
        l4_port_l = dst_port;
        l4_port_h = src_port;
    }

    return swapped;
}

#endif
