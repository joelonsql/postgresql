/*
 * Compile with:
 * clang -o test_metal test_metal.c src/backend/utils/sort/tuplesort_gpu_direct.m \
 *   -framework Metal -framework Foundation -I. -std=c11 -ObjC
 */

#include <stdio.h>
#include <stdlib.h>

// Forward declarations for the zero-copy GPU functions
void *gpu_direct_init_context(void);
void gpu_direct_destroy_context(void *context);
int gpu_direct_sort_int64(void *context, int64_t *keys, uint32_t *indices, int count);

int main() {
    printf("Testing Zero-Copy Metal GPU access...\n");
    
    void *ctx = gpu_direct_init_context();
    if (!ctx) {
        printf("Failed to initialize GPU context\n");
        return 1;
    }
    
    printf("Zero-copy GPU context initialized successfully!\n");
    
    // Test with a small array
    int count = 16; // Must be power of 2
    int64_t keys[] = {15, 8, 3, 12, 1, 9, 6, 14, 2, 11, 5, 13, 4, 10, 7, 0};
    uint32_t indices[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    
    printf("Before sort: ");
    for (int i = 0; i < count; i++) {
        printf("%lld ", keys[i]);
    }
    printf("\n");
    
    printf("Indices before: ");
    for (int i = 0; i < count; i++) {
        printf("%u ", indices[i]);
    }
    printf("\n");
    
    int result = gpu_direct_sort_int64(ctx, keys, indices, count);
    if (result == 0) {
        printf("Zero-copy GPU sort succeeded!\n");
        printf("Keys (unchanged): ");
        for (int i = 0; i < count; i++) {
            printf("%lld ", keys[i]);
        }
        printf("\n");
        printf("Sorted indices: ");
        for (int i = 0; i < count; i++) {
            printf("%u ", indices[i]);
        }
        printf("\n");
        printf("Sorted values: ");
        for (int i = 0; i < count; i++) {
            printf("%lld ", keys[indices[i]]);
        }
        printf("\n");
    } else {
        printf("Zero-copy GPU sort failed with error %d\n", result);
    }
    
    gpu_direct_destroy_context(ctx);
    return 0;
} 