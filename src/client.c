/* =========================================================================
 * OWNED BY: Student A (Client child process, socket handling, reassembly buffer)
 * ROLE: Handles Client initialization, negotiates parameters with Server,
 *       forks child processes to download chunks, and reassembles them.
 * ========================================================================= */

#include "common.h"

int main(int argc, char *argv[]) {
    uint32_t N = 4;        // Default chunks / child processes
    uint32_t T = 8;        // Default threads for operations
    const char *ip_str = "127.0.0.1";

    // Parse command line arguments
    int opt;
    while ((opt = getopt(argc, argv, "p:h:t:")) != -1) {
        switch (opt) {
            case 'p':
                N = atoi(optarg);
                break;
            case 'h':
                ip_str = optarg;
                break;
            case 't':
                T = atoi(optarg);
                break;
            default:
                fprintf(stderr, "Usage: %s -p <num_processes> [-h <ip>] [-t <num_threads>]\n", argv[0]);
                return 1;
        }
    }

    if (N == 0) {
        fprintf(stderr, "Number of chunks (N) must be greater than 0.\n");
        return 1;
    }

    // Connect to server (first connection)
    int *client_fds = malloc(N * sizeof(int));
    if (!client_fds) {
        perror("malloc failed");
        return 1;
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);
    if (inet_pton(AF_INET, ip_str, &serv_addr.sin_addr) <= 0) {
        perror("Invalid address / Address not supported");
        free(client_fds);
        return 1;
    }

    int first_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (first_sock < 0) {
        perror("Socket creation error");
        free(client_fds);
        return 1;
    }

    if (connect(first_sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Connection failed");
        close(first_sock);
        free(client_fds);
        return 1;
    }
    client_fds[0] = first_sock;

    // Send first request to negotiate and get file size
    chunk_request_t req;
    req.seq = htonl(1);
    req.num_chunks = htonl(N);
    if (safe_send(first_sock, &req, sizeof(req)) != sizeof(req)) {
        perror("Failed to send first chunk request");
        close(first_sock);
        free(client_fds);
        return 1;
    }

    // Read file size
    uint8_t fs_bytes[8];
    if (safe_recv(first_sock, fs_bytes, 8) != 8) {
        fprintf(stderr, "Failed to receive file size from server\n");
        close(first_sock);
        free(client_fds);
        return 1;
    }
    uint64_t file_size = decode_uint64(fs_bytes);

    // Connect the remaining N - 1 connections
    for (uint32_t i = 1; i < N; i++) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            perror("Socket creation error");
            for (uint32_t j = 0; j < i; j++) close(client_fds[j]);
            free(client_fds);
            return 1;
        }
        if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
            perror("Connection failed");
            close(sock);
            for (uint32_t j = 0; j < i; j++) close(client_fds[j]);
            free(client_fds);
            return 1;
        }
        client_fds[i] = sock;
    }

    // Map shared reassembly buffer
    int32_t *reassembly_buffer = mmap(NULL, file_size, PROT_READ | PROT_WRITE, 
                                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (reassembly_buffer == MAP_FAILED) {
        perror("mmap reassembly buffer failed");
        for (uint32_t i = 0; i < N; i++) close(client_fds[i]);
        free(client_fds);
        return 1;
    }

    // Map shared synchronization controller
    shared_sync_t *sync_ctrl = mmap(NULL, sizeof(shared_sync_t), PROT_READ | PROT_WRITE,
                                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (sync_ctrl == MAP_FAILED) {
        perror("mmap shared sync controller failed");
        munmap(reassembly_buffer, file_size);
        for (uint32_t i = 0; i < N; i++) close(client_fds[i]);
        free(client_fds);
        return 1;
    }

    // Initialize synchronization primitives as process-shared
    pthread_mutexattr_t mattr;
    pthread_mutexattr_init(&mattr);
    pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&sync_ctrl->buffer_lock, &mattr);
    pthread_mutexattr_destroy(&mattr);

    pthread_condattr_t cattr;
    pthread_condattr_init(&cattr);
    pthread_condattr_setpshared(&cattr, PTHREAD_PROCESS_SHARED);
    pthread_cond_init(&sync_ctrl->chunk_arrived, &cattr);
    pthread_condattr_destroy(&cattr);

    sem_init(&sync_ctrl->received_count, 1, 0);
    sync_ctrl->chunks_received = 0;

    // Fork N child processes to fetch chunks
    pid_t *child_pids = malloc(N * sizeof(pid_t));
    if (!child_pids) {
        perror("malloc child pids failed");
        // clean up
        sem_destroy(&sync_ctrl->received_count);
        pthread_cond_destroy(&sync_ctrl->chunk_arrived);
        pthread_mutex_destroy(&sync_ctrl->buffer_lock);
        munmap(sync_ctrl, sizeof(shared_sync_t));
        munmap(reassembly_buffer, file_size);
        for (uint32_t i = 0; i < N; i++) close(client_fds[i]);
        free(client_fds);
        return 1;
    }

    for (uint32_t i = 0; i < N; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("Fork client child failed");
            for (uint32_t j = 0; j < i; j++) kill(child_pids[j], SIGKILL);
            free(child_pids);
            sem_destroy(&sync_ctrl->received_count);
            pthread_cond_destroy(&sync_ctrl->chunk_arrived);
            pthread_mutex_destroy(&sync_ctrl->buffer_lock);
            munmap(sync_ctrl, sizeof(shared_sync_t));
            munmap(reassembly_buffer, file_size);
            for (uint32_t j = 0; j < N; j++) close(client_fds[j]);
            free(client_fds);
            return 1;
        }

        if (pid == 0) {
            // Client Child i (responsible for chunk seq = i + 1)
            int conn_fd = client_fds[i];
            for (uint32_t j = 0; j < N; j++) {
                if (j != i) close(client_fds[j]);
            }
            free(child_pids);
            free(client_fds);
            uint32_t seq = i + 1;

            // Pin client child process to CPU core (Student A requirement for multicore parallelism)
            int num_cores = sysconf(_SC_NPROCESSORS_ONLN);
            if (num_cores > 0) {
                set_cpu_affinity(i % num_cores);
            }

            printf("[Client Child %u] PID: %d executing on CPU Core: %d (Downloading chunk %u/%u)\n", seq, getpid(), sched_getcpu(), seq, N);

            // If not the first client, send the chunk request to server child
            if (i > 0) {
                chunk_request_t child_req;
                child_req.seq = htonl(seq);
                child_req.num_chunks = htonl(N);
                if (safe_send(conn_fd, &child_req, sizeof(child_req)) != sizeof(child_req)) {
                    fprintf(stderr, "[Client Child %u] Send request failed\n", seq);
                    close(conn_fd);
                    exit(1);
                }
            }

            // Receive header
            chunk_header_t hdr;
            if (safe_recv(conn_fd, &hdr, sizeof(hdr)) != sizeof(hdr)) {
                fprintf(stderr, "[Client Child %u] Receive header failed\n", seq);
                close(conn_fd);
                exit(1);
            }

            uint32_t recv_seq = ntohl(hdr.seq);
            uint32_t payload_size = ntohl(hdr.payload_size);

            if (recv_seq != seq) {
                fprintf(stderr, "[Client Child %u] Mismatched sequence: expected %u, got %u\n", seq, seq, recv_seq);
                close(conn_fd);
                exit(1);
            }

            // Read payload directly into shared memory offset
            uint64_t chunk_size = (file_size + N - 1) / N;
            uint64_t offset = (uint64_t)(seq - 1) * chunk_size;

            if (safe_recv(conn_fd, (char *)reassembly_buffer + offset, payload_size) != (ssize_t)payload_size) {
                fprintf(stderr, "[Client Child %u] Receive payload failed\n", seq);
                close(conn_fd);
                exit(1);
            }

            close(conn_fd);

            // Signal and post to process-shared primitives
            pthread_mutex_lock(&sync_ctrl->buffer_lock);
            sync_ctrl->chunks_received++;
            pthread_cond_signal(&sync_ctrl->chunk_arrived);
            pthread_mutex_unlock(&sync_ctrl->buffer_lock);

            sem_post(&sync_ctrl->received_count);
            exit(0);
        } else {
            // Parent closes socket
            child_pids[i] = pid;
            close(client_fds[i]);
        }
    }

    // Client main process: Wait for all client child processes to complete
    int child_failed = 0;
    for (uint32_t i = 0; i < N; i++) {
        int status;
        if (waitpid(child_pids[i], &status, 0) < 0) {
            perror("waitpid failed");
            child_failed = 1;
        } else {
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                fprintf(stderr, "Client child %u failed\n", i + 1);
                child_failed = 1;
            }
        }
    }

    // Coordinate using condition variables and semaphores
    pthread_mutex_lock(&sync_ctrl->buffer_lock);
    while (sync_ctrl->chunks_received < N && !child_failed) {
        pthread_cond_wait(&sync_ctrl->chunk_arrived, &sync_ctrl->buffer_lock);
    }
    pthread_mutex_unlock(&sync_ctrl->buffer_lock);

    // Wait N times on semaphores to demonstrate counting logic
    for (uint32_t i = 0; i < N; i++) {
        sem_wait(&sync_ctrl->received_count);
    }

    if (child_failed) {
        fprintf(stderr, "One or more client child processes failed. Exiting.\n");
        free(child_pids);
        sem_destroy(&sync_ctrl->received_count);
        pthread_cond_destroy(&sync_ctrl->chunk_arrived);
        pthread_mutex_destroy(&sync_ctrl->buffer_lock);
        munmap(sync_ctrl, sizeof(shared_sync_t));
        munmap(reassembly_buffer, file_size);
        free(client_fds);
        return 1;
    }

    // Write reassembled data to reassembled.dat
    int out_fd = open("reassembled.dat", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) {
        perror("Failed to open reassembled.dat for writing");
        free(child_pids);
        free(client_fds);
        return 1;
    }

    size_t written = 0;
    while (written < file_size) {
        ssize_t w = write(out_fd, (char *)reassembly_buffer + written, file_size - written);
        if (w < 0) {
            if (errno == EINTR) continue;
            perror("Failed to write to reassembled.dat");
            close(out_fd);
            free(child_pids);
            free(client_fds);
            return 1;
        }
        written += w;
    }
    close(out_fd);

    // Write structured log
    FILE *log_file = fopen("execution_log.txt", "w");
    if (!log_file) {
        perror("Failed to open execution_log.txt");
    } else {
        fprintf(log_file, "[PART1] CHUNKS=%u | PROCS=%u | SYNC_USED=mutex,sem,condvar\n", N, N);
        fclose(log_file);
    }

    // Free resources
    free(child_pids);
    free(client_fds);
    sem_destroy(&sync_ctrl->received_count);
    pthread_cond_destroy(&sync_ctrl->chunk_arrived);
    pthread_mutex_destroy(&sync_ctrl->buffer_lock);
    munmap(sync_ctrl, sizeof(shared_sync_t));
    munmap(reassembly_buffer, file_size);

    // Launch Part 2 (operations)
    pid_t op_pid = fork();
    if (op_pid < 0) {
        perror("Fork operations failed");
        return 1;
    }

    if (op_pid == 0) {
        // Child: exec ./operations -t T -f reassembled.dat
        char t_str[32];
        snprintf(t_str, sizeof(t_str), "%u", T);
        execl("./operations", "./operations", "-t", t_str, "-f", "reassembled.dat", NULL);
        perror("execl operations failed");
        exit(1);
    } else {
        // Parent: wait for operations to finish
        int op_status;
        if (waitpid(op_pid, &op_status, 0) < 0) {
            perror("waitpid for operations failed");
            return 1;
        }
        if (WIFEXITED(op_status)) {
            return WEXITSTATUS(op_status);
        }
        return 1;
    }
}
