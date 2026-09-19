/* KNXnet/IP: a group telegram when a contact input changes.
 *
 * The doorbell is the reason. A contact on GPIO1 already becomes an MQTT
 * message and a Home Assistant entity; this sends the same event onto a KNX
 * installation directly, so the bell still works when the broker or the home
 * automation box is down. Nothing is read back and nothing is subscribed --
 * this module only ever writes one bit to a group address.
 *
 * Two ways to reach a KNX bus over IP, and the right one depends on hardware
 * the installation already has:
 *
 *   ROUTING     One multicast datagram to 224.0.23.12:3671 and it is gone.
 *               No connection, no state, no heartbeat -- which is exactly what
 *               a doorbell wants: one event a day and nothing to keep alive in
 *               between. Needs an IP router that actually forwards routing
 *               telegrams onto the twisted pair; an IP *interface* usually
 *               does not, and advertising the Routing service family in its
 *               discovery response does not prove that it does.
 *
 *   TUNNELLING  A real connection to one gateway: CONNECT_REQUEST, a channel
 *               id, a sequence counter per direction, an ACK for every frame
 *               either side sends, and a CONNECTIONSTATE_REQUEST every minute
 *               or the gateway drops the channel. Every IP interface speaks
 *               it, which is why it is the default -- but a gateway has only a
 *               handful of tunnel channels and ETS or a visualisation will
 *               happily occupy them.
 *
 * KNXnet/IP is UDP in both modes. TCP only enters with KNX IP Secure, which
 * this does not implement -- and neither does it implement Data Secure, so a
 * secured installation will ignore what this sends.
 */
#ifndef O3E_KNX_H
#define O3E_KNX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KNX_PORT            3671
#define KNX_MULTICAST       "224.0.23.12"

/* "1/2/3" is eight characters; the parser also takes a plain number. */
#define KNX_ADDR_MAX        16

/* KNXnet/IP service types, in the order the connection uses them. */
#define KNX_CONNECT_REQ     0x0205
#define KNX_CONNECT_RES     0x0206
#define KNX_CONNSTATE_REQ   0x0207
#define KNX_CONNSTATE_RES   0x0208
#define KNX_DISCONNECT_REQ  0x0209
#define KNX_DISCONNECT_RES  0x020A
#define KNX_TUNNEL_REQ      0x0420
#define KNX_TUNNEL_ACK      0x0421
#define KNX_ROUTING_IND     0x0530

/* The largest frame this module builds: header 6 + connection header 4 +
 * cEMI 11. Receiving is bounded separately, since a gateway relays whatever
 * is on the bus. */
#define KNX_FRAME_MAX       64
#define KNX_RECV_MAX        256

typedef enum {
    KNX_MODE_TUNNELLING = 0,
    KNX_MODE_ROUTING,
} knx_mode_t;

typedef struct {
    bool       enabled;
    knx_mode_t mode;
    char       gateway[64];       /* tunnelling only; routing uses multicast */
    uint16_t   port;              /* 3671 unless someone moved it */
    /* Routing only. Tunnelling leaves the source at zero and the gateway
     * substitutes the address it assigned at connect time -- which is the
     * whole point of a tunnel. A router does no such thing, so in that mode
     * this has to name a physical address nothing else on the bus uses. */
    char       source[KNX_ADDR_MAX];
} knx_cfg_t;

typedef struct {
    bool     enabled;
    bool     connected;        /* tunnelling: channel is up */
    uint8_t  channel;
    uint16_t assigned;         /* physical address the gateway handed out */
    uint32_t sent;
    uint32_t failures;
    char     last_error[96];
} knx_status_t;

/* ---- the pure part, in knx_frame.c ------------------------------- */
/*
 * Everything that turns numbers into bytes lives apart from the socket, so
 * test/test_knx.c can check the frames against the specification on a
 * workstation. A wrong length byte or a swapped octet here produces a
 * telegram a gateway silently discards -- no error, no log, nothing on the
 * bus -- which is the most expensive kind of bug to chase on a device.
 */

/* "1/2/3", "1/2" or a plain number into a 16-bit group address.
 * False on anything out of range: main 0..31, middle 0..7, sub 0..255. */
bool knx_ga_parse(const char *s, uint16_t *out);
void knx_ga_format(uint16_t ga, char *out, size_t out_sz);

/* "1.1.250" into a physical address: area 0..15, line 0..15, device 0..255. */
bool knx_pa_parse(const char *s, uint16_t *out);
void knx_pa_format(uint16_t pa, char *out, size_t out_sz);

/* The cEMI L_Data.req carrying A_GroupValue_Write with a one-bit value.
 * Returns the length written, or 0 if the buffer is too small. */
size_t knx_cemi_write_bool(uint8_t *out, size_t out_sz, uint16_t src,
                           uint16_t ga, bool on);

/* Wrap a cEMI in a TUNNELLING_REQUEST or a ROUTING_INDICATION. */
size_t knx_frame_tunnel(uint8_t *out, size_t out_sz, uint8_t channel,
                        uint8_t seq, const uint8_t *cemi, size_t cemi_len);
size_t knx_frame_routing(uint8_t *out, size_t out_sz,
                         const uint8_t *cemi, size_t cemi_len);

/* The connection frames. `ip` and `port` describe our own socket, because
 * every one of these carries an HPAI telling the gateway where to answer. */
size_t knx_frame_connect(uint8_t *out, size_t out_sz, uint32_t ip, uint16_t port);
size_t knx_frame_connstate(uint8_t *out, size_t out_sz, uint8_t channel,
                           uint32_t ip, uint16_t port);
size_t knx_frame_disconnect(uint8_t *out, size_t out_sz, uint8_t channel,
                            uint32_t ip, uint16_t port);
size_t knx_frame_ack(uint8_t *out, size_t out_sz, uint8_t channel, uint8_t seq);

/* Service type and total length of a received frame, validated against what
 * actually arrived. False on a short frame, a wrong header or a length that
 * disagrees with the datagram. */
bool knx_frame_peek(const uint8_t *buf, size_t len, uint16_t *service,
                    uint16_t *total);

/* CONNECT_RESPONSE: channel id, status, and the physical address assigned to
 * this tunnel. False if the frame is malformed; a non-zero `status` is a
 * refusal by the gateway and is reported, not hidden. */
bool knx_parse_connect_res(const uint8_t *buf, size_t len, uint8_t *channel,
                           uint8_t *status, uint16_t *assigned);

/* The connection header shared by TUNNELLING_REQUEST and TUNNELLING_ACK. */
bool knx_parse_conn_header(const uint8_t *buf, size_t len, uint8_t *channel,
                           uint8_t *seq, uint8_t *status);

/* What a gateway's refusal means, for the status line in the web UI. */
const char *knx_status_text(uint8_t status);

/* ---- the socket part, in knx.c ----------------------------------- */

void knx_start(void);       /* also re-reads settings; safe to call again */
void knx_stop(void);
void knx_status(knx_status_t *out);

/* Queue one group write. Returns false only when KNX is switched off or the
 * queue is full -- delivery itself is asynchronous, and for a doorbell that is
 * the right trade: the sampling task must never block on a network write. */
bool knx_send_bool(uint16_t ga, bool on);

#endif /* O3E_KNX_H */
