/* =========================================================================
 * OWNED BY: Student B (Server chunk division, fork management, file I/O)
 * ROLE: Handles Server initialization, TCP listening, and forks child
 *       processes to transmit binary file chunks to the client.
 * ========================================================================= */

#include "common.h"

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <input_file>\n", argv[0]);
        return 1;
    }

    const char *input_file = argv[1];
    int file_fd = open(input_file, O_RDONLY);
    if (file_fd < 0) {
        perror("Failed to open input file");
        return 1;
    }

    // Get file size
    struct stat st;
    if (fstat(file_fd, &st) < 0) {
        perror("Failed to get file stat");
        close(file_fd);
        return 1;
    }
    uint64_t file_size = st.st_size;

    // Create server socket
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("Failed to create listen socket");
        close(file_fd);
        return 1;
    }

    int opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEADDR failed");
        close(listen_fd);
        close(file_fd);
        return 1;
    }

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(listen_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("Bind failed");
        close(listen_fd);
        close(file_fd);
        return 1;
    }

    if (listen(listen_fd, 32) < 0) {
        perror("Listen failed");
        close(listen_fd);
        close(file_fd);
        return 1;
    }

    // Accept first connection to negotiate N (num_chunks) and send file_size
    int client_fds[1024];
    uint32_t N = 0;

    socklen_t addr_len = sizeof(address);
    int first_client = accept(listen_fd, (struct sockaddr *)&address, &addr_len);
    if (first_client < 0) {
        perror("Failed to accept first connection");
        close(listen_fd);
        close(file_fd);
        return 1;
    }
    client_fds[0] = first_client;

    // Read first chunk request to discover N
    chunk_request_t req;
    if (safe_recv(first_client, &req, sizeof(req)) != sizeof(req)) {
        fprintf(stderr, "Failed to read first chunk request\n");
        close(first_client);
        close(listen_fd);
        close(file_fd);
        return 1;
    }

    N = ntohl(req.num_chunks);
    if (N == 0 || N > 1024) {
        fprintf(stderr, "Invalid number of chunks N: %u\n", N);
        close(first_client);
        close(listen_fd);
        close(file_fd);
        return 1;
    }

    // Send file size (in network byte order) to first client
    uint8_t fs_bytes[8];
    encode_uint64(fs_bytes, file_size);
    if (safe_send(first_client, fs_bytes, sizeof(fs_bytes)) != sizeof(fs_bytes)) {
        perror("Failed to send file size");
        close(first_client);
        close(listen_fd);
        close(file_fd);
        return 1;
    }

    // Accept remaining N-1 connections
    for (uint32_t i = 1; i < N; i++) {
        int client_fd = accept(listen_fd, (struct sockaddr *)&address, &addr_len);
        if (client_fd < 0) {
            perror("Failed to accept subsequent connection");
            for (uint32_t j = 0; j < i; j++) close(client_fds[j]);
            close(listen_fd);
            close(file_fd);
            return 1;
        }
        client_fds[i] = client_fd;
    }

    // Fork N children to handle each connection
    pid_t *child_pids = malloc(N * sizeof(pid_t));
    if (!child_pids) {
        perror("malloc failed");
        for (uint32_t i = 0; i < N; i++) close(client_fds[i]);
        close(listen_fd);
        close(file_fd);
        return 1;
    }

    for (uint32_t i = 0; i < N; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("Fork failed");
            // Kill already started children
            for (uint32_t j = 0; j < i; j++) kill(child_pids[j], SIGKILL);
            free(child_pids);
            for (uint32_t j = 0; j < N; j++) close(client_fds[j]);
            close(listen_fd);
            close(file_fd);
            return 1;
        }

        if (pid == 0) {
            // Child process: handle connection i (seq = i + 1)
            close(listen_fd);
            for (uint32_t j = 0; j < N; j++) {
                if (j != i) close(client_fds[j]);
            }

            int conn_fd = client_fds[i];
            uint32_t seq = i + 1;

            // Pin server child process to CPU core (Student B requirement for multicore parallelism)
            int num_cores = sysconf(_SC_NPROCESSORS_ONLN);
            if (num_cores > 0) {
                set_cpu_affinity(i % num_cores);
            }

            printf("[Server Child %u] PID: %d executing on CPU Core: %d (Serving chunk %u/%u)\n", seq, getpid(), sched_getcpu(), seq, N);

            // Re-open input file to get private file offset pointer (Student B bug fix)
            close(file_fd);
            file_fd = open(input_file, O_RDONLY);
            if (file_fd < 0) {
                perror("[Server Child] Failed to re-open input file");
                close(conn_fd);
                exit(1);
            }

            // If not the first client, read the chunk request
            if (i > 0) {
                chunk_request_t child_req;
                if (safe_recv(conn_fd, &child_req, sizeof(child_req)) != sizeof(child_req)) {
                    fprintf(stderr, "[Server Child %u] Failed to read chunk request\n", seq);
                    close(conn_fd);
                    close(file_fd);
                    exit(1);
                }
                seq = ntohl(child_req.seq);
                if (seq != i + 1) {
                    fprintf(stderr, "[Server Child %u] Mismatched sequence requested: expected %u, got %u\n", i + 1, i + 1, seq);
                    close(conn_fd);
                    close(file_fd);
                    exit(1);
                }
            }

            // Calculate chunk offset and payload size
            uint64_t chunk_size = (file_size + N - 1) / N;
            uint64_t offset = (uint64_t)(seq - 1) * chunk_size;
            uint64_t payload_size = chunk_size;
            if (seq == N) {
                payload_size = file_size - offset;
            }

            // Seek to offset
            if (lseek(file_fd, offset, SEEK_SET) < 0) {
                perror("[Server Child] lseek failed");
                close(conn_fd);
                close(file_fd);
                exit(1);
            }

            // Read chunk data from file
            char *buf = malloc(payload_size);
            if (!buf) {
                perror("[Server Child] malloc failed");
                close(conn_fd);
                close(file_fd);
                exit(1);
            }

            size_t bytes_read = 0;
            while (bytes_read < payload_size) {
                ssize_t r = read(file_fd, buf + bytes_read, payload_size - bytes_read);
                if (r < 0) {
                    if (errno == EINTR) continue;
                    perror("[Server Child] Read file failed");
                    free(buf);
                    close(conn_fd);
                    close(file_fd);
                    exit(1);
                }
                if (r == 0) {
                    fprintf(stderr, "[Server Child %u] Unexpected EOF of input file\n", seq);
                    free(buf);
                    close(conn_fd);
                    close(file_fd);
                    exit(1);
                }
                bytes_read += r;
            }

            // Send header
            chunk_header_t hdr;
            hdr.seq = htonl(seq);
            hdr.payload_size = htonl((uint32_t)payload_size);
            if (safe_send(conn_fd, &hdr, sizeof(hdr)) != sizeof(hdr)) {
                fprintf(stderr, "[Server Child %u] Send header failed\n", seq);
                free(buf);
                close(conn_fd);
                close(file_fd);
                exit(1);
            }

            // Send payload
            if (safe_send(conn_fd, buf, payload_size) != (ssize_t)payload_size) {
                fprintf(stderr, "[Server Child %u] Send payload failed\n", seq);
                free(buf);
                close(conn_fd);
                close(file_fd);
                exit(1);
            }

            free(buf);
            close(conn_fd);
            close(file_fd);
            exit(0);
        } else {
            // Parent process: save child pid and close the socket
            child_pids[i] = pid;
            close(client_fds[i]);
        }
    }

    // Wait for all child processes
    int status;
    int success = 1;
    for (uint32_t i = 0; i < N; i++) {
        if (waitpid(child_pids[i], &status, 0) < 0) {
            perror("waitpid failed");
            success = 0;
        } else {
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                fprintf(stderr, "Child process %u failed with status %d\n", i, WEXITSTATUS(status));
                success = 0;
            }
        }
    }

    free(child_pids);
    close(listen_fd);
    close(file_fd);

    return success ? 0 : 1;
}