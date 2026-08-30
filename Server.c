#include <winsock2.h>
#include <ws2tcpip.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#include "include/Packet.h"
#include "include/Config.h"
#include "include/CRC.h"
#include "include/RSWrapper.h"
#include "include/rs.h"

typedef struct
{
    unsigned char
        shards[TOTAL_SHARDS][SHARD_SIZE];

    unsigned char
        present[TOTAL_SHARDS];

    /* actual byte count of each data shard (as reported by the
       sender); only meaningful for indices < DATA_SHARDS */
    uint16_t shard_sizes[DATA_SHARDS];

    uint16_t received_count;

    int reconstructed;

} BlockBuffer;

typedef struct
{
    uint32_t message_id;

    uint32_t block_count;
    uint32_t message_len;

    BlockBuffer* blocks;

    /* how many blocks are fully present/reconstructed so far */
    uint32_t completed_count;

    int active;

    /* who to send the status packet back to once this message is
       finished (or abandoned) */
    struct sockaddr_in client_addr;

    /* GetTickCount() value the last time a shard was received for
       this message; used to detect an abandoned message that the
       client has stopped sending shards for */
    DWORD last_activity;

} MessageState;

/* Sends an ACK/FAILURE status packet for message_id back to addr. */
static void
send_status(
    SOCKET sock,
    const struct sockaddr_in* addr,
    uint32_t message_id,
    uint32_t status)
{
    StatusPacket resp;

    memset(&resp, 0, sizeof(resp));

    resp.message_id = message_id;
    resp.status = status;

    resp.crc32 =
        crc32(&resp, offsetof(StatusPacket, crc32));

    sendto(
        sock,
        (char*)&resp,
        sizeof(resp),
        0,
        (struct sockaddr*)addr,
        sizeof(*addr));
}

static void
reset_message_state(MessageState* state)
{
    if (state->blocks)
        free(state->blocks);

    memset(state, 0, sizeof(*state));
}

static void
start_new_message(
    MessageState* state,
    const Packet* packet,
    const struct sockaddr_in* client_addr,
    SOCKET sock,
    uint32_t* last_resolved_id,
    int* have_last_resolved)
{
    if (state->active &&
        state->completed_count < state->block_count)
    {
        /* the previous message never finished before this new one
           showed up -- it's never going to complete now, so let
           the client know it failed */
        send_status(
            sock,
            &state->client_addr,
            state->message_id,
            STATUS_FAILURE);

        *last_resolved_id = state->message_id;
        *have_last_resolved = 1;
    }

    reset_message_state(state);

    state->message_id = packet->message_id;
    state->block_count = packet->block_count;
    state->message_len = packet->message_len;
    state->client_addr = *client_addr;
    state->last_activity = GetTickCount();

    state->blocks =
        calloc(
            state->block_count,
            sizeof(BlockBuffer));

    state->active = (state->blocks != NULL);

    if (!state->active)
    {
        fprintf(stderr,
                "calloc() failed for %u blocks\n",
                state->block_count);
    }
}

/* Attempts to reassemble and print the fully-received/reconstructed
   message once every block has enough shards. */
static void
try_finish_message(
    MessageState* state,
    SOCKET sock,
    uint32_t* last_resolved_id,
    int* have_last_resolved)
{
    if (!state->active)
        return;

    if (state->completed_count < state->block_count)
        return;

    unsigned char* full_message =
        malloc(
            (size_t)state->block_count * BLOCK_SIZE);

    if (!full_message)
    {
        fprintf(stderr,
                "malloc() failed while reassembling "
                "message %u\n",
                state->message_id);

        reset_message_state(state);
        return;
    }

    size_t offset = 0;

    for (uint32_t b = 0;
         b < state->block_count;
         b++)
    {
        BlockBuffer* block = &state->blocks[b];

        for (int shard = 0;
             shard < DATA_SHARDS;
             shard++)
        {
            memcpy(
                full_message + offset,
                block->shards[shard],
                SHARD_SIZE);

            offset += SHARD_SIZE;
        }
    }

    size_t final_len =
        state->message_len < offset
        ? state->message_len
        : offset;

    printf(
        "\n=== Message %u fully reconstructed "
        "(%zu bytes) ===\n%.*s\n"
        "=== end message %u ===\n",
        state->message_id,
        final_len,
        (int)final_len,
        full_message,
        state->message_id);

    free(full_message);

    send_status(
        sock,
        &state->client_addr,
        state->message_id,
        STATUS_ACK);

    *last_resolved_id = state->message_id;
    *have_last_resolved = 1;

    reset_message_state(state);
}

/* Runs Reed-Solomon reconstruction on a block once it has at least
   DATA_SHARDS shards present. Returns 1 if the block is now
   complete (all data shards known, either received or
   reconstructed), 0 otherwise. */
static int
reconstruct_block(reed_solomon* rs, BlockBuffer* block)
{
    if (block->reconstructed)
        return 1;

    if (block->received_count < DATA_SHARDS)
        return 0;

    unsigned char* shard_ptrs[TOTAL_SHARDS];
    unsigned char marks[TOTAL_SHARDS];

    int missing_data = 0;

    for (int i = 0; i < TOTAL_SHARDS; i++)
    {
        shard_ptrs[i] = block->shards[i];
        marks[i] = block->present[i] ? 0 : 1;

        if (i < DATA_SHARDS && !block->present[i])
            missing_data++;
    }

    if (missing_data == 0)
    {
        /* all data shards already present, nothing to fix */
        block->reconstructed = 1;
        return 1;
    }

    int rc =
        reed_solomon_reconstruct(
            rs,
            shard_ptrs,
            marks,
            TOTAL_SHARDS,
            SHARD_SIZE);

    if (rc != 0)
    {
        /* not enough parity shards yet to recover the missing
           data shards -- keep waiting for more packets */
        return 0;
    }

    block->reconstructed = 1;
    return 1;
}

int main(void)
{
    WSADATA wsa;

    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0)
    {
        fprintf(stderr,
                "WSAStartup failed\n");

        return EXIT_FAILURE;
    }

    SOCKET sock = socket(AF_INET,
                         SOCK_DGRAM,
                         0);

    if (sock == INVALID_SOCKET)
    {
        fprintf(stderr,
                "socket() failed: %d\n",
                WSAGetLastError());

        WSACleanup();

        return EXIT_FAILURE;
    }

    struct sockaddr_in server_addr;

    memset(
        &server_addr,
        0,
        sizeof(server_addr));

    server_addr.sin_family =
        AF_INET;

    server_addr.sin_port =
        htons(PORT);

    server_addr.sin_addr.s_addr =
        INADDR_ANY;

    if (bind(
            sock,
            (struct sockaddr*)&server_addr,
            sizeof(server_addr))
        == SOCKET_ERROR)
    {
        fprintf(stderr,
                "bind() failed: %d\n",
                WSAGetLastError());

        closesocket(sock);

        WSACleanup();

        return EXIT_FAILURE;
    }

    printf(
        "Listening on UDP %d\n",
        PORT);

    reed_solomon* rs = rs_get();

    if (!rs)
    {
        fprintf(stderr,
                "reed_solomon_new() failed\n");

        closesocket(sock);
        WSACleanup();
        return EXIT_FAILURE;
    }

    MessageState state;

    memset(&state, 0, sizeof(state));

    /* Tracks the message_id of whichever message most recently
       resolved (ACK, FAILURE-on-override, or FAILURE-on-timeout).
       Shards that arrive late for that message -- e.g. trailing
       parity packets that show up just after the data shards
       already let the block reconstruct and finish the message --
       are then recognized as harmless stragglers and dropped,
       instead of being mistaken for the start of a new message
       that happens to reuse the same id. */
    uint32_t last_resolved_id = 0;
    int have_last_resolved = 0;

    while (1)
    {
        /* Wait for a packet, but not forever: if the currently
           in-flight message goes quiet for too long, give up on
           it and tell the client, rather than sitting idle until
           some future, unrelated message happens to bump it out. */
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);

        struct timeval tv;
        tv.tv_sec = SERVER_MESSAGE_TIMEOUT_MS / 1000;
        tv.tv_usec = (SERVER_MESSAGE_TIMEOUT_MS % 1000) * 1000;

        int sel =
            select(0, &readfds, NULL, NULL, &tv);

        if (sel == SOCKET_ERROR)
        {
            fprintf(stderr,
                    "select() failed: %d\n",
                    WSAGetLastError());

            continue;
        }

        if (sel == 0)
        {
            /* nothing arrived within the wait window */
            if (state.active &&
                state.completed_count < state.block_count &&
                (GetTickCount() - state.last_activity)
                    >= SERVER_MESSAGE_TIMEOUT_MS)
            {
                printf(
                    "Message %u timed out "
                    "(%u/%u blocks completed)\n",
                    state.message_id,
                    state.completed_count,
                    state.block_count);

                send_status(
                    sock,
                    &state.client_addr,
                    state.message_id,
                    STATUS_FAILURE);

                last_resolved_id = state.message_id;
                have_last_resolved = 1;

                reset_message_state(&state);
            }

            continue;
        }

        Packet packet;

        struct sockaddr_in client_addr;

        int client_len =
            sizeof(client_addr);

        int bytes = recvfrom(
            sock,
            (char*)&packet,
            sizeof(packet),
            0,
            (struct sockaddr*)&client_addr,
            &client_len);

        if (bytes == SOCKET_ERROR)
        {
            fprintf(stderr,
                    "recvfrom() failed: %d\n",
                    WSAGetLastError());

            continue;
        }

        /* CRC covers the whole zero-padded shard, matching what
           the client computes it over (see Client.c) */
        uint32_t calculated_crc =
            crc32(
                packet.data,
                SHARD_SIZE);

        if (calculated_crc != packet.crc32)
        {
            printf(
                "CRC FAILED "
                "(msg=%u block=%u shard=%u)\n",
                packet.message_id,
                packet.block_id,
                packet.shard_id);

            continue;
        }

        if (packet.shard_id >= TOTAL_SHARDS)
        {
            printf(
                "INVALID SHARD ID %u\n",
                packet.shard_id);

            continue;
        }

        if ((!state.active ||
             packet.message_id != state.message_id) &&
            have_last_resolved &&
            packet.message_id == last_resolved_id)
        {
            /* a late shard (typically trailing parity) for a
               message that already finished or failed -- it's no
               longer needed and must not be mistaken for the
               start of a new message reusing this id */
            continue;
        }

        if (!state.active ||
            packet.message_id != state.message_id)
        {
            start_new_message(
                &state,
                &packet,
                &client_addr,
                sock,
                &last_resolved_id,
                &have_last_resolved);

            if (!state.active)
                continue;
        }

        if (packet.block_id >= state.block_count)
        {
            printf(
                "INVALID BLOCK ID %u "
                "(block_count=%u)\n",
                packet.block_id,
                state.block_count);

            continue;
        }

        BlockBuffer* block =
            &state.blocks[packet.block_id];

        if (block->reconstructed)
        {
            /* this block is already complete, nothing more to do */
            continue;
        }

        state.last_activity = GetTickCount();

        memcpy(
            block->shards[packet.shard_id],
            packet.data,
            SHARD_SIZE);

        if (packet.shard_id < DATA_SHARDS)
        {
            block->shard_sizes[packet.shard_id] =
                packet.payload_size;
        }

        if (!block->present[packet.shard_id])
        {
            block->present[packet.shard_id] = 1;
            block->received_count++;
        }

        char ip[INET_ADDRSTRLEN];

        inet_ntop(
            AF_INET,
            &client_addr.sin_addr,
            ip,
            sizeof(ip));

        printf(
            "[%s:%d] "
            "msg=%u "
            "block=%u "
            "shard=%u/%u "
            "received=%u\n",
            ip,
            ntohs(client_addr.sin_port),
            packet.message_id,
            packet.block_id,
            packet.shard_id,
            packet.total_shards,
            block->received_count);

        if (block->received_count >= DATA_SHARDS)
        {
            if (reconstruct_block(rs, block))
            {
                printf(
                    "Block %u reconstructed "
                    "(msg=%u, %u/%u shards used)\n",
                    packet.block_id,
                    packet.message_id,
                    block->received_count,
                    TOTAL_SHARDS);

                state.completed_count++;

                try_finish_message(
                    &state,
                    sock,
                    &last_resolved_id,
                    &have_last_resolved);
            }
            else
            {
                printf(
                    "Block %u has %u/%u shards but "
                    "not enough parity to recover "
                    "missing data shards yet.\n",
                    packet.block_id,
                    block->received_count,
                    TOTAL_SHARDS);
            }
        }
    }

    reset_message_state(&state);

    closesocket(sock);

    WSACleanup();

    return 0;
}