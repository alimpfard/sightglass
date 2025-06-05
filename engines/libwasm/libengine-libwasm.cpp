/*
 * Copyright (c) 2024, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MemoryStream.h>
#include <LibCore/File.h>
#include <LibWasm/AbstractMachine/AbstractMachine.h>
#include <LibWasm/AbstractMachine/BytecodeInterpreter.h>
#include <LibWasm/AbstractMachine/Configuration.h>
#include <LibWasm/AbstractMachine/Validator.h>
#include <LibWasm/Printer/Printer.h>
#include <LibWasm/Wasi.h>
#include <fcntl.h>

struct WasmBenchConfig {
    u8 const* working_dir_ptr;
    size_t working_dir_len;

    u8 const* stdout_path_ptr;
    size_t stdout_path_len;

    u8 const* stderr_path_ptr;
    size_t stderr_path_len;

    u8 const* stdin_path_ptr;
    size_t stdin_path_len;

    u8* compilation_timer;
    void (*compilation_start)(u8*);
    void (*compilation_end)(u8*);

    u8* instantiation_timer;
    void (*instantiation_start)(u8*);
    void (*instantiation_end)(u8*);

    u8* execution_timer;
    void (*execution_start)(u8*);
    void (*execution_end)(u8*);

    u8 const* execution_flags_ptr;
    size_t execution_flags_len;
};

struct Engine {
    WasmBenchConfig config;
    Wasm::Wasi::Implementation wasi;
    int stdin_fd { -1 };
    int stdout_fd { -1 };
    int stderr_fd { -1 };
    Wasm::AbstractMachine machine {};
    RefPtr<Wasm::Module> module {};
    OwnPtr<Wasm::ModuleInstance> instance {};
};

extern "C" i32 wasm_bench_create(WasmBenchConfig config, void** engine_out)
{
    ByteString wd = StringView { config.working_dir_ptr, config.working_dir_len };
    ByteString stdout_path = StringView { config.stdout_path_ptr, config.stdout_path_len };
    ByteString stderr_path = StringView { config.stderr_path_ptr, config.stderr_path_len };
    ByteString stdin_path = StringView { config.stdin_path_ptr, config.stdin_path_len };

    auto stdin_fd = openat(AT_FDCWD, stdin_path.characters(), O_RDONLY);
    auto stdout_fd = openat(AT_FDCWD, stdout_path.characters(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    auto stderr_fd = openat(AT_FDCWD, stderr_path.characters(), O_WRONLY | O_CREAT | O_TRUNC, 0666);

    auto engine = new Engine {
        .config = config,
        .wasi = Wasm::Wasi::Implementation({
            .provide_arguments = {},
            .provide_environment = {},
            .provide_preopened_directories = [wd] {
                return Vector<Wasm::Wasi::Implementation::MappedPath> {
                    {
                        .host_path = LexicalPath{wd},
                        .mapped_path = LexicalPath{"."},
                    }
                };
            },
            .stdin_fd = stdin_fd,
            .stdout_fd = stdout_fd,
            .stderr_fd = stderr_fd,
        }),
        .stdin_fd = stdin_fd,
        .stdout_fd = stdout_fd,
        .stderr_fd = stderr_fd,
    };
    *engine_out = engine;
    return 0;
}

extern "C" void wasm_bench_free(void* engine)
{
    auto& e = *static_cast<Engine*>(engine);
    close(e.stdin_fd);
    close(e.stdout_fd);
    close(e.stderr_fd);
    delete static_cast<Engine*>(engine);
}

extern "C" i32 wasm_bench_compile(void* engine_ptr, u8 const* wasm, size_t wasm_len)
{
    auto& engine = *static_cast<Engine*>(engine_ptr);

    engine.config.compilation_start(engine.config.compilation_timer);
    ScopeGuard guard = [&] {
        engine.config.compilation_end(engine.config.compilation_timer);
    };

    ReadonlyBytes bytes { wasm, wasm_len };
    FixedMemoryStream stream { bytes };
    auto module_result = Wasm::Module::parse(stream);
    if (module_result.is_error()) {
        dbgln("Failed to parse module: {}", parse_error_to_byte_string(module_result.error()));
        return -1;
    }
    engine.module = module_result.release_value();
    auto validation_result = engine.machine.validate(*engine.module);
    if (validation_result.is_error()) {
        dbgln("Failed to validate module: {}", validation_result.error());
        return -1;
    }

    return 0;
}

extern "C" i32 wasm_bench_instantiate(void* engine_ptr)
{
    auto& engine = *static_cast<Engine*>(engine_ptr);
    auto& machine = engine.machine;

    engine.config.instantiation_start(engine.config.instantiation_timer);
    ScopeGuard guard = [&] {
        engine.config.instantiation_end(engine.config.instantiation_timer);
    };

    if (!engine.module)
        return -1;

    Wasm::Linker linker(*engine.module);

    auto ft = Wasm::FunctionType({}, {});
    auto map = HashMap<Wasm::Linker::Name, Wasm::ExternValue> {
        {
            Wasm::Linker::Name { .module = "bench", .name = "start", .type = ft },
            *engine.machine.store().allocate(Wasm::HostFunction([&](Wasm::Configuration&, Vector<Wasm::Value>&) -> Wasm::Result { engine.config.execution_start(engine.config.execution_timer); return Wasm::Result(Vector<Wasm::Value>{}); }, ft, "start")),
        },
        {
            Wasm::Linker::Name { .module = "bench", .name = "end", .type = ft },
            *engine.machine.store().allocate(Wasm::HostFunction([&](Wasm::Configuration&, Vector<Wasm::Value>&) -> Wasm::Result { engine.config.execution_end(engine.config.execution_timer); return Wasm::Result(Vector<Wasm::Value>{}); }, ft, "end")),
        },
    };

    for (auto& name : linker.unresolved_imports()) {
        if (name.module == "wasi_snapshot_preview1") {
            auto fn_or_error = engine.wasi.function_by_name(name.name);
            if (fn_or_error.is_error()) {
                dbgln("Failed to find WASI function: {}", name.name);
                return -1;
            }
            map.set(name, *engine.machine.store().allocate(fn_or_error.release_value()));
        }
    }

    linker.link(map);
    auto link_result = linker.finish();
    if (link_result.is_error()) {
        dbgln("Failed to link module,");
        auto errors = link_result.error();
        for (auto& e : errors.missing_imports)
            dbgln("Missing import: {}", e);

        auto err = MUST(Core::File::standard_error());
        Wasm::Printer printer(*err);
        printer.print(*engine.module);
        return -1;
    }

    auto instantiation_result = machine.instantiate(*engine.module, link_result.release_value());
    if (instantiation_result.is_error()) {
        dbgln("Failed to instantiate module: {}", instantiation_result.error().error);
        return -1;
    }

    engine.instance = instantiation_result.release_value();
    return 0;
}

extern "C" i32 wasm_bench_execute(void* engine_ptr)
{
    auto& engine = *static_cast<Engine*>(engine_ptr);
    auto& machine = engine.machine;

    if (!engine.instance)
        return -1;

    // Find the _start function.
    Optional<Wasm::FunctionAddress> start;
    for (auto& entry : engine.instance->exports()) {
        if (entry.name() == "_start"sv) {
            start = entry.value().get<Wasm::FunctionAddress>();
            break;
        }
    }

    if (!start.has_value()) {
        dbgln("No _start function found in module");
        return -1;
    }

    (void)machine.invoke(*start, {});

    return 0;
}