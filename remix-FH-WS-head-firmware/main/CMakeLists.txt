# main component — FHAdvanced-REMIX Waveshare HEAD (the face / display node).
# Display/touch/power = the PROVEN original-FootHoled board adapter
# (ST7796 SPI + TCA9554 + FT6336 + LVGL 9). REMIX adds the Bus-A HD peer link,
# PING/PONG liveness, and the status UI fed by the hub.
# REQUIRES lists every ESP-IDF component these sources include — keep in sync.
# Managed components (lvgl, esp_lcd_st7796, esp_io_expander_tca9554) come via
# idf_component.yml and are added by the component manager automatically.
idf_component_register(
    SRCS
        "fh_ws_main.c"        # app entry + LVGL loop + Bus-A wiring
        "fh_ws_board.cpp"     # PROVEN board adapter (ST7796/TCA9554/FT6336/LVGL)
        "fh_ws_ui.c"          # LVGL status screen
        "fh_ws_link.c"        # Bus-A SPI-Slave-HD transport (to the P4 hub)
    INCLUDE_DIRS "."
    REQUIRES
        driver              # gpio, ledc (umbrella)
        esp_driver_spi      # spi_slave_hd (Bus A) + spi_master (LCD)
        esp_driver_i2c      # i2c_master (FT6336 touch + TCA9554)
        esp_lcd             # esp_lcd_panel_io/ops (+ managed esp_lcd_st7796)
        esp_timer
        nvs_flash
        fh_protocol         # FROZEN shared wire protocol component
)

# NOTE: the Bus-A slave uses spi_slave_hd (quad, committed — see
# ../../remix-BUS-A-QUAD-HD-DECISION.md) on SPI3. The PROVEN board adapter uses
# SPI2 for the LCD. They are on SEPARATE hosts by design (R-SPIHOST in
# fh_ws_pins.h) — one master-role LCD bus, one slave-HD Bus-A bus.
