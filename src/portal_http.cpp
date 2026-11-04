#include "portal_http.h"
#include <WiFi.h>
#include <lwip/sockets.h>

/* A small HTTP server for the page and the captive-portal redirects. It
 * never waits on a client: requests are read and responses written a piece
 * at a time from portal_http_poll(), only as far as each socket takes them
 * without blocking (WiFiClient::write() can block for up to 10 s on a phone
 * that stopped reading, which held up every other phone and PC). */
static const unsigned SLOTS = 4; /* lwIP has 16 sockets for everything */
static const uint32_t IDLE_MS = 3000; /* a connection that sends no request is dropped */
static const uint32_t SEND_MS = 8000; /* bound on writing one response */
static const size_t REQUEST_MAX = 512; /* only the request line is needed */

struct Slot {
    WiFiClient client;
    char request[REQUEST_MAX];
    size_t used;
    uint32_t since;
    bool sending;
    char head[200];
    size_t head_len, head_sent;
    const char *body;
    size_t body_len, body_sent;
};
static Slot slots[SLOTS];
static WiFiServer server(80);
static const char *page_html;

void portal_http_begin(const char *page) {
    page_html = page;
    server.begin();
    server.setNoDelay(true);
}

static void close_slot(Slot &s) {
    s.client.stop();
    s.sending = false;
}

/* Prepares the response to the request line in s.request. */
static void respond(Slot &s) {
    char path[64] = "";
    sscanf(s.request, "GET %63s", path);
    if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
        s.body = page_html;
        s.body_len = strlen(page_html);
        s.head_len = snprintf(s.head, sizeof(s.head),
                              "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %u\r\n"
                              "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                              (unsigned)s.body_len);
    } else {
        s.body = "";
        s.body_len = 0;
        s.head_len = snprintf(s.head, sizeof(s.head),
                              "HTTP/1.1 302 Found\r\nLocation: http://%s/\r\nContent-Length: 0\r\n"
                              "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                              s.client.localIP().toString().c_str()); /* the address it came in on */
    }
    s.head_sent = s.body_sent = 0;
    s.sending = true;
    s.since = millis();
}

/* Sends what the socket takes now; false on an error. */
static bool send_some(int fd, const char *data, size_t size, size_t &sent) {
    while (sent < size) {
        int n = send(fd, data + sent, size - sent, MSG_DONTWAIT);
        if (n > 0) sent += n;
        else return n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
    }
    return true;
}

static void continue_send(Slot &s) {
    int fd = s.client.fd();
    if (fd < 0 || !send_some(fd, s.head, s.head_len, s.head_sent) ||
        (s.head_sent == s.head_len && !send_some(fd, s.body, s.body_len, s.body_sent)) ||
        millis() - s.since > SEND_MS) {
        close_slot(s);
        return;
    }
    if (s.head_sent == s.head_len && s.body_sent == s.body_len) close_slot(s); /* done */
}

void portal_http_poll() {
    /* Accept every pending connection; when all slots are busy the oldest
     * still waiting for its request (most likely an idle preconnect), or
     * else the oldest, makes room. */
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    for (WiFiClient c = server.accept(); c; c = server.accept()) {
#else
    for (WiFiClient c = server.available(); c; c = server.available()) {
#endif
        Slot *target = nullptr;
        for (Slot &s : slots)
            if (!s.client) {
                target = &s;
                break;
            }
        if (!target) {
            for (Slot &s : slots)
                if (!s.sending && (!target || (int32_t)(s.since - target->since) < 0)) target = &s;
            if (!target) {
                target = &slots[0];
                for (Slot &s : slots)
                    if ((int32_t)(s.since - target->since) < 0) target = &s;
            }
            close_slot(*target);
        }
        target->client = c;
        target->used = 0;
        target->since = millis();
        target->sending = false;
    }
    for (Slot &s : slots) {
        if (!s.client) continue;
        if (s.sending) {
            continue_send(s);
            continue;
        }
        while (s.client.available() && s.used < REQUEST_MAX - 1) s.request[s.used++] = s.client.read();
        s.request[s.used] = '\0';
        /* The request line is enough; the headers are not needed. */
        if (strstr(s.request, "\r\n") || s.used == REQUEST_MAX - 1) {
            respond(s);
            continue_send(s);
        } else if (!s.client.connected() || millis() - s.since > IDLE_MS) {
            close_slot(s);
        }
    }
}
