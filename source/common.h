/* Copyright (c) 2026 Sevana OÜ
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/* common.h - shared packet layout + helpers for the AF_XDP RTP sender and the
 * recvmmsg receiver.
 *
 * The wire format is a plain G.711 (PCMA, payload type 8) RTP/UDP/IPv4 stream:
 *
 *   [ Ethernet 14 ][ IPv4 20 ][ UDP 8 ][ RTP 12 ][ payload N ]
 *
 * One "flow" == one RTP call. Flows share the destination (the receiver's
 * IP:port) and differ only by UDP source port + RTP SSRC, so each is a distinct
 * 5-tuple on the wire - exactly what a passive vq-core listener keys streams on.
 */
#ifndef VQ_AFXDP_COMMON_H
#define VQ_AFXDP_COMMON_H

#include <stdint.h>
#include <string.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <linux/if_ether.h>

#define ETH_HLEN_   14
#define IP_HLEN_    20
#define UDP_HLEN_    8
#define RTP_HLEN_   12
#define HDRS_LEN_   (ETH_HLEN_ + IP_HLEN_ + UDP_HLEN_ + RTP_HLEN_)   /* 54 */

/* G.711 a-law silence byte; payload content is irrelevant to Network MOS but a
 * realistic constant keeps captures sane. */
#define ALAW_SILENCE 0xD5

/* 12-byte RTP header (RFC 3550), wire order. */
struct rtp_hdr {
    uint8_t  vpxcc;   /* version(2)<<6 | padding | extension | csrc count */
    uint8_t  mpt;     /* marker<<7 | payload type */
    uint16_t seq;     /* big-endian */
    uint32_t ts;      /* big-endian, +samples_per_packet each packet */
    uint32_t ssrc;    /* big-endian, unique per flow */
} __attribute__((packed));

/* Standard one's-complement IP header checksum. */
static inline uint16_t ip_checksum(const void *data, int len)
{
    const uint16_t *p = (const uint16_t *)data;
    uint32_t sum = 0;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len) sum += *(const uint8_t *)p;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* Build one complete Ethernet/IP/UDP/RTP/payload frame into `buf`. Returns the
 * total frame length. Per-packet fields (seq, ts) are written here for the
 * first packet; the sender patches just those two on every subsequent send so
 * the expensive header/checksum work happens once per flow. UDP checksum is
 * left 0 (legal and optional for IPv4) so the payload never has to be summed. */
static inline int build_frame(uint8_t *buf,
                              const uint8_t src_mac[6], const uint8_t dst_mac[6],
                              uint32_t src_ip, uint32_t dst_ip,
                              uint16_t src_port, uint16_t dst_port,
                              uint8_t payload_type, uint32_t ssrc,
                              uint16_t seq, uint32_t ts,
                              const uint8_t *payload, int payload_len)
{
    struct ethhdr *eth = (struct ethhdr *)(buf);
    struct iphdr  *ip  = (struct iphdr  *)(buf + ETH_HLEN_);
    struct udphdr *udp = (struct udphdr *)(buf + ETH_HLEN_ + IP_HLEN_);
    struct rtp_hdr *rtp = (struct rtp_hdr *)(buf + ETH_HLEN_ + IP_HLEN_ + UDP_HLEN_);
    uint8_t *pl = buf + HDRS_LEN_;
    int udp_len = UDP_HLEN_ + RTP_HLEN_ + payload_len;
    int ip_len  = IP_HLEN_ + udp_len;

    memcpy(eth->h_dest, dst_mac, 6);
    memcpy(eth->h_source, src_mac, 6);
    eth->h_proto = htons(ETH_P_IP);

    memset(ip, 0, IP_HLEN_);
    ip->ihl = 5; ip->version = 4; ip->tos = 0;
    ip->tot_len = htons(ip_len);
    ip->id = htons(src_port);      /* cheap per-flow id */
    ip->frag_off = 0; ip->ttl = 64; ip->protocol = IPPROTO_UDP;
    ip->saddr = src_ip; ip->daddr = dst_ip;
    ip->check = 0;
    ip->check = ip_checksum(ip, IP_HLEN_);

    udp->source = htons(src_port);
    udp->dest = htons(dst_port);
    udp->len = htons(udp_len);
    udp->check = 0;

    rtp->vpxcc = 0x80;             /* version 2, no padding/extension/csrc */
    rtp->mpt = payload_type & 0x7F;
    rtp->seq = htons(seq);
    rtp->ts = htonl(ts);
    rtp->ssrc = htonl(ssrc);

    if (payload && payload_len)
        memcpy(pl, payload, payload_len);
    else
        memset(pl, ALAW_SILENCE, payload_len);

    return HDRS_LEN_ + payload_len;
}

#endif /* VQ_AFXDP_COMMON_H */
