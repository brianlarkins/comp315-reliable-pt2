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
  rdt_t *next;			         // this is a linked list of active connections
  rdt_t **prev;
  conn_t *c;			           // rlib connection object

  // you will need to add more fields to this structure
};


/*
 * global variables
 */
rdt_t *rdt_list;



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

  // add additional initialization code here

  return r;
}



/**
 * rdt_destroy - shutdown a reliable protocol session
 * @param r - reliable connection to close
 */
void rdt_destroy(rdt_t *r) {
  if (r->next)
    r->next->prev = r->prev;
  *r->prev = r->next;
  conn_destroy (r->c);

  // free any other allocated memory here
}



/**
 * rdt_recvpkt - receive a packet from the unreliable network layer
 * @param r - reliable connection state information
 * @param pkt - received packet
 * @param n - size of received packet
 */
void rdt_recvpkt(rdt_t *r, packet_t *pkt, size_t n) {
  // implement this function
}



/**
 * rdt_read - read packet from application and send to network layer
 * @param r - reliable connection state information
 */
void rdt_read(rdt_t *r) {
  // implement this function
}



/**
 * rdt_output - callback for delivering packet to application layer if buffer was full
 * @param r - reliable connection state information
 */
void rdt_output(rdt_t *r) {
  // implement this function
}



/**
 * rdt_timer() - timer callback invoked 1/5 of the retransmission rate
 */
void rdt_timer() {
  // implement this function
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
  // ignore this function
}
