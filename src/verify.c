#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>

#define BUF_SIZE 1048576

int main() {
    // 1. Read original.dat to compute min, max, sum, and element count
    FILE *f_orig = fopen("original.dat", "rb");
    if (!f_orig) {
        perror("Failed to open original.dat");
        return 1;
    }

    int32_t true_min = INT_MAX;
    int32_t true_max = INT_MIN;
    int64_t true_sum = 0;
    uint64_t orig_count = 0;

    int32_t *buf = malloc(BUF_SIZE * sizeof(int32_t));
    if (!buf) {
        perror("malloc failed");
        fclose(f_orig);
        return 1;
    }

    size_t n;
    while ((n = fread(buf, sizeof(int32_t), BUF_SIZE, f_orig)) > 0) {
        for (size_t i = 0; i < n; i++) {
            if (buf[i] < true_min) true_min = buf[i];
            if (buf[i] > true_max) true_max = buf[i];
            true_sum += buf[i];
            orig_count++;
        }
    }
    fclose(f_orig);

    // 2. Read and verify result_min.txt
    FILE *f_min = fopen("result_min.txt", "r");
    if (!f_min) {
        perror("Failed to open result_min.txt");
        free(buf);
        return 1;
    }
    int file_min;
    if (fscanf(f_min, "MIN=%d", &file_min) != 1) {
        fprintf(stderr, "Failed to parse result_min.txt\n");
        fclose(f_min);
        free(buf);
        return 1;
    }
    fclose(f_min);

    if (file_min != true_min) {
        fprintf(stderr, "Min mismatch: true min is %d, file min is %d\n", true_min, file_min);
        free(buf);
        return 1;
    }
    printf("[PASS] Minimum value matches: %d\n", true_min);

    // 3. Read and verify result_max.txt
    FILE *f_max = fopen("result_max.txt", "r");
    if (!f_max) {
        perror("Failed to open result_max.txt");
        free(buf);
        return 1;
    }
    int file_max;
    if (fscanf(f_max, "MAX=%d", &file_max) != 1) {
        fprintf(stderr, "Failed to parse result_max.txt\n");
        fclose(f_max);
        free(buf);
        return 1;
    }
    fclose(f_max);

    if (file_max != true_max) {
        fprintf(stderr, "Max mismatch: true max is %d, file max is %d\n", true_max, file_max);
        free(buf);
        return 1;
    }
    printf("[PASS] Maximum value matches: %d\n", true_max);

    // 4. Read and verify result_sorted.dat
    FILE *f_sort = fopen("result_sorted.dat", "rb");
    if (!f_sort) {
        perror("Failed to open result_sorted.dat");
        free(buf);
        return 1;
    }

    int32_t prev = INT_MIN;
    int64_t sort_sum = 0;
    uint64_t sort_count = 0;
    int first = 1;

    while ((n = fread(buf, sizeof(int32_t), BUF_SIZE, f_sort)) > 0) {
        for (size_t i = 0; i < n; i++) {
            if (!first && buf[i] < prev) {
                fprintf(stderr, "Sortedness violated at index %lu: %d < %d\n", sort_count, buf[i], prev);
                fclose(f_sort);
                free(buf);
                return 1;
            }
            first = 0;
            prev = buf[i];
            sort_sum += buf[i];
            sort_count++;
        }
    }
    fclose(f_sort);
    free(buf);

    if (sort_count != orig_count) {
        fprintf(stderr, "Sorted count mismatch: original has %lu elements, sorted has %lu\n", orig_count, sort_count);
        return 1;
    }

    if (sort_sum != true_sum) {
        fprintf(stderr, "Checksum mismatch: original sum is %ld, sorted sum is %ld\n", true_sum, sort_sum);
        return 1;
    }

    printf("[PASS] Sorted file verified: %lu elements, correct order and checksum.\n", sort_count);
    return 0;
}
