#include <pagepilot/session.hpp>
#include <functional>
#include <set>

namespace pagepilot {
JsonDoc BrowserSession::arm_dialog(const JsonDoc &arguments) {
  current_session();
  const auto action = arguments.value("action", std::string("accept"));
  JsonDoc rule = {{"accept", action == "accept"}};
  if (arguments.contains("text"))
    rule["promptText"] = arguments.at("text");
  dialog_rules_[current_] = std::move(rule);
  return {{"handler", action}};
}

JsonDoc BrowserSession::manage_cookies(const JsonDoc &arguments) {
  current_session();
  JsonDoc scope = context_parameters();
  const auto action = arguments.value("action", std::string("get"));
  if (action == "clear") {
    browser_call("Storage.clearCookies", scope);
    return {{"cleared", true}};
  }
  if (action == "set") {
    for (const auto *key : {"name", "value", "domain"})
      if (!arguments.contains(key) ||
          (std::string(key) != "value" && arguments.at(key) == ""))
        throw BridgeError("Cookie set requires name, value and domain");
    JsonDoc cookie = {{"name", arguments.at("name")},
                   {"value", arguments.at("value")},
                   {"domain", arguments.at("domain")},
                   {"path", arguments.value("path", std::string("/"))}};
    for (const auto *key : {"expires", "httpOnly", "secure"})
      if (arguments.contains(key))
        cookie[key] = arguments.at(key);
    scope["cookies"] = JsonDoc::array({cookie});
    browser_call("Storage.setCookies", scope);
    return {{"set", arguments.at("name")}};
  }
  if (action == "delete" &&
      (!arguments.contains("name") || arguments.at("name") == ""))
    throw BridgeError("Cookie delete requires a name");
  const auto response = browser_call("Storage.getCookies", scope);
  JsonDoc selected = JsonDoc::array();
  for (const auto &cookie : response.at("cookies")) {
    if (arguments.contains("name") && cookie.at("name") != arguments.at("name"))
      continue;
    if (action == "delete") {
      if ((arguments.contains("domain") &&
           cookie.at("domain") != arguments.at("domain")) ||
          (arguments.contains("path") &&
           cookie.at("path") != arguments.at("path")))
        continue;
      JsonDoc identity = {{"name", cookie.at("name")},
                       {"domain", cookie.at("domain")},
                       {"path", cookie.at("path")}};
      if (cookie.contains("partitionKey"))
        identity["partitionKey"] = cookie.at("partitionKey");
      page_call("Network.deleteCookies", identity);
    } else
      selected.push_back(cookie);
  }
  if (action == "delete")
    return {{"deleted", arguments.at("name")}};
  return {{"cookies", selected}};
}

JsonDoc BrowserSession::manage_storage(const JsonDoc &arguments) {
  // JSON.parse preserves arbitrary literal keys, including __proto__, without
  // object-literal prototype semantics. No argument becomes executable source.
  const auto literal = JsonDoc(arguments.dump()).dump();
  return evaluate(R"JS((a=>{
    const kind=a.type??'local', action=a.action??'get';
    const area=kind==='session'?sessionStorage:localStorage;
    if(action==='get') {
      if(Object.hasOwn(a,'key'))return {[a.key]:area.getItem(a.key)};
      const values=Object.create(null);
      for(let i=0;i<area.length;++i){const key=area.key(i);values[key]=area.getItem(key);}
      return {storage:values};
    }
    if(action==='clear'){area.clear();return {cleared:kind};}
    if(!Object.hasOwn(a,'key'))throw Error('Storage action requires a key');
    if(action==='set'){area.setItem(a.key,a.value??'');return {set:a.key};}
    if(action==='remove'){area.removeItem(a.key);return {removed:a.key};}
    throw Error('Unknown storage action');
  })(JSON.parse()JS" +
                  literal + "))");
}

JsonDoc BrowserSession::accessibility_snapshot() {
  current_session();
  prepare_frames();
  JsonDoc parameters = JsonDoc::object();
  if (!frames_.empty())
    parameters["frameId"] = frames_.back().frame;
  const auto response = context_call("Accessibility.getFullAXTree", parameters);
  const auto &nodes = response.at("nodes");
  if (nodes.size() > 10000)
    throw BridgeError("Accessibility snapshot exceeds 10000 nodes");
  std::map<std::string, const JsonDoc *> indexed;
  for (const auto &node : nodes)
    indexed.emplace(node.at("nodeId").get<std::string>(), &node);
  std::set<std::string> visited;
  std::function<JsonDoc(const std::string &, unsigned)> collect;
  collect = [&](const std::string &identity, unsigned depth) -> JsonDoc {
    if (depth > 128)
      throw BridgeError("Accessibility snapshot exceeds depth 128");
    if (!indexed.contains(identity) || !visited.insert(identity).second)
      return JsonDoc::array();
    const auto &node = *indexed.at(identity);
    const auto role =
        node.value("role", JsonDoc::object()).value("value", std::string());
    // Inline text repeats StaticText. Hidden subtrees are not user-facing.
    if (role == "InlineTextBox")
      return JsonDoc::array();
    for (const auto &reason : node.value("ignoredReasons", JsonDoc::array())) {
      const auto name = reason.value("name", std::string());
      if (name == "ariaHiddenElement" || name == "ariaHiddenSubtree" ||
          name == "notRendered" || name == "notVisible")
        return JsonDoc::array();
    }
    JsonDoc children = JsonDoc::array();
    for (const auto &child : node.value("childIds", JsonDoc::array())) {
      auto descendants = collect(child.get<std::string>(), depth + 1);
      for (auto &item : descendants)
        children.push_back(std::move(item));
    }
    if (node.value("ignored", false) || role == "generic" || role == "none")
      return children;
    JsonDoc item = {{"role", role}};
    for (const auto *field : {"name", "value", "description"})
      if (node.contains(field) && node.at(field).contains("value"))
        item[field] = node.at(field).at("value");
    JsonDoc states = JsonDoc::object();
    for (const auto &property : node.value("properties", JsonDoc::array())) {
      const auto &value = property.at("value");
      if (value.contains("value") && value.at("value").is_primitive())
        states[property.at("name").get<std::string>()] = value.at("value");
    }
    if (!states.empty())
      item["states"] = std::move(states);
    if (!children.empty())
      item["children"] = std::move(children);
    return JsonDoc::array({std::move(item)});
  };
  JsonDoc tree = JsonDoc::array();
  for (const auto &node : nodes)
    if (!node.contains("parentId") ||
        !indexed.contains(node.at("parentId").get<std::string>())) {
      auto roots = collect(node.at("nodeId").get<std::string>(), 0);
      for (auto &root : roots)
        tree.push_back(std::move(root));
    }
  std::string yaml;
  std::function<void(const JsonDoc &, unsigned)> render;
  render = [&](const JsonDoc &items, unsigned depth) {
    const std::string indent(depth * 2, ' ');
    for (const auto &item : items) {
      yaml += indent + "- role: " + item.at("role").dump() + "\n";
      for (const auto *key : {"name", "value", "description", "states"})
        if (item.contains(key))
          yaml += indent + "  " + key + ": " + item.at(key).dump() + "\n";
      if (yaml.size() > 4 * 1024 * 1024)
        throw BridgeError("Accessibility snapshot exceeds 4 MiB");
      if (item.contains("children")) {
        yaml += indent + "  children:\n";
        render(item.at("children"), depth + 2);
      }
    }
  };
  render(tree, 0);
  return {{"snapshot", yaml.empty() ? "[]\n" : yaml},
          {"format", "ax-yaml"},
          {"tree", tree}};
}
} // namespace pagepilot
