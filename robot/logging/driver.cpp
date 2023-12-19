#define CAF_LOG_COMPONENT "app"

#include "caf/caf_main.hpp"
#include "caf/logger.hpp"
#include "caf/type_id.hpp"

#include <string_view>

struct foobar;

CAF_BEGIN_TYPE_ID_BLOCK(driver, caf::first_custom_type_id)

  CAF_ADD_TYPE_ID(driver, (foobar))

CAF_END_TYPE_ID_BLOCK(driver)

constexpr std::string_view component = "app";

struct foobar {
  std::string foo;
  std::string bar;
};

template <class Inspector>
bool inspect(Inspector& f, foobar& x) {
  return f.object(x).fields(f.field("foo", x.foo), f.field("bar", x.bar));
}

void foo([[maybe_unused]] int value, bool use_legacy_api) {
  if (use_legacy_api) {
    CAF_LOG_TRACE(CAF_ARG(value));
    CAF_LOG_DEBUG("this is a debug message");
    CAF_LOG_DEBUG("this is another debug message with foobar(\"one\", \"two\") "
                  "; field = foobar(\"three\", \"four\")");
    CAF_LOG_INFO("this is an info message");
    CAF_LOG_INFO("this is another info message ; foo = bar");
    CAF_LOG_WARNING("this is a warning message");
    CAF_LOG_WARNING("this is another warning message ; foo = bar");
    CAF_LOG_ERROR("this is an error message");
    CAF_LOG_ERROR("this is another error message ; foo = bar");
  } else {
    using caf::logger;
    auto trace_guard = logger::trace(component, "value = {}", value);
    logger::debug(component, "this is a debug message");
    logger::debug(component)
      .message("this is {} with {}", "another debug message",
               foobar{"one", "two"})
      .field("field", "{}", foobar{"three", "four"})
      .send();
    logger::info(component, "this is an info message");
    logger::info(component)
      .message("this is {}", "another info message")
      .field("foo", "bar")
      .send();
    logger::warning(component, "this is a warning message");
    logger::warning(component)
      .message("this is {}", "another warning message")
      .field("foo", "bar")
      .send();
    logger::error(component, "this is an error message");
    logger::error(component)
      .message("this is {}", "another error message")
      .field("foo", "bar")
      .send();
  }
}

class config : public caf::actor_system_config {
public:
  config() {
    opt_group{custom_options_, "global"} //
      .add<std::string>("api", "sets the API");
  }
};

void caf_main(caf::actor_system&, const config& cfg) {
  foo(42, get_or(cfg, "api", "default") == "legacy");
}

// creates a main function for us that calls our caf_main
CAF_MAIN(caf::id_block::driver)
