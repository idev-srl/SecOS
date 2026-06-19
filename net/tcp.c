/*
 * tcp.c — [M24] TCP transport (RFC 793 subset).
 * Copyright (c) 2026 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * Connection table + state machine (active and passive open), cumulative ACKs, a
 * sliding send window, in-order receive buffering, and a retransmit timer driven
 * from net_tick(). Out-of-order segments are dropped and re-ACKed (the peer
 * retransmits) — correct, if not fast. Good enough for a client doing an HTTP
 * GET and a tiny echo server.
 */
#include "tcp.h"
#include "debugcon.h"
#include <stddef.h>

extern uint64_t timer_get_ticks(void);

#define IP_PROTO_TCP 6
#define TCP_MAX_CONN 16
#define TCP_RXBUF    8192
#define TCP_TXBUF    8192
#define TCP_MSS      1460
#define TCP_RTO      600           /* ~600 ms at 1 kHz */
#define TCP_MAX_RETX 6
#define TCP_TIMEWAIT 2000          /* ~2 s linger */

/* flags */
#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_PSH 0x08
#define F_ACK 0x10

struct tcp_conn {
    int          used;
    tcp_state_t  state;
    net_dev_t*   dev;
    uint32_t     remote_ip;            /* network-order octets */
    uint16_t     local_port, remote_port;
    uint32_t     iss, snd_una, snd_nxt;
    uint32_t     rcv_nxt;
    uint16_t     snd_wnd;
    uint8_t      tx[TCP_TXBUF]; uint32_t tx_len;   /* bytes from snd_una onward (unacked+unsent) */
    uint8_t      rx[TCP_RXBUF]; uint32_t rx_head, rx_len;
    uint8_t      fin_pending;          /* user asked to close; send FIN after tx drains */
    uint8_t      fin_sent;             /* our FIN occupies a sequence number */
    uint8_t      reset;                /* peer RST / abort */
    uint64_t     rto_deadline; int retries;
    uint64_t     tw_deadline;          /* TIME_WAIT expiry */
    struct tcp_conn* listener;         /* parent LISTEN conn (passive children) */
    uint8_t      accepted;             /* child handed out by tcp_accept_ready */
};

static struct tcp_conn g_conns[TCP_MAX_CONN];
static uint16_t g_eph = 49152;
static uint32_t g_iss_seed = 0x1000;
static uint8_t  g_seg[1600];           /* TX scratch (header + data) */
static uint8_t  g_cs[12 + 1600];       /* pseudo-header + segment for checksum */

static inline int seq_lt(uint32_t a, uint32_t b){ return (int32_t)(a-b) < 0; }
static inline int seq_leq(uint32_t a, uint32_t b){ return (int32_t)(a-b) <= 0; }
static inline int seq_gt(uint32_t a, uint32_t b){ return (int32_t)(a-b) > 0; }

static uint16_t eph_port(void){
    for(int i=0;i<16384;i++){
        uint16_t p = g_eph++; if(g_eph==0) g_eph=49152;
        int taken=0;
        for(int j=0;j<TCP_MAX_CONN;j++)
            if(g_conns[j].used && g_conns[j].local_port==p){ taken=1; break; }
        if(!taken) return p;
    }
    return 0;
}

tcp_conn_t* tcp_alloc(void){
    for(int i=0;i<TCP_MAX_CONN;i++) if(!g_conns[i].used){
        struct tcp_conn* c=&g_conns[i];
        for(size_t k=0;k<sizeof(*c);k++) ((uint8_t*)c)[k]=0;
        c->used=1; c->state=TCP_CLOSED; return c;
    }
    return NULL;
}
void tcp_free(tcp_conn_t* c){ if(c){ c->used=0; c->state=TCP_CLOSED; } }

tcp_state_t tcp_get_state(const tcp_conn_t* c){ return c?c->state:TCP_CLOSED; }
int tcp_rx_available(const tcp_conn_t* c){ return c?(int)c->rx_len:0; }

// ---- segment transmit ----
static void tcp_xmit(struct tcp_conn* c, uint8_t flags, const uint8_t* data, uint32_t dlen, uint32_t seq){
    uint32_t hdr = 20;
    uint8_t* s = g_seg;
    s[0]=(uint8_t)(c->local_port>>8);  s[1]=(uint8_t)c->local_port;
    s[2]=(uint8_t)(c->remote_port>>8); s[3]=(uint8_t)c->remote_port;
    s[4]=(uint8_t)(seq>>24); s[5]=(uint8_t)(seq>>16); s[6]=(uint8_t)(seq>>8); s[7]=(uint8_t)seq;
    uint32_t ack = c->rcv_nxt;
    s[8]=(uint8_t)(ack>>24); s[9]=(uint8_t)(ack>>16); s[10]=(uint8_t)(ack>>8); s[11]=(uint8_t)ack;
    /* data offset filled after MSS option decision */
    s[13]=flags;
    uint32_t freerx = TCP_RXBUF - c->rx_len; if(freerx>65535) freerx=65535;
    s[14]=(uint8_t)(freerx>>8); s[15]=(uint8_t)freerx;
    s[16]=0; s[17]=0;                   /* checksum */
    s[18]=0; s[19]=0;                   /* urgent */
    if(flags & F_SYN){                  /* MSS option */
        s[20]=2; s[21]=4; s[22]=(uint8_t)(TCP_MSS>>8); s[23]=(uint8_t)TCP_MSS;
        hdr=24;
    }
    s[12]=(uint8_t)((hdr/4)<<4);
    for(uint32_t i=0;i<dlen;i++) s[hdr+i]=data[i];
    uint32_t seglen = hdr + dlen;

    /* pseudo-header checksum */
    uint8_t* ph=g_cs;
    uint32_t sip=c->dev->ip, dip=c->remote_ip;
    ph[0]=(uint8_t)sip; ph[1]=(uint8_t)(sip>>8); ph[2]=(uint8_t)(sip>>16); ph[3]=(uint8_t)(sip>>24);
    ph[4]=(uint8_t)dip; ph[5]=(uint8_t)(dip>>8); ph[6]=(uint8_t)(dip>>16); ph[7]=(uint8_t)(dip>>24);
    ph[8]=0; ph[9]=IP_PROTO_TCP; ph[10]=(uint8_t)(seglen>>8); ph[11]=(uint8_t)seglen;
    for(uint32_t i=0;i<seglen;i++) g_cs[12+i]=s[i];
    uint16_t csum = net_checksum(g_cs, 12+seglen);
    s[16]=(uint8_t)(csum>>8); s[17]=(uint8_t)csum;

    ipv4_send(c->dev, c->remote_ip, IP_PROTO_TCP, s, seglen);
}

static void arm_rto(struct tcp_conn* c){ c->rto_deadline = timer_get_ticks()+TCP_RTO; c->retries=0; }

// Flush newly-sendable data (and a pending FIN) from the tx buffer within window.
static void tcp_output(struct tcp_conn* c){
    if(c->state!=TCP_ESTABLISHED && c->state!=TCP_CLOSE_WAIT &&
       c->state!=TCP_FIN_WAIT_1 && c->state!=TCP_FIN_WAIT_2) {
        /* only ESTABLISHED-ish states send data */
    }
    uint32_t sent_off = c->snd_nxt - c->snd_una;       /* bytes already sent from tx */
    uint32_t wnd = c->snd_wnd ? c->snd_wnd : 1;        /* allow 1 byte if window 0 (probe) */
    while(sent_off < c->tx_len){
        uint32_t avail = c->tx_len - sent_off;
        uint32_t inflight = sent_off;                  /* unacked already in flight */
        if(inflight >= wnd) break;                     /* window full */
        uint32_t can = wnd - inflight;
        uint32_t chunk = avail; if(chunk>TCP_MSS) chunk=TCP_MSS; if(chunk>can) chunk=can;
        if(chunk==0) break;
        tcp_xmit(c, F_ACK|F_PSH, c->tx+sent_off, chunk, c->snd_nxt);
        c->snd_nxt += chunk; sent_off += chunk;
        arm_rto(c);
    }
    /* All queued data sent and a close is pending → send FIN. */
    if(c->fin_pending && !c->fin_sent && (c->snd_nxt - c->snd_una) == c->tx_len){
        tcp_xmit(c, F_ACK|F_FIN, NULL, 0, c->snd_nxt);
        c->fin_sent=1; c->snd_nxt++; arm_rto(c);
        if(c->state==TCP_ESTABLISHED) c->state=TCP_FIN_WAIT_1;
        else if(c->state==TCP_CLOSE_WAIT) c->state=TCP_LAST_ACK;
    }
}

// ---- public non-blocking API ----
int tcp_start_connect(tcp_conn_t* c, net_dev_t* dev, uint32_t dip, uint16_t dport){
    if(!c||!dev) return -1;
    c->dev=dev; c->remote_ip=dip; c->remote_port=dport;
    c->local_port=eph_port();
    c->iss = (g_iss_seed += 0x9E37) ^ (uint32_t)timer_get_ticks();
    c->snd_una=c->iss; c->snd_nxt=c->iss; c->rcv_nxt=0;
    c->state=TCP_SYN_SENT;
    tcp_xmit(c, F_SYN, NULL, 0, c->iss);
    c->snd_nxt = c->iss + 1;
    arm_rto(c);
    return 0;
}

int tcp_start_listen(tcp_conn_t* c, net_dev_t* dev, uint16_t port){
    if(!c||!dev) return -1;
    c->dev=dev; c->local_port=port; c->state=TCP_LISTEN; return 0;
}

tcp_conn_t* tcp_accept_ready(tcp_conn_t* lc){
    if(!lc) return NULL;
    for(int i=0;i<TCP_MAX_CONN;i++){
        struct tcp_conn* c=&g_conns[i];
        if(c->used && c->listener==lc && !c->accepted && c->state==TCP_ESTABLISHED){
            c->accepted=1; return c;
        }
    }
    return NULL;
}

int tcp_write(tcp_conn_t* c, const void* data, uint32_t len){
    if(!c||(c->state!=TCP_ESTABLISHED && c->state!=TCP_CLOSE_WAIT)) return -1;
    uint32_t space = TCP_TXBUF - c->tx_len;
    if(len>space) len=space;
    const uint8_t* p=(const uint8_t*)data;
    for(uint32_t i=0;i<len;i++) c->tx[c->tx_len+i]=p[i];
    c->tx_len += len;
    tcp_output(c);
    return (int)len;
}

int tcp_read(tcp_conn_t* c, void* buf, uint32_t len){
    if(!c) return -1;
    if(c->rx_len==0){
        if(c->state==TCP_CLOSE_WAIT || c->state==TCP_CLOSED ||
           c->state==TCP_LAST_ACK || c->state==TCP_TIME_WAIT) return -1;  /* peer closed, drained */
        return 0;
    }
    uint32_t n = len; if(n>c->rx_len) n=c->rx_len;
    uint8_t* d=(uint8_t*)buf;
    for(uint32_t i=0;i<n;i++) d[i]=c->rx[(c->rx_head+i)%TCP_RXBUF];
    c->rx_head=(c->rx_head+n)%TCP_RXBUF; c->rx_len-=n;
    /* Window opened up → advertise it so a stalled sender resumes. */
    if(c->state==TCP_ESTABLISHED || c->state==TCP_FIN_WAIT_1 || c->state==TCP_FIN_WAIT_2)
        tcp_xmit(c, F_ACK, NULL, 0, c->snd_nxt);
    return (int)n;
}

int tcp_start_close(tcp_conn_t* c){
    if(!c) return -1;
    if(c->state==TCP_ESTABLISHED || c->state==TCP_CLOSE_WAIT){
        c->fin_pending=1; tcp_output(c);
    } else if(c->state==TCP_SYN_SENT || c->state==TCP_LISTEN){
        c->state=TCP_CLOSED;
    }
    return 0;
}

// ---- receive: locate the connection ----
static struct tcp_conn* find_conn(net_dev_t* dev, uint32_t sip, uint16_t sport, uint16_t dport){
    for(int i=0;i<TCP_MAX_CONN;i++){
        struct tcp_conn* c=&g_conns[i];
        if(c->used && c->state!=TCP_LISTEN && c->state!=TCP_CLOSED &&
           c->local_port==dport && c->remote_port==sport && c->remote_ip==sip){
            (void)dev; return c;
        }
    }
    return NULL;
}
static struct tcp_conn* find_listener(uint16_t dport){
    for(int i=0;i<TCP_MAX_CONN;i++)
        if(g_conns[i].used && g_conns[i].state==TCP_LISTEN && g_conns[i].local_port==dport)
            return &g_conns[i];
    return NULL;
}

// Append in-order payload to the receive ring; returns bytes accepted.
static uint32_t rx_accept(struct tcp_conn* c, const uint8_t* data, uint32_t len){
    uint32_t space = TCP_RXBUF - c->rx_len;
    if(len>space) len=space;
    uint32_t tail=(c->rx_head + c->rx_len)%TCP_RXBUF;
    for(uint32_t i=0;i<len;i++) c->rx[(tail+i)%TCP_RXBUF]=data[i];
    c->rx_len+=len;
    return len;
}

void tcp_input(net_dev_t* dev, uint32_t src_ip, const uint8_t* seg, uint32_t len){
    if(len<20) return;
    uint16_t sport=(uint16_t)((seg[0]<<8)|seg[1]);
    uint16_t dport=(uint16_t)((seg[2]<<8)|seg[3]);
    uint32_t seq=((uint32_t)seg[4]<<24)|((uint32_t)seg[5]<<16)|((uint32_t)seg[6]<<8)|seg[7];
    uint32_t ack=((uint32_t)seg[8]<<24)|((uint32_t)seg[9]<<16)|((uint32_t)seg[10]<<8)|seg[11];
    uint32_t doff=(seg[12]>>4)*4; if(doff<20||doff>len) return;
    uint8_t flags=seg[13];
    uint16_t wnd=(uint16_t)((seg[14]<<8)|seg[15]);
    const uint8_t* data=seg+doff; uint32_t plen=len-doff;

    struct tcp_conn* c=find_conn(dev, src_ip, sport, dport);

    if(!c){
        struct tcp_conn* lc=find_listener(dport);
        if(lc && (flags&F_SYN) && !(flags&F_ACK)){
            struct tcp_conn* ch=tcp_alloc();
            if(!ch) return;
            ch->dev=dev; ch->remote_ip=src_ip; ch->remote_port=sport; ch->local_port=dport;
            ch->listener=lc;
            ch->iss=(g_iss_seed+=0x9E37)^(uint32_t)timer_get_ticks();
            ch->rcv_nxt=seq+1; ch->snd_una=ch->iss; ch->snd_nxt=ch->iss;
            ch->snd_wnd=wnd; ch->state=TCP_SYN_RCVD;
            tcp_xmit(ch, F_SYN|F_ACK, NULL, 0, ch->iss);
            ch->snd_nxt=ch->iss+1; arm_rto(ch);
        }
        return;
    }

    if(flags&F_RST){ c->reset=1; c->state=TCP_CLOSED; if(c->listener) c->used=0; return; }

    c->snd_wnd=wnd;

    if(c->state==TCP_SYN_SENT){
        if((flags&F_SYN) && (flags&F_ACK)){
            if(ack!=c->snd_nxt) return;             /* unexpected ack */
            c->rcv_nxt=seq+1; c->snd_una=ack; c->state=TCP_ESTABLISHED;
            tcp_xmit(c, F_ACK, NULL, 0, c->snd_nxt);
            c->rto_deadline=0;
        } else if(flags&F_SYN){                     /* simultaneous open */
            c->rcv_nxt=seq+1; c->state=TCP_SYN_RCVD;
            tcp_xmit(c, F_SYN|F_ACK, NULL, 0, c->iss); arm_rto(c);
        }
        return;
    }

    /* ACK processing for all synchronized states. */
    if(flags&F_ACK){
        if(seq_gt(ack, c->snd_una) && seq_leq(ack, c->snd_nxt)){
            uint32_t acked = ack - c->snd_una;
            /* Our SYN (SYN_RCVD) or data/FIN bytes get acknowledged. Trim the tx
             * buffer for the data portion; SYN/FIN occupy a seq but no tx bytes. */
            uint32_t databytes = acked;
            if(c->state==TCP_SYN_RCVD){ databytes = acked>0?acked-1:0; }  /* SYN consumed 1 */
            if(c->fin_sent){
                /* If this ack covers our FIN (the last seq), one of the acked
                 * units is the FIN, not tx data. */
                if(seq_leq(c->snd_nxt, ack)) { if(databytes>0) databytes-=1; }
            }
            if(databytes>c->tx_len) databytes=c->tx_len;
            if(databytes){
                for(uint32_t i=databytes;i<c->tx_len;i++) c->tx[i-databytes]=c->tx[i];
                c->tx_len-=databytes;
            }
            c->snd_una=ack;
            if(c->snd_una==c->snd_nxt) c->rto_deadline=0; else arm_rto(c);

            if(c->state==TCP_SYN_RCVD) c->state=TCP_ESTABLISHED;
            else if(c->state==TCP_FIN_WAIT_1 && c->snd_una==c->snd_nxt) c->state=TCP_FIN_WAIT_2;
            else if(c->state==TCP_CLOSING && c->snd_una==c->snd_nxt){ c->state=TCP_TIME_WAIT; c->tw_deadline=timer_get_ticks()+TCP_TIMEWAIT; }
            else if(c->state==TCP_LAST_ACK && c->snd_una==c->snd_nxt){ c->state=TCP_CLOSED; if(c->listener) c->used=0; return; }
        }
    }

    /* In-order data. */
    int did_ack=0;
    if(plen>0 && (c->state==TCP_ESTABLISHED || c->state==TCP_FIN_WAIT_1 || c->state==TCP_FIN_WAIT_2)){
        if(seq==c->rcv_nxt){
            uint32_t got=rx_accept(c, data, plen);
            c->rcv_nxt+=got;
        }
        tcp_xmit(c, F_ACK, NULL, 0, c->snd_nxt); did_ack=1;  /* ACK (cumulative; re-ACK on OOO) */
    }

    /* FIN handling: the FIN's sequence is seq+plen; honor it only when in-order. */
    if(flags&F_FIN){
        if(seq+plen==c->rcv_nxt){
            c->rcv_nxt++;                            /* FIN consumes one seq */
            if(c->state==TCP_ESTABLISHED) c->state=TCP_CLOSE_WAIT;
            else if(c->state==TCP_FIN_WAIT_1){ c->state=TCP_CLOSING; }
            else if(c->state==TCP_FIN_WAIT_2){ c->state=TCP_TIME_WAIT; c->tw_deadline=timer_get_ticks()+TCP_TIMEWAIT; }
            tcp_xmit(c, F_ACK, NULL, 0, c->snd_nxt); did_ack=1;
        }
    }
    (void)did_ack;

    tcp_output(c);
}

void tcp_tick(void){
    uint64_t now=timer_get_ticks();
    for(int i=0;i<TCP_MAX_CONN;i++){
        struct tcp_conn* c=&g_conns[i];
        if(!c->used) continue;
        if(c->state==TCP_TIME_WAIT){
            if(now>=c->tw_deadline){ c->state=TCP_CLOSED; if(c->listener) c->used=0; }
            continue;
        }
        if(c->state==TCP_CLOSED) continue;
        if(c->rto_deadline && now>=c->rto_deadline && c->snd_una!=c->snd_nxt){
            if(++c->retries>TCP_MAX_RETX){ c->reset=1; c->state=TCP_CLOSED; if(c->listener) c->used=0; continue; }
            /* Retransmit the oldest unacked unit. */
            if(c->state==TCP_SYN_SENT) tcp_xmit(c, F_SYN, NULL, 0, c->iss);
            else if(c->state==TCP_SYN_RCVD) tcp_xmit(c, F_SYN|F_ACK, NULL, 0, c->iss);
            else {
                uint32_t unacked = c->snd_nxt - c->snd_una;
                uint32_t databytes = unacked;
                if(c->fin_sent && databytes>0) databytes-=1;   /* FIN is the trailing seq */
                if(databytes>c->tx_len) databytes=c->tx_len;
                if(databytes){
                    uint32_t chunk=databytes; if(chunk>TCP_MSS) chunk=TCP_MSS;
                    tcp_xmit(c, F_ACK|F_PSH, c->tx, chunk, c->snd_una);
                } else if(c->fin_sent){
                    tcp_xmit(c, F_ACK|F_FIN, NULL, 0, c->snd_nxt-1);
                }
            }
            c->rto_deadline = now + (TCP_RTO << (c->retries<4?c->retries:4));
        }
    }
}

// ---- blocking convenience ----
tcp_conn_t* tcp_connect(net_dev_t* dev, uint32_t dip, uint16_t dport){
    // Warm the next-hop ARP first (gateway for off-subnet) so the very first SYN
    // goes out instead of being dropped on a pending ARP — otherwise a fresh
    // destination could report "connect failed" until a retransmit happened.
    if(dev){
        uint32_t nh = (dev->netmask && (dip & dev->netmask)==(dev->ip & dev->netmask)) ? dip : dev->gateway;
        uint8_t mac[6];
        if(arp_resolve(dev, nh, mac)!=0){
            uint64_t adl=timer_get_ticks()+1000;
            while(timer_get_ticks()<adl && arp_lookup(nh, mac)!=0) __asm__ volatile("sti; hlt");
        }
    }
    tcp_conn_t* c=tcp_alloc();
    if(!c) return NULL;
    if(tcp_start_connect(c, dev, dip, dport)!=0){ tcp_free(c); return NULL; }
    uint64_t deadline=timer_get_ticks()+8000;
    while(timer_get_ticks()<deadline){
        if(c->state==TCP_ESTABLISHED) return c;
        if(c->reset || c->state==TCP_CLOSED) break;
        __asm__ volatile("sti; hlt");
    }
    tcp_free(c);
    return NULL;
}

int tcp_send_all(tcp_conn_t* c, const void* data, uint32_t len){
    const uint8_t* p=(const uint8_t*)data; uint32_t off=0;
    uint64_t deadline=timer_get_ticks()+8000;
    while(off<len && timer_get_ticks()<deadline){
        int n=tcp_write(c, p+off, len-off);
        if(n<0) return -1;
        off+=(uint32_t)n;
        if(off<len) __asm__ volatile("sti; hlt");   /* tx buffer full: let ACKs drain it */
    }
    return off==len?0:-1;
}

int tcp_recv_block(tcp_conn_t* c, void* buf, uint32_t len, uint32_t ms){
    uint64_t deadline=timer_get_ticks()+ms;
    while(timer_get_ticks()<deadline){
        int n=tcp_read(c, buf, len);
        if(n>0) return n;
        if(n<0) return 0;                            /* peer closed, drained = EOF */
        __asm__ volatile("sti; hlt");
    }
    return 0;
}

void tcp_close(tcp_conn_t* c){
    if(!c) return;
    tcp_start_close(c);
    uint64_t deadline=timer_get_ticks()+4000;
    while(timer_get_ticks()<deadline){
        if(c->state==TCP_CLOSED || c->state==TCP_TIME_WAIT) break;
        __asm__ volatile("sti; hlt");
    }
    tcp_free(c);
}
