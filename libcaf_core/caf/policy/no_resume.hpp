#ifndef NO_RESUME_HPP
#define NO_RESUME_HPP

#include <chrono>
#include <utility>

#include "caf/exception.hpp"
#include "caf/exit_reason.hpp"
#include "caf/policy/resume_policy.hpp"

namespace caf {
namespace policy {

class no_resume {
 public:
  template <class Base, class Derived>
  struct mixin : Base {
    template <class... Ts>
    mixin(Ts&&... args) : Base(std::forward<Ts>(args)...), m_hidden(true) {
      // nop
    }

    void attach_to_scheduler() {
      this->ref();
    }

    void detach_from_scheduler() {
      this->deref();
    }

    resumable::resume_result resume(execution_unit*, size_t) {
      auto done_cb = [=](uint32_t reason) {
        this->planned_exit_reason(reason);
        this->on_exit();
        this->cleanup(reason);
      };
      try {
        this->act();
        done_cb(exit_reason::normal);
      }
      catch (actor_exited& e) {
        done_cb(e.reason());
      }
      catch (...) {
        done_cb(exit_reason::unhandled_exception);
      }
      return resumable::done;
    }

    bool m_hidden;
  };

  template <class Actor>
  void await_ready(Actor* self) {
    self->await_data();
  }
};

} // namespace policy
} // namespace caf

#endif // NO_RESUME_HPP