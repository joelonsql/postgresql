const std = @import("std");
const libpq = @import("libpq");

pub fn main() !void {
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    defer _ = gpa.deinit();
    const allocator = gpa.allocator();

    // Test version
    const version = libpq.libVersion();
    std.debug.print("libpq-zig version: {d}\n", .{version});

    // Test connection defaults
    const defaults = libpq.conndefaults();
    if (defaults) |defs| {
        defer libpq.conninfoFree(defs);
        
        var i: usize = 0;
        while (defs[i].keyword) |keyword| : (i += 1) {
            std.debug.print("  {s}: ", .{keyword});
            if (defs[i].val) |val| {
                std.debug.print("{s}", .{val});
            } else {
                std.debug.print("(null)", .{});
            }
            std.debug.print("\n", .{});
        }
    }

    // Test basic connection
    const conninfo = "host=localhost port=5432 dbname=postgres";
    const conn = libpq.connectdb(conninfo);
    if (conn) |c| {
        defer libpq.finish(c);

        const status = libpq.status(c);
        if (status == .CONNECTION_OK) {
            std.debug.print("Connection successful!\n", .{});
            
            // Get some connection info
            std.debug.print("Database: {s}\n", .{libpq.db(c)});
            std.debug.print("User: {s}\n", .{libpq.user(c)});
            std.debug.print("Host: {s}\n", .{libpq.host(c)});
            std.debug.print("Port: {s}\n", .{libpq.port(c)});
            std.debug.print("Protocol version: {d}\n", .{libpq.protocolVersion(c)});
            std.debug.print("Server version: {d}\n", .{libpq.serverVersion(c)});

            // Execute a simple query
            const result = libpq.exec(c, "SELECT version()");
            if (result) |res| {
                defer libpq.clear(res);

                const res_status = libpq.resultStatus(res);
                if (res_status == .PGRES_TUPLES_OK) {
                    const ntuples = libpq.ntuples(res);
                    const nfields = libpq.nfields(res);
                    
                    std.debug.print("Query returned {d} rows with {d} fields\n", .{ ntuples, nfields });
                    
                    var row: usize = 0;
                    while (row < ntuples) : (row += 1) {
                        var col: usize = 0;
                        while (col < nfields) : (col += 1) {
                            const value = libpq.getvalue(res, @intCast(row), @intCast(col));
                            std.debug.print("{s}\t", .{value});
                        }
                        std.debug.print("\n", .{});
                    }
                } else {
                    std.debug.print("Query failed: {s}\n", .{libpq.resultErrorMessage(res)});
                }
            }
        } else {
            std.debug.print("Connection failed: {s}\n", .{libpq.errorMessage(c)});
        }
    } else {
        std.debug.print("Failed to allocate connection\n", .{});
    }
}