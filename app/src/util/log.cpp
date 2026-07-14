#include "util/log.h"

#include <unistd.h>

#include <memory>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace st {

namespace {

// Emits systemd's kernel-style level prefix ("<3>", "<4>", ...) at the head of each line.
// journald strips it and files the message at that PRIORITY, because the unit runs with
// SyslogLevelPrefix=yes (systemd's default for stdout).
//
// Without it a plain stdout line carries no level, so every line the daemon writes lands at
// PRIORITY=6 (info) and `journalctl -u soundtesterd -p err` reports "No entries" on a board
// that has logged real errors.
class LevelPrefix : public spdlog::custom_flag_formatter {
 public:
  void format(const spdlog::details::log_msg& msg, const std::tm&,
              spdlog::memory_buf_t& dest) override {
    const char* p = "<6>";
    switch (msg.level) {
      case spdlog::level::critical: p = "<2>"; break;
      case spdlog::level::err:      p = "<3>"; break;
      case spdlog::level::warn:     p = "<4>"; break;
      case spdlog::level::debug:
      case spdlog::level::trace:    p = "<7>"; break;
      default:                      p = "<6>"; break;
    }
    dest.append(p, p + 3);
  }

  std::unique_ptr<custom_flag_formatter> clone() const override {
    return spdlog::details::make_unique<LevelPrefix>();
  }
};

}  // namespace

void init_logging(bool verbose) {
  auto logger = spdlog::stdout_color_mt("soundtester");
  spdlog::set_default_logger(logger);
  spdlog::set_level(verbose ? spdlog::level::debug : spdlog::level::info);

  if (isatty(STDOUT_FILENO)) {
    // Run by hand: colour, and the level as a letter. No machine-readable prefix.
    spdlog::set_pattern("%^%L%$ %v");
  } else {
    // Under systemd: journald supplies the timestamps, the prefix supplies the level.
    auto f = std::make_unique<spdlog::pattern_formatter>();
    f->add_flag<LevelPrefix>('*').set_pattern("%*%v");
    spdlog::set_formatter(std::move(f));
  }

  spdlog::flush_on(spdlog::level::info);
}

}  // namespace st
