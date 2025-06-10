#!/bin/bash

# A script to convert the C source code of libpq to Zig.
#
# This script should be run from the root of the PostgreSQL source repository.
# It performs the following actions:
# 1. Creates a 'libpq-zig' directory to store the converted files.
# 2. Finds all C source and header files (.c, .h) within src/interfaces/libpq.
# 3. For each file, it runs 'zig translate-c' to convert it to Zig.
# 4. The output .zig files are placed in 'libpq-zig', mirroring the
#    original directory structure.

# Exit immediately if a command exits with a non-zero status, and treat
# pipe failures as errors.
set -eo pipefail

# --- Configuration ---
SOURCE_DIR="src/interfaces/libpq"
DEST_DIR="libpq-zig"
# ANSI color codes for better output
COLOR_BLUE='\033[0;34m'
COLOR_GREEN='\033[0;32m'
COLOR_RED='\033[0;31m'
COLOR_YELLOW='\033[0;33m'
COLOR_RESET='\033[0m'

# --- Pre-flight Checks ---
echo -e "${COLOR_BLUE}Running pre-flight checks...${COLOR_RESET}"

# Check if zig is installed
if ! command -v zig &> /dev/null; then
    echo -e "${COLOR_RED}Error: 'zig' command not found. Please install Zig.${COLOR_RESET}"
    echo "See: https://ziglang.org/learn/getting-started/"
    exit 1
fi

# Ensure the script is run from the postgres git repo root
if [ ! -d "$SOURCE_DIR" ]; then
    echo -e "${COLOR_RED}Error: This script must be run from the root of the PostgreSQL source repository.${COLOR_RESET}"
    echo "Directory '$SOURCE_DIR' not found."
    exit 1
fi

# Find the generated pg_config.h, which is required by many headers.
echo -e "${COLOR_BLUE}Locating generated header pg_config.h...${COLOR_RESET}"
PG_CONFIG_H_PATH=$(find . -name 'pg_config.h' -print -quit)

if [ -z "$PG_CONFIG_H_PATH" ]; then
    echo -e "${COLOR_RED}Error: Could not find 'pg_config.h'.${COLOR_RESET}"
    echo "Please run './configure' and 'make' in the repository root before running this script."
    exit 1
fi

PG_CONFIG_H_DIR=$(dirname "$PG_CONFIG_H_PATH")
echo -e "${COLOR_GREEN}Found pg_config.h in: ${PG_CONFIG_H_DIR}${COLOR_RESET}"

echo -e "${COLOR_GREEN}Checks passed.${COLOR_RESET}"
echo

# --- Main Logic ---
echo -e "${COLOR_BLUE}Creating destination directory: ${DEST_DIR}${COLOR_RESET}"
mkdir -p "$DEST_DIR"

# Base include paths for postgres source, required for libclang to find headers.
# These are derived from the project's Makefiles.
INCLUDE_PATHS=(
    -I"src/include"
    -I"src/interfaces/libpq"
    -I"src/port"
    -I"$PG_CONFIG_H_DIR"  # Add path to generated headers
)

# Required defines for compiling libpq frontend code.
# The build system would normally define these based on configure/meson results.
# We hard-code them here assuming a typical macOS/Linux build with SSL and GSSAPI.
DEFINES=(
    -DFRONTEND
    -DUSE_SSL             # For OpenSSL support
    -DENABLE_GSS          # For GSSAPI support
)

# On macOS, add Homebrew's OpenSSL and krb5 include paths if available.
if [[ "$(uname)" == "Darwin" ]] && command -v brew &> /dev/null; then
    # OpenSSL
    if brew --prefix openssl &> /dev/null; then
        OPENSSL_PREFIX=$(brew --prefix openssl)
        if [ -d "$OPENSSL_PREFIX/include" ]; then
            echo -e "${COLOR_BLUE}Adding Homebrew OpenSSL include path: $OPENSSL_PREFIX/include${COLOR_RESET}"
            INCLUDE_PATHS+=(-I"$OPENSSL_PREFIX/include")
        fi
    else
        echo -e "${COLOR_YELLOW}Warning: Homebrew is installed, but openssl is not. SSL headers might not be found.${COLOR_RESET}"
        DEFINES=(${DEFINES[@]/-DUSE_SSL/})
    fi
    # krb5 (for GSSAPI)
    if brew --prefix krb5 &> /dev/null; then
        KRB5_PREFIX=$(brew --prefix krb5)
        if [ -d "$KRB5_PREFIX/include" ]; then
            echo -e "${COLOR_BLUE}Adding Homebrew krb5 (GSSAPI) include path: $KRB5_PREFIX/include${COLOR_RESET}"
            INCLUDE_PATHS+=(-I"$KRB5_PREFIX/include")
        fi
    else
        echo -e "${COLOR_YELLOW}Warning: Homebrew krb5 is not installed. GSSAPI headers might not be found.${COLOR_RESET}"
        DEFINES=(${DEFINES[@]/-DENABLE_GSS/})
    fi
fi

echo
echo -e "${COLOR_BLUE}Finding and converting C source files from $SOURCE_DIR...${COLOR_RESET}"

# Create temporary files to capture stderr and for the C wrapper
error_log=$(mktemp)
wrapper_file=$(mktemp wrapper.XXXXXX.c)
# Ensure the temp files are cleaned up on exit
trap 'rm -f "$error_log" "$wrapper_file"' EXIT

# Find all C/H files, but explicitly exclude Windows-specific implementation files.
find "$SOURCE_DIR" \( -name "*.c" -o -name "*.h" \) \
    -not -name "win32.c" \
    -not -name "win32.h" \
    -not -name "pthread-win32.c" \
    -print0 | while IFS= read -r -d '' c_file; do
    
    # Get the path relative to the source directory to preserve the structure.
    relative_path="${c_file#$SOURCE_DIR/}"
    
    # Determine the output filename (.c -> .zig, .h -> .zig).
    if [[ "$relative_path" == *.c ]]; then
        zig_relative_path="${relative_path%.c}.zig"
    elif [[ "$relative_path" == *.h ]]; then
        zig_relative_path="${relative_path%.h}.zig"
    else
        # This case should not be reached due to the find command pattern.
        echo -e "${COLOR_YELLOW}Warning: Skipping non-C/H file: $c_file${COLOR_RESET}"
        continue
    fi
    
    zig_file_path="$DEST_DIR/$zig_relative_path"
    
    # Create the corresponding subdirectory in the destination if it doesn't exist.
    zig_dir=$(dirname "$zig_file_path")
    mkdir -p "$zig_dir"

    echo -e "  Translating ${COLOR_YELLOW}$c_file${COLOR_RESET} -> ${COLOR_GREEN}$zig_file_path${COLOR_RESET}"
    
    # Create a temporary wrapper C file that first includes the main postgres
    # header, and then the actual file to be translated. This ensures all
    # necessary types and macros (like 'bool') are defined.
    printf '#include "src/include/postgres_fe.h"\n#include "%s"\n' "$c_file" > "$wrapper_file"
    
    # Execute the translation on the wrapper file.
    # Redirect stderr to a temp file to provide better error messages.
    if ! zig translate-c "${INCLUDE_PATHS[@]}" "${DEFINES[@]}" "$wrapper_file" > "$zig_file_path" 2>"$error_log"; then
        echo -e "${COLOR_RED}Error: Failed to translate '$c_file'.${COLOR_RESET}" >&2
        echo -e "${COLOR_YELLOW}Zig's stderr output:${COLOR_RESET}" >&2
        # Indent Zig's error for readability
        sed 's/^/    /' "$error_log" >&2
        exit 1
    fi
    
done

echo
echo -e "${COLOR_GREEN}Conversion complete.${COLOR_RESET}"
echo "Zig files have been generated in the '${DEST_DIR}' directory."
