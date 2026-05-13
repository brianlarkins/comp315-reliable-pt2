#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>

#include "rlib.h"

#define DATA_HDRLEN 12
#define ACK_HDRLEN   8

#define TERM_REMOTE_EOF 8
#define TERM_LOCAL_EOF  4
#define TERM_ALL_ACKS   2
#define TERM_ALL_OUT    1

/*
 * sender/receiver window buffer information
 */
struct window_buf_s {
  uint32_t        len;       // packet length
  uint32_t        valid;     // this buffer contains a received packet (recv-only)
  uint32_t        ack;       // this buffer has been acknowledeged by receiver (send-only)
  uint64_t        timestamp; // when this packet was sent (send-only)
  packet_t        p;         // buffered packet
};
typedef struct window_buf_s window_buf_t;

/*
 * reliable connection state information
 */
struct reliable_state {
  rdt_t          *next;      // this is a linked list of active connections
  rdt_t         **prev;
  conn_t         *c;	       // rlib connection object

  uint32_t        timeout;   // timeout time in milliseconds
  uint32_t        winsize;   // send/recv window size

  window_buf_t   *swin;      // send window (for re-xmits)
  uint32_t        sbase;     // send window base ack #
  uint32_t        sbidx;     // send window base index
  uint32_t        slen;      // send window actual size
  window_buf_t   *rwin;      // receive window (for out-of-order reciept)
  uint32_t        rbase;     // receive window base seq #
  uint32_t        rbidx;     // receive window base index
  uint32_t        rlen;      // receive window actual size
  uint32_t        nseq;      // next sequence #
  uint32_t        rseq;      // last received seq#

  uint32_t        swinfull;  // sender window was full, but app-level data remains
  uint32_t        tstatus;   // termination status
};


/*
 * global variables
 */
rdt_t *rdt_list;



/* ---- network failure injection (testing knobs from rlib.c) ---------------- */

/* True with probability pct/100. pct==0 → never; pct==100 → always. */
static int netfx_roll(int pct) {
  if (pct <= 0) return 0;
  if (pct >= 100) return 1;
  return (random() % 100) < pct;
}

/* Print one descriptive event line to stderr, prefixed for grep'ability. */
static void netfx_log(const char *dir, const char *what, const packet_t *pkt, size_t n) {
  if (n == ACK_HDRLEN)
    fprintf(stderr, "[netfx] %s %s ack-only ackno=%u len=%zu\n",
            dir, what, ntohl(pkt->ackno), n);
  else
    fprintf(stderr, "[netfx] %s %s seqno=%u ackno=%u len=%zu\n",
            dir, what, ntohl(pkt->seqno), ntohl(pkt->ackno), n);
}

/* Flip one random bit in the n-byte packet buffer. Any bit flip will
 * invalidate the IP-style checksum, so this exercises the receiver's
 * cksum check; flipping inside `len` additionally exercises framing checks. */
static void netfx_corrupt(packet_t *pkt, size_t n) {
  size_t bit = ((size_t) random()) % (n * 8);
  ((uint8_t *) pkt)[bit / 8] ^= (uint8_t) (1u << (bit % 8));
}

/* Is r still on the active connection list? rdt_destroy may have freed it
 * during the first pass of a duplicated receive; guard before re-entering. */
static int rdt_alive(const rdt_t *r) {
  rdt_t *p;
  for (p = rdt_list; p; p = p->next)
    if (p == r) return 1;
  return 0;
}

/* Send wrapper: applies loss / corrupt / duplicate to outbound packets.
 * Mutates a local copy for corruption so the caller's sendbuf stays clean. */
static int netfx_send(conn_t *c, const packet_t *pkt, size_t n) {
  packet_t buf;
  int rc;

  if (netfx_roll(opt_netfx_loss)) {
    netfx_log("send", "drop", pkt, n);
    return (int) n;  // pretend it went out
  }

  memcpy(&buf, pkt, n);
  if (netfx_roll(opt_netfx_corrupt)) {
    netfx_corrupt(&buf, n);
    netfx_log("send", "corrupt", pkt, n);
  }

  rc = conn_sendpkt(c, &buf, n);

  if (rc >= 0 && netfx_roll(opt_netfx_dup)) {
    netfx_log("send", "dup", pkt, n);
    conn_sendpkt(c, &buf, n);
  }
  return rc;
}



/**
 * rdt_create - creates a new reliable protocol session.
 * @param c  - connection object (when running in single-connection mode, NULL otherwise)
 * @param ss - sockaddr info (when running in multi-connection mode, NULL otherwise)
 * @param cc - global configuration information
 * @returns new reliable state structure, NULL on failure
 */
rdt_t *rdt_create(conn_t *c, const struct sockaddr_storage *ss, const struct config_common *cc) {
  rdt_t *r;

  r = xmalloc (sizeof (*r));
  memset (r, 0, sizeof (*r));

  if (!c) {
    c = conn_create (r, ss);
    if (!c) {
      free (r);
      return NULL;
    }
  }

  r->c = c;
  r->next = rdt_list;
  r->prev = &rdt_list;
  if (rdt_list)
    rdt_list->prev = &r->next;
  rdt_list = r;

  /* Do any other initialization you need here */

  r->timeout = cc->timeout;
  r->winsize = cc->window;

  r->rwin = xmalloc(sizeof(window_buf_t) * r->winsize);
  memset(r->rwin, 0, (sizeof(window_buf_t) * r->winsize));
  r->rbase = 1;
  r->rbidx = 0;
  r->rlen  = 0;

  r->swin = xmalloc(sizeof(window_buf_t) * r->winsize);
  memset(r->swin, 0, (sizeof(window_buf_t) * r->winsize));
  r->sbase = 1;
  r->sbidx = 0;
  r->slen  = 0;

  r->tstatus |= TERM_ALL_OUT;
  r->tstatus |= TERM_ALL_ACKS;

  return r;
}



/**
 * rdt_destroy - shutdown a reliable protocol session
 * @param r - reliable connection to close
 */
void rdt_destroy(rdt_t *r) {

  if (r->tstatus & (TERM_LOCAL_EOF|TERM_REMOTE_EOF)) {
    if ((r->tstatus & TERM_ALL_OUT) && (r->tstatus & TERM_ALL_ACKS)) {

      if (r->next)
        r->next->prev = r->prev;
      *r->prev = r->next;
      conn_destroy (r->c);

      /* Free any other allocated memory here */
      free(r->rwin);
      free(r->swin);
    }
  }
}



static void rdt_recvpkt_inner(rdt_t *r, packet_t *pkt, size_t n);

/**
 * rdt_recvpkt - receive a packet from the unreliable network layer
 * @param r - reliable connection state information
 * @param pkt - received packet
 * @param n - size of received packet
 *
 * Applies the netfx loss/corrupt/dup hooks to inbound packets before
 * dispatching to the protocol logic in rdt_recvpkt_inner. Duplicate is
 * simulated by reprocessing the packet a second time after the first
 * call returns; rdt_alive() guards against the case where the first
 * call tore down the connection.
 */
void rdt_recvpkt(rdt_t *r, packet_t *pkt, size_t n) {
  if (netfx_roll(opt_netfx_loss)) {
    netfx_log("recv", "drop", pkt, n);
    return;
  }
  if (netfx_roll(opt_netfx_corrupt)) {
    netfx_log("recv", "corrupt", pkt, n);
    netfx_corrupt(pkt, n);
  }
  int dup = netfx_roll(opt_netfx_dup);
  if (dup)
    netfx_log("recv", "dup", pkt, n);

  rdt_recvpkt_inner(r, pkt, n);
  if (dup && rdt_alive(r))
    rdt_recvpkt_inner(r, pkt, n);
}

static void rdt_recvpkt_inner(rdt_t *r, packet_t *pkt, size_t n) {
  uint32_t seqno, ackno, reclaimed;

  if (opt_debug)
    fprintf(stderr, "rdt_recvpkt:  received packet of %zd bytes\n", n);

  // don't accept damaged goods
  if (cksum(pkt, n) != 0xffff)
    goto done;

  // is packet data or ack?
  if ((n > ACK_HDRLEN) && (n <= sizeof(packet_t))) {
    // if packet is data:
    //   - check to see if its seq# is in range of rbase + winsize
    //     drop if not
    //   - is packet EOF?
    //     - if so, then update termination status and attempt to quit
    //   - recieve packet into recv buffer
    //   - call rdt_output() to output packets
    if (opt_debug)
      fprintf(stderr, "rcvdata: seqno: %d last: %d\n", ntohl(pkt->seqno), r->rbase);

    // check for EOF
    if (n == DATA_HDRLEN) {
      r->tstatus |= TERM_REMOTE_EOF;
      rdt_destroy(r); // not quite - need to do union, not intersection of events
      goto done;
    }

    seqno = ntohl(pkt->seqno);

    // check to ensure that packet is within our receive window
    if ((seqno >= r->rbase) && (seqno < (r->rbase  + r->winsize))) {
      // seq # inside recv window

      int winseq = seqno - r->rbase;   // find where in the window this packet goes
      int windex = (r->rbidx + winseq) % r->winsize;  // calculate index in window

      if (opt_debug) {
        fprintf(stderr, "receiving %d winseq: %d windex: %d\n", seqno, winseq, windex);
      }

      r->rwin[windex].len   = n;
      r->rwin[windex].valid = 1;
      r->rwin[windex].p     = *pkt;
      r->rlen++;

      rdt_output(r); // handles outputting to application-layer + ack sending

    } else {
      // seq # outside of window
      goto done;
    }

  } else if (n == ACK_HDRLEN) {
    //  if packet is ack:
    //    - update send window with ack for packet
    //    - see if we need to update base of window
    //    - if send window was full, read data with rdt_read()

    ackno = ntohl(pkt->ackno);

    if (opt_debug) {
      fprintf(stderr, "receiving ack: %d sbase: %d slen: %d\n", ackno, r->sbase, r->slen);
    }
    if ((ackno > r->sbase) && (ackno <= (r->sbase + r->slen))) {

      int winseq = (ackno-1) - r->sbase;   // find where in the window this packet goes
      int windex = (r->sbidx + winseq) % r->winsize;  // calculate index in window

      if (opt_debug) {
        fprintf(stderr, "receiving ack: %d winseq: %d windex: %d\n", ackno, winseq, windex);
      }

      // mark packet as acknowledged
      r->swin[windex].ack = 1;

      // update sender window
      uint32_t len = r->slen;

      for (uint32_t i=0; i < len; i++) {
        int idx = (r->sbidx + i) % r->winsize;
        if (r->swin[idx].ack) {
          r->swin[idx].ack = 0;
          r->sbase++;
          r->slen--;
        } else {
          break;
        }
      }
      reclaimed = len - r->slen;
      r->sbidx = (r->sbidx + reclaimed) % r->winsize;

      // if we reclaimed send window space and more app-layer data remains
      if ((len != r->slen) && (r->swinfull))
        rdt_read(r);

    } else {
      // ack # outside send window
      goto done;
    }

  } else {
    // unexpected packet
    goto done;
  }

done:
  return;
}



/**
 * rdt_read - read packet from application and send to network layer
 * @param r - reliable connection state information
 */
void rdt_read(rdt_t *r) {
  struct timespec ts;
  uint32_t nfree, nextidx;
  int rc = 0, len = 0;

  nfree   = r->winsize - r->slen;
  nextidx = (r->sbidx + r->slen) % r->winsize;

  if (!nfree) {
    r->swinfull = 1;
    return; // no room in seen
  } else {
    r->swinfull = 0;
  }

  if (opt_debug)
    fprintf(stderr, "rdt_read: reading from app-layer : sbase: %u slen: %u sbidx: %u\n", r->sbase, r->slen, r->sbidx);

  // only attempt to read as much data as we have room in send window
  for (uint32_t i=0; i < nfree; i++) {
    memset(&r->swin[nextidx], 0, sizeof(window_buf_t)); // zero-out window buffer

    rc = conn_input(r->c, &(r->swin[nextidx].p.data), sizeof(r->swin[nextidx].p.data));
    if (rc == 0) {
      // no data to read
      return;
    } else if (rc == -1) {
      // EOF
      if (opt_debug)
        fprintf(stderr, "got EOF -- sending shutdown packet\n");
      r->tstatus |= TERM_LOCAL_EOF;

      len = 0;

    } else {
      len = rc;
    }

    r->swin[nextidx].len       = DATA_HDRLEN + len;      // packet length
    r->swin[nextidx].ack       = 0;        // not ack'd yet
    clock_gettime (CLOCK_MONOTONIC, &ts);  // start timer for retransmission
    r->swin[nextidx].timestamp = (1000 * ts.tv_sec) + (ts.tv_nsec / 1000000);

    r->swin[nextidx].p.cksum   = htons(0);
    r->swin[nextidx].p.len     = htons(DATA_HDRLEN + len);
    r->swin[nextidx].p.seqno   = htonl(r->sbase + r->slen);
    r->swin[nextidx].p.ackno   = htonl(r->rbase);
    r->swin[nextidx].p.cksum   = cksum(&r->swin[nextidx].p, DATA_HDRLEN + len);
    r->slen++; // increment #full buffers in send window

    if (opt_debug)
      fprintf(stderr, "rdt_read: sending data\n");

    // send packet on wire
    rc = netfx_send(r->c, &r->swin[nextidx].p, DATA_HDRLEN + len);
    if (rc < 0) {
      fprintf(stderr,"rdt_read: sendpkt");
    }

    // shutdown if EOF case
    if (!len) {
      rdt_destroy(r);
      break;
    }
  }
}



/**
 * rdt_output - callback for delivering packet data to application layer
 * @param r - reliable connection state information
 */
void rdt_output(rdt_t *r) {
  packet_t ackp;
  uint16_t payload_size;
  uint32_t nextidx, nfull, ndelivered = 0, base, i;
  int rc = 0;

  // loop from base to first invalid packet
  nfull = r->rlen;
  base  = r->rbidx;

  if (opt_debug)
    fprintf(stderr, "rdt_output: delivering to app-layer\n");

  for (i=0; i < nfull; i++) {
    nextidx = (base + i) % r->winsize;

    // is this a packet that needs to be delivered?
    if (!r->rwin[nextidx].valid)
      break; // no

    payload_size = r->rwin[nextidx].len - DATA_HDRLEN;

    // is there enough room in app-layer buffer?
    if (conn_bufspace(r->c) < payload_size)
      break; // no

    // deliver received data packet and send acknowledgement
    rc = conn_output(r->c, &r->rwin[nextidx].p.data, payload_size);
    if (rc != payload_size)
      fprintf(stderr, "rdt_output: conn_output returned %d for write of %d bytes\n", rc, payload_size);

    r->rwin[nextidx].valid = 0;
    r->rlen--;
    r->rbidx = (r->rbidx + 1) % r->winsize;
    ndelivered++;
  }

  // did we deliver any packets?
  if (ndelivered) {
    // generate a cumulative ack
    memset(&ackp, 0, sizeof(ackp));
    ackp.cksum = htons(0);
    ackp.len   = htons(ACK_HDRLEN);
    r->rbase += ndelivered;
    ackp.ackno = htonl(r->rbase);
    ackp.cksum = cksum(&ackp, ACK_HDRLEN);

    if (opt_debug)
      fprintf(stderr, "rdt_output: sending ack - ndelivered: %d rbase: %d\n", ndelivered, r->rbase);

    // send ack packet
    rc = netfx_send(r->c, &ackp, ACK_HDRLEN);
    if (rc < 0) {
      perror("rdt_output: ack sendpkt");
    }
  }

  if (ndelivered == nfull) {
    r->tstatus |= TERM_ALL_OUT;
    rdt_destroy(r); // only terminates if everything else is over
  }
}



/**
 * rdt_timer() - timer callback invoked 1/5 of the retransmission rate
 */
void rdt_timer() {
  struct timespec ts;
  rdt_t  *rconn = rdt_list;
  uint64_t now;
  uint32_t nextidx, npending, base, i;
  int rc = 0;

  clock_gettime (CLOCK_MONOTONIC, &ts);
  now = (1000 * ts.tv_sec) + (ts.tv_nsec / 1000000);

  for (rconn = rdt_list; rconn; rconn = rconn->next) {

    npending = rconn->slen;
    base     = rconn->sbidx;

    // loop over all unack'd packets in the sender window
    for (i=0; i < npending; i++) {
      nextidx = (base + i) % rconn->winsize;
      // no ack and timout expired?
      if ((!(rconn->swin[nextidx].ack)) &&
          ((now - rconn->swin[nextidx].timestamp) > rconn->timeout)) {
        if (opt_debug)
          fprintf(stderr, "rdt_timer: retransmitting: ack: %d index: %d\n",
              rconn->swin[nextidx].ack, nextidx);
        rc = netfx_send(rconn->c, &rconn->swin[nextidx].p, rconn->swin[nextidx].len);
        if (rc < 0)
          fprintf(stderr, "rdt_timer: retransmit\n");
        rconn->swin[nextidx].timestamp = now;
      }
    }
  }
}



/* This function only gets called when the process is running as a
 * server and must handle connections from multiple clients.  You have
 * to look up the rdt_t structure based on the address in the
 * sockaddr_storage passed in.  If this is a new connection (sequence
 * number 1), you will need to allocate a new conn_t using rdt_create
 * ().  (Pass rdt_create NULL for the conn_t, so it will know to
 * allocate a new connection.)
 */
void rdt_demux(const struct config_common *cc, const struct sockaddr_storage *ss, packet_t *pkt, size_t len) {
}
