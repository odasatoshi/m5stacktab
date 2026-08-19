# ESP-IDF コンポーネント。ホストテストは test_vt100.cpp を直接コンパイルして走らせるので
# ここには含めない (test_vt100.cpp のヘッダのコマンドを参照)。
idf_component_register(SRCS "vt100.cpp" INCLUDE_DIRS ".")
