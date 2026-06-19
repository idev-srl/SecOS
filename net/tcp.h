/*
 * tcp.h — [M24] TCP transport (RFC 793 subset).
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * A small but correct TCP: connection table, full open/close state machine,
 * cumulative ACKs, a sliding send window, in-order receive buffering, and a
 * retransmit timer driven from net_tick(). Active open (connect) and passive
 * open (listen/accept) are both supported with a tiny backlog. Single-threaded
 * (poll/shell context) so no locking. The blocking helpers spin on `hlt` so the
 * timer tick can drive RX (same idiom as the ping self-test / DHCP).
 */
#ifndef NET_TCP_H
#define NET_TCP_H
#include <stdint.h>
#include "net.h"

typedef enum {
    TCP_CLOSED = 0, TCP_LISTEN, TCP_SYN_SENT, TCP_SYN_RCVD, TCP_ESTABLISHED,
    TCP_FIN_WAIT_1, TCP_FIN_WAIT_2, TCP_CLOSE_WAIT, TCP_CLOSING, TCP_LAST_ACK,
    TCP_TIME_WAIT
} tcp_state_t;

typedef struct tcp_conn tcp_conn_t;

/* ---- non-blocking core (used by the socket layer) ---- */
tcp_conn_t* tcp_alloc(void);                              /* a free CLOSED conn, or NULL */
void        tcp_free(tcp_conn_t* c);                      /* release (aborts if open) */
int         tcp_start_connect(tcp_conn_t* c, net_dev_t* dev, uint32_t dip, uint16_t dport);
int         tcp_start_listen(tcp_conn_t* c, net_dev_t* dev, uint16_t port);
tcp_conn_t* tcp_accept_ready(tcp_conn_t* lc);             /* a newly ESTABLISHED child, or NULL */
int         tcp_write(tcp_conn_t* c, const void* data, uint32_t len);   /* bytes queued, <0 err */
int         tcp_read(tcp_conn_t* c, void* buf, uint32_t len);           /* bytes read; 0=none; -1=closed+empty */
int         tcp_start_close(tcp_conn_t* c);              /* begin orderly shutdown */
tcp_state_t tcp_get_state(const tcp_conn_t* c);
int         tcp_rx_available(const tcp_conn_t* c);       /* buffered receive bytes */

/* ---- blocking convenience (shell tcptest) ---- */
tcp_conn_t* tcp_connect(net_dev_t* dev, uint32_t dip, uint16_t dport);   /* NULL on fail */
int         tcp_send_all(tcp_conn_t* c, const void* data, uint32_t len); /* 0 ok */
int         tcp_recv_block(tcp_conn_t* c, void* buf, uint32_t len, uint32_t ms); /* bytes, 0=EOF/timeout */
void        tcp_close(tcp_conn_t* c);                    /* orderly close + free */

/* ---- stack hooks ---- */
void tcp_input(net_dev_t* dev, uint32_t src_ip, const uint8_t* seg, uint32_t len);
void tcp_tick(void);                                     /* retransmit + TIME_WAIT (from net_tick) */

#endif /* NET_TCP_H */
