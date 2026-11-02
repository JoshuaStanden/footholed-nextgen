# main component — FHAdvanced-REMIX XIAO-S3 (2.4 GHz sniffer, SPI slave).
idf_component_register(
    SRCS
        "fh_xs3_main.c"
        "fh_xs3_link.c"      # SPI-slave transport (replaces the UART path)
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        esp_timer
        driver              # legacy umbrella (gpio moved out on IDF 6.x)
        esp_driver_gpio     # driver/gpio.h — REQUIRED on IDF 6.1 (the `driver`
                            # umbrella no longer covers it; found by the first
                            # fresh-clone build 2026-11-02)
        esp_driver_spi      # spi_slave_hd (quad SPI Slave HD, Bus A)
        fh_protocol         # FROZEN shared wire protocol component
)

# NOTE: if the build reports esp_driver_spi unknown, the `driver` umbrella
# covers spi_slave_hd on some IDF 6.x layouts — drop the esp_driver_spi line then.
