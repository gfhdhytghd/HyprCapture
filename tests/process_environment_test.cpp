#include "shared/process_environment.hpp"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message) {
    if (condition)
        return;

    std::cerr << "process environment test failed: " << message << '\n';
    std::exit(1);
}

} // namespace

int main() {
    using hyprcapture::desktopEnvironmentNameAllowed;

    require(desktopEnvironmentNameAllowed("XDG_DATA_HOME"), "XDG data home is preserved");
    require(desktopEnvironmentNameAllowed("XDG_CONFIG_HOME"), "XDG config home is preserved");
    require(desktopEnvironmentNameAllowed("XDG_DATA_DIRS"), "XDG data directories are preserved");
    require(desktopEnvironmentNameAllowed("DESKTOP_SESSION"), "desktop session is preserved");
    require(desktopEnvironmentNameAllowed("LC_ALL"), "locale variables are preserved");
    require(!desktopEnvironmentNameAllowed("PATH"), "caller-supplied PATH remains rejected");
    require(!desktopEnvironmentNameAllowed("LD_PRELOAD"), "dynamic loader injection remains rejected");
    require(!desktopEnvironmentNameAllowed("QT_PLUGIN_PATH"), "Qt plugin injection remains rejected");
    require(!desktopEnvironmentNameAllowed("BROWSER"), "browser command injection remains rejected");

    std::cout << "hyprcapture process environment tests passed\n";
    return 0;
}
