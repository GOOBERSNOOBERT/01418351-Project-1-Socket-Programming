#include "include/Chunker.h"

#include <stdlib.h>
#include <string.h>

ChunkedMessage*
chunk_message(
    const unsigned char* input,
    size_t input_len)
{
    ChunkedMessage* msg =
        malloc(sizeof(ChunkedMessage));

    if (!msg)
        return NULL;

    uint32_t block_count =
        (uint32_t)(
            (input_len + BLOCK_SIZE - 1)
            / BLOCK_SIZE);

    if (block_count == 0)
        block_count = 1;

    msg->block_count =
        block_count;

    msg->total_len =
        input_len;

    msg->blocks =
        calloc(
            block_count,
            sizeof(Block));

    if (!msg->blocks)
    {
        free(msg);
        return NULL;
    }

    size_t consumed = 0;

    for (uint32_t block_id = 0;
         block_id < block_count;
         block_id++)
    {
        Block* block =
            &msg->blocks[block_id];

        block->block_id =
            block_id;

        for (int shard = 0;
             shard < DATA_SHARDS;
             shard++)
        {
            size_t remaining;

            if (consumed >= input_len)
                remaining = 0;
            else
                remaining =
                    input_len - consumed;

            size_t copy_size =
                remaining > SHARD_SIZE
                ? SHARD_SIZE
                : remaining;

            block->shard_sizes[shard] =
                (uint16_t)copy_size;

            memset(
                block->shards[shard],
                0,
                SHARD_SIZE);

            if (copy_size > 0)
            {
                memcpy(
                    block->shards[shard],
                    input + consumed,
                    copy_size);

                consumed += copy_size;
            }
        }
    }

    return msg;
}

void
free_chunked_message(
    ChunkedMessage* msg)
{
    if (!msg)
        return;

    free(msg->blocks);

    free(msg);
}