/* WebSocket Echo Server Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <sys/param.h>
#include "esp_netif.h"
#include "esp_eth.h"
#include "protocol_examples_common.h"
#include "esp_insights.h"
#include <esp_http_server.h>


/* A simple example that demonstrates using websocket echo server
 */
static const char *TAG = "ws_echo_server";


#include "Arduino.h"
#include "Shellminator.hpp"
#include "Shellminator-IO.hpp"
#include "Shellminator-Browser-Response.hpp" // <- It contains the webpage data


#ifdef CONFIG_ESP_INSIGHTS_TRANSPORT_HTTPS
extern const char insights_auth_key_start[] asm("_binary_insights_auth_key_txt_start");
extern const char insights_auth_key_end[] asm("_binary_insights_auth_key_txt_end");
#endif



// Create websocket object.
WebSocketsServer webSocket = WebSocketsServer( 81 );


// Create a Shellminator object, and initialize it to use WebSocketsServer
Shellminator shell( &webSocket );


const char logo[] =

"   _____ __         ____          _             __            \r\n"
"  / ___// /_  ___  / / /___ ___  (_)___  ____ _/ /_____  _____\r\n"
"  \\__ \\/ __ \\/ _ \\/ / / __ `__ \\/ / __ \\/ __ `/ __/ __ \\/ ___/\r\n"
" ___/ / / / /  __/ / / / / / / / / / / / /_/ / /_/ /_/ / /    \r\n"
"/____/_/ /_/\\___/_/_/_/ /_/ /_/_/_/ /_/\\__,_/\\__/\\____/_/     \r\n"
"\r\n\033[0;37m"
"Visit on GitHub:\033[1;32m https://github.com/dani007200964/Shellminator\r\n\r\n"

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

/*
 * async send function, which we put into the httpd work queue
 */
static void ws_async_send(void* arg) {
    static const char *data = "Async data";
    async_resp_arg* resp_arg = reinterpret_cast<async_resp_arg*>(arg);

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = reinterpret_cast<uint8_t*>(const_cast<char*>(data));  // Correct payload type
    ws_pkt.len = strlen(data);
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    httpd_ws_send_frame_async(resp_arg->hd, resp_arg->fd, &ws_pkt);
    free(resp_arg);
}

static esp_err_t trigger_async_send(httpd_handle_t handle, httpd_req_t *req) {
    async_resp_arg* resp_arg = static_cast<async_resp_arg*>(malloc(sizeof(async_resp_arg)));
    if (resp_arg == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    resp_arg->hd = req->handle;
    resp_arg->fd = httpd_req_to_sockfd(req);

    esp_err_t ret = httpd_queue_work(handle, ws_async_send, resp_arg);

    if (ret != ESP_OK) {
        free(resp_arg);
    }
    return ret;
}

// This function will be called on websocket GET event.
static esp_err_t shellminator_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "Handshake done, the new connection was opened");

        return ESP_OK;
    }
    httpd_ws_frame_t ws_pkt;
    uint8_t *buf = NULL;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    /* Set max_len = 0 to get the frame len */
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame failed to get frame len with %d", ret);
        return ret;
    }
    ESP_LOGI(TAG, "frame len is %d", ws_pkt.len);
    if (ws_pkt.len) {
        /* ws_pkt.len + 1 is for NULL termination as we are expecting a string */
        buf = (uint8_t*)calloc(1, ws_pkt.len + 1);
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
        ESP_LOGI(TAG, "Got packet with message: %s", ws_pkt.payload);
    }
    ESP_LOGI(TAG, "Packet type: %d", ws_pkt.type);
    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT)
    {
        Serial.printf(" get Text: %s\n", ws_pkt.payload);

        // In the Shellminator-IO files, the websocket channel
        // is implemented as a circular buffer. The incoming data
        // from the clients has to be pushed to this circular buffer
        // in the websocket event.
        shell.webSocketPush( ws_pkt.payload,  ws_pkt.len );
    }
    // ret = httpd_ws_send_frame(req, &ws_pkt);
    // if (ret != ESP_OK) {
    //     ESP_LOGE(TAG, "httpd_ws_send_frame failed with %d", ret);
    // }
    free(buf);
    return ret;
}




static const httpd_uri_t ws = {
        .uri        = "/ws",
        .method     = HTTP_GET,
        .handler    = shellminator_handler,
        .user_ctx   = NULL,
        .is_websocket = true
};


//------------------------------------------------------------
// generates a response for the index page.
//------------------------------------------------------------
extern const char root_start[] asm("_binary_index_html_start");
extern const char root_end[] asm("_binary_index_html_end");

static esp_err_t root_get_handler(httpd_req_t *req)
{
    const uint32_t root_len = root_end - root_start;

    ESP_LOGI(TAG, "Serve root");
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


static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    // Start the httpd server
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        // Registering the ws handler
        ESP_LOGI(TAG, "Registering URI handlers");

//   // Attach page handlers.
//   server.on("/", handleIndex);
//   server.on("/xterm.js", handleXtermJs);
//   server.on("/xterm.css", handleXtermCss);
//   server.on("/xterm-addon-web-links.js", handleXtermAddonWebLinks);
//   server.onNotFound(handleNotFound);

        httpd_register_uri_handler(server, &ws);
        httpd_register_uri_handler(server, &root);
        httpd_register_uri_handler(server, &xterm_js);
        httpd_register_uri_handler(server, &xterm_css);
        httpd_register_uri_handler(server, &xterm_addon_web_links_js);
        httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, http_404_error_handler);
        return server;
    }

    ESP_LOGI(TAG, "Error starting server!");
    return NULL;
}

static esp_err_t stop_webserver(httpd_handle_t server)
{
    // Stop the httpd server
    return httpd_stop(server);
}

static void disconnect_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    httpd_handle_t* server = (httpd_handle_t*) arg;
    if (*server) {
        ESP_LOGI(TAG, "Stopping webserver");
        if (stop_webserver(*server) == ESP_OK) {
            *server = NULL;
        } else {
            ESP_LOGE(TAG, "Failed to stop http server");
        }
    }
}

static void connect_handler(void* arg, esp_event_base_t event_base,
                            int32_t event_id, void* event_data)
{
    httpd_handle_t* server = (httpd_handle_t*) arg;
    if (*server == NULL) {
        ESP_LOGI(TAG, "Starting webserver");
        *server = start_webserver();
    }
}



static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    // Start the httpd server
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        // Registering the ws handler
        ESP_LOGI(TAG, "Registering URI handlers");

//   // Attach page handlers.
//   server.on("/", handleIndex);
//   server.on("/xterm.js", handleXtermJs);
//   server.on("/xterm.css", handleXtermCss);
//   server.on("/xterm-addon-web-links.js", handleXtermAddonWebLinks);
//   server.onNotFound(handleNotFound);

        httpd_register_uri_handler(server, &ws);
        httpd_register_uri_handler(server, &root);
        httpd_register_uri_handler(server, &xterm_js);
        httpd_register_uri_handler(server, &xterm_css);
        httpd_register_uri_handler(server, &xterm_addon_web_links_js);
        httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, http_404_error_handler);
        return server;
    }

    ESP_LOGI(TAG, "Error starting server!");
    return NULL;
}

static esp_err_t stop_webserver(httpd_handle_t server)
{
    // Stop the httpd server
    return httpd_stop(server);
}

static void disconnect_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    httpd_handle_t* server = (httpd_handle_t*) arg;
    if (*server) {
        ESP_LOGI(TAG, "Stopping webserver");
        if (stop_webserver(*server) == ESP_OK) {
            *server = NULL;
        } else {
            ESP_LOGE(TAG, "Failed to stop http server");
        }
    }
}

static void connect_handler(void* arg, esp_event_base_t event_base,
                            int32_t event_id, void* event_data)
{
    httpd_handle_t* server = (httpd_handle_t*) arg;
    if (*server == NULL) {
        ESP_LOGI(TAG, "Starting webserver");
        *server = start_webserver();
    }
}


extern "C" void  app_main()
{
    initArduino();
    Serial.begin( 115200 );

    while( !Serial );

    shell.clear();
    shell.attachLogo( logo );
    Serial.println( "RetroVMS starting..." );

    // initialize shell object.
    shell.begin( "RetroVMS" );


    static httpd_handle_t server = NULL;

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

#ifdef CONFIG_ESP_INSIGHTS_ENABLED
    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    ESP_ERROR_CHECK(example_connect());

    esp_insights_config_t config = {
        .log_type = ESP_DIAG_LOG_TYPE_ERROR | ESP_DIAG_LOG_TYPE_WARNING | ESP_DIAG_LOG_TYPE_EVENT,
#ifdef CONFIG_ESP_INSIGHTS_TRANSPORT_HTTPS
        .auth_key = insights_auth_key_start,
#endif
    };
    esp_err_t ret = esp_insights_init(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize ESP Insights, err:0x%x", ret);
    }
    ESP_ERROR_CHECK(ret);
#endif

    /* Register event handlers to stop the server when Wi-Fi or Ethernet is disconnected,
     * and re-start it upon connection.
     */
#ifdef CONFIG_EXAMPLE_CONNECT_WIFI
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &connect_handler, &server));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnect_handler, &server));
#endif // CONFIG_EXAMPLE_CONNECT_WIFI
#ifdef CONFIG_EXAMPLE_CONNECT_ETHERNET
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &connect_handler, &server));
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ETHERNET_EVENT_DISCONNECTED, &disconnect_handler, &server));
#endif // CONFIG_EXAMPLE_CONNECT_ETHERNET

    /* Start the server for the first time */
    server = start_webserver();

while (1){

  shell.update();
  vTaskDelay(10/portTICK_PERIOD_MS);

}

}
