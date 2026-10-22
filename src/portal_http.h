#pragma once
/* Minimal HTTP server for the captive portal: "/" returns the scanner
 * page, every other URL (including the phones' connectivity checks)
 * redirects to it. Unlike the synchronous WebServer, several connections
 * are served side by side, so an idle browser preconnect cannot hold up
 * the page request behind it. */
void portal_http_begin(const char *page);
void portal_http_poll();
