#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

int main(int argc, char *argv[]) {
    const char *out_name = "original.dat";
    if (argc >= 2) {
        out_name = argv[1];
    }

    FILE *f = fopen(out_name, "wb");
    if (!f) {
        perror("Failed to open output file");
        return 1;
    }

    // 1GB is 1,073,741,824 bytes.
    // Each int32_t is 4 bytes.
    // Number of elements = 268,435,456.
    size_t num_elements = 268435456;
    size_t buffer_size = 1048576; // 1M integers (4MB buffer)
    int32_t *buf = malloc(buffer_size * sizeof(int32_t));
    if (!buf) {
        perror("Failed to allocate buffer");
        fclose(f);
        return 1;
    }

    srand(time(NULL));
    size_t written = 0;
    while (written < num_elements) {
        size_t to_write = num_elements - written;
        if (to_write > buffer_size) {
            to_write = buffer_size;
        }

        for (size_t i = 0; i < to_write; i++) {
            // Generate some random integers (positive and negative)
            buf[i] = (rand() % 2000000000) - 1000000000;
        }

        size_t w = fwrite(buf, sizeof(int32_t), to_write, f);
        if (w != to_write) {
            perror("fwrite failed");
            free(buf);
            fclose(f);
            return 1;
        }
        written += w;
    }

    free(buf);
    fclose(f);
    printf("Successfully generated 1GB test file: %s\n", out_name);
    return 0;
}
