# Deliberately free of IDF dependencies: the same sources build on the host, where
# the link layer is actually tested (tools/run_host_tests.ps1).
idf_component_register(
    SRCS
        "cobs.c"
        "crc16.c"
        "frame.c"
        "codec.c"
        "messages.c"
    INCLUDE_DIRS "include"
)
