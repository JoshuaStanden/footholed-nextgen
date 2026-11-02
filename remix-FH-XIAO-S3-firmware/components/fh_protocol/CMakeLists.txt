# fh_protocol — shared UART wire protocol component (FHAdvanced).
# Plain C, no ESP-IDF component dependencies (no REQUIRES needed): it only uses
# the C standard library (stdint/stddef/string). Included by all node firmwares
# via EXTRA_COMPONENT_DIRS / a local components/ dir so the frame layout cannot
# drift.
idf_component_register(
    SRCS "fh_frame.c"
    INCLUDE_DIRS "include"
)
