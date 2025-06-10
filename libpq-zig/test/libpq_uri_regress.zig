const std = @import("std");
const libpq = @import("libpq");

const TestCase = struct {
    uri: []const u8,
    expected_error: ?[]const u8,
    expected_params: ?[]const Param,
    
    const Param = struct {
        keyword: []const u8,
        value: []const u8,
    };
};

// Test cases from the original libpq_uri_regress.c
const test_cases = [_]TestCase{
    // Valid URIs
    .{
        .uri = "postgresql://",
        .expected_error = null,
        .expected_params = &[_]TestCase.Param{},
    },
    .{
        .uri = "postgresql://localhost",
        .expected_error = null,
        .expected_params = &[_]TestCase.Param{
            .{ .keyword = "host", .value = "localhost" },
        },
    },
    .{
        .uri = "postgresql://localhost:5433",
        .expected_error = null,
        .expected_params = &[_]TestCase.Param{
            .{ .keyword = "host", .value = "localhost" },
            .{ .keyword = "port", .value = "5433" },
        },
    },
    .{
        .uri = "postgresql://localhost/mydb",
        .expected_error = null,
        .expected_params = &[_]TestCase.Param{
            .{ .keyword = "host", .value = "localhost" },
            .{ .keyword = "dbname", .value = "mydb" },
        },
    },
    .{
        .uri = "postgresql://user@localhost",
        .expected_error = null,
        .expected_params = &[_]TestCase.Param{
            .{ .keyword = "host", .value = "localhost" },
            .{ .keyword = "user", .value = "user" },
        },
    },
    .{
        .uri = "postgresql://user:secret@localhost",
        .expected_error = null,
        .expected_params = &[_]TestCase.Param{
            .{ .keyword = "host", .value = "localhost" },
            .{ .keyword = "user", .value = "user" },
            .{ .keyword = "password", .value = "secret" },
        },
    },
    .{
        .uri = "postgresql://host1:123,host2:456/somedb",
        .expected_error = null,
        .expected_params = &[_]TestCase.Param{
            .{ .keyword = "host", .value = "host1,host2" },
            .{ .keyword = "port", .value = "123,456" },
            .{ .keyword = "dbname", .value = "somedb" },
        },
    },
    .{
        .uri = "postgresql:///mydb",
        .expected_error = null,
        .expected_params = &[_]TestCase.Param{
            .{ .keyword = "dbname", .value = "mydb" },
        },
    },
    .{
        .uri = "postgresql://[::1]:5433/mydb",
        .expected_error = null,
        .expected_params = &[_]TestCase.Param{
            .{ .keyword = "host", .value = "::1" },
            .{ .keyword = "port", .value = "5433" },
            .{ .keyword = "dbname", .value = "mydb" },
        },
    },
    .{
        .uri = "postgresql://[::1]/mydb",
        .expected_error = null,
        .expected_params = &[_]TestCase.Param{
            .{ .keyword = "host", .value = "::1" },
            .{ .keyword = "dbname", .value = "mydb" },
        },
    },
    .{
        .uri = "postgresql://localhost?connect_timeout=10",
        .expected_error = null,
        .expected_params = &[_]TestCase.Param{
            .{ .keyword = "host", .value = "localhost" },
            .{ .keyword = "connect_timeout", .value = "10" },
        },
    },
    
    // Invalid URIs
    .{
        .uri = "postgresql://host:123/",
        .expected_error = "invalid port number",
        .expected_params = null,
    },
    .{
        .uri = "postgresql://host:123a/",
        .expected_error = "invalid port number",
        .expected_params = null,
    },
    .{
        .uri = "postgresql://host:999999/",
        .expected_error = "invalid port number",
        .expected_params = null,
    },
    .{
        .uri = "postgresql://[::1",
        .expected_error = "end of string reached when looking for matching",
        .expected_params = null,
    },
    .{
        .uri = "postgresql://[::1]xyz",
        .expected_error = "unexpected character",
        .expected_params = null,
    },
    .{
        .uri = "postgresql://host1,/",
        .expected_error = "missing host name",
        .expected_params = null,
    },
    .{
        .uri = "postgresql://host1:1,host2:2/",
        .expected_error = "could not match",
        .expected_params = null,
    },
    .{
        .uri = "postgres://",
        .expected_error = "invalid URI scheme",
        .expected_params = null,
    },
};

pub fn main() !void {
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    defer _ = gpa.deinit();
    const allocator = gpa.allocator();
    
    var failed: u32 = 0;
    var total: u32 = 0;
    
    for (test_cases) |tc| {
        total += 1;
        
        var error_msg: ?[*:0]u8 = null;
        const options = libpq.conninfoParse(tc.uri.ptr, &error_msg);
        
        if (tc.expected_error) |expected_err| {
            // We expect an error
            if (options == null and error_msg != null) {
                const actual_err = std.mem.span(error_msg.?);
                if (std.mem.indexOf(u8, actual_err, expected_err) != null) {
                    std.debug.print("PASS: URI '{s}' correctly failed with expected error\n", .{tc.uri});
                } else {
                    std.debug.print("FAIL: URI '{s}' failed with unexpected error: {s}\n", .{ tc.uri, actual_err });
                    failed += 1;
                }
                libpq.freemem(error_msg.?);
            } else {
                std.debug.print("FAIL: URI '{s}' should have failed but didn't\n", .{tc.uri});
                if (options) |opts| {
                    libpq.conninfoFree(opts);
                }
                failed += 1;
            }
        } else {
            // We expect success
            if (options) |opts| {
                defer libpq.conninfoFree(opts);
                
                var passed = true;
                if (tc.expected_params) |expected_params| {
                    for (expected_params) |ep| {
                        var found = false;
                        var i: usize = 0;
                        while (opts[i].keyword) |keyword| : (i += 1) {
                            if (std.mem.eql(u8, std.mem.span(keyword), ep.keyword)) {
                                if (opts[i].val) |val| {
                                    if (std.mem.eql(u8, std.mem.span(val), ep.value)) {
                                        found = true;
                                        break;
                                    }
                                }
                            }
                        }
                        if (!found) {
                            std.debug.print("FAIL: URI '{s}' missing expected param {s}={s}\n", .{ tc.uri, ep.keyword, ep.value });
                            passed = false;
                            failed += 1;
                            break;
                        }
                    }
                }
                
                if (passed) {
                    std.debug.print("PASS: URI '{s}' parsed correctly\n", .{tc.uri});
                }
            } else {
                std.debug.print("FAIL: URI '{s}' should have succeeded but failed", .{tc.uri});
                if (error_msg) |msg| {
                    std.debug.print(": {s}", .{msg});
                    libpq.freemem(msg);
                }
                std.debug.print("\n", .{});
                failed += 1;
            }
        }
    }
    
    std.debug.print("\nTotal tests: {d}, Failed: {d}\n", .{ total, failed });
    
    if (failed > 0) {
        return error.TestsFailed;
    }
}