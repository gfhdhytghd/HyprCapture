#include <QApplication>
#include <QPainter>
#include <QWidget>

namespace {
class Fixture final : public QWidget {
  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(34, 149, 98));
    }
};
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setDesktopFileName("fsdeco-fixture");
    Fixture window;
    window.setWindowTitle("fsdeco-fixture");
    window.setAttribute(Qt::WA_OpaquePaintEvent);
    window.resize(400, 300);
    window.show();
    return app.exec();
}
