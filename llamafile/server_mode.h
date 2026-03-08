// -*- mode:c++;indent-tabs-mode:nil;c-basic-offset:4;coding:utf-8 -*-
// vi: set et ft=cpp ts=4 sts=4 sw=4 fenc=utf-8 :vi

#pragma once

#include <string_view>

namespace lf {
namespace chatbot {

bool has_translategemma_flags(int argc, char **argv);
bool has_explicit_chat_template_override(int argc, char **argv);
bool should_use_server_safe_gemma_template(std::string_view architecture,
                                           std::string_view chat_template_source);

} // namespace chatbot
} // namespace lf
