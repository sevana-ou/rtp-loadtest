/* Copyright (c) 2026 Sevana OÜ
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/* rtp_afxdp_sender.c - kernel-bypass G.711 RTP load generator over AF_XDP.
 *
 * Why AF_XDP: a SIPp/sendto() generator caps at ~110k pps per gve queue because
 * every packet is its own syscall through the kernel UDP stack. This tool crafts
 * each RTP packet straight into an AF_XDP UMEM frame and hands a whole tick's
 * worth to the NIC in one TX-ring submit, bypassing the stack entirely.
 *
 * Model: N flows (calls), each with its own UDP source port + RTP SSRC and its
 * own monotonic seq/timestamp held in userspace (this per-flow state is exactly
 * what trafgen can't keep). Each flow owns one UMEM frame, built once; every
 * ptime (20 ms) tick we patch only seq+ts and resubmit, so the per-packet cost
 * is ~6 bytes + a descriptor.
 *
 * Scope: IPv4, plain RTP/AVP (no SRTP), one TX queue per process. For more than
 * one queue's worth of pps, run one process per --queue. Needs CAP_NET_RAW +
 * CAP_BPF (run via sudo). See README.md.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <net/if.h>
#include <linux/if_link.h>
#include <linux/if_xdp.h>
#include <xdp/xsk.h>

#include "common.h"

#define FRAME_SIZE 2048
#define RESERVE_BATCH 256          /* TX descriptors reserved per attempt */

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

struct flow_state {
    uint16_t seq;
    uint32_t ts;
    uint16_t pkt_len;
    uint64_t frame_off;            /* byte offset of this flow's frame in UMEM */
};

static uint32_t pow2_ceil(uint32_t v) { uint32_t p = 1; while (p < v) p <<= 1; return p; }

static int parse_mac(const char *s, uint8_t mac[6])
{
    return sscanf(s, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                  &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) == 6 ? 0 : -1;
}

static int mac_from_sysfs(const char *iface, uint8_t mac[6])
{
    char path[256], buf[64];
    snprintf(path, sizeof(path), "/sys/class/net/%s/address", iface);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int ok = fgets(buf, sizeof(buf), f) ? parse_mac(buf, mac) : -1;
    fclose(f);
    return ok;
}

static double now_sec(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void usage(const char *p)
{
    fprintf(stderr,
        "Usage: %s --iface IF --dst-ip IP --dst-mac MAC [options]\n"
        "  --iface IF            egress interface (AF_XDP bound here)\n"
        "  --queue Q             NIC queue id (default 0)\n"
        "  --flows N             number of concurrent RTP flows/calls (default 1000)\n"
        "  --src-ip IP           source IPv4, or first IP if --src-ips > 1 (default: 10.0.0.1)\n"
        "  --src-ips N           spread flows over N consecutive src IPs (default 1).\n"
        "                        Needed to fan load across RX queues: vq-core hashes\n"
        "                        RSS on IP only, so one src IP pins all flows to one queue.\n"
        "  --dst-ip IP           destination IPv4 (the receiver)            [required]\n"
        "  --dst-mac MAC         next-hop MAC (gateway, or receiver on same L2) [required]\n"
        "  --src-mac MAC         source MAC (default: read from iface)\n"
        "  --dst-port P          destination UDP port (default 40000)\n"
        "  --src-port-base P     first flow's UDP source port (default 10000)\n"
        "  --ssrc-base S         first flow's RTP SSRC (default 0x10000000)\n"
        "  --ptime-ms MS         packetization interval (default 20)\n"
        "  --payload-bytes B     RTP payload size (default 160 = 20ms G.711)\n"
        "  --pt N                RTP payload type (default 8 = PCMA)\n"
        "  --duration-sec T      stop after T seconds (default 0 = until Ctrl-C)\n"
        "  --skb                 force SKB/copy mode (default: try native, fall back)\n",
        p);
}

int main(int argc, char **argv)
{
    const char *iface = NULL, *dst_ip_s = NULL, *dst_mac_s = NULL, *src_mac_s = NULL;
    const char *src_ip_s = "10.0.0.1";
    uint32_t queue = 0, flows = 1000;
    uint16_t dst_port = 40000, src_port_base = 10000;
    uint32_t ssrc_base = 0x10000000;
    int ptime_ms = 20, payload_bytes = 160, pt = 8, duration = 0, force_skb = 0;
    uint32_t src_ips = 1;          /* spread flows over this many consecutive src IPs */

    static const struct { const char *name; int has_arg; } opts[] = { {0,0} };
    (void)opts;
    for (int i = 1; i < argc; i++) {
        #define ARG(flag) (strcmp(argv[i], flag) == 0)
        #define VAL (i + 1 < argc ? argv[++i] : (usage(argv[0]), exit(1), ""))
        if      (ARG("--iface")) iface = VAL;
        else if (ARG("--queue")) queue = atoi(VAL);
        else if (ARG("--flows")) flows = atoi(VAL);
        else if (ARG("--src-ip")) src_ip_s = VAL;
        else if (ARG("--src-ips")) src_ips = atoi(VAL);
        else if (ARG("--dst-ip")) dst_ip_s = VAL;
        else if (ARG("--dst-mac")) dst_mac_s = VAL;
        else if (ARG("--src-mac")) src_mac_s = VAL;
        else if (ARG("--dst-port")) dst_port = atoi(VAL);
        else if (ARG("--src-port-base")) src_port_base = atoi(VAL);
        else if (ARG("--ssrc-base")) ssrc_base = strtoul(VAL, NULL, 0);
        else if (ARG("--ptime-ms")) ptime_ms = atoi(VAL);
        else if (ARG("--payload-bytes")) payload_bytes = atoi(VAL);
        else if (ARG("--pt")) pt = atoi(VAL);
        else if (ARG("--duration-sec")) duration = atoi(VAL);
        else if (ARG("--skb")) force_skb = 1;
        else { usage(argv[0]); return 1; }
    }
    if (!iface || !dst_ip_s || !dst_mac_s) { usage(argv[0]); return 1; }
    if (flows < 1 || payload_bytes < 1 || payload_bytes > FRAME_SIZE - HDRS_LEN_) {
        fprintf(stderr, "invalid --flows/--payload-bytes\n"); return 1;
    }

    uint8_t src_mac[6], dst_mac[6];
    if (parse_mac(dst_mac_s, dst_mac)) { fprintf(stderr, "bad --dst-mac\n"); return 1; }
    if (src_mac_s ? parse_mac(src_mac_s, src_mac) : mac_from_sysfs(iface, src_mac)) {
        fprintf(stderr, "bad/unknown --src-mac (set it explicitly)\n"); return 1;
    }
    uint32_t src_ip, dst_ip;
    if (inet_pton(AF_INET, src_ip_s, &src_ip) != 1 ||
        inet_pton(AF_INET, dst_ip_s, &dst_ip) != 1) {
        fprintf(stderr, "bad --src-ip/--dst-ip\n"); return 1;
    }

    /* AF_XDP UMEM must be locked memory. */
    struct rlimit rl = { RLIM_INFINITY, RLIM_INFINITY };
    if (setrlimit(RLIMIT_MEMLOCK, &rl))
        fprintf(stderr, "warning: setrlimit(MEMLOCK) failed: %s\n", strerror(errno));

    uint64_t num_frames = flows;                 /* one frame per flow */
    uint64_t umem_size = num_frames * FRAME_SIZE;
    void *umem_area = mmap(NULL, umem_size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (umem_area == MAP_FAILED) { perror("mmap umem"); return 1; }

    uint32_t ring = pow2_ceil(flows);
    if (ring < 2048) ring = 2048;
    if (ring > 8192) ring = 8192;

    struct xsk_umem *umem;
    struct xsk_ring_prod fq;        /* fill ring: unused for pure TX but required */
    struct xsk_ring_cons cq;        /* completion ring */
    struct xsk_umem_config ucfg = {
        .fill_size = 2048, .comp_size = ring,
        .frame_size = FRAME_SIZE, .frame_headroom = 0, .flags = 0,
    };
    if (xsk_umem__create(&umem, umem_area, umem_size, &fq, &cq, &ucfg)) {
        fprintf(stderr, "xsk_umem__create: %s\n", strerror(errno)); return 1;
    }

    struct xsk_socket *xsk;
    struct xsk_ring_cons rx;        /* unused */
    struct xsk_ring_prod tx;
    struct xsk_socket_config scfg = {
        .rx_size = 2048, .tx_size = ring, .libxdp_flags = 0,
        .xdp_flags = force_skb ? XDP_FLAGS_SKB_MODE : 0,
        .bind_flags = XDP_USE_NEED_WAKEUP,
    };
    int err = xsk_socket__create(&xsk, iface, queue, umem, &rx, &tx, &scfg);
    if (err && !force_skb) {        /* native mode unavailable -> fall back to copy/SKB */
        fprintf(stderr, "native AF_XDP failed (%s); retrying in SKB mode\n", strerror(-err));
        scfg.xdp_flags = XDP_FLAGS_SKB_MODE;
        scfg.bind_flags = XDP_USE_NEED_WAKEUP;
        err = xsk_socket__create(&xsk, iface, queue, umem, &rx, &tx, &scfg);
    }
    if (err) {
        fprintf(stderr, "xsk_socket__create(%s q%u): %s\n", iface, queue, strerror(-err));
        return 1;
    }
    int xfd = xsk_socket__fd(xsk);

    /* Build every flow's frame once. */
    struct flow_state *fs = calloc(flows, sizeof(*fs));
    for (uint32_t i = 0; i < flows; i++) {
        fs[i].frame_off = (uint64_t)i * FRAME_SIZE;
        fs[i].seq = 0;
        fs[i].ts = 0;
        uint8_t *frame = (uint8_t *)xsk_umem__get_data(umem_area, fs[i].frame_off);
        /* Spread flows over N consecutive source IPs. vq-core hashes RSS on IP
         * only (RTE_ETH_RSS_IP), so a single src IP pins every flow to one RX
         * queue; cycling the src IP is what fans the load across all N queues. */
        uint32_t fsrc = (src_ips > 1)
                            ? htonl(ntohl(src_ip) + (i % src_ips))
                            : src_ip;
        fs[i].pkt_len = build_frame(frame, src_mac, dst_mac, fsrc, dst_ip,
                                    src_port_base + i, dst_port, pt,
                                    ssrc_base + i, 0, 0, NULL, payload_bytes);
    }
    uint32_t samples_per_pkt = payload_bytes;    /* G.711: 1 byte == 1 sample @ 8kHz */

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    fprintf(stderr,
        "AF_XDP sender: %u flows on %s q%u, %d ms ptime, %d B payload\n"
        "  %u->%u/udp, %.0f pps total target (%.0f pps/flow)\n",
        flows, iface, queue, ptime_ms, payload_bytes, src_port_base, dst_port,
        flows * 1000.0 / ptime_ms, 1000.0 / ptime_ms);

    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    double t_start = now_sec(), t_report = t_start;
    unsigned long long total = 0, since_report = 0;
    long tick_ns = (long)ptime_ms * 1000000L;

    while (!g_stop) {
        /* one tick: every flow emits exactly one packet */
        uint32_t done = 0;
        while (done < flows && !g_stop) {
            uint32_t want = flows - done;
            if (want > RESERVE_BATCH) want = RESERVE_BATCH;
            uint32_t idx;
            uint32_t got = xsk_ring_prod__reserve(&tx, want, &idx);
            if (got == 0) {
                /* TX ring full: kick the NIC and reclaim completed slots */
                if (xsk_ring_prod__needs_wakeup(&tx))
                    sendto(xfd, NULL, 0, MSG_DONTWAIT, NULL, 0);
                uint32_t cidx, c = xsk_ring_cons__peek(&cq, ring, &cidx);
                if (c) xsk_ring_cons__release(&cq, c);
                continue;
            }
            for (uint32_t j = 0; j < got; j++) {
                struct flow_state *f = &fs[done + j];
                uint8_t *frame = (uint8_t *)xsk_umem__get_data(umem_area, f->frame_off);
                struct rtp_hdr *rtp =
                    (struct rtp_hdr *)(frame + ETH_HLEN_ + IP_HLEN_ + UDP_HLEN_);
                rtp->seq = htons(f->seq++);
                rtp->ts = htonl(f->ts);
                f->ts += samples_per_pkt;
                struct xdp_desc *d = xsk_ring_prod__tx_desc(&tx, idx + j);
                d->addr = f->frame_off;
                d->len = f->pkt_len;
            }
            xsk_ring_prod__submit(&tx, got);
            done += got;
            since_report += got; total += got;
            if (xsk_ring_prod__needs_wakeup(&tx))
                sendto(xfd, NULL, 0, MSG_DONTWAIT, NULL, 0);
            uint32_t cidx, c = xsk_ring_cons__peek(&cq, ring, &cidx);
            if (c) xsk_ring_cons__release(&cq, c);
        }

        /* per-second stats */
        double t = now_sec();
        if (t - t_report >= 1.0) {
            fprintf(stderr, "  tx %.0f pps (%.1f Mpps), %llu pkts total\n",
                    since_report / (t - t_report), since_report / (t - t_report) / 1e6,
                    total);
            since_report = 0; t_report = t;
        }
        if (duration && t - t_start >= duration) break;

        /* sleep to the next tick boundary */
        next.tv_nsec += tick_ns;
        while (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }

    /* drain outstanding completions */
    for (int i = 0; i < 100; i++) {
        uint32_t cidx, c = xsk_ring_cons__peek(&cq, ring, &cidx);
        if (c) xsk_ring_cons__release(&cq, c);
        else break;
    }
    double elapsed = now_sec() - t_start;
    fprintf(stderr, "stopped: %llu packets in %.1fs = %.0f pps avg\n",
            total, elapsed, elapsed > 0 ? total / elapsed : 0.0);

    xsk_socket__delete(xsk);
    xsk_umem__delete(umem);
    munmap(umem_area, umem_size);
    free(fs);
    return 0;
}
