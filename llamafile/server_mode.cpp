// -*- mode:c++;indent-tabs-mode:nil;c-basic-offset:4;coding:utf-8 -*-
// vi: set et ft=cpp ts=4 sts=4 sw=4 fenc=utf-8 :vi

#include "server_mode.h"

#include <array>
#include <string_view>

namespace lf {
namespace chatbot {

static std::string_view arg_name(std::string_view arg) {
    const size_t eq = arg.find('=');
    return eq == std::string_view::npos ? arg : arg.substr(0, eq);
}

bool has_translategemma_flags(int argc, char **argv) {
    constexpr std::array<std::string_view, 6> kTranslateFlags = {{
        "--translate-text",
        "--translate-image",
        "--translate-messages-json",
        "--source-lang",
        "--target-lang",
        "--translation-instruction",
    }};

    for (int i = 1; i < argc; ++i) {
        const std::string_view current = arg_name(argv[i]);
        for (const auto flag : kTranslateFlags) {
            if (current == flag) {
                return true;
            }
        }
    }
    return false;
}

bool has_explicit_chat_template_override(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view current = arg_name(argv[i]);
        if (current == "--chat-template" || current == "--chat-template-file") {
            return true;
        }
    }
    return false;
}

bool should_use_server_safe_gemma_template(std::string_view architecture,
                                           std::string_view chat_template_source) {
    return architecture == "gemma3" &&
           chat_template_source.find("source_lang_code") != std::string_view::npos &&
           chat_template_source.find("target_lang_code") != std::string_view::npos;
}

} // namespace chatbot
} // namespace lf
