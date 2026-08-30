#ifndef PACKET_H
#define PACKET_H

#include <stdint.h>

#include "Config.h"

typedef struct
{
    uint32_t message_id;

    /* total number of blocks the original message was split into,
       and the original (unpadded) message length in bytes -- both
       needed by the receiver to know when a message is fully
       reassembled and how to trim the final block's padding. */
       
    uint32_t block_count;
    uint32_t message_len;

    uint32_t block_id;

    uint16_t shard_id;
    uint16_t total_shards;
    uint16_t data_shards;
    uint16_t parity_shards;

    uint16_t payload_size;

    uint32_t crc32;

    unsigned char data[SHARD_SIZE];

} Packet;

/* Sent by the server back to the client once a message has either
   been fully reconstructed (STATUS_ACK) or is known to have failed
   (STATUS_FAILURE, e.g. a new message arrived before the previous
   one could be completed). crc32 covers every field above it so
   the client can detect a corrupted/garbled status response. */
typedef struct
{
    uint32_t message_id;
    uint32_t status;

    uint32_t crc32;

} StatusPacket;

#define STATUS_ACK     0
#define STATUS_FAILURE 1

#endif