#ifndef COMMON_H
#define COMMON_H

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <fcntl.h>
#include <sched.h>

#define PORT 9090

// Part 1 Protocol Structures
typedef struct {
    uint32_t seq;        // 1-indexed sequence number
    uint32_t num_chunks; // Total chunks (N)
} chunk_request_t;

typedef struct {
    uint32_t seq;          // 1-indexed sequence number
    uint32_t payload_size; // Payload size in bytes
} chunk_header_t;

typedef struct {
    pthread_mutex_t buffer_lock;
    pthread_cond_t chunk_arrived;
    sem_t received_count;
    uint32_t chunks_received;
} shared_sync_t;

// Utility functions for safe socket communication over TCP
static inline ssize_t safe_recv(int fd, void *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t r = recv(fd, (char*)buf + total, len - total, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return total; // EOF
        total += r;
    }
    return total;
}

static inline ssize_t safe_send(int fd, const void *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t s = send(fd, (const char*)buf + total, len - total, 0);
        if (s < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += s;
    }
    return total;
}

static inline void encode_uint64(uint8_t *buf, uint64_t val) {
    buf[0] = (val >> 56) & 0xFF;
    buf[1] = (val >> 48) & 0xFF;
    buf[2] = (val >> 40) & 0xFF;
    buf[3] = (val >> 32) & 0xFF;
    buf[4] = (val >> 24) & 0xFF;
    buf[5] = (val >> 16) & 0xFF;
    buf[6] = (val >> 8)  & 0xFF;
    buf[7] = val         & 0xFF;
}

static inline uint64_t decode_uint64(const uint8_t *buf) {
    return ((uint64_t)buf[0] << 56) |
           ((uint64_t)buf[1] << 48) |
           ((uint64_t)buf[2] << 40) |
           ((uint64_t)buf[3] << 32) |
           ((uint64_t)buf[4] << 24) |
           ((uint64_t)buf[5] << 16) |
           ((uint64_t)buf[6] << 8)  |
           ((uint64_t)buf[7]);
}

static inline int set_cpu_affinity(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    return sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
}

#endif // COMMON_H
