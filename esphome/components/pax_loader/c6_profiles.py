"""Select approved C6 images from the resolved host configuration, never filenames."""
def select_profile(config):
    if not config or str(config.get('variant','')).upper() != 'ESP32C6':
        return ''
    transport = config.get('type', 'sdio')
    if transport == 'sdio' and config.get('bus_width',4) == 4:
        return 'c6-sdio'
    aio = {'reset_pin':2, 'cs_pin':14, 'clk_pin':15, 'mosi_pin':7,
           'miso_pin':9, 'data_ready_pin':10, 'handshake_pin':11}
    if (transport == 'spi' and config.get('active_high',False) and config.get('spi_mode',3) == 3 and
        config.get('handshake_active_high',True) and config.get('data_ready_active_high',True) and
        all(config.get(key) == value for key,value in aio.items())):
        return 'c6-spi-aio'
    return ''
