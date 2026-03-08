// -*- mode:c++;indent-tabs-mode:nil;c-basic-offset:4;coding:utf-8 -*-
// vi: set et ft=cpp ts=4 sts=4 sw=4 fenc=utf-8 :vi

#include "embedded_resource.h"

#include <cstdio>
#include <unistd.h>

#include "common.h"

namespace lf {
namespace chatbot {

static bool path_exists_on_host(std::string_view path) {
    return !path.empty() && access(std::string(path).c_str(), R_OK) == 0;
}

EmbeddedPathResolution resolve_embedded_path(std::string_view path, const PathExistsFn &path_exists) {
    EmbeddedPathResolution result;
    result.path = std::string(path);
    result.is_zip = path.rfind("/zip/", 0) == 0;
    if (path.empty() || result.is_zip) {
        return result;
    }
    if (path.find('/') != std::string_view::npos) {
        return result;
    }
    if (path_exists(path)) {
        return result;
    }

    const std::string zip_path = "/zip/" + std::string(path);
    if (path_exists(zip_path)) {
        result.path = zip_path;
        result.is_zip = true;
        result.resolved_from_bundle = true;
    }
    return result;
}

EmbeddedPathResolution resolve_embedded_path(std::string_view path) {
    return resolve_embedded_path(path, path_exists_on_host);
}

void normalize_embedded_model_params(common_params *params, bool verbose) {
    if (!params) {
        return;
    }
    if (!params->model.path.empty()) {
        const auto model = resolve_embedded_path(params->model.path);
        if (model.is_zip) {
            params->use_mmap = false;
        }
        if (verbose && model.resolved_from_bundle) {
            std::fprintf(stderr,
                         "info: using bundled model from %s (mmap disabled for /zip)\n",
                         model.path.c_str());
        }
        params->model.path = model.path;
    }

    if (!params->mmproj.path.empty()) {
        const auto mmproj = resolve_embedded_path(params->mmproj.path);
        if (verbose && mmproj.resolved_from_bundle) {
            std::fprintf(stderr, "info: using bundled vision model from %s\n", mmproj.path.c_str());
        }
        params->mmproj.path = mmproj.path;
    }
}

} // namespace chatbot
} // namespace lf
