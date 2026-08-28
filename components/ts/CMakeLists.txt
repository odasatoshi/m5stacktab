idf_component_register(SRCS "ts_noise.cpp" "h2.cpp" "ts_control.cpp" "ts_client.cpp" "netmap.cpp" "disco.cpp" "disco_responder.cpp" INCLUDE_DIRS "." REQUIRES wg mbedtls lwip json tcp_transport)
