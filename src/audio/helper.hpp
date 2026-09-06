#pragma once
namespace hyprcapture::audio {
// Separate, headless processes dispatched before QApplication construction.
int runHelper(int argc, char** argv);
}
