#include "uWS/uWS.h"

// ... (include other necessary headers)

void onWebSocketMessage(uWS::WebSocket<uWS::SERVER> *ws, char *message, size_t length, uWS::OpCode opCode) {
    // Handle WebSocket message (parse and process)
    // You may call the existing console logic here
    // For example: processConsoleCommand(message);
}

void onWebSocketConnection(uWS::WebSocket<uWS::SERVER> *ws, uWS::HttpRequest req) {
    ESP_LOGI(TAG, "WebSocket connected");
}

void onWebSocketDisconnection(uWS::WebSocket<uWS::SERVER> *ws, int code, char *message, size_t length) {
    ESP_LOGI(TAG, "WebSocket disconnected");
}

void app_main(void) {
    // ... (existing code)

    // Initialize WebSocket server
    uWS::App().ws<PROMPT_STR>("/ws", {
        .compression = uWS::SHARED_COMPRESSOR,
        .maxPayloadLength = 16 * 1024 * 1024,
        .idleTimeout = 10,
        .open = onWebSocketConnection,
        .message = onWebSocketMessage,
        .close = onWebSocketDisconnection
    }).listen(9000, [](auto *token) {
        if (token) {
            ESP_LOGI(TAG, "WebSocket listening on port 9000");
        }
    }).run();

    // ... (existing code)
}
