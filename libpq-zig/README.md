# libpq-zig

A pure Zig implementation of PostgreSQL's libpq client library.

## Overview

libpq-zig is a complete reimplementation of PostgreSQL's libpq client library in Zig. It maintains exact compatibility with the original libpq API while leveraging Zig's safety features and standard library.

## Features

- Full PostgreSQL wire protocol v3 implementation
- Supports all authentication methods (password, MD5, SCRAM-SHA-256)
- TLS/SSL support using Zig's standard library
- Connection pooling and pipeline mode
- Large object support
- COPY protocol support
- Asynchronous/non-blocking operations
- Cross-platform support (Linux, macOS)

## Requirements

- Zig 0.11.0 or later
- No external dependencies (pure Zig implementation)

## Building

```bash
zig build
```

## Testing

```bash
zig build test
zig build test-client
zig build test-uri
```

## Usage

```zig
const std = @import("std");
const libpq = @import("libpq");

pub fn main() !void {
    // Connect to database
    const conn = libpq.connectdb("host=localhost dbname=mydb");
    defer libpq.finish(conn);
    
    if (libpq.status(conn) != .CONNECTION_OK) {
        std.debug.print("Connection failed: {s}\n", .{libpq.errorMessage(conn)});
        return;
    }
    
    // Execute query
    const result = libpq.exec(conn, "SELECT * FROM users");
    defer libpq.clear(result);
    
    if (libpq.resultStatus(result) == .PGRES_TUPLES_OK) {
        const rows = libpq.ntuples(result);
        const cols = libpq.nfields(result);
        
        for (0..rows) |row| {
            for (0..cols) |col| {
                const value = libpq.getvalue(result, @intCast(row), @intCast(col));
                std.debug.print("{s}\t", .{value});
            }
            std.debug.print("\n", .{});
        }
    }
}
```

## API Compatibility

This library provides the exact same API as libpq, with all functions, types, and constants preserved. The only difference is that it's implemented in Zig rather than C.

## License

Same as PostgreSQL - see COPYRIGHT file.