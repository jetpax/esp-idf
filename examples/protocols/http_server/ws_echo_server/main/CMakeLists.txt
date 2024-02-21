
set(requires
    esp_https_server
    )


idf_component_register(SRCS "ws_echo_server.cpp"
                    INCLUDE_DIRS "include"
                    REQUIRES ${requires}
                    EMBED_FILES index.html xterm.js xterm.css xterm-addon-web-links.js)
if (CONFIG_ESP_INSIGHTS_TRANSPORT_HTTPS)
    target_add_binary_data(${COMPONENT_TARGET} "insights_auth_key.txt" TEXT)
endif()
