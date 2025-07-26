#include <metal_stdlib>
using namespace metal;

struct GPUSortItem {
    long key;
    uint original_index;
};

struct BitonicParams {
    uint stage;
    uint pass_of_stage;
    uint sort_order;
};

kernel void bitonic_sort_kernel(device GPUSortItem *data [[buffer(0)]],
                                constant BitonicParams &params [[buffer(1)]],
                                uint tid [[thread_position_in_grid]])
{
    uint stage = params.stage;
    uint pass_of_stage = params.pass_of_stage;
    uint sort_order = params.sort_order;
    
    uint pair_distance = 1 << (stage - pass_of_stage);
    uint block_width = 2 * pair_distance;
    
    uint left_id = (tid / pair_distance) * block_width + (tid % pair_distance);
    uint right_id = left_id + pair_distance;
    
    GPUSortItem left_elem = data[left_id];
    GPUSortItem right_elem = data[right_id];
    
    bool swap = false;
    if (sort_order == 0) {
        // Ascending order
        uint sort_dir = (tid / (1 << stage)) & 1;
        swap = (sort_dir == 0) ? (left_elem.key > right_elem.key) : (left_elem.key < right_elem.key);
    } else {
        // Descending order
        uint sort_dir = (tid / (1 << stage)) & 1;
        swap = (sort_dir == 0) ? (left_elem.key < right_elem.key) : (left_elem.key > right_elem.key);
    }
    
    if (swap) {
        data[left_id] = right_elem;
        data[right_id] = left_elem;
    }
} 