// -*- mode:c++;indent-tabs-mode:nil;c-basic-offset:4;coding:utf-8 -*-
// vi: set et ft=cpp ts=4 sts=4 sw=4 fenc=utf-8 :vi

#pragma once

#include <functional>
#include <string>
#include <string_view>

struct common_params;

namespace lf {
namespace chatbot {

struct EmbeddedPathResolution {
    std::string path;
    bool is_zip = false;
    bool resolved_from_bundle = false;
};

using PathExistsFn = std::function<bool(std::string_view)>;

EmbeddedPathResolution resolve_embedded_path(std::string_view path);
EmbeddedPathResolution resolve_embedded_path(std::string_view path, const PathExistsFn &path_exists);
void normalize_embedded_model_params(common_params *params, bool verbose);

} // namespace chatbot
} // namespace lf
