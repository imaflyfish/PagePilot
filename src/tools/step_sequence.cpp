#include <algorithm>
#include <pagepilot/step_sequence.hpp>
#include <thread>
namespace pagepilot {
namespace {
constexpr std::size_t maximum_output = 64 * 1024 * 1024;
class StepDeadlineGuard {
public:
  StepDeadlineGuard(BrowserSession &browser, MsDuration allowance)
      : browser_(browser) {
    const auto until = browser.bounded_deadline(allowance);
    prior_ = browser_.exchange_deadline(until);
  }
  ~StepDeadlineGuard() { browser_.exchange_deadline(prior_); }

private:
  BrowserSession &browser_;
  BrowserSession::Deadline prior_;
};
bool truthy(const JsonDoc &value) {
  if (value.is_null())
    return false;
  if (value.is_boolean())
    return value.get<bool>();
  if (value.is_number())
    return value.get<double>() != 0;
  if (value.is_string())
    return !value.get_ref<const std::string &>().empty();
  return true; // JS arrays and objects are truthy, including empty ones.
}
std::string shorten(const std::string &text, std::size_t characters = 150) {
  std::size_t end = 0;
  while (end < text.size() && characters--) {
    const auto lead = static_cast<unsigned char>(text[end]);
    const auto width = lead < 0x80   ? 1U
                       : lead < 0xe0 ? 2U
                       : lead < 0xf0 ? 3U
                                     : 4U;
    if (end + width > text.size())
      break;
    end += width;
  }
  return text.substr(0, end);
}
std::string describe(const JsonDoc &value) {
  return value.is_string() ? value.get<std::string>() : value.dump();
}
std::optional<std::string> brief(const std::string &operation,
                                 const JsonDoc &value) {
  if (!value.is_object())
    return {};
  if (operation == "fill" || operation == "type")
    return value.contains("currentValue")
               ? shorten("value=" + value.at("currentValue").dump())
               : "ok";
  if (operation == "click") {
    std::string result =
        value.contains("elementText") ? value.at("elementText").dump() : "";
    if (value.contains("url"))
      result += (result.empty() ? "" : " → ") + describe(value.at("url"));
    return result.empty() ? "ok" : shorten(result);
  }
  if (operation == "navigate")
    return shorten(
        value.value("title", std::string()) + " (" +
            value.value("finalUrl", value.value("navigated", std::string())) +
            ")",
        120);
  if (operation == "get_page") {
    for (const auto *key : {"url", "title", "text"})
      if (value.contains(key))
        return shorten(describe(value.at(key)), 100);
    return shorten(value.dump(), 100);
  }
  if (operation == "get")
    return shorten(value.dump());
  if (operation == "eval")
    return shorten(describe(value.value("result", JsonDoc(nullptr))));
  if (operation == "find")
    return "found=" + value.value("found", JsonDoc(0)).dump() + "/" +
           value.value("total", JsonDoc(0)).dump();
  if (operation == "check") {
    for (const auto &[key, state] : value.items())
      if (state.is_boolean())
        return key + "=" + state.dump();
    return shorten(value.dump());
  }
  if (operation == "assert")
    return value.value("passed", false)
               ? "passed"
               : shorten("failed: " +
                             value.value("error",
                                         value.value("message", std::string())),
                         80);
  if (operation == "wait")
    return "waited " + value.value("waited", JsonDoc(0)).dump() + "ms";
  if (operation == "select")
    return value.contains("selected") ? shorten(describe(value.at("selected")))
                                      : "ok";
  if (operation == "status")
    return value.value("connected", false)
               ? "connected, " + value.value("tabCount", JsonDoc(0)).dump() +
                     " tabs"
               : "disconnected";
  if (operation == "list_tabs")
    return value.value("count", JsonDoc(0)).dump() + " tabs";
  if (operation == "switch_tab")
    return "tab " + value.value("switched", JsonDoc(nullptr)).dump();
  if (operation == "screenshot")
    return "captured";
  if (operation == "press_key" || operation == "hotkey")
    return shorten(value.value("pressed", std::string("ok")));
  return {};
}
JsonDoc label(const JsonDoc &row) {
  return row.is_object() && row.contains("tool") && row.at("tool").is_string()
             ? row.at("tool")
             : JsonDoc("?");
}
bool flag(const JsonDoc &row, const std::string &key) {
  return row.is_object() && row.contains(key) && row.at(key).is_boolean() &&
         row.at(key).get<bool>();
}
MsDuration duration(const JsonDoc &row, const std::string &key,
                    double fallback = 0) {
  return MsDuration(static_cast<std::int64_t>(row.value(key, fallback)));
}
} // namespace
bool StepSequence::supports(const std::string &operation) {
  return operation == "batch" || operation == "retry" ||
         operation == "run_steps";
}
bool StepSequence::expired() const {
  try {
    browser_.time_left(MsDuration(1));
    return false;
  } catch (const DeadlineReached &) {
    return true;
  }
}
void StepSequence::pause(MsDuration amount) {
  if (amount.count() < 0)
    throw BridgeError("Workflow delay must not be negative");
  if (browser_.time_left(amount) < amount)
    throw DeadlineReached(
        "Workflow deadline cannot accommodate the requested delay");
  const auto until = std::chrono::steady_clock::now() + amount;
  while (std::chrono::steady_clock::now() < until) {
    const auto left = std::chrono::duration_cast<MsDuration>(
        until - std::chrono::steady_clock::now());
    browser_.pump();
    interruptible_pause(
        std::min({left, MsDuration(10), browser_.time_left(MsDuration(10))}));
  }
}
void StepSequence::append(JsonDoc &rows, JsonDoc value) {
  const auto size = value.dump().size();
  if (size > maximum_output - output_bytes_)
    throw ActionLimitReached("Workflow result exceeds 64 MiB");
  output_bytes_ += size;
  rows.push_back(std::move(value));
}
JsonDoc StepSequence::row(const JsonDoc &input,
                          const std::string &operation) const {
  if (!input.is_object())
    throw BridgeError("Each action must be an object");
  for (const auto &definition : catalog_.definitions())
    if (definition.operation == operation)
      return normalize_arguments(
          input, definition.schema.at("properties")
                     .at(operation == "batch" ? "actions" : "steps")
                     .at("items"));
  throw BridgeError("Workflow row schema is unavailable");
}
JsonDoc StepSequence::child(const std::string &name, const JsonDoc &arguments,
                            std::optional<MsDuration> timeout) {
  const auto invocation = catalog_.resolve(name, arguments, allow_legacy_);
  if (timeout) {
    StepDeadlineGuard window(browser_, *timeout);
    return dispatch_(invocation);
  }
  return dispatch_(invocation);
}
JsonDoc StepSequence::run(const ToolInvocation &invocation) {
  JsonDoc result;
  if (invocation.operation == "batch")
    result = batch(invocation.arguments);
  else if (invocation.operation == "retry")
    result = retry(invocation.arguments);
  else if (invocation.operation == "run_steps")
    result = steps(invocation.arguments);
  else
    throw BridgeError("Unknown workflow operation");
  if (result.dump().size() > maximum_output)
    throw ActionLimitReached("Workflow result exceeds 64 MiB");
  return result;
}
JsonDoc StepSequence::batch(const JsonDoc &arguments) {
  JsonDoc rows = JsonDoc::array();
  bool timed_out = false;
  for (const auto &input : arguments.at("actions")) {
    if (expired()) {
      timed_out = true;
      break;
    }
    JsonDoc record = {{"tool", label(input)}, {"success", false}};
    try {
      const auto metadata = row(input, "batch");
      record["result"] =
          child(metadata.at("tool"), metadata.value("args", JsonDoc::object()));
      record["success"] = true;
    } catch (const ActionLimitReached &) {
      throw;
    } catch (const RequestAborted &) {
      throw;
    } catch (const std::exception &error) {
      record["error"] = error.what();
    }
    const bool success = record.at("success");
    append(rows, std::move(record));
    if (expired()) {
      timed_out = true;
      break;
    }
    if (!success && flag(input, "stopOnError"))
      break;
  }
  JsonDoc result = {{"executed", rows.size()}, {"results", std::move(rows)}};
  if (timed_out)
    result["timed_out"] = true;
  return result;
}
JsonDoc StepSequence::retry(const JsonDoc &arguments) {
  const auto name = arguments.at("tool").get<std::string>();
  const auto args = arguments.value("args", JsonDoc::object());
  // Validate even when the requested number of attempts is zero.
  catalog_.resolve(name, args, allow_legacy_);
  const auto limit = arguments.value("max_retries", 3);
  const auto key = arguments.value("success_check", std::string());
  JsonDoc response = {{"success", false}, {"attempts", 0}};
  for (int attempt = 0; attempt < limit; ++attempt) {
    if (expired()) {
      response["timed_out"] = true;
      break;
    }
    response["attempts"] = attempt + 1;
    try {
      auto result = child(name, args);
      response.erase("error");
      if (key.empty() || (result.is_object() && result.contains(key) &&
                          truthy(result.at(key))))
        return {{"success", true},
                {"attempts", attempt + 1},
                {"result", std::move(result)}};
    } catch (const ActionLimitReached &) {
      throw;
    } catch (const RequestAborted &) {
      throw;
    } catch (const std::exception &error) {
      response["error"] = error.what();
    }
    if (expired()) {
      response["timed_out"] = true;
      break;
    }
    if (attempt + 1 < limit) {
      try {
        pause(duration(arguments, "delay_ms", 1000));
      } catch (const DeadlineReached &error) {
        response["timed_out"] = true;
        response["error"] = error.what();
        break;
      }
    }
  }
  return response;
}
JsonDoc StepSequence::steps(const JsonDoc &arguments) {
  const auto &input_rows = arguments.at("steps");
  if (input_rows.empty() || input_rows.size() > 50)
    throw BridgeError("steps must contain between 1 and 50 operations");
  const auto started = std::chrono::steady_clock::now();
  const bool stop = arguments.value("stop_on_error", true),
             intermediate = arguments.value("return_intermediate", false);
  const int attempts = arguments.value("retry_on_fail", true)
                           ? 1 + arguments.value("max_step_retries", 2)
                           : 1;
  JsonDoc rows = JsonDoc::array(), last = nullptr;
  bool timed_out = false;
  for (std::size_t index = 0; index < input_rows.size(); ++index) {
    if (expired()) {
      timed_out = true;
      break;
    }
    const auto &input = input_rows[index];
    JsonDoc record = {
        {"step", index}, {"tool", label(input)}, {"success", false}};
    JsonDoc metadata;
    ToolInvocation inspected;
    try {
      metadata = row(input, "run_steps");
      inspected = catalog_.resolve(metadata.at("tool"),
                                   metadata.value("args", JsonDoc::object()),
                                   allow_legacy_);
      if (supports(inspected.operation))
        throw BridgeError(metadata.at("tool").get<std::string>() +
                          " is not allowed within run_steps");
    } catch (const RequestAborted &) {
      throw;
    } catch (const std::exception &error) {
      record["error"] = error.what();
      append(rows, std::move(record));
      if (stop && !flag(input, "optional"))
        break;
      continue;
    }
    try {
      pause(std::min(duration(metadata, "wait_before"), MsDuration(5000)));
    } catch (const DeadlineReached &error) {
      record["attempts"] = 0;
      record["error"] = error.what();
      append(rows, std::move(record));
      timed_out = true;
      break;
    }
    const auto per_attempt = duration(metadata, "timeout").count() > 0
                                 ? duration(metadata, "timeout")
                                 : duration(arguments, "step_timeout", 10000);
    JsonDoc value;
    for (int attempt = 0; attempt < attempts; ++attempt) {
      record["attempts"] = attempt + 1;
      try {
        value = child(metadata.at("tool"),
                      metadata.value("args", JsonDoc::object()), per_attempt);
        record["success"] = true;
        record.erase("error");
        break;
      } catch (const ActionLimitReached &) {
        throw;
      } catch (const RequestAborted &) {
        throw;
      } catch (const std::exception &error) {
        record["error"] = error.what();
      }
      if (expired()) {
        timed_out = true;
        break;
      }
      if (attempt + 1 < attempts && arguments.value("auto_wait", true)) {
        try {
          pause(MsDuration(500));
        } catch (const DeadlineReached &error) {
          record["error"] = error.what();
          timed_out = true;
          break;
        }
      }
    }
    const bool success = record.at("success");
    if (success) {
      last = value;
      if (const auto summary = brief(inspected.operation, value))
        record["brief"] = *summary;
      if (intermediate)
        record["result"] = std::move(value);
    }
    append(rows, std::move(record));
    if (timed_out || expired()) {
      timed_out = true;
      break;
    }
    if (!success && stop && !metadata.value("optional", false))
      break;
    try {
      pause(std::min(duration(metadata, "wait_after"), MsDuration(5000)));
    } catch (const DeadlineReached &) {
      timed_out = true;
      break;
    }
  }
  const auto succeeded =
      std::count_if(rows.begin(), rows.end(), [](const auto &record) {
        return record.at("success") == true;
      });
  JsonDoc result = {
      {"total_steps", input_rows.size()},
      {"executed", rows.size()},
      {"succeeded", succeeded},
      {"failed", rows.size() - static_cast<std::size_t>(succeeded)},
      {"steps", std::move(rows)},
      {"last_result", std::move(last)}};
  if (timed_out)
    result["timed_out"] = true;
  if (browser_.connected() && !expired()) {
    try {
      const auto page = browser_.page_call(
          "Runtime.evaluate",
          {{"expression", "({url:location.href,title:document.title})"},
           {"returnByValue", true}},
          MsDuration(1000));
      if (page.at("result").contains("value"))
        result["page"] = page.at("result").at("value");
    } catch (const RequestAborted &) {
      throw;
    } catch (const std::exception &) {
    }
  }
  result["total_time_ms"] = std::chrono::duration_cast<MsDuration>(
                                std::chrono::steady_clock::now() - started)
                                .count();
  return result;
}
} // namespace pagepilot
