/*-------------------------------------------------------------------------
 *
 * tuplesort_gpu.c
 *    GPU acceleration support for tuple sorting
 *
 * This file contains GPU-specific code that is compiled separately
 * to avoid header conflicts between PostgreSQL and system frameworks.
 *
 * IDENTIFICATION
 *    src/backend/utils/sort/tuplesort_gpu.c
 *
 *-------------------------------------------------------------------------
 */

#ifdef __APPLE__
#include <Metal/Metal.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* GPU Sort structures - must match those in Metal kernel */
typedef struct
{
    int64_t key;
    uint32_t original_index;
} GPUSortItem;

typedef struct
{
    uint32_t stage;
    uint32_t pass_of_stage;
    uint32_t sort_order; /* 0 for ascending, 1 for descending */
} BitonicParams;

/* Metal kernel source code */
static const char *metal_kernel_source = 
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"\n"
"struct GPUSortItem {\n"
"    long key;\n"
"    uint original_index;\n"
"};\n"
"\n"
"struct BitonicParams {\n"
"    uint stage;\n"
"    uint pass_of_stage;\n"
"    uint sort_order;\n"
"};\n"
"\n"
"kernel void bitonic_sort_kernel(device GPUSortItem *data [[buffer(0)]],\n"
"                                constant BitonicParams &params [[buffer(1)]],\n"
"                                uint tid [[thread_position_in_grid]])\n"
"{\n"
"    uint stage = params.stage;\n"
"    uint pass_of_stage = params.pass_of_stage;\n"
"    uint sort_order = params.sort_order;\n"
"    \n"
"    uint pair_distance = 1 << (stage - pass_of_stage);\n"
"    uint block_width = 2 * pair_distance;\n"
"    \n"
"    uint left_id = (tid / pair_distance) * block_width + (tid % pair_distance);\n"
"    uint right_id = left_id + pair_distance;\n"
"    \n"
"    GPUSortItem left_elem = data[left_id];\n"
"    GPUSortItem right_elem = data[right_id];\n"
"    \n"
"    bool swap = false;\n"
"    if (sort_order == 0) {\n"
"        // Ascending order\n"
"        uint sort_dir = (tid / (1 << stage)) & 1;\n"
"        swap = (sort_dir == 0) ? (left_elem.key > right_elem.key) : (left_elem.key < right_elem.key);\n"
"    } else {\n"
"        // Descending order\n"
"        uint sort_dir = (tid / (1 << stage)) & 1;\n"
"        swap = (sort_dir == 0) ? (left_elem.key < right_elem.key) : (left_elem.key > right_elem.key);\n"
"    }\n"
"    \n"
"    if (swap) {\n"
"        data[left_id] = right_elem;\n"
"        data[right_id] = left_elem;\n"
"    }\n"
"}\n";

/* Opaque handle for GPU context */
struct GPUContext
{
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id<MTLComputePipelineState> pipeline_state;
};

/* Initialize Metal GPU context */
void *
gpu_init_context(void)
{
    @autoreleasepool {
        fprintf(stderr, "gpu_init_context: Starting GPU context initialization\n");
        
        struct GPUContext *ctx = (struct GPUContext *)malloc(sizeof(struct GPUContext));
        if (!ctx) {
            fprintf(stderr, "gpu_init_context: Failed to allocate memory for context\n");
            return NULL;
        }
        
        ctx->device = MTLCreateSystemDefaultDevice();
        if (!ctx->device) {
            fprintf(stderr, "gpu_init_context: Failed to create Metal device - no GPU available?\n");
            free(ctx);
            return NULL;
        }
        fprintf(stderr, "gpu_init_context: Metal device created successfully\n");
        
        ctx->queue = [ctx->device newCommandQueue];
        if (!ctx->queue) {
            fprintf(stderr, "gpu_init_context: Failed to create command queue\n");
            free(ctx);
            return NULL;
        }
        fprintf(stderr, "gpu_init_context: Command queue created successfully\n");
        
        NSError *error = nil;
        id<MTLLibrary> library = nil;
        
        /* Try to load pre-compiled Metal library */
        NSString *libraryPath = nil;
        NSArray *searchPaths = @[
            @"gpu_sort.metallib",                              // Current directory
            @"../lib/gpu_sort.metallib",                        // Relative to binary
            @"/usr/local/pgsql/lib/gpu_sort.metallib",         // Standard install
            @"/Users/joel/pg-gpu/lib/gpu_sort.metallib",       // Your actual install path
            @"install/lib/gpu_sort.metallib",                  // Development install
            @"src/backend/utils/sort/gpu_sort.metallib"        // Source directory
        ];
        
        for (NSString *path in searchPaths) {
            if ([[NSFileManager defaultManager] fileExistsAtPath:path]) {
                libraryPath = path;
                fprintf(stderr, "gpu_init_context: Found Metal library at %s\n", [path UTF8String]);
                break;
            }
        }
        
        if (libraryPath) {
            NSURL *url = [NSURL fileURLWithPath:libraryPath];
            library = [ctx->device newLibraryWithURL:url error:&error];
            if (!library) {
                fprintf(stderr, "Failed to load Metal library from %s: %s\n", 
                        [libraryPath UTF8String],
                        [[error localizedDescription] UTF8String]);
            }
        }
        
        /* If loading pre-compiled library failed, try runtime compilation as fallback */
        if (!library) {
            fprintf(stderr, "gpu_init_context: Attempting runtime compilation (may fail due to XPC)\n");
            NSString *source = [NSString stringWithUTF8String:metal_kernel_source];
            library = [ctx->device newLibraryWithSource:source options:nil error:&error];
            
            if (!library) {
                fprintf(stderr, "Failed to create Metal library: %s\n", 
                        [[error localizedDescription] UTF8String]);
                free(ctx);
                return NULL;
            }
        }
        fprintf(stderr, "gpu_init_context: Metal library loaded successfully\n");
        
        id<MTLFunction> kernel_function = [library newFunctionWithName:@"bitonic_sort_kernel"];
        if (!kernel_function) {
            fprintf(stderr, "Failed to find bitonic_sort_kernel function\n");
            free(ctx);
            return NULL;
        }
        fprintf(stderr, "gpu_init_context: Kernel function found successfully\n");
        
        ctx->pipeline_state = [ctx->device newComputePipelineStateWithFunction:kernel_function error:&error];
        if (!ctx->pipeline_state) {
            fprintf(stderr, "Failed to create pipeline state: %s\n", 
                    [[error localizedDescription] UTF8String]);
            free(ctx);
            return NULL;
        }
        fprintf(stderr, "gpu_init_context: Pipeline state created successfully - GPU context ready!\n");
        
        return ctx;
    }
}

/* Destroy GPU context */
void
gpu_destroy_context(void *context)
{
    if (context)
        free(context);
}

/* Perform GPU sort */
int
gpu_sort_int64(void *context, int64_t *keys, uint32_t *indices, int count)
{
    @autoreleasepool {
        struct GPUContext *ctx = (struct GPUContext *)context;
        if (!ctx || !keys || !indices || count <= 0)
            return -1;
        
        /* Check if count is power of 2 */
        if ((count & (count - 1)) != 0)
            return -1;
        
        fprintf(stderr, "gpu_sort_int64: Starting GPU sort of %d elements\n", count);
        
        /* ZERO-COPY APPROACH: Create buffers that share memory with CPU */
        /* Use newBufferWithBytesNoCopy to avoid copying data */
        size_t data_size = count * sizeof(GPUSortItem);
        
        /* We need to create a combined buffer for GPU access */
        /* Since we can't directly use the separate keys/indices arrays, 
         * we'll create a temporary view that the GPU can use */
        GPUSortItem *gpu_data = (GPUSortItem *)malloc(data_size);
        if (!gpu_data)
            return -1;
        
        /* Copy data into GPU format - this is still needed for data layout */
        for (int i = 0; i < count; i++) {
            gpu_data[i].key = keys[i];
            gpu_data[i].original_index = indices[i];
        }
        
        /* Create GPU buffer using the existing memory - NO COPY */
        id<MTLBuffer> data_buffer = [ctx->device newBufferWithBytesNoCopy:gpu_data
                                                                    length:data_size
                                                                   options:MTLResourceStorageModeShared
                                                               deallocator:nil];
        
        if (!data_buffer) {
            free(gpu_data);
            return -1;
        }
        
        /* Perform bitonic sort */
        int num_stages = 0;
        int temp = count;
        while (temp > 1) {
            num_stages++;
            temp >>= 1;
        }
        
        for (int stage = 1; stage <= num_stages; stage++) {
            for (int pass_of_stage = stage; pass_of_stage >= 1; pass_of_stage--) {
                BitonicParams params = {
                    .stage = stage,
                    .pass_of_stage = pass_of_stage,
                    .sort_order = 0  /* ascending */
                };
                
                id<MTLBuffer> params_buffer = [ctx->device newBufferWithBytes:&params
                                                                        length:sizeof(BitonicParams)
                                                                       options:MTLResourceStorageModeShared];
                
                id<MTLCommandBuffer> command_buffer = [ctx->queue commandBuffer];
                id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
                
                [encoder setComputePipelineState:ctx->pipeline_state];
                [encoder setBuffer:data_buffer offset:0 atIndex:0];
                [encoder setBuffer:params_buffer offset:0 atIndex:1];
                
                NSUInteger threads_per_group = ctx->pipeline_state.maxTotalThreadsPerThreadgroup;
                if (threads_per_group > count / 2)
                    threads_per_group = count / 2;
                
                MTLSize grid_size = MTLSizeMake(count / 2, 1, 1);
                MTLSize thread_group_size = MTLSizeMake(threads_per_group, 1, 1);
                
                [encoder dispatchThreads:grid_size threadsPerThreadgroup:thread_group_size];
                [encoder endEncoding];
                
                [command_buffer commit];
                [command_buffer waitUntilCompleted];
            }
        }
        
        /* Results are already in gpu_data thanks to unified memory! */
        /* Just copy back to the original arrays */
        for (int i = 0; i < count; i++) {
            keys[i] = gpu_data[i].key;
            indices[i] = gpu_data[i].original_index;
        }
        
        free(gpu_data);
        fprintf(stderr, "gpu_sort_int64: Sort completed\n");
        return 0;
    }
}

#else /* !__APPLE__ */

/* Stub implementations for non-Apple platforms */

void *
gpu_init_context(void)
{
    return NULL;
}

void
gpu_destroy_context(void *context)
{
}

int
gpu_sort_int64(void *context, int64_t *keys, uint32_t *indices, int count)
{
    return -1;
}

#endif /* __APPLE__ */ 