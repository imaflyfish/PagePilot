#include <algorithm>
#include <functional>
#include <pagepilot/session.hpp>
#include <thread>
namespace pagepilot {
std::string BrowserSession::frame_session(const std::string &frame,
                                          const std::string &parent) {
  pump();
  if (documents_[parent].contains(frame))
    return parent;
  if (frame_sessions_.contains(frame)) {
    const auto cached = frame_sessions_.at(frame);
    if (documents_[cached].contains(frame))
      return cached;
  }
  const auto listing = send("Target.getTargets");
  for (const auto &target : listing.at("targetInfos")) {
    if (target.at("targetId") != frame ||
        target.value("type", std::string()) != "iframe")
      continue;
    if (frame_sessions_.contains(frame))
      return frame_sessions_.at(frame);
    std::string session;
    try {
      session = send("Target.attachToTarget",
                     {{"targetId", frame}, {"flatten", true}})
                    .at("sessionId")
                    .get<std::string>();
    } catch (const WireFailure &failure) {
      // Target discovery and renderer destruction are not atomic. The parent
      // document may receive the same frame immediately after this failed
      // attach.
      if (failure.code == -32602 &&
          failure.description == "No target with given id found")
        return parent;
      throw;
    }
    frame_sessions_[frame] = session;
    session_pages_[session] = session_pages_.at(parent);
    try {
      enable_session(session);
    } catch (...) {
      discard_session(session);
      throw;
    }
    return session;
  }
  return parent;
}
void BrowserSession::prepare_frames() {
  if (frames_.empty())
    return;
  const auto end =
      std::chrono::steady_clock::now() + time_left(MsDuration(10000));
  auto parent = sessions_.at(current_);
  for (auto &scope : frames_) {
    while (true) {
      scope.session = frame_session(scope.frame, parent);
      pump();
      auto &contexts = documents_[scope.session];
      if (contexts.contains(scope.frame)) {
        const auto &document = contexts.at(scope.frame);
        scope.context = document.at("id").get<std::int64_t>();
        scope.unique_context = document.at("uniqueId").get<std::string>();
        scope.owner_session = parent;
        break;
      }
      scope.context.reset();
      scope.unique_context.clear();
      if (std::chrono::steady_clock::now() + MsDuration(10) >= end)
        throw BridgeError("Selected frame is detached or has no live document; "
                          "leave the frame or wait for its navigation");
      interruptible_pause(MsDuration(10));
    }
    parent = scope.session;
  }
}
JsonDoc BrowserSession::enter_frame(const std::string &object,
                                    MsDuration timeout) {
  if (frames_.size() >= 32)
    throw BridgeError("Frame nesting exceeds 32 levels");
  const auto root = current_session();
  prepare_frames();
  const auto owner = frames_.empty() ? root : frames_.back().session;
  const auto node = send("DOM.describeNode",
                         {{"objectId", object}, {"depth", 0}}, owner, timeout)
                        .at("node");
  if (!node.contains("frameId") ||
      (node.at("localName") != "iframe" && node.at("localName") != "frame"))
    throw BridgeError("Selected element is not a frame owner");
  FrameSelection scope;
  scope.frame = node.at("frameId");
  scope.owner_node = node.at("backendNodeId");
  scope.owner_session = owner;
  frames_.push_back(std::move(scope));
  try {
    prepare_frames();
  } catch (...) {
    frames_.pop_back();
    throw;
  }
  return {{"depth", frames_.size()},
          {"frameId", frames_.back().frame},
          {"separateSession", frames_.back().session != owner}};
}
JsonDoc BrowserSession::list_frames() {
  const auto root = current_session();
  JsonDoc rows = JsonDoc::array();
  std::map<std::string, std::string> scope_sessions;
  std::function<void(const JsonDoc &, const std::string &, unsigned)> collect;
  collect = [&](const JsonDoc &tree, const std::string &session,
                unsigned depth) {
    if (depth > 32 || rows.size() > 4096)
      throw BridgeError("Frame inventory exceeds its structural limit");
    const auto &frame = tree.at("frame");
    const auto id = frame.at("id").get<std::string>();
    if (!scope_sessions.contains(id)) {
      scope_sessions[id] = session;
      rows.push_back({{"index", rows.size()},
                      {"id", id},
                      {"url", frame.at("url")},
                      {"name", frame.value("name", std::string())},
                      {"parent", frame.value("parentId", std::string())}});
    }
    for (const auto &child : tree.value("childFrames", JsonDoc::array()))
      collect(child, session, depth + 1);
  };
  collect(send("Page.getFrameTree", JsonDoc::object(), root).at("frameTree"),
          root, 0);
  const auto targets = send("Target.getTargets");
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &target : targets.at("targetInfos")) {
      if (target.value("type", std::string()) != "iframe")
        continue;
      const auto id = target.at("targetId").get<std::string>();
      const auto parent = target.value("parentFrameId",
                                       target.value("parentId", std::string()));
      if (scope_sessions.contains(id) || !scope_sessions.contains(parent))
        continue;
      const auto session = frame_session(id, scope_sessions.at(parent));
      auto tree =
          send("Page.getFrameTree", JsonDoc::object(), session).at("frameTree");
      if (tree.at("frame").at("id") != id)
        continue;
      tree["frame"]["parentId"] = parent;
      collect(tree, session, 0);
      changed = true;
    }
  }
  return {{"frames", rows}, {"current_depth", frames_.size()}};
}
} // namespace pagepilot
