#include "plugin/real_background_status.hpp"
#include <cstdio>
#include <cstdlib>

static void require(bool result, const char* message) {
    if (!result) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
int main() {
    using hyprcapture::usableRealBackground;
    require(!usableRealBackground(false, false, false), "absent prepass cannot succeed");
    require(usableRealBackground(true, false, false), "unblurred targets retain their ordinary prepass");
    // With shader blending the only later draw contains the target window.
    // Without a separate blur capture the raw prepass must not be exported.
    require(!usableRealBackground(true, true, false), "shader blend must reject provisional unblurred pixels");
    require(!usableRealBackground(false, true, false), "transformed target without a capture stays unavailable");
    require(usableRealBackground(true, true, true), "standalone blur hook result must stay supported");
    require(!usableRealBackground(false, true, true), "failed readback is never an available background");
}
