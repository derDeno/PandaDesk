#include <cassert>
#include "pandadesk/status_led.hpp"

int main() {
  using pandadesk::status_led::level;
  static_assert(pandadesk::status_led::kDefaultBrightness == 50);
  // AP blinking takes priority, including after a desk has been detected.
  assert(level(0, 50, true, true, true) == 50);
  assert(level(124, 50, true, true, true) == 50);
  assert(level(125, 50, true, true, true) == 0);
  assert(level(249, 50, true, true, true) == 0);
  assert(level(250, 50, true, true, true) == 50);
  // A three-second pulse reaches the saved peak and returns to zero.
  assert(level(0, 50, false, true, false) == 0);
  assert(level(750, 50, false, true, false) == 25);
  assert(level(1500, 50, false, true, false) == 50);
  assert(level(2250, 50, false, true, false) == 25);
  assert(level(3000, 50, false, true, false) == 0);
  for (unsigned ms = 0; ms < 6000; ++ms) {
    assert(level(ms, 23, false, true, true) == 23);
    assert(level(ms, 23, false, false, true) <= 23);
    assert(level(ms, 0, true, false, false) == 0);
    assert(level(ms, 0, false, true, false) == 0);
    assert(level(ms, 0, false, true, true) == 0);
  }
}
