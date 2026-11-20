const std = @import("std");

pub fn build(b: *std.Build) void {
    // The baseline CPU unless told otherwise: the library picks its AVX2 or AVX-512 code at run time.
    var query = b.standardTargetOptionsQueryOnly(.{ .default_target = .{ .cpu_model = .baseline } });
    const optimize = b.standardOptimizeOption(.{});
    const linkage = b.option(std.lang.LinkMode, "linkage", "Build a static or a shared library (default: static)") orelse .static;
    const portable = b.option(bool, "portable", "Only build the plain C code, without SIMD (default: false)") orelse false;

    // WebAssembly can't check for SIMD at run time, so turn it on unless a CPU was given.
    if (query.cpu_arch != null and query.cpu_arch.?.isWasm() and query.cpu_model == .determined_by_arch_os) {
        query.cpu_features_add.addFeature(@intFromEnum(std.Target.wasm.Feature.simd128));
    }
    const target = b.resolveTargetQuery(query);
    const wasm = target.result.cpu.arch.isWasm();

    // Zig doesn't ship a C library for WebAssembly, and the library only needs a few compiler builtins.
    const libc = !wasm and target.result.os.tag != .freestanding;
    const flags: []const []const u8 = if (libc) &.{"-std=c11"} else &.{ "-std=c11", "-ffreestanding" };
    // Zig keeps frame pointers in C code by default, which costs about 3% on short messages.
    const module_options: std.Build.Module.CreateOptions = .{
        .target = target,
        .optimize = optimize,
        .link_libc = libc,
        .omit_frame_pointer = optimize != .debug,
    };

    const lib = b.addLibrary(.{
        .name = "flightdeck",
        .linkage = linkage,
        .version = std.SemanticVersion.parse(@import("build.zig.zon").version) catch unreachable,
        .root_module = b.createModule(module_options),
    });
    lib.root_module.addCSourceFile(.{ .file = b.path("src/flightdeck.c"), .flags = flags });
    if (portable) {
        lib.root_module.addCMacro("FLIGHTDECK_PORTABLE", "1");
    }
    lib.installHeader(b.path("src/flightdeck.h"), "flightdeck.h");
    b.installArtifact(lib);

    // Like `make test`: on x86-64, the code without AVX-512 and the plain C code get their own builds of the library.
    const variants = [_]struct { name: []const u8, macro: ?[]const u8 }{
        .{ .name = "test", .macro = null },
        .{ .name = "test-avx2", .macro = "FLIGHTDECK_NO_AVX512" },
        .{ .name = "test-portable", .macro = "FLIGHTDECK_PORTABLE" },
    };
    const variant_count: usize = if (!libc and !wasm) 0 else if (target.result.cpu.arch == .x86_64 and !portable) 3 else 1;
    const test_step = b.step("test", "Run the tests");
    for (variants[0..variant_count]) |variant| {
        const tests = b.addExecutable(.{ .name = variant.name, .root_module = b.createModule(module_options) });
        const module = tests.root_module;
        module.addIncludePath(b.path("src"));
        module.addCSourceFile(.{ .file = b.path("tests/test.c"), .flags = flags });
        if (variant.macro) |macro| {
            module.addCSourceFile(.{ .file = b.path("src/flightdeck.c"), .flags = flags });
            module.addCMacro(macro, "1");
        } else {
            module.linkLibrary(lib);
            if (portable) {
                module.addCMacro("FLIGHTDECK_PORTABLE", "1");
            }
        }
        if (wasm) {
            // The C library comes from wasm-minilibc, and tests/wasm/wasi.c adds what it leaves to the application.
            module.addIncludePath(b.path("tests/wasm"));
            module.addIncludePath(b.path("tests/wasm/minilibc/include"));
            module.addCSourceFiles(.{ .files = &.{ "tests/wasm/wasi.c", "tests/wasm/minilibc/src/libc.c" }, .flags = flags });
        }
        test_step.dependOn(&b.addRunArtifact(tests).step);
    }

    // Always optimized, so that the numbers mean something.
    if (libc) {
        const bench = b.addExecutable(.{
            .name = "bench",
            .root_module = b.createModule(.{ .target = target, .optimize = .fast, .link_libc = true, .omit_frame_pointer = true }),
        });
        bench.root_module.addIncludePath(b.path("src"));
        bench.root_module.addCSourceFiles(.{ .files = &.{ "bench/bench.c", "src/flightdeck.c" }, .flags = flags });
        if (portable) {
            bench.root_module.addCMacro("FLIGHTDECK_PORTABLE", "1");
        }
        b.step("bench", "Measure the speed").dependOn(&b.addRunArtifact(bench).step);
    }
}
