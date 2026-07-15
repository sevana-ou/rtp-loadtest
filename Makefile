# Copyright (c) 2026 Sevana OÜ
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

# AF_XDP RTP sender + recvmmsg receiver.
# Deps (Ubuntu/Debian): apt install libxdp-dev libbpf-dev build-essential
CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
LDLIBS  := -lxdp -lbpf

SRCDIR  := source
BINDIR  := bin

BINS := $(BINDIR)/rtp_afxdp_sender $(BINDIR)/recvmmsg_receiver

all: $(BINS)

$(BINDIR):
	mkdir -p $(BINDIR)

# Only the sender needs libxdp/libbpf; the receiver is plain sockets, so it still
# builds on a host without the XDP headers.
$(BINDIR)/rtp_afxdp_sender: $(SRCDIR)/rtp_afxdp_sender.c $(SRCDIR)/common.h | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

$(BINDIR)/recvmmsg_receiver: $(SRCDIR)/recvmmsg_receiver.c $(SRCDIR)/common.h | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -rf $(BINDIR)

.PHONY: all clean
