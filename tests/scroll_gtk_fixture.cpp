// Opt-in GTK 3 receiver for scroll_capture_live_test. GTK, unlike Qt, flushes
// motion and scroll separately, so it detects a missing wl_pointer.frame.
#include <gtk/gtk.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

struct Fixture {
  cairo_surface_t* page = nullptr;
  int offset = 400;
  bool receivedScroll = false;
};

static gboolean draw(GtkWidget*, cairo_t* cr, gpointer data) {
  const auto& fixture = *static_cast<Fixture*>(data);
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_surface(cr, fixture.page, 0, -fixture.offset);
  cairo_paint(cr);
  return TRUE;
}

static gboolean scroll(GtkWidget* widget, GdkEventScroll* event, gpointer data) {
  auto& fixture = *static_cast<Fixture*>(data);
  double dx = 0, dy = 0;
  if (!gdk_event_get_scroll_deltas(reinterpret_cast<GdkEvent*>(event), &dx, &dy))
    return TRUE;
  auto* source = gdk_event_get_source_device(reinterpret_cast<GdkEvent*>(event));
  const bool finger = source && gdk_device_get_source(source) == GDK_SOURCE_TOUCHPAD;
  if (dy != 0 && !fixture.receivedScroll) {
    // Fail closed if the virtual driver mislabeled a requested finger event.
    if (std::getenv("HYPRCAPTURE_SCROLL_NATIVE_FINGER") && !finger) {
      std::fprintf(stderr, "expected FINGER source, received non-touchpad event\n");
      std::exit(1);
    }
    fixture.receivedScroll = true;
  }
  fixture.offset = std::clamp(fixture.offset + static_cast<int>(std::lround(dy * 10)), 0, 2000);
  std::printf("fixture offset=%d source=%s\n", fixture.offset, finger ? "touchpad" : "wheel");
  std::fflush(stdout);
  gtk_widget_queue_draw(widget);
  return TRUE;
}

int main(int argc, char** argv) {
  if (argc != 2 || !std::getenv("HYPRCAPTURE_SCROLL_LIVE_TEST"))
    return 77;
  g_set_prgname("hyprcapture-scroll-fixture");
  gtk_init(&argc, &argv);
  Fixture fixture;
  fixture.page = cairo_image_surface_create_from_png(argv[1]);
  if (cairo_surface_status(fixture.page) != CAIRO_STATUS_SUCCESS)
    return 2;
  const double scale = cairo_image_surface_get_width(fixture.page) / 800.;
  cairo_surface_set_device_scale(fixture.page, scale, scale);
  auto* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(window), "GTK scroll capture fixture");
  gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
  gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);
  auto* area = gtk_drawing_area_new();
  gtk_widget_add_events(area, GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);
  gtk_container_add(GTK_CONTAINER(window), area);
  g_signal_connect(area, "draw", G_CALLBACK(draw), &fixture);
  g_signal_connect(area, "scroll-event", G_CALLBACK(scroll), &fixture);
  g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
  gtk_widget_show_all(window);
  gtk_main();
  cairo_surface_destroy(fixture.page);
  return 0;
}
