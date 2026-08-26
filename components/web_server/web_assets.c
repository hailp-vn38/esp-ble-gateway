#include "web_modules.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "web_http.h"

static const char *TAG = "web_assets";

// Same policy as the provisioning page: the dashboard loads only same-origin
// CSS/font plus inline script/style blocks (Plan v2 §64).
#define DASHBOARD_CSP                                                          \
    "default-src 'none'; connect-src 'self'; script-src 'unsafe-inline'; "     \
    "style-src 'unsafe-inline'; font-src 'self'; img-src 'self' data:; "       \
    "base-uri 'none'; form-action 'self'; frame-ancestors 'none'"

static void set_security_headers(httpd_req_t *request)
{
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(request, "Referrer-Policy", "no-referrer");
}

extern const uint8_t dashboard_html_start[] asm("_binary_dashboard_html_start");
extern const uint8_t dashboard_html_end[] asm("_binary_dashboard_html_end");
extern const uint8_t setup_html_gz_start[] asm("_binary_setup_html_gz_start");
extern const uint8_t setup_html_gz_end[] asm("_binary_setup_html_gz_end");
extern const uint8_t dashboard_css_start[] asm("_binary_dashboard_css_start");
extern const uint8_t dashboard_css_end[] asm("_binary_dashboard_css_end");
extern const uint8_t icons_css_start[] asm("_binary_icons_css_start");
extern const uint8_t icons_css_end[] asm("_binary_icons_css_end");
extern const uint8_t phosphor_woff2_start[] asm("_binary_Phosphor_woff2_start");
extern const uint8_t phosphor_woff2_end[] asm("_binary_Phosphor_woff2_end");

static esp_err_t send_embedded_file(httpd_req_t *request, const uint8_t *start,
                                    const uint8_t *end, const char *content_type,
                                    const char *cache_control, const char *csp)
{
    httpd_resp_set_type(request, content_type);
    httpd_resp_set_hdr(request, "Cache-Control", cache_control);
    if (csp != NULL) {
        httpd_resp_set_hdr(request, "Content-Security-Policy", csp);
    }
    set_security_headers(request);
    return httpd_resp_send(request, (const char *)start, end - start);
}

static esp_err_t send_embedded_gzip_file(httpd_req_t *request,
                                         const uint8_t *start,
                                         const uint8_t *end,
                                         const char *content_type,
                                         const char *cache_control)
{
    httpd_resp_set_type(request, content_type);
    httpd_resp_set_hdr(request, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(request, "Cache-Control", cache_control);
    httpd_resp_set_hdr(
        request, "Content-Security-Policy",
        "default-src 'none'; connect-src 'self'; script-src 'unsafe-inline'; "
        "style-src 'unsafe-inline'; img-src data:; base-uri 'none'; "
        "form-action 'self'; frame-ancestors 'none'");
    set_security_headers(request);
    return httpd_resp_send(request, (const char *)start, end - start);
}

static esp_err_t index_get_handler(httpd_req_t *request)
{
    return send_embedded_file(request, dashboard_html_start, dashboard_html_end,
                              "text/html; charset=utf-8", "no-cache",
                              DASHBOARD_CSP);
}

static bool host_equals_domain(const char *host, const char *domain)
{
    size_t domain_length = strlen(domain);
    return strncasecmp(host, domain, domain_length) == 0 &&
           (host[domain_length] == '\0' || host[domain_length] == ':');
}

static bool is_adguard_injection_request(httpd_req_t *request)
{
    char host[64] = {0};
    if (httpd_req_get_hdr_value_str(request, "Host", host, sizeof(host)) != ESP_OK) {
        return false;
    }
    return host_equals_domain(host, "local.adguard.org") ||
           host_equals_domain(host, "local.adguard.com");
}

static esp_err_t provisioning_index_get_handler(httpd_req_t *request)
{
    if (is_adguard_injection_request(request)) {
        ESP_LOGI(TAG, "Discarded AdGuard-injected content-script request");
        httpd_resp_set_status(request, "204 No Content");
        httpd_resp_set_type(request, "application/javascript");
        httpd_resp_set_hdr(request, "Cache-Control", "no-store");
        return httpd_resp_send(request, NULL, 0);
    }

    int64_t started_us = esp_timer_get_time();
    esp_err_t result = send_embedded_gzip_file(
        request, setup_html_gz_start, setup_html_gz_end,
        "text/html; charset=utf-8", "no-cache");
    ESP_LOGI(TAG, "Provisioning page sent: %u gzip bytes in %lld ms",
             (unsigned)(setup_html_gz_end - setup_html_gz_start),
             (long long)((esp_timer_get_time() - started_us) / 1000));
    return result;
}

static esp_err_t dashboard_css_get_handler(httpd_req_t *request)
{
    return send_embedded_file(request, dashboard_css_start, dashboard_css_end,
                              "text/css; charset=utf-8", "public, max-age=86400",
                              NULL);
}

static esp_err_t icons_css_get_handler(httpd_req_t *request)
{
    return send_embedded_file(request, icons_css_start, icons_css_end,
                              "text/css; charset=utf-8", "public, max-age=86400",
                              NULL);
}

static esp_err_t phosphor_font_get_handler(httpd_req_t *request)
{
    return send_embedded_file(request, phosphor_woff2_start, phosphor_woff2_end,
                              "font/woff2", "public, max-age=604800", NULL);
}

static esp_err_t favicon_get_handler(httpd_req_t *request)
{
    httpd_resp_set_status(request, "204 No Content");
    httpd_resp_set_hdr(request, "Cache-Control", "public, max-age=604800");
    return httpd_resp_send(request, NULL, 0);
}

static esp_err_t captive_redirect_handler(httpd_req_t *request)
{
    httpd_resp_set_status(request, "302 Found");
    httpd_resp_set_hdr(request, "Location", "/");
    return httpd_resp_send(request, NULL, 0);
}

esp_err_t web_assets_register_gateway(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_get_handler},
        {.uri = "/dashboard.css", .method = HTTP_GET,
         .handler = dashboard_css_get_handler},
        {.uri = "/icons.css", .method = HTTP_GET, .handler = icons_css_get_handler},
        {.uri = "/assets/Phosphor.woff2", .method = HTTP_GET,
         .handler = phosphor_font_get_handler},
        {.uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_get_handler},
    };
    return web_register_routes(server, routes, WEB_ARRAY_SIZE(routes));
}

esp_err_t web_assets_register_provisioning(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = provisioning_index_get_handler},
        {.uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_get_handler},
        {.uri = "/generate_204", .method = HTTP_GET,
         .handler = captive_redirect_handler},
        {.uri = "/hotspot-detect.html", .method = HTTP_GET,
         .handler = captive_redirect_handler},
        {.uri = "/connecttest.txt", .method = HTTP_GET,
         .handler = captive_redirect_handler},
        {.uri = "/ncsi.txt", .method = HTTP_GET,
         .handler = captive_redirect_handler},
    };
    return web_register_routes(server, routes, WEB_ARRAY_SIZE(routes));
}
