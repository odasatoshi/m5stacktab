# main は全コンポーネントに暗黙依存するので REQUIRES は書かない
# (書くと暗黙依存が外れて esp_console などが見えなくなる)。
idf_component_register(SRCS "main.cpp" "wifi.cpp" "term_render.cpp" "ssh.cpp" "kbd_ui.cpp" "status_bar.cpp" "menu_ui.cpp" "kbd_hw.cpp" INCLUDE_DIRS ".")
# components/ime と components/vt100 は main の暗黙依存で入る
