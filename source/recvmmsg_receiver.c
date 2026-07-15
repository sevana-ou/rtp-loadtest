/* Copyright (c) 2026 Sevana OÜ
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/* recvmmsg_receiver.c - high-rate UDP/RTP sink using recvmmsg().
 *
 * Counterpart to rtp_afxdp_sender. Every flow targets the same fixed UDP port
 * (the UAS media port), so all of them land on ONE socket - which is exactly
 * why recvmmsg() fits here where sendmmsg() didn't fit the per-call-socket
 * sender: one syscall drains a whole batch of datagrams instead of one per
 * packet, lifting the kernel RX ceiling well past plain recvfrom().
 *
 * It just absorbs and accounts (pps / Mbps / distinct sources / RTP loss) so the
 * flow is "established" and you can confirm what actually arrived. It is not a
 * full RTP endpoint (no RTCP, no playout).
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
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "common.h"

#define MAX_BATCH    1024
#define BUF_SIZE     2048
#define TRACK_SLOTS  (1 << 20)     /* open-addressed table for distinct 5-tuples */

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static double now_sec(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

/* cheap hash of (src ip, src port) for a distinct-flow estimate */
static uint32_t srckey(const struct sockaddr_in *a)
{
    uint32_t h = a->sin_addr.s_addr * 2654435761u ^ (a->sin_port * 40503u);
    return h & (TRACK_SLOTS - 1);
}

static void usage(const char *p)
{
    fprintf(stderr,
        "Usage: %s [--port P] [--batch N] [--rcvbuf BYTES] [--report-sec S] [--no-track]\n"
        "  --port P         UDP port to bind (default 40000)\n"
        "  --batch N        datagrams per recvmmsg (default 256, max %d)\n"
        "  --rcvbuf BYTES   SO_RCVBUF (default 64 MiB)\n"
        "  --report-sec S   stats interval (default 1)\n"
        "  --no-track       skip distinct-source / loss accounting (lowest overhead)\n",
        p, MAX_BATCH);
}

int main(int argc, char **argv)
{
    int port = 40000, batch = 256, report_sec = 1, track = 1;
    long rcvbuf = 64L << 20;

    for (int i = 1; i < argc; i++) {
        #define ARG(flag) (strcmp(argv[i], flag) == 0)
        #define VAL (i + 1 < argc ? argv[++i] : (usage(argv[0]), exit(1), ""))
        if      (ARG("--port")) port = atoi(VAL);
        else if (ARG("--batch")) batch = atoi(VAL);
        else if (ARG("--rcvbuf")) rcvbuf = atol(VAL);
        else if (ARG("--report-sec")) report_sec = atoi(VAL);
        else if (ARG("--no-track")) track = 0;
        else { usage(argv[0]); return 1; }
    }
    if (batch < 1 || batch > MAX_BATCH) { fprintf(stderr, "bad --batch\n"); return 1; }

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)))
        fprintf(stderr, "warning: SO_RCVBUF: %s\n", strerror(errno));
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(port),
                                .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr))) { perror("bind"); return 1; }
    /* So a quiet socket still returns periodically and we notice g_stop. */
    struct timeval rcvto = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &rcvto, sizeof(rcvto));

    /* recvmmsg scatter buffers */
    struct mmsghdr *msgs = calloc(batch, sizeof(*msgs));
    struct iovec *iov = calloc(batch, sizeof(*iov));
    uint8_t *bufs = malloc((size_t)batch * BUF_SIZE);
    struct sockaddr_in *srcs = calloc(batch, sizeof(*srcs));
    for (int i = 0; i < batch; i++) {
        iov[i].iov_base = bufs + (size_t)i * BUF_SIZE;
        iov[i].iov_len = BUF_SIZE;
        msgs[i].msg_hdr.msg_iov = &iov[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
        msgs[i].msg_hdr.msg_name = &srcs[i];
        msgs[i].msg_hdr.msg_namelen = sizeof(srcs[i]);
    }

    /* Per-source min/max seq + count -> reorder-proof loss = (max-min+1)-count.
     * (Assumes no 16-bit seq wrap, i.e. < 65536 pkts/flow ~= 21 min @ 50 pps.) */
    uint8_t  *seen  = track ? calloc(TRACK_SLOTS, 1) : NULL;
    uint16_t *smin  = track ? calloc(TRACK_SLOTS, sizeof(uint16_t)) : NULL;
    uint16_t *smax  = track ? calloc(TRACK_SLOTS, sizeof(uint16_t)) : NULL;
    uint32_t *scnt  = track ? calloc(TRACK_SLOTS, sizeof(uint32_t)) : NULL;
    uint32_t *active = track ? calloc(TRACK_SLOTS, sizeof(uint32_t)) : NULL;
    uint32_t nactive = 0;

    /* sigaction WITHOUT SA_RESTART so a blocked recvmmsg returns EINTR on signal
     * (signal() would set SA_RESTART and recvmmsg would never unblock). */
    struct sigaction sa = { .sa_handler = on_signal };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    fprintf(stderr, "recvmmsg receiver: bound :%d, batch %d, rcvbuf %ld MiB%s\n",
            port, batch, rcvbuf >> 20, track ? "" : ", no-track");

    double t_start = now_sec(), t_report = t_start;
    unsigned long long total = 0, bytes = 0, since = 0, sbytes = 0;

    while (!g_stop) {
        int n = recvmmsg(s, msgs, batch, MSG_WAITFORONE, NULL);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            perror("recvmmsg"); break;
        }
        for (int i = 0; i < n; i++) {
            unsigned len = msgs[i].msg_len;
            total++; bytes += len; since++; sbytes += len;
            if (track && len >= RTP_HLEN_) {
                /* msg starts at the UDP payload (RTP header) */
                const struct rtp_hdr *rtp = (const struct rtp_hdr *)iov[i].iov_base;
                uint16_t seq = ntohs(rtp->seq);
                uint32_t k = srckey(&srcs[i]);
                if (!seen[k]) {
                    seen[k] = 1; active[nactive++] = k;
                    smin[k] = smax[k] = seq; scnt[k] = 1;
                } else {
                    scnt[k]++;
                    if (seq < smin[k]) smin[k] = seq;
                    if (seq > smax[k]) smax[k] = seq;
                }
            }
        }
        double t = now_sec();
        if (t - t_report >= report_sec) {
            double dt = t - t_report;
            fprintf(stderr, "  rx %.0f pps (%.1f Mpps, %.2f Gbps)\n",
                    since / dt, since / dt / 1e6, sbytes * 8.0 / dt / 1e9);
            if (track) {
                unsigned long long lost = 0;
                for (uint32_t a = 0; a < nactive; a++) {
                    uint32_t k = active[a];
                    unsigned long long span = (unsigned)(smax[k] - smin[k]) + 1;
                    if (span > scnt[k]) lost += span - scnt[k];
                }
                fprintf(stderr, "     distinct sources=%u  loss=%llu (%.3f%%)\n",
                        nactive, lost, total ? 100.0 * lost / (lost + total) : 0.0);
            }
            since = 0; sbytes = 0; t_report = t;
        }
    }
    unsigned long long lost = 0;
    if (track)
        for (uint32_t a = 0; a < nactive; a++) {
            uint32_t k = active[a];
            unsigned long long span = (unsigned)(smax[k] - smin[k]) + 1;
            if (span > scnt[k]) lost += span - scnt[k];
        }

    double elapsed = now_sec() - t_start;
    fprintf(stderr,
        "stopped: %llu packets, %.1f MB in %.1fs = %.0f pps avg, %.2f Gbps avg\n",
        total, bytes / 1e6, elapsed,
        elapsed > 0 ? total / elapsed : 0.0,
        elapsed > 0 ? bytes * 8.0 / elapsed / 1e9 : 0.0);
    if (track)
        fprintf(stderr, "  distinct sources=%u, RTP loss=%llu (%.3f%%)\n",
                nactive, lost, total ? 100.0 * lost / (lost + total) : 0.0);
    return 0;
}
