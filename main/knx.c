/* The socket half of KNXnet/IP: one task, one UDP socket, one connection.
 *
 * Everything that turns numbers into bytes is in knx_frame.c and tested on a
 * workstation; what is left here is the part that only a network can exercise
 * -- keeping a tunnel alive, acknowledging what the gateway sends, and
 * rebuilding the connection after it goes away.
 *
 * Sending is queued rather than done in the caller. The contact input samples
 * every five milliseconds and must never sit in a socket call waiting for a
 * gateway that has been unplugged; a doorbell that delays the next sample is
 * worse than a doorbell that reports 30 ms late.
 */
#include "knx.h"

#include <errno.h>
#include <stdarg.h>
#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "app_config.h"

static const char *TAG = "knx";

/* Heartbeat and timeouts from KNX volume 3/8/2 section 5.4: the client sends
 * CONNECTIONSTATE_REQUEST every 60 s, the gateway answers within 10 s, and a
 * connection is given up after three failed attempts. */
#define HEARTBEAT_MS      60000
#define RESPONSE_MS       10000
#define HEARTBEAT_TRIES   3
#define RECONNECT_MS      15000

typedef struct {
    uint16_t ga;
    bool     on;
} knx_job_t;

static QueueHandle_t  jobs;
static TaskHandle_t   task_h;
static volatile bool  running;
static volatile bool  reload = true;

static knx_cfg_t     cfg;
static knx_status_t  st;
static int           sock = -1;
static uint32_t      local_ip;
static uint16_t      local_port;
static uint8_t       tx_seq;
static uint16_t      src_addr;

static void set_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(st.last_error, sizeof(st.last_error), fmt, ap);
    va_end(ap);
    st.failures++;
    ESP_LOGW(TAG, "%s", st.last_error);
}

static bool socket_open(void)
{
    if (sock >= 0) {
        return true;
    }
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        set_error("no socket available");
        return false;
    }
    struct sockaddr_in me = { .sin_family = AF_INET, .sin_port = 0,
                              .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(sock, (struct sockaddr *)&me, sizeof(me)) < 0) {
        set_error("could not bind a local port");
        close(sock);
        sock = -1;
        return false;
    }
    socklen_t sl = sizeof(me);
    getsockname(sock, (struct sockaddr *)&me, &sl);
    local_port = ntohs(me.sin_port);

    /* The HPAI in every connection frame has to carry the address the gateway
     * can actually answer on, so it comes from the interface rather than from
     * the socket -- a socket bound to INADDR_ANY reports 0.0.0.0, and a
     * gateway told to answer there answers nowhere. */
    esp_netif_ip_info_t ip;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif || esp_netif_get_ip_info(netif, &ip) != ESP_OK || !ip.ip.addr) {
        set_error("no IP address yet");
        close(sock);
        sock = -1;
        return false;
    }
    local_ip = ntohl(ip.ip.addr);

    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return true;
}

static void socket_close(void)
{
    if (sock >= 0) {
        close(sock);
        sock = -1;
    }
}

static bool send_to(const char *host, uint16_t port, const uint8_t *b, size_t n)
{
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(port) };
    if (inet_pton(AF_INET, host, &to.sin_addr) != 1) {
        set_error("\"%s\" is not an address", host);
        return false;
    }
    int r = sendto(sock, b, n, 0, (struct sockaddr *)&to, sizeof(to));
    if (r != (int)n) {
        set_error("sending to %s failed (errno %d)", host, errno);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* routing: no connection, nothing to keep alive                        */

static bool routing_send(const knx_job_t *j)
{
    uint8_t cemi[16], frame[KNX_FRAME_MAX];
    size_t cl = knx_cemi_write_bool(cemi, sizeof(cemi), src_addr, j->ga, j->on);
    size_t fl = knx_frame_routing(frame, sizeof(frame), cemi, cl);
    if (!fl || !send_to(KNX_MULTICAST, cfg.port, frame, fl)) {
        return false;
    }
    st.sent++;
    return true;
}

/* ------------------------------------------------------------------ */
/* tunnelling                                                           */

/* Wait for one frame, handling anything that is not what we are waiting for.
 * A gateway relays live bus traffic down the tunnel, so unrelated
 * TUNNELLING_REQUESTs arrive constantly and every one of them must be
 * acknowledged -- an unacknowledged frame makes the gateway retransmit and
 * then drop the channel. */
static bool wait_for(uint16_t want, uint32_t timeout_ms, uint8_t *out, size_t *out_len)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    uint8_t buf[KNX_RECV_MAX];

    while (xTaskGetTickCount() < deadline) {
        int n = recv(sock, buf, sizeof(buf), 0);
        if (n <= 0) {
            continue;
        }
        uint16_t service;
        if (!knx_frame_peek(buf, (size_t)n, &service, NULL)) {
            continue;
        }
        if (service == KNX_TUNNEL_REQ) {
            uint8_t ch, seq;
            if (knx_parse_conn_header(buf, (size_t)n, &ch, &seq, NULL)
                && ch == st.channel) {
                uint8_t ack[16];
                size_t al = knx_frame_ack(ack, sizeof(ack), ch, seq);
                send_to(cfg.gateway, cfg.port, ack, al);
            }
            continue;
        }
        if (service == KNX_DISCONNECT_REQ) {
            st.connected = false;
            return false;
        }
        if (service == want) {
            if (out && out_len && (size_t)n <= *out_len) {
                memcpy(out, buf, (size_t)n);
                *out_len = (size_t)n;
            }
            return true;
        }
    }
    return false;
}

static bool tunnel_connect(void)
{
    uint8_t frame[KNX_FRAME_MAX];
    size_t fl = knx_frame_connect(frame, sizeof(frame), local_ip, local_port);
    if (!fl || !send_to(cfg.gateway, cfg.port, frame, fl)) {
        return false;
    }

    uint8_t res[KNX_RECV_MAX];
    size_t rl = sizeof(res);
    if (!wait_for(KNX_CONNECT_RES, RESPONSE_MS, res, &rl)) {
        set_error("%s did not answer the connection request", cfg.gateway);
        return false;
    }
    uint8_t ch, status;
    uint16_t assigned;
    if (!knx_parse_connect_res(res, rl, &ch, &status, &assigned)) {
        set_error("the gateway sent a malformed connection response");
        return false;
    }
    if (status != 0x00) {
        set_error("%s: %s", cfg.gateway, knx_status_text(status));
        return false;
    }
    st.channel = ch;
    st.assigned = assigned;
    st.connected = true;
    tx_seq = 0;
    char pa[KNX_ADDR_MAX];
    knx_pa_format(assigned, pa, sizeof(pa));
    ESP_LOGI(TAG, "tunnel to %s open, channel %u, address %s",
             cfg.gateway, ch, pa);
    st.last_error[0] = '\0';
    return true;
}

static void tunnel_disconnect(void)
{
    if (!st.connected) {
        return;
    }
    uint8_t frame[KNX_FRAME_MAX];
    size_t fl = knx_frame_disconnect(frame, sizeof(frame), st.channel,
                                     local_ip, local_port);
    if (fl) {
        send_to(cfg.gateway, cfg.port, frame, fl);
    }
    st.connected = false;
    st.channel = 0;
}

static bool tunnel_heartbeat(void)
{
    for (int try = 0; try < HEARTBEAT_TRIES; try++) {
        uint8_t frame[KNX_FRAME_MAX];
        size_t fl = knx_frame_connstate(frame, sizeof(frame), st.channel,
                                        local_ip, local_port);
        if (fl && send_to(cfg.gateway, cfg.port, frame, fl)
            && wait_for(KNX_CONNSTATE_RES, RESPONSE_MS, NULL, NULL)) {
            return true;
        }
    }
    set_error("%s stopped answering; reconnecting", cfg.gateway);
    st.connected = false;
    return false;
}

static bool tunnel_send(const knx_job_t *j)
{
    uint8_t cemi[16], frame[KNX_FRAME_MAX];
    /* Source stays zero: the gateway substitutes the address it assigned to
     * this tunnel, which is the one the installation knows us by. */
    size_t cl = knx_cemi_write_bool(cemi, sizeof(cemi), 0, j->ga, j->on);
    size_t fl = knx_frame_tunnel(frame, sizeof(frame), st.channel, tx_seq, cemi, cl);
    if (!fl || !send_to(cfg.gateway, cfg.port, frame, fl)) {
        return false;
    }
    /* The gateway acknowledges by echoing the sequence number. Not waiting for
     * it would leave the counter running ahead of what the gateway has seen,
     * and every later frame would be discarded as out of order. */
    if (!wait_for(KNX_TUNNEL_ACK, RESPONSE_MS, NULL, NULL)) {
        set_error("no acknowledgement for telegram %u", tx_seq);
        st.connected = false;
        return false;
    }
    tx_seq++;
    st.sent++;
    st.last_error[0] = '\0';
    return true;
}

/* ------------------------------------------------------------------ */

static void knx_task(void *arg)
{
    (void)arg;
    TickType_t next_beat = 0;
    TickType_t next_try = 0;

    while (running) {
        if (reload) {
            reload = false;
            tunnel_disconnect();
            socket_close();
            sys_cfg_t sys;
            sys_cfg_get(&sys);
            cfg = sys.knx;
            memset(&st, 0, sizeof(st));
            st.enabled = cfg.enabled;
            src_addr = 0;
            if (cfg.mode == KNX_MODE_ROUTING && cfg.source[0]) {
                knx_pa_parse(cfg.source, &src_addr);
            }
            next_try = 0;
            if (cfg.enabled) {
                ESP_LOGI(TAG, "%s, gateway %s:%u",
                         cfg.mode == KNX_MODE_ROUTING ? "routing" : "tunnelling",
                         cfg.mode == KNX_MODE_ROUTING ? KNX_MULTICAST : cfg.gateway,
                         cfg.port);
            }
        }

        if (!cfg.enabled) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (!socket_open()) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        if (cfg.mode == KNX_MODE_TUNNELLING) {
            if (!st.connected) {
                /* Spaced out rather than retried in a tight loop: a gateway
                 * with no free channel stays that way for minutes, and the
                 * log should not fill up in the meantime. */
                if (xTaskGetTickCount() < next_try) {
                    vTaskDelay(pdMS_TO_TICKS(500));
                    continue;
                }
                next_try = xTaskGetTickCount() + pdMS_TO_TICKS(RECONNECT_MS);
                if (!tunnel_connect()) {
                    continue;
                }
                next_beat = xTaskGetTickCount() + pdMS_TO_TICKS(HEARTBEAT_MS);
            }
            if (xTaskGetTickCount() >= next_beat) {
                next_beat = xTaskGetTickCount() + pdMS_TO_TICKS(HEARTBEAT_MS);
                if (!tunnel_heartbeat()) {
                    continue;
                }
            }
        }

        /* Short wait, so the heartbeat stays roughly on time and inbound bus
         * traffic gets acknowledged even when nothing is being sent. */
        knx_job_t j;
        if (xQueueReceive(jobs, &j, pdMS_TO_TICKS(500)) == pdTRUE) {
            bool ok = cfg.mode == KNX_MODE_ROUTING ? routing_send(&j) : tunnel_send(&j);
            char ga[KNX_ADDR_MAX];
            knx_ga_format(j.ga, ga, sizeof(ga));
            ESP_LOGI(TAG, "%s -> %s: %s", ga, j.on ? "ON" : "OFF",
                     ok ? "sent" : "failed");
        } else if (cfg.mode == KNX_MODE_TUNNELLING && st.connected) {
            wait_for(0xFFFF, 100, NULL, NULL);   /* drain and acknowledge */
        }
    }

    tunnel_disconnect();
    socket_close();
    task_h = NULL;
    vTaskDelete(NULL);
}

void knx_start(void)
{
    reload = true;
    if (task_h) {
        return;
    }
    if (!jobs) {
        jobs = xQueueCreate(8, sizeof(knx_job_t));
        if (!jobs) {
            ESP_LOGE(TAG, "no memory for the send queue");
            return;
        }
    }
    running = true;
    if (xTaskCreate(knx_task, "knx", 4096, NULL, 4, &task_h) != pdPASS) {
        running = false;
        ESP_LOGE(TAG, "could not start the KNX task");
    }
}

void knx_stop(void)
{
    running = false;
}

void knx_status(knx_status_t *out) { *out = st; }

bool knx_send_bool(uint16_t ga, bool on)
{
    if (!cfg.enabled || !jobs || !ga) {
        return false;
    }
    knx_job_t j = { .ga = ga, .on = on };
    return xQueueSend(jobs, &j, 0) == pdTRUE;
}
