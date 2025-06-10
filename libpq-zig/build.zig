const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    // Main library
    const lib = b.addStaticLibrary(.{
        .name = "pq",
        .root_source_file = b.path("src/libpq.zig"),
        .target = target,
        .optimize = optimize,
    });
    b.installArtifact(lib);

    // Module for other projects to import
    const libpq_module = b.addModule("libpq", .{
        .root_source_file = b.path("src/libpq.zig"),
    });

    // Tests
    const main_tests = b.addTest(.{
        .root_source_file = b.path("src/libpq.zig"),
        .target = target,
        .optimize = optimize,
    });

    const run_main_tests = b.addRunArtifact(main_tests);

    const test_step = b.step("test", "Run library tests");
    test_step.dependOn(&run_main_tests.step);

    // Test executables
    const test_client = b.addExecutable(.{
        .name = "libpq_testclient",
        .root_source_file = b.path("test/libpq_testclient.zig"),
        .target = target,
        .optimize = optimize,
    });
    test_client.root_module.addImport("libpq", libpq_module);
    b.installArtifact(test_client);

    const uri_regress = b.addExecutable(.{
        .name = "libpq_uri_regress",
        .root_source_file = b.path("test/libpq_uri_regress.zig"),
        .target = target,
        .optimize = optimize,
    });
    uri_regress.root_module.addImport("libpq", libpq_module);
    b.installArtifact(uri_regress);

    // Run commands for test executables
    const run_test_client = b.addRunArtifact(test_client);
    const run_uri_regress = b.addRunArtifact(uri_regress);

    const test_client_step = b.step("test-client", "Run libpq test client");
    test_client_step.dependOn(&run_test_client.step);

    const uri_regress_step = b.step("test-uri", "Run URI regression tests");
    uri_regress_step.dependOn(&run_uri_regress.step);
}