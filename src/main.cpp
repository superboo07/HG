#include "renderer/opengl_renderer.h"

#include <iostream>
#include <string_view>

int main(int argc, char** argv)
{
    constexpr std::string_view version = "0.1.0-dev";
    if (argc == 2 && std::string_view{argv[1]} == "--version") {
        std::cout << "hg_runner " << version << '\n';
        return 0;
    }

    const hg::renderer::OpenGLRenderer renderer;
    std::cout << "Haunting Ground static recompilation runner ("
              << renderer.name() << " backend scaffold)\n";
    std::cout << "No recompiled game code has been linked yet.\n";
    return 0;
}

