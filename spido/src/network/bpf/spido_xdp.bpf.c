// SPDX-License-Identifier: GPL-2.0
//
// spido XDP redirector.
//
// Action: any TCP segment with dst port == SPIDO_PORT is steered into the
// XSKMAP at the current rx_queue_index, where an AF_XDP socket bound by
// userspace picks it up. Everything else (other ports, ARP, ICMP, IPv6
// here = pass) returns to the kernel so the host's normal TCP/IP stack
// keeps serving — SSH on :22, DNS, etc. all stay reachable.

#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/tcp.h>

#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

// Userspace edits this constant by patching the loaded object (or via
// __u16 const volatile if you want CO-RE). 8080 is the project default.
#ifndef SPIDO_PORT
#define SPIDO_PORT 8080
#endif

struct {
    __uint(type, BPF_MAP_TYPE_XSKMAP);
    __uint(max_entries, 64); // bounded by likely queue count
    __type(key, int);
    __type(value, int);
} xsks_map SEC(".maps");

SEC("xdp")
int spido_xdp(struct xdp_md *ctx) {
    void *data     = (void *)(long)ctx->data;
    void *data_end = (void *)(long)ctx->data_end;

    struct ethhdr *eth = data;
    if ((void *)(eth + 1) > data_end) return XDP_PASS;
    if (eth->h_proto != bpf_htons(ETH_P_IP)) return XDP_PASS;

    struct iphdr *ip = (void *)(eth + 1);
    if ((void *)(ip + 1) > data_end) return XDP_PASS;
    if (ip->protocol != IPPROTO_TCP) return XDP_PASS;
    if (ip->ihl < 5) return XDP_PASS;

    // ihl is in 32-bit words; verifier needs the bound proven explicitly.
    __u32 hdr = (__u32)ip->ihl * 4;
    if ((void *)ip + hdr + sizeof(struct tcphdr) > data_end) return XDP_PASS;

    struct tcphdr *tcp = (void *)ip + hdr;
    if (tcp->dest != bpf_htons(SPIDO_PORT)) return XDP_PASS;

    // bpf_redirect_map returns XDP_PASS on miss (when no AF_XDP socket
    // is bound to this queue), so unmatched queues fall back to kernel.
    return bpf_redirect_map(&xsks_map, ctx->rx_queue_index, XDP_PASS);
}

char LICENSE[] SEC("license") = "GPL";
