/* Simple HTTP + SSL + WS Server Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "Arduino.h"

#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <sys/param.h>
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_wifi.h"
#include "protocol_examples_common.h"
#include <esp_https_server.h>
#include "keep_alive.h"
#include "sdkconfig.h"
#include "lwip/sockets.h"

#if !CONFIG_HTTPD_WS_SUPPORT
#error This example cannot be used unless HTTPD_WS_SUPPORT is enabled in esp-http-server component configuration
#endif


#include "Shellminator.hpp"
#include "Shellminator-IO.hpp"
#include "Shellminator-Browser-Response.hpp" // <- It contains the webpage data

#include "Commander-API.hpp"
#include "Commander-IO.hpp"
#include "Commander-API-Commands.hpp"



httpd_handle_t http_server = NULL;

// Create a Shellminator object
Shellminator shell;

// Create a Commander object.
Commander commander;

// Commander API-tree
Commander::API_t API_tree[] = {
    API_ELEMENT_MILLIS,
    API_ELEMENT_MICROS,
    API_ELEMENT_UPTIME,
    API_ELEMENT_PINMODE,
    API_ELEMENT_DIGITALWRITE,
    API_ELEMENT_DIGITALREAD,
    API_ELEMENT_ANALOGREAD,
    API_ELEMENT_IPCONFIG,
    API_ELEMENT_WIFISTAT,
    API_ELEMENT_WIFISCAN,
    API_ELEMENT_CONFIGTIME,
    API_ELEMENT_DATETIME,
    API_ELEMENT_NEOFETCH,
    API_ELEMENT_SIN,
    API_ELEMENT_COS,
    API_ELEMENT_ABS,
    API_ELEMENT_RANDOM,
    API_ELEMENT_NOT
};


const char logo[] =
"\033[38;05;208;1m\r\n"
"    ____       __           _    ____  ________\r\n"
"   / __ \\___  / /__________| |  / /  |/  / ___/\r\n"
"  / /_/ / _ \\/ __/ ___/ __ \\ | / / /|_/ /\\__ \\ \r\n"
" / _, _/  __/ /_/ /  / /_/ / |/ / /  / /___/ / \r\n"
"/_/ |_|\\___/\\__/_/   \\____/|___/_/  /_//____/  \r\n"
"                                               \r\n"
"\r\n\033[0;37m"
"Visit:\033[1;32m https://retrovms.com\r\n\r\n"
;


/*
 * Structure holding server handle
 * and internal socket fd in order
 * to use out of request send
 */
struct async_resp_arg {
    httpd_handle_t hd;
    int fd;
};



//------------------------------------------------------------
// generates a response to a ws request
//------------------------------------------------------------

static const char *TAG = "wss_echo_server";
static const size_t max_clients = 4;

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGE(TAG, "New WS opened: httpd_handle_t=%p, sockfd=%d, client_info:%d", req->handle,
            httpd_req_to_sockfd(req), httpd_ws_get_fd_info(req->handle, httpd_req_to_sockfd(req)));

        // At start, Commander does not know anything about our commands.
        // We have to attach the API_tree array from the previous steps
        // to Commander to work properly.
        commander.attachTree( API_tree );

        // Initialize Commander.
        commander.init();

        shell.attachCommander( &commander );

        // initialize shell object.
        shell.begin( &req->handle, httpd_req_to_sockfd(req), "RetroVMS" );

        return ESP_OK;
    }
    httpd_ws_frame_t ws_pkt;
    uint8_t *buf = NULL;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));

    // First receive the full ws message
    /* Set max_len = 0 to get the frame len */
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame failed to get frame len with %d", ret);
        return ret;
    }
    if (ws_pkt.len) {
        /* ws_pkt.len + 1 is for NULL termination as we are expecting a string */
        // buf = calloc(1, ws_pkt.len + 1);
        buf = static_cast<uint8_t*>(calloc(1, ws_pkt.len + 1));
        if (buf == NULL) {
            ESP_LOGE(TAG, "Failed to calloc memory for buf");
            return ESP_ERR_NO_MEM;
        }
        ws_pkt.payload = buf;
        /* Set max_len = ws_pkt.len to get the frame payload */
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "httpd_ws_recv_frame failed with %d", ret);
            free(buf);
            return ret;
        }
    }
    // If it was a PONG, update the keep-alive
    if (ws_pkt.type == HTTPD_WS_TYPE_PONG) {
        ESP_LOGD(TAG, "Received PONG message");
        free(buf);
        // return wss_keep_alive_client_is_active(httpd_get_global_user_ctx(req->handle),
        //         httpd_req_to_sockfd(req));
        // return wss_keep_alive_client_is_active(static_cast<wss_keep_alive_t>(httpd_get_global_user_ctx(req->handle)),
        //         httpd_req_to_sockfd(req));

    // If it was a TEXT message, just echo it back
    } else if (ws_pkt.type == HTTPD_WS_TYPE_TEXT || ws_pkt.type == HTTPD_WS_TYPE_PING || ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
            // ESP_LOGI(TAG, "Received packet with message: %s", ws_pkt.payload);
            shell.webSocketPush( ws_pkt.payload, ws_pkt.len );
        } else if (ws_pkt.type == HTTPD_WS_TYPE_PING) {
            // Response PONG packet to peer
            ESP_LOGI(TAG, "Got a WS PING frame, Replying PONG");
            ws_pkt.type = HTTPD_WS_TYPE_PONG;
        } else if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
            // Response CLOSE packet with no payload to peer
            ws_pkt.len = 0;
            ws_pkt.payload = NULL;
        }
        // ret = httpd_ws_send_frame(req, &ws_pkt);
        // if (ret != ESP_OK) {
        //     ESP_LOGE(TAG, "httpd_ws_send_frame failed with %d", ret);
        // }
        // ESP_LOGW(TAG, "ws_TXT: httpd_handle_t=%p, sockfd=%d, client_info:%d", req->handle,
        //          httpd_req_to_sockfd(req), httpd_ws_get_fd_info(req->handle, httpd_req_to_sockfd(req)));
        free(buf);
        return ret;
    }
    free(buf);
    return ESP_OK;
}

static const httpd_uri_t ws = {
        .uri        = "/ws",
        .method     = HTTP_GET,
        .handler    = ws_handler,
        .user_ctx   = NULL,
        .is_websocket = true,
        .handle_ws_control_frames = true
};


esp_err_t wss_open_fd(httpd_handle_t hd, int sockfd)
{
    ESP_LOGI(TAG, "New client connected %d", sockfd);
    // wss_keep_alive_t h = httpd_get_global_user_ctx(hd);
    wss_keep_alive_t h = static_cast<wss_keep_alive_t>(httpd_get_global_user_ctx(hd));
    return wss_keep_alive_add_client(h, sockfd);
}

void wss_close_fd(httpd_handle_t hd, int sockfd)
{
    ESP_LOGI(TAG, "Client disconnected %d", sockfd);
    wss_keep_alive_t h = static_cast<wss_keep_alive_t>(httpd_get_global_user_ctx(hd));
    wss_keep_alive_remove_client(h, sockfd);
    close(sockfd);
}

static void send_hello(void *arg)
{
    static const char * data = "Hello client";
    // struct async_resp_arg *resp_arg = arg;
    struct async_resp_arg* resp_arg = static_cast<struct async_resp_arg*>(arg);
    httpd_handle_t hd = resp_arg->hd;
    int fd = resp_arg->fd;
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = (uint8_t*)data;
    ws_pkt.len = strlen(data);
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    httpd_ws_send_frame_async(hd, fd, &ws_pkt);
    delete resp_arg;
}

static void send_ping(void *arg)
{
    // struct async_resp_arg *resp_arg = arg;
    struct async_resp_arg* resp_arg = static_cast<struct async_resp_arg*>(arg);
    httpd_handle_t hd = resp_arg->hd;
    int fd = resp_arg->fd;
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = NULL;
    ws_pkt.len = 0;
    ws_pkt.type = HTTPD_WS_TYPE_PING;

    httpd_ws_send_frame_async(hd, fd, &ws_pkt);
    delete resp_arg;
}

bool client_not_alive_cb(wss_keep_alive_t h, int fd)
{
    ESP_LOGE(TAG, "Client not alive, closing fd %d", fd);
    httpd_sess_trigger_close(wss_keep_alive_get_user_ctx(h), fd);
    return true;
}

bool check_client_alive_cb(wss_keep_alive_t h, int fd)
{
    ESP_LOGD(TAG, "Checking if client (fd=%d) is alive", fd);
    struct async_resp_arg *resp_arg = new struct async_resp_arg;
    resp_arg->hd = wss_keep_alive_get_user_ctx(h);
    resp_arg->fd = fd;

    if (httpd_queue_work(resp_arg->hd, send_ping, resp_arg) == ESP_OK) {
        return true;
    }
    return false;
}

//------------------------------------------------------------
// generates a response for the index page.
//------------------------------------------------------------
extern const char root_start[] asm("_binary_index_html_start");
extern const char root_end[] asm("_binary_index_html_end");

static esp_err_t root_get_handler(httpd_req_t *req)
{
    const uint32_t root_len = root_end - root_start;

    ESP_LOGE(TAG, "Serve root: httpd_handle_t=%p, sockfd=%d, client_info:%d", req->handle,
                 httpd_req_to_sockfd(req), httpd_ws_get_fd_info(req->handle, httpd_req_to_sockfd(req)));
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, root_start, root_len);

    return ESP_OK;
}

static const httpd_uri_t root = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = root_get_handler
};

//------------------------------------------------------------
// generates a response /favicon.ico
//------------------------------------------------------------
static esp_err_t favicon_get_handler(httpd_req_t *req)
{
    extern const unsigned char favicon_ico_start[] asm("_binary_favicon_ico_start");
    extern const unsigned char favicon_ico_end[]   asm("_binary_favicon_ico_end");
    const size_t favicon_ico_size = (favicon_ico_end - favicon_ico_start);
    httpd_resp_set_type(req, "image/x-icon");
    httpd_resp_send(req, (const char *)favicon_ico_start, favicon_ico_size);
    return ESP_OK;
}

static const httpd_uri_t favicon = {
    .uri = "/favicon.ico",
    .method = HTTP_GET,
    .handler = favicon_get_handler
};

//------------------------------------------------------------
// generates a response /logo.svg
//------------------------------------------------------------
static esp_err_t logo_get_handler(httpd_req_t *req)
{
    extern const unsigned char logo_svg_start[] asm("_binary_logo_svg_start");
    extern const unsigned char logo_svg_end[]   asm("_binary_logo_svg_end");
    const size_t logo_svg_size = (logo_svg_end - logo_svg_start);
    httpd_resp_set_type(req, "image/svg+xml");
    httpd_resp_send(req, (const char *)logo_svg_start, logo_svg_size-1);
    return ESP_OK;
}

static const httpd_uri_t servelogo = {
    .uri = "/logo.svg",
    .method = HTTP_GET,
    .handler = logo_get_handler
};

//------------------------------------------------------------
// generates a response /close.svg
//------------------------------------------------------------
static esp_err_t close_get_handler(httpd_req_t *req)
{
    extern const unsigned char close_svg_start[] asm("_binary_close_svg_start");
    extern const unsigned char close_svg_end[]   asm("_binary_close_svg_end");
    const size_t close_svg_size = (close_svg_end - close_svg_start);
    httpd_resp_set_type(req, "image/svg+xml");
    httpd_resp_send(req, (const char *)close_svg_start, close_svg_size-1);
    return ESP_OK;
}

static const httpd_uri_t closesvg = {
    .uri = "/close.svg",
    .method = HTTP_GET,
    .handler = close_get_handler
};
//------------------------------------------------------------
// generates a response for /xterm.js
//------------------------------------------------------------

extern const char xterm_js_start[] asm("_binary_xterm_js_start");
extern const char xterm_js_end[] asm("_binary_xterm_js_end");

static esp_err_t xterm_js_handler(httpd_req_t *req)
{
    const uint32_t xterm_js_len = xterm_js_end - xterm_js_start;

    ESP_LOGI(TAG, "Serve xterm_js");
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_send(req, xterm_js_start, xterm_js_len);

    return ESP_OK;
}

static const httpd_uri_t xterm_js = {
    .uri = "/xterm.js",
    .method = HTTP_GET,
    .handler = xterm_js_handler
};


//------------------------------------------------------------
// generates a response for /xterm.css
//------------------------------------------------------------

extern const char xterm_css_start[] asm("_binary_xterm_css_start");
extern const char xterm_css_end[] asm("_binary_xterm_css_end");

static esp_err_t xterm_css_handler(httpd_req_t *req)
{
    const uint32_t xterm_css_len = xterm_css_end - xterm_css_start;

    ESP_LOGI(TAG, "Serve xterm_css");
    httpd_resp_set_type(req, "text/css");
    httpd_resp_send(req, xterm_css_start, xterm_css_len);

    return ESP_OK;
}

static const httpd_uri_t xterm_css = {
    .uri = "/xterm.css",
    .method = HTTP_GET,
    .handler = xterm_css_handler
};

//------------------------------------------------------------
// generates a response for /xterm-addon-web-links.js
//------------------------------------------------------------

extern const char xterm_addon_web_links_js_start[] asm("_binary_xterm_addon_web_links_js_start");
extern const char xterm_addon_web_links_js_end[] asm("_binary_xterm_addon_web_links_js_end");

static esp_err_t xterm_addon_web_links_js_handler(httpd_req_t *req)
{
    const uint32_t xterm_addon_web_links_js_len = xterm_addon_web_links_js_end - xterm_addon_web_links_js_start;

    ESP_LOGI(TAG, "Serve xterm_addon_web_links_js");
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_send(req, xterm_addon_web_links_js_start, xterm_addon_web_links_js_len);

    return ESP_OK;
}

static const httpd_uri_t xterm_addon_web_links_js = {
    .uri = "/xterm-addon-web-links.js",
    .method = HTTP_GET,
    .handler = xterm_addon_web_links_js_handler
};


//------------------------------------------------------------
// HTTP Error (404) Handler - Redirects all requests to the root page
//------------------------------------------------------------
esp_err_t http_404_error_handler(httpd_req_t *req, httpd_err_code_t err)
{
    // Set status
    httpd_resp_set_status(req, "302 Temporary Redirect");
    // Redirect to the "/" root directory
    httpd_resp_set_hdr(req, "Location", "/");
    // iOS requires content in the response to detect a captive portal, simply redirecting is not sufficient.
    httpd_resp_send(req, "Redirect to the captive portal", HTTPD_RESP_USE_STRLEN);

    ESP_LOGI(TAG, "Redirecting to root");
    return ESP_OK;
}



static httpd_handle_t start_http_server(void)
{
    // Prepare keep-alive engine
    // wss_keep_alive_config_t keep_alive_config = KEEP_ALIVE_CONFIG_DEFAULT();
    // keep_alive_config.max_clients = max_clients;
    // keep_alive_config.client_not_alive_cb = client_not_alive_cb;
    // keep_alive_config.check_client_alive_cb = check_client_alive_cb;
    // wss_keep_alive_t keep_alive = wss_keep_alive_start(&keep_alive_config);

    httpd_handle_t server = NULL;

    ESP_LOGI(TAG, "Starting server");

    httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();
    conf.httpd.max_open_sockets = max_clients;
    // conf.httpd.global_user_ctx = keep_alive;
    // conf.httpd.open_fn = wss_open_fd;
    // conf.httpd.close_fn = wss_close_fd;

    extern const unsigned char servercert_start[] asm("_binary_servercert_pem_start");
    extern const unsigned char servercert_end[]   asm("_binary_servercert_pem_end");
    conf.servercert = servercert_start;
    conf.servercert_len = servercert_end - servercert_start;

    extern const unsigned char prvtkey_pem_start[] asm("_binary_prvtkey_pem_start");
    extern const unsigned char prvtkey_pem_end[]   asm("_binary_prvtkey_pem_end");
    conf.prvtkey_pem = prvtkey_pem_start;
    conf.prvtkey_len = prvtkey_pem_end - prvtkey_pem_start;

    // Start the httpd server
    // ESP_LOGI(TAG, "Starting server on port: '%d'", conf.httpd.server_port);
    if (httpd_ssl_start(&server, &conf) == ESP_OK) {
        ESP_LOGI(TAG, "Registering URI handlers");
        // Set URI handlers
        ESP_LOGI(TAG, "Registering URI handlers");
        httpd_register_uri_handler(server, &ws);
        httpd_register_uri_handler(server, &root);
        httpd_register_uri_handler(server, &favicon);
        httpd_register_uri_handler(server, &servelogo);
        httpd_register_uri_handler(server, &closesvg);
        httpd_register_uri_handler(server, &xterm_js);
        httpd_register_uri_handler(server, &xterm_css);
        httpd_register_uri_handler(server, &xterm_addon_web_links_js);
        httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, http_404_error_handler);

        // wss_keep_alive_set_user_ctx(keep_alive, server);

        return server;
    }
    ESP_LOGI(TAG, "Error starting server!");
    return NULL;
}

static esp_err_t stop_http_server(httpd_handle_t server)
{
    // Stop keep alive thread
    // wss_keep_alive_stop(reinterpret_cast<wss_keep_alive_t>(httpd_get_global_user_ctx(server)));
    // Stop the httpd server
    return httpd_ssl_stop(server);
}

static void disconnect_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    httpd_handle_t* server = (httpd_handle_t*) arg;
    if (*server) {
        if (stop_http_server(*server) == ESP_OK) {
            *server = NULL;
        } else {
            ESP_LOGE(TAG, "Failed to stop https server");
        }
    }
}

static void connect_handler(void* arg, esp_event_base_t event_base,
                            int32_t event_id, void* event_data)
{
    httpd_handle_t* server = (httpd_handle_t*) arg;
    if (*server == NULL) {
        *server = start_http_server();
    }
}



extern "C" void  app_main(void)
{

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Register event handlers to start server when Wi-Fi or Ethernet is connected,
     * and stop server when disconnection happens.
     */

#ifdef CONFIG_EXAMPLE_CONNECT_WIFI
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &connect_handler, &http_server));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnect_handler, &http_server));
#endif // CONFIG_EXAMPLE_CONNECT_WIFI


    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    ESP_ERROR_CHECK(example_connect());

    /* This function demonstrates periodic sending Websocket messages
     * to all connected clients to this server
     */
    // wss_server_send_messages(&server);




    // Attach the logo.
    shell.attachLogo( logo );

    while (1) {
        shell.update();
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}
