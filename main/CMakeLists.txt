# main は全コンポーネントに暗黙依存するので REQUIRES は書かない
# (書くと暗黙依存が外れて esp_console などが見えなくなる)。
idf_component_register(SRCS "main.cpp" "wifi.cpp" "term_render.cpp" INCLUDE_DIRS ".")
