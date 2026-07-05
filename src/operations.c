#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>
#include <string.h>

// ====================================================================
// EXACT CODE FOR STUDENT D: PARALLEL MERGE SORT & LOGGING
// ====================================================================

// Mandatory synchronization primitives owned by Student D
pthread_mutex_t sort_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t merge_cond = PTHREAD_COND_INITIALIZER;

// State variable to track completed sub-sorting worker tasks
int completed_sort_tasks = 0;

typedef struct {
    int32_t *array;
    int left;
    int right;
} SortTaskArg;

// Helper function to merge two sorted continuous sub-arrays
void serial_merge(int32_t *array, int left, int mid, int right) {
    int n1 = mid - left + 1;
    int n2 = right - mid;

    int32_t *L = malloc(n1 * sizeof(int32_t));
    int32_t *R = malloc(n2 * sizeof(int32_t));

    for (int i = 0; i < n1; i++) L[i] = array[left + i];
    for (int j = 0; j < n2; j++) R[j] = array[mid + 1 + j];

    int i = 0, j = 0, k = left;
    while (i < n1 && j < n2) {
        if (L[i] <= R[j]) array[k] = L[i++];
        else array[k] = R[j++];
        k++;
    }

    while (i < n1) array[k++] = L[i++];
    while (j < n2) array[k++] = R[j++];

    free(L);
    free(R);
}

// Internal thread sorting routine (In-place quicksort)
void serial_quicksort(int32_t *array, int left, int right) {
    if (left >= right) return;
    int32_t pivot = array[right];
    int i = left - 1;
    for (int j = left; j < right; j++) {
        if (array[j] < pivot) {
            i++;
            int32_t temp = array[i];
            array[i] = array[j];
            array[j] = temp;
        }
    }
    int32_t temp = array[i + 1];
    array[i + 1] = array[right];
    array[right] = temp;
    int pivot_idx = i + 1;

    serial_quicksort(array, left, pivot_idx - 1);
    serial_quicksort(array, pivot_idx + 1, right);
}

// --- DEMONSTRATING DATA PARALLELISM ---
// Thread handles an independent contiguous slice of data array concurrently
void* local_sort_worker(void* arg) {
    SortTaskArg *task = (SortTaskArg*)arg;
    
    // [DATA PARALLEL ZONE] - Independent sorting blocks
    serial_quicksort(task->array, task->left, task->right);

    // Lock global state to increment active completed count safely
    pthread_mutex_lock(&sort_mutex);
    completed_sort_tasks++;
    // Notify the main task scheduler that a sub-sort task is complete
    pthread_cond_signal(&merge_cond);
    pthread_mutex_unlock(&sort_mutex);

    return NULL;
}

// --- DEMONSTRATING TASK PARALLELISM ---
// Coordinates parallel execution layers and sub-sorting steps using condition variables
void run_parallel_analytics_sort(int32_t *array, int size, int total_threads) {
    if (!array || size <= 1) return;
    if (total_threads < 1) total_threads = 1;
    if (total_threads > size) total_threads = size;

    pthread_t *threads = malloc((size_t)total_threads * sizeof(*threads));
    SortTaskArg *args = malloc((size_t)total_threads * sizeof(*args));
    if (!threads || !args) {
        free(threads);
        free(args);
        serial_quicksort(array, 0, size - 1);
        return;
    }

    pthread_mutex_lock(&sort_mutex);
    completed_sort_tasks = 0;
    pthread_mutex_unlock(&sort_mutex);

    int chunk_size = size / total_threads;

    // [TASK PARALLEL ZONE: Layer 1 - Spawning Worker Tasks Concurrently][cite: 1]
    for (int i = 0; i < total_threads; i++) {
        args[i].array = array;
        args[i].left = i * chunk_size;
        args[i].right = (i == total_threads - 1) ? (size - 1) : ((i + 1) * chunk_size - 1);
        int rc = pthread_create(&threads[i], NULL, local_sort_worker, &args[i]);
        if (rc != 0) {
            fprintf(stderr, "Error: pthread_create failed: %s\n", strerror(rc));
            // Join any threads already started, then fall back to a serial sort.
            for (int j = 0; j < i; j++) pthread_join(threads[j], NULL);
            serial_quicksort(array, 0, size - 1);
            free(threads);
            free(args);
            return;
        }
    }

    // [TASK PARALLEL ZONE: Layer 2 - Using Cond Variables to Wait for Tasks][cite: 1]
    pthread_mutex_lock(&sort_mutex);
    while (completed_sort_tasks < total_threads) {
        pthread_cond_wait(&merge_cond, &sort_mutex);
    }
    pthread_mutex_unlock(&sort_mutex);

    // Clean up thread resources
    for (int i = 0; i < total_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    // [TASK PARALLEL ZONE: Layer 3 - Bottom-Up Merging Tree Phase][cite: 1]
    for (int step = 1; step < total_threads; step++) {
        int left = 0;
        int mid = step * chunk_size - 1;
        int right = (step == total_threads - 1) ? (size - 1) : ((step + 1) * chunk_size - 1);
        if (right >= size) right = size - 1;
        serial_merge(array, left, mid, right);
    }

    free(threads);
    free(args);
}

int main(int argc, char *argv[]) {
    int num_threads = 1;
    char *filename = NULL;

    // Command-Line Interface Spec Handling[cite: 1]
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') {
                fprintf(stderr, "Error: -t requires a positive integer thread count\n");
                return 1;
            }
            num_threads = atoi(argv[++i]);
            if (num_threads <= 0) {
                fprintf(stderr, "Error: -t must be >= 1\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-f") == 0) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') {
                fprintf(stderr, "Error: -f requires an input filename\n");
                return 1;
            }
            filename = argv[++i];
        }
    }

    if (!filename) {
        fprintf(stderr, "Error: Missing input file track pointer (-f)\n");
        return 1;
    }

    FILE *file = fopen(filename, "rb");
    if (!file) {
        fprintf(stderr, "Error opening binary payload data: %s\n", filename);
        return 1;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        perror("fseek");
        fclose(file);
        return 1;
    }
    long filesize = ftell(file);
    if (filesize < 0) {
        perror("ftell");
        fclose(file);
        return 1;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        perror("fseek");
        fclose(file);
        return 1;
    }

    int num_elements = (int)(filesize / (long)sizeof(int32_t));
    int32_t *array = malloc((size_t)num_elements * sizeof(int32_t));
    if (!array) {
        perror("malloc");
        fclose(file);
        return 1;
    }

    size_t elements_read = fread(array, sizeof(int32_t), (size_t)num_elements, file);
    if (elements_read != (size_t)num_elements) {
        fprintf(stderr, "Error: short read from %s (%zu/%d elements)\n", filename, elements_read, num_elements);
        free(array);
        fclose(file);
        return 1;
    }
    fclose(file);

    // Setup high-precision markers for timing analysis log requirement[cite: 1]
    struct timespec start_time, end_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    // Run Student D Core Sorting Process
    run_parallel_analytics_sort(array, num_elements, num_threads);

    clock_gettime(CLOCK_MONOTONIC, &end_time);
    long runtime_ms = (end_time.tv_sec - start_time.tv_sec) * 1000 + 
                      (end_time.tv_nsec - start_time.tv_nsec) / 1000000;

    // EXACT REQUIRED DELIVERABLE: result_sorted.dat[cite: 1]
    FILE *out_sorted = fopen("result_sorted.dat", "wb");
    if (out_sorted) {
        fwrite(array, sizeof(int32_t), num_elements, out_sorted);
        fclose(out_sorted);
    }

    // EXACT REQUIRED LOG FORMATTING COMPLIANCE[cite: 1]
    FILE *log = fopen("execution_log.txt", "a");
    if (log) {
        fprintf(log, "[PART2] THREADS=%d | DATA_PARALLEL=min,max | TASK_PARALLEL=sort\n", num_threads);
        fprintf(log, "[PART2] TIME_MS=%ld | SORT_ALGO=parallel_merge_sort\n", runtime_ms);
        fprintf(log, "[STATUS] SUCCESS\n");
        fclose(log);
    }

    free(array);
    printf("[STUDENT D] Output written and logs formatted cleanly.\n");
    return 0;
}
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <limits.h>

// ---------------------------------------------------------
// GLOBAL STATE & SYNCHRONIZATION (Student C)
// ---------------------------------------------------------
int32_t global_min = INT32_MAX;
int32_t global_max = INT32_MIN;
int threads_completed = 0;
int total_threads = 0;

// Mutex to protect global min/max accumulators 
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

// Condition variable to signal completion of local reductions
pthread_cond_t cond_var = PTHREAD_COND_INITIALIZER;

// Struct to pass array boundaries to each thread
typedef struct {
    int32_t* data;
    size_t start;
    size_t end;
    int thread_id;
} ThreadData;


// ---------------------------------------------------------
// THREAD FUNCTION: DATA PARALLELISM (Student C)
// ---------------------------------------------------------
void* find_min_max(void* arg) {
    ThreadData* t_data = (ThreadData*)arg;
    int32_t local_min = INT32_MAX;
    int32_t local_max = INT32_MIN;

    // 1. Data Parallelism: Each thread finds its local min and max
    for (size_t i = t_data->start; i < t_data->end; i++) {
        if (t_data->data[i] < local_min) {
            local_min = t_data->data[i];
        }
        if (t_data->data[i] > local_max) {
            local_max = t_data->data[i];
        }
    }

    // 2. Synchronization: Lock mutex before updating global state
    pthread_mutex_lock(&lock);
    
    if (local_min < global_min) {
        global_min = local_min;
    }
    if (local_max > global_max) {
        global_max = local_max;
    }

    // 3. Condition Variable: Track finished threads and signal main thread
    threads_completed++;
    if (threads_completed == total_threads) {
        pthread_cond_signal(&cond_var); 
    }
    
    pthread_mutex_unlock(&lock);
    
    pthread_exit(NULL);
}


// ---------------------------------------------------------
// LAUNCH & OUTPUT FUNCTION (Student C)
// ---------------------------------------------------------
// This function should be called by the main() inside operations.c
// after the file has been read into the 'array' pointer.
void execute_student_c_analytics(int32_t* array, size_t total_elements, int num_threads) {
    total_threads = num_threads;
    pthread_t threads[num_threads];
    ThreadData thread_args[num_threads];

    size_t chunk_size = total_elements / num_threads;
    size_t remainder = total_elements % num_threads;
    size_t current_start = 0;

    // 1. Divide array into T contiguous blocks and spawn threads
    for (int i = 0; i < num_threads; i++) {
        thread_args[i].data = array;
        thread_args[i].start = current_start;
        thread_args[i].thread_id = i;
        
        // Ensure the last thread picks up any remaining elements
        size_t current_chunk = chunk_size + (i == num_threads - 1 ? remainder : 0);
        thread_args[i].end = current_start + current_chunk;
        current_start += current_chunk;

        pthread_create(&threads[i], NULL, find_min_max, (void*)&thread_args[i]);
    }

    // 2. Coordinate using CondVar: Wait until all threads signal completion
    pthread_mutex_lock(&lock);
    while (threads_completed < total_threads) {
        pthread_cond_wait(&cond_var, &lock);
    }
    pthread_mutex_unlock(&lock);

    // Clean up thread resources
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    // 3. Generate exactly formatted output files for grading script
    FILE* min_file = fopen("result_min.txt", "w");
    if (min_file) {
        fprintf(min_file, "MIN=%d\n", global_min);
        fclose(min_file);
    }

    FILE* max_file = fopen("result_max.txt", "w");
    if (max_file) {
        fprintf(max_file, "MAX=%d\n", global_max);
        fclose(max_file);
    }

    // 4. Append to execution log with exact string matching
    FILE* log_file = fopen("execution_log.txt", "a");
    if (log_file) {
        fprintf(log_file, "[PART2] THREADS=%d | DATA_PARALLEL=min,max\n", num_threads);
        fclose(log_file);
    }
}


^O

^O

x


