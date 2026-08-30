#ifndef CHUNKER_H
#define CHUNKER_H

#include <stdint.h>
#include <stddef.h>

#include "Config.h"

typedef struct
{
    uint32_t block_id;

    /* actual byte counts for the DATA_SHARDS data shards only;
       parity shards are always a full SHARD_SIZE */
    uint16_t shard_sizes[DATA_SHARDS];

    /* holds both the DATA_SHARDS data shards (indices
       0..DATA_SHARDS-1) and, once generated, the PARITY_SHARDS
       parity shards (indices DATA_SHARDS..TOTAL_SHARDS-1) */
    unsigned char shards[TOTAL_SHARDS][SHARD_SIZE];

} Block;

typedef struct
{
    uint32_t block_count;

    /* original, unpadded length of the input that was chunked */
    size_t total_len;

    Block* blocks;

} ChunkedMessage;

ChunkedMessage* 
chunk_message(
    const unsigned char* input,
    size_t input_len);

void 
free_chunked_message(
    ChunkedMessage* msg);

    
#endif