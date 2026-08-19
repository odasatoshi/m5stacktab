idf_component_register(SRCS "blake2s.cpp" "noise.cpp" "crypto_mbedtls.cpp" "transport.cpp" "wg_netif.cpp" "salsa20.cpp" INCLUDE_DIRS "." REQUIRES mbedtls lwip esp_netif esp_timer)
