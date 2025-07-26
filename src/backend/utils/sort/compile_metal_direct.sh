#!/bin/bash
# Compile Metal shader for zero-copy direct implementation

echo "Compiling Metal shader for zero-copy GPU sort..."
xcrun -sdk macosx metal -c gpu_sort_direct.metal -o gpu_sort_direct.air
xcrun -sdk macosx metallib gpu_sort_direct.air -o gpu_sort_direct.metallib

if [ -f gpu_sort_direct.metallib ]; then
    echo "Successfully created gpu_sort_direct.metallib"
    # Copy to install directory if it exists
    if [ -d "../../../../install/lib" ]; then
        cp gpu_sort_direct.metallib ../../../../install/lib/
        echo "Copied to install/lib/"
    fi
    if [ -d "/Users/joel/pg-gpu/lib" ]; then
        cp gpu_sort_direct.metallib /Users/joel/pg-gpu/lib/
        echo "Copied to /Users/joel/pg-gpu/lib/"
    fi
else
    echo "Failed to create Metal library"
    exit 1
fi 