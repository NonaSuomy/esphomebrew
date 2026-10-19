"""Shared, idempotent ESP-Hosted DMA adaptation for ESP-IDF 6.1."""
def patch_hosted_spi_source(spi_source):
    if not spi_source.exists():
        return
    source = spi_source.read_text()
    marker = "    t.flags |= SPI_TRANS_DMA_BUFFER_ALIGN_MANUAL;"
    addition = """
#if defined(SPI_TRANS_DMA_USE_PSRAM)
    /* IDF 6.1 validates DMA capability and cache-line length alignment.
     * Permit its bounce buffer when a hosted pool buffer needs realignment. */
    t.flags &= ~SPI_TRANS_DMA_BUFFER_ALIGN_MANUAL;
#if CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM
    t.flags |= SPI_TRANS_DMA_USE_PSRAM;
#endif
#endif"""
    old_addition = """
#if CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM && defined(SPI_TRANS_DMA_USE_PSRAM)
    t.flags |= SPI_TRANS_DMA_USE_PSRAM;
#endif"""
    source = source.replace(old_addition, "")
    if marker in source and addition not in source:
        spi_source.write_text(source.replace(marker, marker + addition, 1))
        print("IDF 6.1 compatibility: enabled hosted SPI PSRAM DMA transactions")

