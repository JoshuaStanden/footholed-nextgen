# main component for the FHAdvanced-REMIX HUB (ESP32-P4-WIFI6-DB).
# REQUIRES lists EVERY ESP-IDF component these sources include. Keeping this in
# sync with the #includes is a build invariant (a new include needs its
# component added here IN THE SAME EDIT).
idf_component_register(
    SRCS
        "fh_hub_main.c"
        "fh_hub_board.c"
        "fh_hub_config.c"
        "fh_hub_logger.c"
        "fh_hub_time.c"
        "fh_hub_link.c"       # NEW: SPI link layer (replaces fh_hub_agg.c)
        "fh_hub_req.c"        # NEW: on-demand requester (blade REQ tracking)
        "fh_hub_sysstate.c"   # NEW: SYS_STATE (0x2D/0x2E) hub-answered verb
        "fh_hub_ble.c"        # pass-1 stub
        "fh_hub_health.c"
        "fh_hub_ui.c"
        "fh_hub_bridge.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver              # gpio, i2c_master, sdmmc_host (umbrella)
        esp_driver_spi      # spi_master (Bus A quad HD-ESSL + Bus B conventional)
        esp_serial_slave_link  # ESSL HD master for Bus A peers (idf-extra-component;
                               # see main/idf_component.yml). Rd_DMA/Wr_DMA to the
                               # spi_slave_hd peers (XIAO-S3/C5, Waveshare head).
        esp_driver_i2c      # i2c_master_* (optional hub-local I2C)
        sdmmc               # sdmmc_cmd.h / sdmmc_host.h (TF slot)
        fatfs               # esp_vfs_fat_sdmmc_mount
        vfs                 # esp_vfs_fat
        nvs_flash           # config override layer
        esp_timer           # esp_timer_get_time
        esp_hw_support      # esp_read_mac / esp_mac.h
        bt                  # NimBLE host (controller = onboard C6 via ESP-Hosted/SDIO)
        fh_protocol         # FROZEN shared wire protocol component
)

# NOTE (carried 5.5->6.1 port hazard): on some IDF 6.x layouts the split
# esp_driver_spi / esp_driver_i2c components are reachable via the `driver`
# umbrella instead. If the build reports an unknown component, drop the
# esp_driver_* line(s) — the `driver` umbrella covers them.
