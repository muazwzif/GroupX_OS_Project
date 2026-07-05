/* =========================================================================
 * PART 2 OWNERSHIP MATRIX:
 * - Student C: Min/Max Parallel Reduction, reduction_lock/reduction_cond coordination.
 * - Student D: Parallel Sort implementation (qsort + merge), task scheduling, logging.
 * ========================================================================= */

#include "common.h"

// Global parameters and primitives
static uint32_t global_T;
static uint64_t global_E;
static int32_t *global_arr;
static int32_t *global_temp_arr;

static int32_t global_min = INT32_MAX;
static int32_t global_max = INT32_MIN;
static uint32_t completed_reductions = 0;

static pthread_mutex_t reduction_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t reduction_cond = PTHREAD_COND_INITIALIZER;

static pthread_mutex_t merge_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t merge_cond = PTHREAD_COND_INITIALIZER;

static sem_t active_sort_sem;

typedef struct {
    uint64_t start;
    uint64_t end;
    uint64_t ready_step;
} segment_t;

static segment_t *segments;

typedef struct {
    uint32_t thread_id;
    uint64_t start;
    uint64_t end;
} thread_arg_t;

int compare_ints(const void *a, const void *b) {
    int32_t arg1 = *(const int32_t*)a;
    int32_t arg2 = *(const int32_t*)b;
    if (arg1 < arg2) return -1;
    if (arg1 > arg2) return 1;
    return 0;
}

void merge(int32_t *arr, int32_t *temp, uint64_t start, uint64_t mid, uint64_t end) {
    uint64_t i = start;
    uint64_t j = mid;
    uint64_t k = start;

    memcpy(&temp[start], &arr[start], (end - start + 1) * sizeof(int32_t));

    while (i < mid && j <= end) {
        if (temp[i] <= temp[j]) {
            arr[k++] = temp[i++];
        } else {
            arr[k++] = temp[j++];
        }
    }
    while (i < mid) {
        arr[k++] = temp[i++];
    }
    while (j <= end) {
        arr[k++] = temp[j++];
    }
}

double get_time_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

void* worker_thread(void* arg) {
    thread_arg_t *t_arg = (thread_arg_t*)arg;
    uint32_t tid = t_arg->thread_id;
    uint64_t start = t_arg->start;
    uint64_t end = t_arg->end;

    // Pin worker thread to CPU core (Student C/D requirement for multicore parallelism)
    int num_cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (num_cores > 0) {
        set_cpu_affinity(tid % num_cores);
    }

    printf("[Worker Thread %u] TID: %lu running on CPU Core: %d (Processing segment [%lu to %lu])\n",
           tid, (unsigned long)pthread_self(), sched_getcpu(), start, end);

    /* -------------------------------------------------------------
     * OWNED BY: Student C (Nafiz)
     * DATA PARALLELISM: Find Minimum and Find Maximum
     * Each thread scans its local segment of the array.
     * ------------------------------------------------------------- */
    int32_t local_min = INT32_MAX;
    int32_t local_max = INT32_MIN;

    for (uint64_t i = start; i <= end; i++) {
        if (global_arr[i] < local_min) local_min = global_arr[i];
        if (global_arr[i] > local_max) local_max = global_arr[i];
    }

    // Lock and reduce to global min/max
    pthread_mutex_lock(&reduction_lock);
    if (local_min < global_min) global_min = local_min;
    if (local_max > global_max) global_max = local_max;
    completed_reductions++;
    if (completed_reductions == global_T) {
        pthread_cond_signal(&reduction_cond);
    }
    pthread_mutex_unlock(&reduction_lock);

    /* -------------------------------------------------------------
     * OWNED BY: Student D (Muaz)
     * DATA PARALLELISM: Sort individual blocks
     * Thread acquires the semaphore to limit concurrent sorting tasks.
     * ------------------------------------------------------------- */
    sem_wait(&active_sort_sem);
    printf("[Worker Thread %u] Starting block sort of size %lu on CPU Core: %d...\n", tid, end - start + 1, sched_getcpu());
    qsort(&global_arr[start], end - start + 1, sizeof(int32_t), compare_ints);
    sem_post(&active_sort_sem);

    // Mark segment as sorted (ready for merge level 1)
    pthread_mutex_lock(&merge_lock);
    segments[tid].ready_step = 1;
    pthread_cond_broadcast(&merge_cond);
    pthread_mutex_unlock(&merge_lock);

    /* -------------------------------------------------------------
     * OWNED BY: Student D
     * TASK PARALLELISM: Bottom-up merge sort phase
     * Concurrently merge adjacent blocks step-by-step.
     * Synchronization coordinated via condition variables.
     * ------------------------------------------------------------- */
    uint64_t step = 1;
    while (step < global_T) {
        if (tid % (2 * step) == 0) {
            uint64_t sibling = tid + step;
            if (sibling < global_T) {
                // Wait for the sibling segment to be ready at the current step level
                pthread_mutex_lock(&merge_lock);
                while (segments[sibling].ready_step < step) {
                    pthread_cond_wait(&merge_cond, &merge_lock);
                }
                pthread_mutex_unlock(&merge_lock);

                // Merge left segment with right segment
                printf("[Worker Thread %u] Merging with Sibling %lu on CPU Core: %d (Range: [%lu to %lu] and [%lu to %lu])\n",
                       tid, sibling, sched_getcpu(), segments[tid].start, segments[sibling].start - 1, segments[sibling].start, segments[sibling].end);
                merge(global_arr, global_temp_arr, segments[tid].start, segments[sibling].start, segments[sibling].end);

                // Update current segment's end boundary and update ready_step
                pthread_mutex_lock(&merge_lock);
                segments[tid].end = segments[sibling].end;
                segments[tid].ready_step = step * 2;
                pthread_cond_broadcast(&merge_cond);
                pthread_mutex_unlock(&merge_lock);
            } else {
                // Sibling is out of bounds (odd number of active segments), promote directly
                pthread_mutex_lock(&merge_lock);
                segments[tid].ready_step = step * 2;
                pthread_cond_broadcast(&merge_cond);
                pthread_mutex_unlock(&merge_lock);
            }
        } else {
            // Not a merger at this level; work is finished for this thread
            break;
        }
        step *= 2;
    }

    return NULL;
}

int main(int argc, char *argv[]) {
    uint32_t T = 8;
    const char *file_path = "reassembled.dat";

    int opt;
    while ((opt = getopt(argc, argv, "t:f:")) != -1) {
        switch (opt) {
            case 't':
                T = atoi(optarg);
                break;
            case 'f':
                file_path = optarg;
                break;
            default:
                fprintf(stderr, "Usage: %s -t <num_threads> -f <file_path>\n", argv[0]);
                return 1;
        }
    }

    if (T == 0) {
        fprintf(stderr, "Number of threads (T) must be greater than 0.\n");
        return 1;
    }

    // Open file
    int fd = open(file_path, O_RDONLY);
    if (fd < 0) {
        perror("Failed to open input file for operations");
        return 1;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        perror("Failed to get file stat");
        close(fd);
        return 1;
    }
    uint64_t file_size = st.st_size;
    uint64_t num_elements = file_size / sizeof(int32_t);

    if (num_elements == 0) {
        fprintf(stderr, "Input file is empty.\n");
        close(fd);
        return 1;
    }

    // Allocate memory for integers
    int32_t *arr = malloc(file_size);
    int32_t *temp_arr = malloc(file_size);
    if (!arr || !temp_arr) {
        perror("Memory allocation failed for arrays");
        close(fd);
        if (arr) free(arr);
        if (temp_arr) free(temp_arr);
        return 1;
    }

    // Read file in one block
    size_t bytes_read = 0;
    while (bytes_read < file_size) {
        ssize_t r = read(fd, (char *)arr + bytes_read, file_size - bytes_read);
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("Read file failed");
            free(arr);
            free(temp_arr);
            close(fd);
            return 1;
        }
        if (r == 0) break;
        bytes_read += r;
    }
    close(fd);

    // Safeguard: Ensure T does not exceed elements
    if (T > num_elements) {
        T = num_elements;
    }

    global_T = T;
    global_E = num_elements;
    global_arr = arr;
    global_temp_arr = temp_arr;

    // Initialize sorting semaphore to limit active sorting phases (Student D: Task scheduling limit)
    sem_init(&active_sort_sem, 0, (T > 1) ? (T / 2) : 1);

    // Allocate segment boundaries
    segments = malloc(T * sizeof(segment_t));
    thread_arg_t *t_args = malloc(T * sizeof(thread_arg_t));
    pthread_t *threads = malloc(T * sizeof(pthread_t));
    if (!segments || !t_args || !threads) {
        perror("malloc failed");
        free(arr);
        free(temp_arr);
        if (segments) free(segments);
        if (t_args) free(t_args);
        if (threads) free(threads);
        return 1;
    }

    // Start timer
    double start_time = get_time_ms();

    // Divide elements cleanly and launch threads
    uint64_t base_size = num_elements / T;
    uint64_t remainder = num_elements % T;
    uint64_t current_start = 0;

    for (uint32_t i = 0; i < T; i++) {
        uint64_t segment_size = base_size + (i < remainder ? 1 : 0);
        segments[i].start = current_start;
        segments[i].end = current_start + segment_size - 1;
        segments[i].ready_step = 0;

        t_args[i].thread_id = i;
        t_args[i].start = segments[i].start;
        t_args[i].end = segments[i].end;

        current_start += segment_size;

        if (pthread_create(&threads[i], NULL, worker_thread, &t_args[i]) != 0) {
            perror("Failed to create thread");
            return 1;
        }
    }

    // Wait for the min/max data reduction to complete (demonstrating CV synchronization)
    pthread_mutex_lock(&reduction_lock);
    while (completed_reductions < T) {
        pthread_cond_wait(&reduction_cond, &reduction_lock);
    }
    pthread_mutex_unlock(&reduction_lock);

    // Write min/max text results immediately to satisfy Part 2 parallel reduction output requirement
    FILE *min_f = fopen("result_min.txt", "w");
    if (min_f) {
        fprintf(min_f, "MIN=%d\n", global_min);
        fclose(min_f);
    }
    FILE *max_f = fopen("result_max.txt", "w");
    if (max_f) {
        fprintf(max_f, "MAX=%d\n", global_max);
        fclose(max_f);
    }

    // Join all threads to ensure the merge sort completes
    for (uint32_t i = 0; i < T; i++) {
        pthread_join(threads[i], NULL);
    }

    double end_time = get_time_ms();
    long elapsed_ms = (long)(end_time - start_time);

    // Save sorted array to result_sorted.dat
    int out_fd = open("result_sorted.dat", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd >= 0) {
        size_t written = 0;
        while (written < file_size) {
            ssize_t w = write(out_fd, (char *)arr + written, file_size - written);
            if (w < 0) {
                if (errno == EINTR) continue;
                perror("Failed to write result_sorted.dat");
                break;
            }
            written += w;
        }
        close(out_fd);
    } else {
        perror("Failed to open result_sorted.dat");
    }

    // Append Part 2 log and completion status to execution_log.txt
    FILE *log_file = fopen("execution_log.txt", "a");
    if (log_file) {
        fprintf(log_file, "[PART2] THREADS=%u | DATA_PARALLEL=min,max | TASK_PARALLEL=sort\n", T);
        fprintf(log_file, "[PART2] TIME_MS=%ld | SORT_ALGO=parallel_merge_sort\n", elapsed_ms);
        fprintf(log_file, "[STATUS] SUCCESS\n");
        fclose(log_file);
    } else {
        perror("Failed to append to execution_log.txt");
    }

    // Clean up resources
    sem_destroy(&active_sort_sem);
    free(threads);
    free(t_args);
    free(segments);
    free(arr);
    free(temp_arr);

    return 0;
}
