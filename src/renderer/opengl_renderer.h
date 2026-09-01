#pragma once

#include "platform/renderer.h"

namespace hg::renderer {

class OpenGLRenderer final : public platform::Renderer {
public:
    [[nodiscard]] std::string_view name() const noexcept override;
};

} // namespace hg::renderer

