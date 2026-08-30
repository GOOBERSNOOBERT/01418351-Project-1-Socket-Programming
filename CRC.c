#include "include/CRC.h"

static uint32_t crc_table[256];

static int table_initialized = 0;

static void
crc32_init(void)
{
    for (uint32_t i = 0;
         i < 256;
         i++)
    {
        uint32_t crc = i;

        for (int bit = 0;
             bit < 8;
             bit++)
        {
            if (crc & 1)
            {
                crc =
                    (crc >> 1)
                    ^ 0xEDB88320;
            }
            else
            {
                crc >>= 1;
            }
        }

        crc_table[i] = crc;
    }

    table_initialized = 1;
}

uint32_t crc32(
    const void* data,
    size_t length)
{
    if (!table_initialized)
    {
        crc32_init();
    }

    const unsigned char* bytes =
        (const unsigned char*)data;

    uint32_t crc =
        0xFFFFFFFF;

    for (size_t i = 0;
         i < length;
         i++)
    {
        uint8_t index =
            (uint8_t)(
                (crc ^ bytes[i])
                & 0xFF);

        crc =
            (crc >> 8)
            ^ crc_table[index];
    }

    return crc ^ 0xFFFFFFFF;
}