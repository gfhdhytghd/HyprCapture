#pragma once

#include <QString>

class QScreen;
class QWidget;

// Creates and shows an independent desktop pin. The caller owns the returned
// widget and must keep it alive while running QApplication::exec(). Closing any
// of its output surfaces closes the whole pin and quits its helper process.
//
// Source files are preserved by default. With consumePrivateRuntimeSource,
// only a regular, user-owned image in HyprCapture's private runtime directory
// may be removed, and only after it has been successfully read into memory.
// Invalid images and arbitrary user-supplied paths are never removed.
QWidget* createPinnedImage(const QString& path,
                           bool consumePrivateRuntimeSource = false,
                           QString* error = nullptr,
                           QScreen* targetScreen = nullptr);
