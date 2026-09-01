#pragma once

#include <string_view>

namespace hg::platform {

class Renderer {
public:
    virtual ~Renderer() = default;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
};

} // namespace hg::platform

