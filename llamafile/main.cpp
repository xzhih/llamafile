// -*- mode:c++;indent-tabs-mode:nil;c-basic-offset:4;coding:utf-8 -*-
// vi: set et ft=cpp ts=4 sts=4 sw=4 fenc=utf-8 :vi
//
// Copyright 2024 Mozilla Foundation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

//
// llamafile - Main entry point
//
// This is the main entry point for llamafile. It provides a TUI (Text User
// Interface) for interactive chatting with LLMs using the llama.cpp backend,
// or can run as an HTTP server for API access.
//
// Usage:
//   llamafile -m model.gguf              # Start TUI with model (default)
//   llamafile -m model.gguf --mmproj ... # TUI with vision model
//   llamafile -m model.gguf --chat       # TUI mode (explicit)
//   llamafile -m model.gguf --server     # HTTP server mode
//

#include "chatbot.h"
#include "embedded_resource.h"
#include "llamafile.h"
#include "server_mode.h"

#include "gguf.h"

#include <cstdio>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#ifdef COSMOCC
#include <cosmo.h>
#endif

enum Program {
    PROG_UNKNOWN,
    PROG_CHAT,
    PROG_SERVER,
};

int server_main(int argc, char **argv);

namespace {

constexpr const char *kServerTranslateModeError =
    "TranslateGemma CLI/TUI flags are not supported with --server; use the existing HTTP API instead";

struct PreparedServerArgs {
    std::vector<std::string> args;
    std::vector<std::string> info_messages;
};

struct ServerModelMetadata {
    std::string architecture;
    std::string chat_template_source;
};

static std::string_view arg_name(std::string_view arg) {
    const size_t eq = arg.find('=');
    return eq == std::string_view::npos ? arg : arg.substr(0, eq);
}

static bool read_server_model_metadata(const std::string &model_path, ServerModelMetadata *out) {
    if (!out || model_path.empty()) {
        return false;
    }

    const gguf_init_params params = {
        /*.no_alloc = */ true,
        /*.ctx      = */ nullptr,
    };
    gguf_context *ctx = gguf_init_from_file(model_path.c_str(), params);
    if (!ctx) {
        return false;
    }

    const int arch_key = gguf_find_key(ctx, "general.architecture");
    if (arch_key >= 0 && gguf_get_kv_type(ctx, arch_key) == GGUF_TYPE_STRING) {
        out->architecture = gguf_get_val_str(ctx, arch_key);
    }

    const int tmpl_key = gguf_find_key(ctx, "tokenizer.chat_template");
    if (tmpl_key >= 0 && gguf_get_kv_type(ctx, tmpl_key) == GGUF_TYPE_STRING) {
        out->chat_template_source = gguf_get_val_str(ctx, tmpl_key);
    }

    gguf_free(ctx);
    return !out->architecture.empty() || !out->chat_template_source.empty();
}

static bool prepare_server_args(int argc, char **argv, PreparedServerArgs *out, std::string *error) {
    if (!out) {
        if (error) {
            *error = "internal error: missing server argument buffer";
        }
        return false;
    }

    out->args.clear();
    out->info_messages.clear();
    out->args.reserve(argc + 4);
    for (int i = 0; i < argc; ++i) {
        out->args.emplace_back(argv[i]);
    }

    bool needs_no_mmap = false;
    bool has_explicit_template = lf::chatbot::has_explicit_chat_template_override(argc, argv);
    std::string resolved_model_path;

    for (size_t i = 1; i < out->args.size(); ++i) {
        const std::string_view name = arg_name(out->args[i]);
        if (name != "-m" && name != "--model" && name != "--mmproj") {
            continue;
        }

        bool inline_value = false;
        size_t value_index = i;
        std::string value;
        const size_t eq = out->args[i].find('=');
        if (eq != std::string::npos) {
            inline_value = true;
            value = out->args[i].substr(eq + 1);
        } else {
            if (i + 1 >= out->args.size()) {
                continue;
            }
            value_index = i + 1;
            value = out->args[value_index];
        }

        const auto resolved = lf::chatbot::resolve_embedded_path(value);
        if (!resolved.path.empty() && resolved.path != value) {
            if (inline_value) {
                out->args[i] = std::string(name) + "=" + resolved.path;
            } else {
                out->args[value_index] = resolved.path;
            }
        }

        if (name == "-m" || name == "--model") {
            resolved_model_path = resolved.path.empty() ? value : resolved.path;
            if (resolved.is_zip) {
                needs_no_mmap = true;
            }
            if (resolved.resolved_from_bundle) {
                out->info_messages.emplace_back(
                    "info: using bundled model from " + resolved.path + " (mmap disabled for /zip)");
            }
        } else if (name == "--mmproj" && resolved.resolved_from_bundle) {
            out->info_messages.emplace_back("info: using bundled vision model from " + resolved.path);
        }
    }

    if (needs_no_mmap) {
        out->args.emplace_back("--no-mmap");
    }

    if (!has_explicit_template && !resolved_model_path.empty()) {
        ServerModelMetadata metadata;
        if (read_server_model_metadata(resolved_model_path, &metadata) &&
            lf::chatbot::should_use_server_safe_gemma_template(
                metadata.architecture, metadata.chat_template_source)) {
            out->args.emplace_back("--chat-template");
            out->args.emplace_back("gemma");
            out->info_messages.emplace_back(
                "info: using server-safe chat template 'gemma' for TranslateGemma HTTP routes");
        }
    }

    return true;
}

} // namespace

static enum Program determine_program(char *argv[]) {
    enum Program prog = PROG_UNKNOWN;
    for (int i = 0; argv[i]; ++i) {
        if (!strcmp(argv[i], "--chat")) {
            prog = PROG_CHAT;
        } else if (!strcmp(argv[i], "--server")) {
            prog = PROG_SERVER;
        }
    }
    return prog;
}

int removeArgs(int argc, char* argv[],
               const std::set<std::string>& flags_to_remove,
               const std::set<std::string>& args_with_param_to_remove) {

    int write_idx = 0;
    for (int read_idx = 0; read_idx < argc; ++read_idx) {
        std::string current_arg = argv[read_idx];

        // Check if it's a simple flag to remove
        if (flags_to_remove.count(current_arg)) {
            continue;
        }

        // Check if it's an argument with a parameter to remove
        if (args_with_param_to_remove.count(current_arg)) {
            // Skip the parameter too (if present)
            if (read_idx + 1 < argc) {
                ++read_idx;
            }
            continue;
        }

        // Keep this argument
        if (write_idx != read_idx) {
            argv[write_idx] = argv[read_idx];
        }
        write_idx++;
    }

    // `write_idx` is now the new number of arguments
    // NULL out argv[write_idx] to guarantee that argv[argc] == NULL
    argv[write_idx] = nullptr;

    return write_idx;
}


int main(int argc, char **argv) {
    // Load arguments from zip file if present (for bundled llamafiles)
    argc = cosmo_args("/zip/.args", &argv);

    enum Program prog = determine_program(argv);
    if (prog == PROG_SERVER && lf::chatbot::has_translategemma_flags(argc, argv)) {
        std::fprintf(stderr, "error: %s\n", kServerTranslateModeError);
        return 64;
    }

    std::string translate_error;
    if (!lf::chatbot::parse_translategemma_options(argc, argv, &lf::chatbot::g_translate_options, &translate_error)) {
        std::fprintf(stderr, "error: %s\n", translate_error.c_str());
        return 64;
    }

    PreparedServerArgs prepared_server_args;
    if (prog == PROG_SERVER &&
        !prepare_server_args(argc, argv, &prepared_server_args, &translate_error)) {
        std::fprintf(stderr, "error: %s\n", translate_error.c_str());
        return 64;
    }

    // Check GPU flags early to determine if we should load GPU support
    // This must be called BEFORE llamafile_has_metal() etc.
    llamafile_early_gpu_init(argv);

    // Initialize GPU support early (must happen BEFORE llama_backend_init())
    // This triggers dynamic loading of GPU backends (CUDA, ROCm, Metal)
    // The llamafile_has_* functions use lazy initialization via cosmo_once()
    llamafile_has_gpu();

    // remove arguments which llama.cpp does not support
    // (first set: flags, second set: arguments with params)
    argc = removeArgs(argc, argv, 
                    {"--server"},
                    {"--gpu", "--translate-text", "--translate-image", "--translate-messages-json", "--source-lang", "--target-lang", "--translation-instruction"}
                    );

    if (prog == PROG_SERVER) {
        for (const auto &message : prepared_server_args.info_messages) {
            std::fprintf(stderr, "%s\n", message.c_str());
        }

        std::vector<char *> server_argv;
        server_argv.reserve(prepared_server_args.args.size() + 1);
        for (auto &arg : prepared_server_args.args) {
            server_argv.push_back(arg.data());
        }
        server_argv.push_back(nullptr);

        int server_argc = removeArgs(
            static_cast<int>(prepared_server_args.args.size()),
            server_argv.data(),
            {"--server"},
            {"--gpu"});
        return server_main(server_argc, server_argv.data());
    }

    // Chat mode (explicit --chat or default when no -p/-f/--random-prompt)
    if (prog == PROG_CHAT ||
        (prog == PROG_UNKNOWN &&
         !llamafile_has(argv, "-p") &&
         !llamafile_has(argv, "-f"))) {
        return lf::chatbot::main(argc, argv);
    }

    // If we have -p, -f, or --random-prompt without explicit mode,
    // default to chatbot for now (could add CLI mode later)
    return lf::chatbot::main(argc, argv);
}
