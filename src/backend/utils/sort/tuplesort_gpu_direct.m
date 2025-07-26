/*-------------------------------------------------------------------------
 *
 * tuplesort_gpu_direct.c
 *    GPU acceleration with true zero-copy using unified memory
 *
 * This implementation sorts PostgreSQL's data directly without copying,
 * taking advantage of Apple Silicon's unified memory architecture.
 *
 * IDENTIFICATION
 *    src/backend/utils/sort/tuplesort_gpu_direct.c
 *
 *-------------------------------------------------------------------------
 */

#ifdef __APPLE__
#include <Metal/Metal.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* Metal kernel source for direct sorting of PostgreSQL data */
static const char *metal_direct_kernel_source = 
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"\n"
"kernel void direct_sort_int64_kernel(device int64_t *keys [[buffer(0)]],\n"
"                                     device uint32_t *indices [[buffer(1)]],\n"
"                                     constant uint &stage [[buffer(2)]],\n"
"                                     constant uint &pass_of_stage [[buffer(3)]],\n"
"                                     uint tid [[thread_position_in_grid]])\n"
"{\n"
"    uint pair_distance = 1 << (stage - pass_of_stage);\n"
"    uint block_width = 2 * pair_distance;\n"
"    \n"
"    uint left_id = (tid / pair_distance) * block_width + (tid % pair_distance);\n"
"    uint right_id = left_id + pair_distance;\n"
"    \n"
"    int64_t left_key = keys[indices[left_id]];\n"
"    int64_t right_key = keys[indices[right_id]];\n"
"    \n"
"    // Bitonic sort comparison\n"
"    uint sort_dir = (tid / (1 << stage)) & 1;\n"
"    bool swap = (sort_dir == 0) ? (left_key > right_key) : (left_key < right_key);\n"
"    \n"
"    if (swap) {\n"
"        uint temp = indices[left_id];\n"
"        indices[left_id] = indices[right_id];\n"
"        indices[right_id] = temp;\n"
"    }\n"
"}\n";

/* Opaque handle for GPU context */
struct GPUDirectContext
{
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id<MTLComputePipelineState> pipeline_state;
    id<MTLLibrary> library;
};

/* Initialize Metal GPU context for direct sorting */
void *
gpu_direct_init_context(void)
{
    @autoreleasepool {
        struct GPUDirectContext *ctx = malloc(sizeof(struct GPUDirectContext));
        if (!ctx)
            return NULL;
        
        /* fprintf(stderr, "gpu_direct_init: Initializing zero-copy GPU context\n"); */
        
        ctx->device = MTLCreateSystemDefaultDevice();
        if (!ctx->device) {
            free(ctx);
            return NULL;
        }
        
        ctx->queue = [ctx->device newCommandQueue];
        if (!ctx->queue) {
            free(ctx);
            return NULL;
        }
        
        /* Try to load pre-compiled library first, fall back to runtime compilation */
        NSError *error = nil;
        NSString *libraryPath = @"/Users/joel/pg-gpu/lib/gpu_sort_direct.metallib";
        
        if ([[NSFileManager defaultManager] fileExistsAtPath:libraryPath]) {
            NSURL *url = [NSURL fileURLWithPath:libraryPath];
            ctx->library = [ctx->device newLibraryWithURL:url error:&error];
        }
        
        if (!ctx->library) {
            NSString *source = [NSString stringWithUTF8String:metal_direct_kernel_source];
            ctx->library = [ctx->device newLibraryWithSource:source options:nil error:&error];
        }
        
        if (!ctx->library) {
            fprintf(stderr, "Failed to create Metal library: %s\n", 
                    [[error localizedDescription] UTF8String]);
            free(ctx);
            return NULL;
        }
        
        id<MTLFunction> kernel = [ctx->library newFunctionWithName:@"direct_sort_int64_kernel"];
        if (!kernel) {
            free(ctx);
            return NULL;
        }
        
        ctx->pipeline_state = [ctx->device newComputePipelineStateWithFunction:kernel error:&error];
        if (!ctx->pipeline_state) {
            fprintf(stderr, "Failed to create pipeline: %s\n", 
                    [[error localizedDescription] UTF8String]);
            free(ctx);
            return NULL;
        }
        
        /* fprintf(stderr, "gpu_direct_init: Zero-copy GPU context ready\n"); */
        return ctx;
    }
}

/* Destroy GPU context */
void
gpu_direct_destroy_context(void *context)
{
    if (context)
        free(context);
}

/* 
 * Sort int64 keys directly without copying
 * This function sorts indices based on key values, avoiding any data copying
 */
int
gpu_direct_sort_int64(void *context, int64_t *keys, uint32_t *indices, int count)
{
    @autoreleasepool {
        struct GPUDirectContext *ctx = (struct GPUDirectContext *)context;
        if (!ctx || !keys || !indices || count <= 0)
            return -1;
        
        /* Check if count is power of 2 */
        if ((count & (count - 1)) != 0)
            return -1;
        
        /* fprintf(stderr, "gpu_direct_sort: Sorting %d elements with zero-copy\n", count); */
        
        /* Create Metal buffers that reference existing memory - NO COPY! */
        id<MTLBuffer> keys_buffer = [ctx->device newBufferWithBytesNoCopy:keys
                                                                    length:count * sizeof(int64_t)
                                                                   options:MTLResourceStorageModeShared
                                                               deallocator:nil];
        
        id<MTLBuffer> indices_buffer = [ctx->device newBufferWithBytesNoCopy:indices
                                                                       length:count * sizeof(uint32_t)
                                                                      options:MTLResourceStorageModeShared
                                                                  deallocator:nil];
        
        if (!keys_buffer || !indices_buffer)
            return -1;
        
        /* Perform bitonic sort */
        for (uint k = 2; k <= count; k *= 2) {
            for (uint j = k/2; j > 0; j /= 2) {
                id<MTLCommandBuffer> command_buffer = [ctx->queue commandBuffer];
                id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
                
                [encoder setComputePipelineState:ctx->pipeline_state];
                [encoder setBuffer:keys_buffer offset:0 atIndex:0];
                [encoder setBuffer:indices_buffer offset:0 atIndex:1];
                [encoder setBytes:&k length:sizeof(uint) atIndex:2];
                [encoder setBytes:&j length:sizeof(uint) atIndex:3];
                [encoder setBytes:&count length:sizeof(uint) atIndex:4];
                
                NSUInteger threads_per_group = ctx->pipeline_state.maxTotalThreadsPerThreadgroup;
                if (threads_per_group > count)
                    threads_per_group = count;
                
                MTLSize grid_size = MTLSizeMake(count, 1, 1);
                MTLSize thread_group_size = MTLSizeMake(threads_per_group, 1, 1);
                
                [encoder dispatchThreads:grid_size threadsPerThreadgroup:thread_group_size];
                [encoder endEncoding];
                
                [command_buffer commit];
                [command_buffer waitUntilCompleted];
            }
        }
        
        /* fprintf(stderr, "gpu_direct_sort: Completed - data sorted in-place\n"); */
        return 0;
    }
}

#else /* !__APPLE__ */

/* Stub implementations for non-Apple platforms */
void *gpu_direct_init_context(void) { return NULL; }
void gpu_direct_destroy_context(void *context) { }
int gpu_direct_sort_int64(void *context, int64_t *keys, uint32_t *indices, int count) { return -1; }

#endif /* __APPLE__ */ 