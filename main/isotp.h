/* ISO-TP (ISO 15765-2) transport over classic CAN, normal 11-bit addressing.
 *
 * Parameters match open3e's isotp configuration exactly (Open3Eclass.py:119),
 * because an E3 bus is tuned for that peer:
 *   STmin 10 ms, BlockSize 0 (send everything after one flow control),
 *   TX padding to 8 bytes with 0x00, no CAN-FD.
 *
 * There is no ESP-IDF ISO-TP driver, so this is a from-scratch implementation.
 * CAN access sits behind isotp_hal_t so the same code runs against the TWAI
 * peripheral on the device and against a simulated bus in test/test_isotp.c.
 */
#ifndef O3E_ISOTP_H
#define O3E_ISOTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Largest datapoint in the open3e database is 199 bytes; 4095 is the ceiling
 * the ISO-TP 12-bit length field can express anyway. */
#define ISOTP_MAX_PAYLOAD 4095

/* open3e's values. */
#define ISOTP_STMIN_MS      10
#define ISOTP_BLOCKSIZE     0
#define ISOTP_PAD_BYTE      0x00

/* N_Bs / N_Cr: how long to wait for the peer's flow control or next
 * consecutive frame. ISO 15765-2 puts both at 1000 ms. */
#define ISOTP_N_BS_MS     1000
#define ISOTP_N_CR_MS     1000

/* Ceiling on a whole segmented message, independent of the per-frame timeouts.
 *
 * Per-frame timeouts alone are not enough on a shared bus: the E3 devices talk
 * to each other constantly, so a receive call almost always returns *some*
 * frame, and a loop that only restarts its timeout on foreign traffic never
 * ends. That is a hang holding the bus, not a slow transfer -- it was observed
 * running for hours. The largest datapoint is 199 bytes, which is 29 frames at
 * 10 ms separation, so three seconds is generous. */
#define ISOTP_MSG_MS      3000

/* N_WFTmax: how many consecutive "wait" flow controls to accept before giving
 * up. Without a limit a peer that keeps asking for more time holds the bus
 * indefinitely. ISO 15765-2 leaves the value to the implementation. */
#define ISOTP_N_WFT_MAX   4

typedef struct {
    bool     (*send)(void *ctx, uint32_t id, const uint8_t *data, uint8_t len);
    /* Receive the next frame addressed to us, or false on timeout. */
    bool     (*recv)(void *ctx, uint32_t *id, uint8_t *data, uint8_t *len,
                     uint32_t timeout_ms);
    void     (*delay_us)(void *ctx, uint32_t us);
    uint32_t (*now_ms)(void *ctx);
    void      *ctx;
} isotp_hal_t;

typedef struct {
    const isotp_hal_t *hal;
    uint32_t tx_id;    /* our requests go out on this COB-ID */
    uint32_t rx_id;    /* the ECU answers here; on an E3 bus rx = tx + 0x10 */
    uint8_t  stmin_ms; /* what we ask senders to wait between frames;
                          ISOTP_STMIN_MS is the value open3e uses */
} isotp_link_t;

typedef enum {
    ISOTP_OK = 0,
    ISOTP_ERR_TIMEOUT,     /* no answer, i.e. no ECU at this address */
    ISOTP_ERR_TOO_LONG,
    ISOTP_ERR_BUS,         /* the CAN layer refused to transmit */
    ISOTP_ERR_PROTOCOL,    /* bad sequence number, unexpected PCI, overflow */
    ISOTP_ERR_OVERFLOW,    /* peer cannot accept the message */
} isotp_err_t;

const char *isotp_strerror(isotp_err_t e);

/* Decode the STmin byte of a flow control frame into microseconds.
 * Exposed for testing; 0x00..0x7F are milliseconds, 0xF1..0xF9 are hundreds of
 * microseconds, everything else is reserved and treated as the 127 ms maximum. */
uint32_t isotp_stmin_to_us(uint8_t stmin);

/* Send one ISO-TP message (single or segmented). Blocks until the last frame
 * is on the wire or an error occurs. */
isotp_err_t isotp_send(const isotp_link_t *link, const uint8_t *data, size_t len);

/* Receive one ISO-TP message. `timeout_ms` bounds the wait for the *first*
 * frame; the inter-frame timeouts above apply after that. */
isotp_err_t isotp_recv(const isotp_link_t *link, uint8_t *buf, size_t buf_sz,
                       size_t *out_len, uint32_t timeout_ms);

#endif /* O3E_ISOTP_H */
