#pragma once

// konCePCja — minimal RAII helpers.

#include <type_traits>
#include <utility>

namespace memutils {

// Runs the stored callable when the scope ends. Move-only; a moved-from
// guard is disarmed. Usage:
//   auto closer = [&] { fclose(f); };
//   memutils::scope_exit<decltype(closer)> guard(closer);
template <typename Fn>
class [[nodiscard]] scope_exit {
 public:
  explicit scope_exit(Fn fn) : fn_(std::move(fn)) {}
  scope_exit(const scope_exit&) = delete;
  scope_exit& operator=(const scope_exit&) = delete;
  scope_exit& operator=(scope_exit&&) = delete;
  scope_exit(scope_exit&& other) noexcept(
      std::is_nothrow_move_constructible_v<Fn>)
      : fn_(std::move(other.fn_)), armed_(std::exchange(other.armed_, false)) {}
  ~scope_exit() {
    if (armed_) fn_();
  }

 private:
  Fn fn_;
  bool armed_ = true;
};

}  // namespace memutils
