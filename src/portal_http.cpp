#include "portal_http.h"
#include <WiFi.h>

static const unsigned SLOTS = 4; /* lwIP has about 10 sockets for everything */
static const uint32_t IDLE_MS = 3000;   /* a connection that sends no request is dropped */
static const uint32_t SEND_MS = 2000;   /* bound on writing one response */
static const size_t REQUEST_MAX = 512;  /* only the request line is needed */

struct Slot {
    WiFiClient client;
    char request[REQUEST_MAX];
    size_t used;
    uint32_t since;
};
static Slot slots[SLOTS];
static WiFiServer server(80);
static const char *page_html;

void portal_http_begin(const char *page) {
    page_html = page;
    server.begin();
    server.setNoDelay(true);
}

static void write_all(WiFiClient &client, const uint8_t *data, size_t size) {
    uint32_t start = millis();
    while (size && client.connected() && millis() - start < SEND_MS) {
        size_t sent = client.write(data, size > 1436 ? 1436 : size);
        if (!sent) {
            delay(1);
            continue;
        }
        data += sent;
        size -= sent;
    }
}

static void respond(Slot &s) {
    char path[64] = "";
    sscanf(s.request, "GET %63s", path);
    WiFiClient &c = s.client;
    if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
        size_t length = strlen(page_html);
        char head[160];
        int n = snprintf(head, sizeof(head),
                         "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %u\r\n"
                         "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                         (unsigned)length);
        write_all(c, (const uint8_t *)head, n);
        write_all(c, (const uint8_t *)page_html, length);
    } else {
        char head[192];
        int n = snprintf(head, sizeof(head),
                         "HTTP/1.1 302 Found\r\nLocation: http://%s/\r\nContent-Length: 0\r\n"
                         "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                         WiFi.softAPIP().toString().c_str());
        write_all(c, (const uint8_t *)head, n);
    }
    c.stop();
}

void portal_http_poll() {
    /* Accept every pending connection; when all slots are busy the oldest
     * (most likely an idle preconnect) makes room. */
    for (WiFiClient c = server.available(); c; c = server.available()) {
        Slot *target = nullptr;
        for (Slot &s : slots)
            if (!s.client) {
                target = &s;
                break;
            }
        if (!target) {
            target = &slots[0];
            for (Slot &s : slots)
                if ((int32_t)(s.since - target->since) < 0) target = &s;
            target->client.stop();
        }
        target->client = c;
        target->used = 0;
        target->since = millis();
    }
    for (Slot &s : slots) {
        if (!s.client) continue;
        while (s.client.available() && s.used < REQUEST_MAX - 1) s.request[s.used++] = s.client.read();
        s.request[s.used] = '\0';
        /* The request line is enough; the headers are not needed. */
        if (strstr(s.request, "\r\n") || s.used == REQUEST_MAX - 1)
            respond(s);
        else if (!s.client.connected() || millis() - s.since > IDLE_MS)
            s.client.stop();
    }
}
