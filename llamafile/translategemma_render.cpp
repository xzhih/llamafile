// -*- mode:c++;indent-tabs-mode:nil;c-basic-offset:4;coding:utf-8 -*-
// vi: set et ft=cpp ts=4 sts=4 sw=4 fenc=utf-8 :vi

#include "translategemma_request.h"

#include <stdexcept>

#include "chat-auto-parser.h"
#include "chat.h"
#include "common.h"
#include "llama.h"

namespace lf {
namespace chatbot {

namespace {

std::string replace_rendered_image_placeholders(
    std::string prompt,
    const std::vector<TranslateGemmaImagePayload> &images,
    std::string_view media_placeholder) {
    size_t search_from = 0;
    for (const auto &image : images) {
        const size_t pos = prompt.find("<start_of_image>", search_from);
        if (pos == std::string::npos) {
            throw std::invalid_argument("rendered prompt is missing <start_of_image> placeholder for image content");
        }
        const std::string replacement = media_placeholder.empty() ? image.data_uri : std::string(media_placeholder);
        prompt.replace(pos, strlen("<start_of_image>"), replacement);
        search_from = pos + replacement.size();
    }
    if (prompt.find("<start_of_image>", search_from) != std::string::npos) {
        throw std::invalid_argument("rendered prompt contains unexpected extra <start_of_image> placeholders");
    }
    return prompt;
}

std::string inject_translation_instruction_into_prompt(
    std::string prompt, std::string_view translation_instruction) {
    const std::string instruction = trim_translategemma_whitespace(std::string(translation_instruction));
    if (instruction.empty()) {
        return prompt;
    }

    static const char *const kAnchors[] = {
        "\nProduce only the ",
        "\nPlease translate the ",
    };

    size_t insert_pos = std::string::npos;
    for (const char *anchor : kAnchors) {
        const size_t pos = prompt.find(anchor);
        if (pos != std::string::npos && (insert_pos == std::string::npos || pos < insert_pos)) {
            insert_pos = pos + 1;
        }
    }
    if (insert_pos == std::string::npos) {
        throw std::invalid_argument(
            "failed to locate translation instruction insertion point in rendered prompt");
    }

    prompt.insert(insert_pos, "Translation preference: " + instruction + "\n");
    return prompt;
}

} // namespace

TranslateGemmaRenderedRequest render_translategemma_request(
    std::string_view template_source,
    const llama_vocab *vocab,
    const TranslateGemmaPreparedMessagesRequest &request,
    std::string_view translation_instruction,
    std::string_view media_placeholder) {
    if (template_source.empty()) {
        throw std::invalid_argument("chat template source is empty");
    }

    const llama_token bos_id = vocab ? llama_vocab_bos(vocab) : LLAMA_TOKEN_NULL;
    const llama_token eos_id = vocab ? llama_vocab_eos(vocab) : LLAMA_TOKEN_NULL;
    const std::string bos_token = bos_id == LLAMA_TOKEN_NULL ? std::string() : common_token_to_piece(vocab, bos_id, true);
    const std::string eos_token = eos_id == LLAMA_TOKEN_NULL ? std::string() : common_token_to_piece(vocab, eos_id, true);

    common_chat_template chat_template(std::string(template_source), bos_token, eos_token);

    autoparser::templates_params params;
    params.messages = request.messages;
    params.add_generation_prompt = true;
    params.add_bos = vocab && llama_vocab_get_add_bos(vocab);
    params.add_eos = vocab && llama_vocab_get_add_eos(vocab);

    TranslateGemmaRenderedRequest rendered;
    rendered.has_media = request.has_media;
    rendered.prompt = common_chat_template_direct_apply(chat_template, params);
    rendered.prompt = inject_translation_instruction_into_prompt(
        std::move(rendered.prompt), translation_instruction);
    if (!request.images.empty()) {
        rendered.prompt = replace_rendered_image_placeholders(
            std::move(rendered.prompt), request.images, media_placeholder);
    }
    rendered.image_bytes.reserve(request.images.size());
    for (const auto &image : request.images) {
        rendered.image_bytes.push_back(image.bytes);
    }
    return rendered;
}

} // namespace chatbot
} // namespace lf
