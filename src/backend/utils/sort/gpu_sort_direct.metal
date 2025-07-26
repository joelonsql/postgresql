#include <metal_stdlib>
using namespace metal;

kernel void direct_sort_int64_kernel(device int64_t *keys [[buffer(0)]],
                                     device uint32_t *indices [[buffer(1)]],
                                     constant uint &k [[buffer(2)]],
                                     constant uint &j [[buffer(3)]],
                                     constant uint &n [[buffer(4)]],
                                     uint i [[thread_position_in_grid]])
{
    uint l = i ^ j;  // XOR operation
    
    if (l > i) {
        // Get the actual indices we're comparing
        uint idx_i = indices[i];
        uint idx_l = indices[l];
        
        // Get the keys for those indices
        int64_t key_i = keys[idx_i];
        int64_t key_l = keys[idx_l];
        
        // Determine if we need to swap based on the bitonic sort criteria
        bool should_swap = false;
        
        if (k == n) {
            // Final stage - always sort ascending
            should_swap = (key_i > key_l);
        } else if ((i & k) == 0) {
            // Ascending order for this part
            should_swap = (key_i > key_l);
        } else {
            // Descending order for this part
            should_swap = (key_i < key_l);
        }
        
        if (should_swap) {
            indices[i] = idx_l;
            indices[l] = idx_i;
        }
    }
} 