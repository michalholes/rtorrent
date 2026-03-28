#ifndef RTORRENT_CORE_PORT_CHECK_H
#define RTORRENT_CORE_PORT_CHECK_H

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include <torrent/utils/scheduler.h>

namespace core {

class PortCheck {
public:
  enum class Provider {
    off,
    yougetsignal,
    portchecker,
  };

  enum class Status {
    unknown,
    open,
    closed,
    error,
  };

  PortCheck();
  ~PortCheck();

  Provider            provider() const;
  std::string         provider_name() const;
  void                set_provider(const std::string& value);

  int64_t             interval_seconds() const;
  void                set_interval_seconds(int64_t value);

  Status              status() const;
  std::string         status_name() const;
  int64_t             last_check_at() const;
  const std::string&  last_error() const;
  const std::string&  last_ip() const;
  int64_t             last_port() const;
  bool                is_active() const;
  bool                checked() const;

  void                start();
  void                stop();
  void                restart();

  struct WorkerInput {
    Provider          provider{Provider::off};
    std::string       explicit_ip;
    int64_t           port{};
    long              timeout_seconds{};
  };

  struct WorkerResult {
    Provider          provider{Provider::off};
    Status            status{Status::unknown};
    int64_t           checked_at{};
    std::string       last_error;
    std::string       ip;
    int64_t           port{};
  };

private:
  void                reset_cached_state();
  void                schedule_after(std::chrono::seconds delay);
  void                on_timeout();
  void                launch_check();
  void                finish_check(WorkerResult result);

  static Provider     parse_provider(const std::string& value);
  static std::string  provider_to_string(Provider provider);
  static std::string  status_to_string(Status status);
  static WorkerResult perform_check(const WorkerInput& input);

  torrent::utils::SchedulerEntry m_timeout;

  std::chrono::seconds m_interval{std::chrono::seconds(120)};
  Provider            m_provider{Provider::yougetsignal};
  Status              m_status{Status::unknown};
  int64_t             m_lastCheckAt{};
  std::string         m_lastError;
  std::string         m_lastIp;
  int64_t             m_lastPort{};
  bool                m_inFlight{};
  std::atomic<bool>   m_stopRequested{false};
  std::thread         m_worker;
};

inline PortCheck::Provider
PortCheck::provider() const {
  return m_provider;
}

inline int64_t
PortCheck::interval_seconds() const {
  return m_interval.count();
}

inline PortCheck::Status
PortCheck::status() const {
  return m_status;
}

inline int64_t
PortCheck::last_check_at() const {
  return m_lastCheckAt;
}

inline const std::string&
PortCheck::last_error() const {
  return m_lastError;
}

inline const std::string&
PortCheck::last_ip() const {
  return m_lastIp;
}

inline int64_t
PortCheck::last_port() const {
  return m_lastPort;
}

inline bool
PortCheck::is_active() const {
  return m_provider != Provider::off;
}

inline bool
PortCheck::checked() const {
  return m_lastCheckAt != 0;
}

} // namespace core

#endif
