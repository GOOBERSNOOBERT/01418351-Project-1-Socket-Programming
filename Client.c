#include <winsock2.h>
#include <ws2tcpip.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#include "include/Packet.h"
#include "include/Chunker.h"
#include "include/CRC.h"
#include "include/RSWrapper.h"
#include "include/rs.h"

int main(void)
{
    WSADATA wsa;

    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0)
    {
        fprintf(stderr, "WSAStartup failed\n");
        return EXIT_FAILURE;
    }

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);

    if (sock == INVALID_SOCKET)
    {
        fprintf(stderr,
                "socket() failed: %d\n",
                WSAGetLastError());

        WSACleanup();
        return EXIT_FAILURE;
    }

    struct sockaddr_in server_addr;

    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);

    inet_pton(
        AF_INET,
        "127.0.0.1",
        &server_addr.sin_addr);

    reed_solomon* rs = rs_get();

    if (!rs)
    {
        fprintf(stderr,
                "reed_solomon_new() failed\n");

        closesocket(sock);
        WSACleanup();
        return EXIT_FAILURE;
    }

    uint32_t message_counter = 1;

    while (1)
    {
        char input[BUFFER_SIZE];

        printf("> ");

        if (!fgets(
                input,
                sizeof(input),
                stdin))
        {
            break;
        }

        ChunkedMessage* msg =
            chunk_message(
                (unsigned char*)input,
                strlen(input));

        if (!msg)
        {
            fprintf(stderr,
                    "chunk_message failed\n");

            continue;
        }

        for (uint32_t block_idx = 0;
             block_idx < msg->block_count;
             block_idx++)
        {
            Block* block =
                &msg->blocks[block_idx];

            /* Generate PARITY_SHARDS parity shards from the
               DATA_SHARDS data shards for this block. Data shards
               are already zero-padded to SHARD_SIZE by
               chunk_message(), so they can be fed to the encoder
               directly. */
            unsigned char* data_ptrs[DATA_SHARDS];
            unsigned char* parity_ptrs[PARITY_SHARDS];

            for (int i = 0; i < DATA_SHARDS; i++)
                data_ptrs[i] = block->shards[i];

            for (int i = 0; i < PARITY_SHARDS; i++)
                parity_ptrs[i] = block->shards[DATA_SHARDS + i];

            if (reed_solomon_encode(
                    rs,
                    data_ptrs,
                    parity_ptrs,
                    SHARD_SIZE) != 0)
            {
                fprintf(stderr,
                        "reed_solomon_encode() failed "
                        "(block=%u)\n",
                        block->block_id);

                continue;
            }

            /* Send every shard -- data and parity alike -- so the
               server has PARITY_SHARDS worth of redundancy to
               recover from lost UDP packets. */
            for (int shard = 0;
                 shard < TOTAL_SHARDS;
                 shard++)
            {
                Packet packet;

                memset(
                    &packet,
                    0,
                    sizeof(packet));

                packet.message_id =
                    message_counter;

                packet.block_count =
                    msg->block_count;

                packet.message_len =
                    (uint32_t)msg->total_len;

                packet.block_id =
                    block->block_id;

                packet.shard_id =
                    (uint16_t)shard;

                packet.total_shards =
                    TOTAL_SHARDS;

                packet.data_shards =
                    DATA_SHARDS;

                packet.parity_shards =
                    PARITY_SHARDS;

                /* data shards report their real (possibly partial)
                   size; parity shards are always a full shard */
                packet.payload_size =
                    (shard < DATA_SHARDS)
                    ? block->shard_sizes[shard]
                    : SHARD_SIZE;

                memcpy( packet.data,
                        block->shards[shard],
                        SHARD_SIZE);

                /* CRC covers the whole (zero-padded) shard so the
                   server can validate shards it later needs for
                   Reed-Solomon reconstruction, not just the
                   "meaningful" bytes. */
                packet.crc32 = crc32(packet.data, SHARD_SIZE);

                int sent =
                    sendto(
                        sock,
                        (char*)&packet,
                        sizeof(packet),
                        0,
                        (struct sockaddr*)&server_addr,
                        sizeof(server_addr));

                if (sent == SOCKET_ERROR)
                {
                    fprintf(stderr,
                            "sendto() failed: %d\n",
                            WSAGetLastError());
                }
            }
        }

        free_chunked_message(msg);

        /* Start the timer for this message's status response.
           We do NOT block indefinitely waiting for it -- we poll
           the socket with a bounded overall timeout and give up
           (TIMED OUT) once STATUS_TIMEOUT_MS has elapsed. */
        DWORD send_time = GetTickCount();
        const char* status_str = "TIMED OUT";

        while (1)
        {
            DWORD elapsed = GetTickCount() - send_time;

            if (elapsed >= STATUS_TIMEOUT_MS)
                break;

            DWORD remaining = STATUS_TIMEOUT_MS - elapsed;

            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(sock, &readfds);

            struct timeval tv;
            tv.tv_sec = remaining / 1000;
            tv.tv_usec = (remaining % 1000) * 1000;

            int sel =
                select(0, &readfds, NULL, NULL, &tv);

            if (sel <= 0)
            {
                /* timeout elapsed, or select() failed */
                break;
            }

            StatusPacket resp;

            int recvd =
                recvfrom(
                    sock,
                    (char*)&resp,
                    sizeof(resp),
                    0,
                    NULL,
                    NULL);

            if (recvd != sizeof(resp))
            {
                /* not a well-formed status packet, keep waiting */
                continue;
            }

            if (resp.message_id != message_counter)
            {
                /* stale response for a previous message */
                continue;
            }

            uint32_t calc_crc =
                crc32(&resp, offsetof(StatusPacket, crc32));

            if (calc_crc != resp.crc32)
            {
                /* response arrived but is corrupted, so we can't
                   trust its contents */
                status_str = "UNKNOWN";
            }
            else if (resp.status == STATUS_ACK)
            {
                status_str = "ACK";
            }
            else if (resp.status == STATUS_FAILURE)
            {
                status_str = "FAILURE";
            }
            else
            {
                status_str = "UNKNOWN";
            }

            break;
        }

        printf("Status: %s\n", status_str);

        message_counter++;
    }

    closesocket(sock);

    WSACleanup();

    return 0;
}