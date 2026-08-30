#ifndef CONFIG_H
#define CONFIG_H

#define SHARD_SIZE 128

#define DATA_SHARDS 8
#define PARITY_SHARDS 4

#define TOTAL_SHARDS \
    (DATA_SHARDS + PARITY_SHARDS)

#define BLOCK_SIZE \
    (SHARD_SIZE * DATA_SHARDS)

#define PORT 8080
#define BUFFER_SIZE 4096

/* how long (in milliseconds) the client waits for a status packet
   from the server after sending a message before giving up and
   reporting "TIMED OUT" */
#define STATUS_TIMEOUT_MS 3000

/* how long (in milliseconds) the server will wait without
   receiving any new shard for the current in-flight message
   before giving up on it and proactively sending STATUS_FAILURE,
   rather than waiting for the next message to bump it out */
#define SERVER_MESSAGE_TIMEOUT_MS 5000

#endif