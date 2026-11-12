#pragma once
#include "shared/config.hpp"
#include <optional>
#include <tuple>

namespace hyprcapture {
// Evaluate only at config load/reload, never on capture dispatch.
class OcrFallbackWarning {
  public:
    bool update(const OcrConfig& config) {
        const Key key{config.mode, config.engine, config.languages, config.command, config.output, config.timeout};
        if (m_previous && *m_previous == key) return false;
        m_previous = key;
        return (config.mode == "text_layer" || config.mode == "both") && config.engine == "command" && config.output == "text";
    }
    static constexpr auto message = "ocr_mode = text_layer needs text positions, but ocr_engine = command with ocr_output = text only returns text; the OCR button will copy all text instead. Set ocr_output = json or use tesseract/paddleocr.";
  private:
    using Key = std::tuple<std::string,std::string,std::string,std::string,std::string,std::int64_t>;
    std::optional<Key> m_previous;
};
}
