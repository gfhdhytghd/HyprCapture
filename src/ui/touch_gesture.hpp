#pragma once
#include <QPointF>
#include <QVector>
#include <cmath>

namespace hyprcapture::ui {
// One device owns a gesture until all fingers lift. In particular, 2 -> 1
// cannot accidentally start a stroke, and cancellation never commits a draft.
class TouchGesture {
public:
  struct Point {
    int id;
    QPointF position;
  };
  enum class Action {
    None,
    BeginStroke,
    MoveStroke,
    EndStroke,
    CancelStroke,
    Scroll,
    EndScroll
  };
  struct Update {
    Action action = Action::None;
    QPointF position;
    qreal delta = 0;
    QPointF movement;
  };
  QVector<Update> update(const QVector<Point> &points, bool cancelled = false,
                         bool pan = false) {
    QVector<Update> out;
    if (cancelled || points.size() > 2) {
      if (m_state == Single)
        out.push_back({Action::CancelStroke});
      if (m_state == Scrolling)
        out.push_back({Action::EndScroll, m_anchor});
      m_state = points.empty() ? Idle : Blocked;
      return out;
    }
    if (points.empty()) {
      if (m_state == Single)
        out.push_back({Action::EndStroke, m_last});
      if (m_state == Scrolling)
        out.push_back({Action::EndScroll, m_anchor});
      m_state = Idle;
      return out;
    }
    if (m_state == Blocked)
      return out;
    if (points.size() == 1) {
      if (m_state == Idle) {
        m_state = Single;
        m_id = points[0].id;
        m_last = points[0].position;
        out.push_back({Action::BeginStroke, m_last});
      } else if (m_state == Single && m_id == points[0].id) {
        m_last = points[0].position;
        out.push_back({Action::MoveStroke, m_last});
      } else {
        if (m_state == Scrolling)
          out.push_back({Action::EndScroll, m_anchor});
        m_state = Blocked;
      }
      return out;
    }
    const QPointF center = (points[0].position + points[1].position) / 2;
    if (m_state == Single || m_state == Idle) {
      if (m_state == Single)
        out.push_back({Action::CancelStroke});
      m_state = Two;
      m_anchor = m_last = center;
      m_first = points[0].id;
      m_second = points[1].id;
      return out;
    }
    if (!((points[0].id == m_first && points[1].id == m_second) ||
          (points[1].id == m_first && points[0].id == m_second))) {
      if (m_state == Scrolling)
        out.push_back({Action::EndScroll, m_anchor});
      m_state = Blocked;
      return out;
    }
    const QPointF distance = center - m_anchor;
    if (m_state == Two && (pan ? std::hypot(distance.x(), distance.y()) >= 8 :
        std::abs(distance.y()) >= 8 && std::abs(distance.y()) > std::abs(distance.x())))
      m_state = Scrolling;
    if (m_state == Scrolling) {
      const qreal dy = center.y() - m_last.y();
      if (dy != 0 || (pan && center.x() != m_last.x()))
        out.push_back({Action::Scroll, m_anchor, -dy, center - m_last});
      m_last = center;
    }
    return out;
  }

private:
  enum { Idle, Single, Two, Scrolling, Blocked } m_state = Idle;
  int m_id = 0, m_first = 0, m_second = 0;
  QPointF m_last, m_anchor;
};
} // namespace hyprcapture::ui
