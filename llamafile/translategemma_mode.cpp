// -*- mode:c++;indent-tabs-mode:nil;c-basic-offset:4;coding:utf-8 -*-
// vi: set et ft=cpp ts=4 sts=4 sw=4 fenc=utf-8 :vi

#include "chatbot.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>

#include <nlohmann/json.hpp>

#include "chat-auto-parser.h"
#include "common.h"
#include "image.h"
#include "llama.h"
#include "llamafile.h"
#include "sampling.h"
#include "string.h"
#include "translategemma_request.h"

namespace lf {
namespace chatbot {

static const common_chat_templates *ensure_translate_chat_templates() {
    if (!g_chat_templates) {
        if (g_params->chat_template.empty() && llama_model_chat_template(g_model, nullptr) == nullptr) {
            throw std::invalid_argument("model does not provide a chat template required by translate mode");
        }
        g_chat_templates = common_chat_templates_init(g_model, g_params->chat_template);
    }
    if (!g_chat_templates) {
        throw std::invalid_argument("failed to initialize chat template");
    }
    return g_chat_templates.get();
}

static std::string ensure_translate_template_source() {
    const common_chat_templates *tmpls = ensure_translate_chat_templates();
    std::string template_source = common_chat_templates_source(tmpls);
    if (template_source.empty()) {
        throw std::invalid_argument("chat template source is empty");
    }
    return template_source;
}

static std::string generate_translation() {
    std::string output;
    const llama_vocab *vocab = llama_model_get_vocab(g_model);
    for (;;) {
        if (g_got_sigint) {
            g_got_sigint = false;
            break;
        }
        llama_token id = common_sampler_sample(g_sampler, g_ctx, -1);
        common_sampler_accept(g_sampler, id, true);
        if (!eval_token(id)) {
            break;
        }
        if (llama_vocab_is_eog(vocab, id)) {
            break;
        }
        output += token_to_piece(g_ctx, id, false);
        if (output.find("<|im_end|>") != std::string::npos ||
            output.find("<end_of_turn>") != std::string::npos ||
            output.find("</start_of_turn>") != std::string::npos ||
            output.find("</end_of_turn>") != std::string::npos) {
            break;
        }
    }
    llama_synchronize(g_ctx);
    return sanitize_translategemma_output(output);
}

int run_translate_mode() {
    FLAG_nologo = true;
    g_params->prompt.clear();

    std::string request;
    bool request_has_media = false;
    const std::string template_source = ensure_translate_template_source();
    const llama_vocab *vocab = llama_model_get_vocab(g_model);

    if (!g_translate_options.messages_json.empty()) {
        try {
            auto prepared = parse_translategemma_messages_json_source(
                g_translate_options.messages_json,
                TranslateGemmaImageReferencePolicy::kAllowLocalPathOrDataUri);
            auto rendered = render_translategemma_request(
                template_source, vocab, prepared, {});
            if (rendered.has_media && !g_mtmd) {
                err("multimodal model not loaded (use --mmproj to specify vision model)");
                return 11;
            }
            request_has_media = rendered.has_media;
            request = std::move(rendered.prompt);
        } catch (const std::exception &e) {
            err("%s", e.what());
            return 12;
        }
    } else if (g_translate_options.image_mode) {
        if (!g_mtmd) {
            err("multimodal model not loaded (use --mmproj to specify vision model)");
            return 11;
        }
        try {
            auto image_request = build_translategemma_image_request_from_path(
                g_translate_options.source_lang,
                g_translate_options.target_lang,
                g_translate_options.image_path);
            auto rendered = render_translategemma_request(
                template_source, vocab, image_request, g_translate_options.translation_instruction);
            request_has_media = rendered.has_media;
            request = std::move(rendered.prompt);
        } catch (const std::exception &e) {
            err("%s", e.what());
            return 12;
        }
    } else {
        try {
            auto text_request = build_translategemma_text_request(
                g_translate_options.source_lang,
                g_translate_options.target_lang,
                g_translate_options.text);
            auto rendered = render_translategemma_request(
                template_source, vocab, text_request, g_translate_options.translation_instruction);
            request = std::move(rendered.prompt);
        } catch (const std::exception &e) {
            err("%s", e.what());
            return 12;
        }
    }

    // Work around an aarch64 IQK kernel assert hit by some multimodal requests.
    if (request_has_media) {
        setenv("LLAMAFILE_DISABLE_SGEMM", "1", 1);
        setenv("LLAMAFILE_DISABLE_IQK_MIXMUL", "1", 1);
    }

    if (request.empty()) {
        err("translation prompt is empty");
        return 13;
    }

    if (!eval_string(request, ADD_SPECIAL, DONT_PARSE_SPECIAL)) {
        return 13;
    }

    std::string output = generate_translation();
    if (output.empty()) {
        err("translation output is empty");
        return 14;
    }

    std::fputs(output.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    return 0;
}

} // namespace chatbot
} // namespace lf
