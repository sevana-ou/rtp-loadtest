# rtp-loadtest — AF_XDP RTP sender + recvmmsg receiver

**Authored and maintained by [Sevana OÜ](https://sevana.biz).**
Copyright © 2026 Sevana OÜ. Licensed under the [Mozilla Public License
2.0](LICENSE).

---

A kernel-bypass G.711 RTP load generator (`rtp_afxdp_sender`) and a matching
high-rate UDP sink (`recvmmsg_receiver`). Together they establish a full
sender→receiver RTP flow that a passive listener can score for Network MOS —
without SIPp's per-packet `sendto()` ceiling.

## Why this exists

A SIPp-based generator bottlenecks at roughly **110k pps per VM**: SIPp issues one
`send()` syscall per RTP packet through the kernel UDP stack, and a single NIC
queue tops out there with the generator CPU still mostly idle. ptime is fixed at
20 ms, so the only way past the wall is to stop going through the kernel stack.

- **Sender → AF_XDP.** Each RTP packet is crafted straight into an AF_XDP UMEM
  frame and a whole tick's worth is handed to the NIC in one TX-ring submit. Per
  flow we keep userspace seq/timestamp/SSRC state (the thing `trafgen` can't do),
  so every flow is a clean, distinct 5-tuple RTP stream.
- **Receiver → recvmmsg.** Every flow targets the same fixed media port, so all of
  them land on **one socket** — which is exactly why `recvmmsg()` fits the receiver
  where `sendmmsg()` did *not* fit the per-call-socket sender. One syscall drains a
  whole batch.

## Measured throughput

One sender process, one TX queue, one core, G.711 at 20 ms ptime (50 pps/flow).
**Per-core TX depends far more on the NIC driver's AF_XDP path than on the CPU**,
so the platform matters — a lot:

| platform            | NIC / driver  |                        one-core TX |
|---------------------|---------------|-----------------------------------:|
| GCP `n2-standard-4` | gVNIC / `gve` | **2,005,260 pps** at 78% of a core |
| AWS `c5n.xlarge`    | ENA / `ena`   |          **~576k pps**, core-bound |

For reference, a SIPp `sendto()` generator on the same class of box walls out at
**~110k pps** while burning ~5,000 threads and ~2 cores.

### GCP gVNIC — n2-standard-4, native AF_XDP (measured 2026-06-27)

|  flows | target pps |     actual TX | sender CPU |
|-------:|-----------:|--------------:|-----------:|
|  2,000 |       100k |       100,457 |         3% |
| 10,000 |       500k |       503,022 |        23% |
| 20,000 |         1M |     1,000,121 |        39% |
| 40,000 |         2M | **2,005,260** |    **78%** |

Dead on target at every step — ~18x the SIPp wall. Note that **2M pps was the
highest point tested, not a measured ceiling**: the core still had ~22% headroom,
so where gve actually tops out is unknown.

gve needs the data NIC's *max* queues >= 2 (it reserves half for XDP) and the
default is 1, so create the NIC with `queue-count=2` and run
`ethtool -L <nic> rx 1 tx 1` before attaching.

### AWS ENA — c5n.xlarge (the outlier)

The sender **tops out at ~576k pps from one core** on ENA, roughly 3.5x below gve.
ENA's AF_XDP TX path is much slower per core — likely no zero-copy. AWS's
`pps_allowance_exceeded` counter stayed 0 throughout, so this is the driver's TX
path and not an AWS rate limit. Budget more generator instances on AWS
accordingly; one small VM will not saturate a large receiver there.

ENA native XDP also refuses an MTU > 3498 and a configured channel count > half
the device max, so lower the data NIC to MTU 1500 and run
`ethtool -L <nic> combined <max/2>` before attaching.

(This is a single observed ceiling figure rather than a full ramp — no per-step
flow counts or CPU were recorded, so treat it as less precise than the gve table.)

### veth loopback — single host, kernel 6.8

10,000 flows / ~500k pps on **~50% of one core** at 0.000% loss. This is the
`scripts/veth-test.sh` smoke path, not a NIC benchmark — veth is loopback and
reorders within a burst.

Treat all of these as the shape of the curve — linear in flows, driver-bound
rather than CPU-bound — rather than a guarantee on your hardware.

## Requirements

- Linux with AF_XDP (kernel 5.4+; developed and measured on 6.8)
- `libxdp-dev`, `libbpf-dev`, `build-essential`
- `CAP_NET_RAW` + `CAP_BPF` for the sender (run via `sudo`)
- For **native** (zero-copy) AF_XDP the driver must support XDP. Several drivers
  reserve half the NIC's queues for XDP and so refuse to attach unless the *max*
  queue count is >= 2 — see the per-platform notes under "Measured throughput"
  for the gve and ENA specifics. The sender falls back to SKB/copy mode
  automatically if native binding fails, so it still runs either way — just
  slower.

## Build

```sh
sudo apt install libxdp-dev libbpf-dev build-essential
make
```

Binaries land in `bin/`.

## Quick local test (veth, no real NIC)

```sh
make
sudo ./scripts/veth-test.sh [flows] [seconds]      # default 1000 flows, 5s
```

Creates two netns + a veth pair, runs receiver↔sender across them, prints pps /
distinct sources / loss, and tears everything down.

## Usage

Sender (needs `CAP_NET_RAW` + `CAP_BPF`; run via sudo):

```sh
sudo ./bin/rtp_afxdp_sender --iface eth1 --dst-ip <receiver-ip> --dst-mac <next-hop-mac> \
    --flows 5000 --dst-port 40000 --src-port-base 10000 [--src-ip A] [--duration-sec T]
```

- `--dst-mac` is the **next-hop** MAC: the receiver's MAC if on the same L2, else
  the gateway's (`ip neigh get <dst-ip> dev <iface>`). The sender does no ARP.
- `--src-mac` defaults to the iface's MAC from sysfs.
- `--src-ips N` spreads flows over N consecutive source IPs. Receivers that hash
  RSS on IP alone will pin every flow to one RX queue from a single source IP, so
  raise this when you want the load fanned across the receiver's queues.
- Run one process per `--queue` to use multiple NIC queues.

`--help`-style usage for every flag is printed when required args are missing.

Receiver:

```sh
./bin/recvmmsg_receiver --port 40000 [--batch 256] [--rcvbuf $((128<<20))] [--no-track]
```

Reports pps / Mbps / distinct sources / reorder-proof RTP loss. `--no-track` drops
per-flow accounting for the lowest possible overhead.

## Deploying against a passive monitor

The sender's NIC **stays in the kernel** (AF_XDP attaches an XDP program; it does
not bind to vfio), so it coexists with everything else on the box. Point it at the
target's IP with `--dst-mac` = the next-hop MAC. A passive listener on the path
captures these RTP flows off the wire and scores them exactly as it would score
SIPp media — no SIP signaling is involved.

> Note: if the monitor's capture NIC is vfio-bound to a DPDK listener, the kernel
> can't also receive on it — so the `recvmmsg` *terminator* and the DPDK *listener*
> can't share that NIC. Either put the terminator on a second NIC, or rely on the
> listener alone (for wire-only Network MOS the flow just needs to be deliverable).

## Scope / caveats

- **IPv4 + plain RTP/AVP only.** No SRTP, no IPv6, no RTCP, no SIP signaling.
- **UDP checksum is 0** (legal/optional for IPv4) so the payload is never summed.
- Per-flow **source ports** carry stream identity; don't collapse them.
- The veth path reorders within a burst — that's reorder, not loss (the receiver's
  min/max/count metric separates them). A real NIC behaves differently.
- 16-bit RTP seq wraps after 65,536 pkts/flow (~21 min @ 50 pps); the loss stat
  assumes no wrap, fine for typical runs.
- One TX queue per process; scale with `--queue` + multiple processes.

## License

Copyright © 2026 Sevana OÜ.

This Source Code Form is subject to the terms of the Mozilla Public License,
v. 2.0. If a copy of the MPL was not distributed with this file, You can obtain
one at <https://mozilla.org/MPL/2.0/>. The full text is in [LICENSE](LICENSE).

MPL-2.0 is a per-file copyleft: you may use these tools in a larger work under
your own terms, but modifications to *these* files must be shared under the same
licence.
