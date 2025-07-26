#!/bin/bash
# Compile Metal shader to library

echo "Compiling Metal shader..."
xcrun -sdk macosx metal -c gpu_sort.metal -o gpu_sort.air
xcrun -sdk macosx metallib gpu_sort.air -o gpu_sort.metallib

if [ -f gpu_sort.metallib ]; then
    echo "Successfully created gpu_sort.metallib"
    # Copy to install directory if it exists
    if [ -d "../../../../install/lib" ]; then
        cp gpu_sort.metallib ../../../../install/lib/
        echo "Copied to install/lib/"
    fi
else
    echo "Failed to create Metal library"
    exit 1
fi 