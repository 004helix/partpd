/*
 *
 * Pulseaudio RTP source
 *
 * Copyright 2017 Raman Shishniou
 *
 * Build:
 *   $ gcc -Wall -o partpd partpd.c
 *
 * RTP proxy:
 *   $ partpd <stream-port> <pulseaudio-pipe-path>
 *
 * Pulseaudio source configuration:
 *   $ pactl load-module module-pipe-source file=<pulseaudio-pipe-path> format=s16be rate=44100 channels=2 source_name=partpd_source
 *
 * RTP sender example (ffmpeg):
 *   $ ffmpeg -re -i <input> -acodec pcm_s16be -ar 44100 -ac 2 -f rtp rtp://<partpd-host>:<partpd-port>
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <errno.h>
#include <time.h>

#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>


#define NEXTSEQ(seq) ((seq) == 65535 ? 0 : (seq) + 1)


void error(char *msg)
{
    perror(msg);
    exit(1);
}


void run(int sock, int pipefd)
{
    int cc, payload;
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    unsigned char buffer[4096];
    uint16_t sequence, expected;
    uint32_t tstamp, ssrc;
    ssize_t metadata_size;
    ssize_t data_size;
    ssize_t size, rv;
    int connected = 0;
    int skip;

    while (1) {
        if (connected) {
            size = recv(sock, buffer, sizeof(buffer), 0);

            if (size < 0 && errno == EINTR)
                continue;

            if (size < 0 && errno == EAGAIN) {
                fprintf(stderr, "RTP Stream disconnected\n");
                connected = 0;

                // dissolve the association
                bzero((char *) &addr, sizeof(addr));
                addr.sin_family = AF_UNSPEC;
                if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
                    error("connect(AF_UNSPEC)");

                continue;
            }

            if (size < 0)
                error("recv");

        } else {
            size = recvfrom(sock, buffer, sizeof(buffer), 0,
                            (struct sockaddr *)&addr, &len);

            if (size < 0 && (errno == EINTR || errno == EAGAIN))
                continue;

            if (size < 0)
                error("recvfrom");
        }

        skip = 0;

        if (size < 12) {
            fprintf(stderr, "RTP Packet too short\n");
            continue;
        }

        // check version
        if (buffer[0] >> 6 != 2) {
            fprintf(stderr, "Unsupported RTP version\n");
            continue;
        }

        // check padding
        if ((buffer[0] >> 5) & 1) {
            fprintf(stderr, "RTP padding not supported\n");
            continue;
        }

        // check extensions
        if ((buffer[0] >> 4) & 1) {
            fprintf(stderr, "RTP header extensions not supported\n");
            continue;
        }

        cc = buffer[0] & 0x0F;
        payload = buffer[1] & 0x7F;

        memcpy(&sequence, buffer + 2, 2);
        memcpy(&tstamp, buffer + 4, 4);
        memcpy(&ssrc, buffer + 8, 4);

        sequence = ntohs(sequence);
        tstamp = ntohl(tstamp);
        ssrc = ntohl(ssrc);

        metadata_size = 12 + cc * 4;
        if (metadata_size > size) {
            fprintf(stderr, "RTP packet too short. (CSRC)\n");
            continue;
        }

        data_size = size - metadata_size;

        if (connected) {
            if (sequence > expected) {
                fprintf(stderr, "out of order rtp packet: num %d, expected %d\n", sequence, expected);
                expected = NEXTSEQ(sequence);
            } else
            if (sequence < expected) {
                fprintf(stderr, "dropped rtp packet: num %d, expected %d\n", sequence, expected);
                skip++;
            } else
                expected = NEXTSEQ(sequence);
        } else {
            char peeraddr[64];
            unsigned peerport;

            if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
                error("connect");

            inet_ntop(AF_INET, &addr.sin_addr, peeraddr, sizeof(peeraddr));
            peerport = ntohs(addr.sin_port);

            fprintf(stderr, "RTP Stream connected from %s:%u, payload type %d\n", peeraddr, peerport, payload);

            expected = NEXTSEQ(sequence);
            connected = 1;
        }

        if (skip)
            continue;

        // send audio data to pulseaudio
        rv = write(pipefd, buffer + metadata_size, data_size);
        if (rv < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                fprintf(stderr, "pulseaudio pipe source overrun: num %u, timestamp %u, size %u\n", sequence, tstamp, (unsigned)data_size);
                continue;
            }
            error("pipe write");
        }
    }
}


int main(int argc, char **argv)
{
    int port, sock, pipefd, optval = 1;
    struct sockaddr_in addr;
    struct timeval timeout;

    signal(SIGPIPE, SIG_IGN);

    // check command line arguments
    if (argc < 3) {
        fprintf(stderr, "usage: %s <port> <pulseaudio-pipe>\n", argv[0]);
        return 1;
    }

    // parse port
    port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "bad port: %s\n", argv[1]);
        return 1;
    }

    // create listen socket
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
        error("socket");

    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
               (const void *)&optval , sizeof(optval));

    // server's address to listen on
    bzero((char *) &addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)port);

    // bind
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        error("bind");

    // set receive timeout
    timeout.tv_sec = 0;
    timeout.tv_usec = 500000; // 500 msec to reset stream counters
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout,
                   sizeof(timeout)) < 0)
        error("setsockopt failed");

    // open pipe
    pipefd = open(argv[2], O_WRONLY | O_NONBLOCK);
    if (pipefd < 0)
        error("pipe open");

    // run
    run(sock, pipefd);

    return 0;
}
