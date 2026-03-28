#include "config.h"

#include "canvas.h"
#include "window_title.h"

#include <algorithm>
#include <ctime>

#include "control.h"
#include "core/manager.h"
#include "core/port_check.h"

namespace display {

namespace {

std::string format_age(int64_t checked_at) {
  if (checked_at <= 0)
    return std::string();

  int64_t age = std::max<int64_t>(0, std::time(nullptr) - checked_at);

  if (age >= 3600)
    return std::to_string(age / 3600) + "h";
  if (age >= 60)
    return std::to_string(age / 60) + "m";
  return std::to_string(age) + "s";
}

std::string build_port_check_status() {
  auto* port_check = control->core()->port_check();

  if (!port_check->is_active())
    return "Port: off";

  std::string text = "Port: " + port_check->status_name();

  auto age = format_age(port_check->last_check_at());
  if (!age.empty())
    text += " " + age;

  return text;
}

} // namespace

void
WindowTitle::redraw() {
  if (m_canvas->daemon())
    return;

  schedule_update();
  m_canvas->erase();

  std::string title = "*** " + m_title + " ***";
  auto title_x = std::max(0, ((int)m_canvas->width() - (int)title.size()) / 2);
  m_canvas->print(title_x, 0, "%s", title.c_str());

  std::string port_text = build_port_check_status();
  auto port_x = std::max(0, (int)m_canvas->width() - (int)port_text.size() - 1);

  if (port_x > title_x + (int)title.size())
    m_canvas->print(port_x, 0, "%s", port_text.c_str());
}

} // namespace display
