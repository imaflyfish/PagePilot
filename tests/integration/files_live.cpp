#include <pagepilot/tool_runtime.hpp>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <thread>
#include <unistd.h>
using namespace pagepilot;
namespace fs = std::filesystem;
unsigned passed = 0, failed = 0;
std::string step;
void check(bool condition, const std::string &name) {
  if (condition)
    ++passed;
  else {
    ++failed;
    std::cerr << "FAIL: " << name << '\n';
  }
}
template <class F> std::string rejects(F action, const char *name) {
  try {
    action();
    check(false, name);
    return "action unexpectedly succeeded";
  } catch (const std::exception &error) {
    check(true, name);
    return error.what();
  }
}
void write(const fs::path &path, const std::string &value) {
  std::ofstream stream(path, std::ios::binary);
  stream.write(value.data(), static_cast<std::streamsize>(value.size()));
  if (!stream)
    throw BridgeError("Fixture write failed");
}
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  auto pattern =
      (fs::temp_directory_path() / "pagepilot-files-XXXXXX").string();
  if (!::mkdtemp(pattern.data()))
    return 2;
  const auto base = fs::canonical(pattern), allowed = base / "allowed",
             outside = base / "outside";
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code error;
      fs::remove_all(path, error);
    }
  } cleanup{base};
  try {
    const auto *site = std::getenv("PAGEPILOT_FIXTURE_URL"),
               *artifact = std::getenv("PAGEPILOT_CHILD_EVIDENCE");
    if (!site || !artifact)
      throw BridgeError("Missing fixture environment");
    const fs::path evidence = fs::absolute(artifact);
    fs::create_directories(evidence);
    fs::create_directory(allowed);
    fs::create_directory(outside);
    const auto first = allowed / "资料😀.txt", second = allowed / "bytes.bin";
    write(first, "Browser file 中文😀\n");
    write(second, std::string("\0\1\xff"
                              "A",
                              4));
    write(outside / "sentinel.txt", "untouched");
    ToolRuntime runtime(static_cast<unsigned>(std::stoul(argv[1])),
                          {allowed, evidence});
    ToolCatalog catalog;
    auto call = [&](const std::string &name, JsonDoc arguments = JsonDoc::object()) {
      step = name + " " + arguments.dump();
      return runtime.invoke(catalog.resolve(name, arguments, true));
    };
    auto eval = [&](const std::string &expression) {
      return call("page_evaluate", {{"script", expression}}).at("result");
    };
    call("tab_create");
    call("page_navigate", {{"url", std::string(site) + "/page.html"}});
    eval("window.fileProof=[];document.querySelector('#files')."
         "addEventListener('change',e=>fileProof.push({trusted:e.isTrusted,"
         "count:e.target.files.length}));true");
    check(
        call("form_upload", {{"selector", "#files"}, {"files", first.string()}})
                .at("uploaded") == 1,
        "single string upload accepted");
    check(eval("document.querySelector('#files').files[0].name") ==
              "资料😀.txt",
          "native upload preserves Unicode basename");
    check(eval("document.querySelector('#files').files[0].text()") ==
              "Browser file 中文😀\n",
          "browser reads actual uploaded file bytes");
    check(eval("fileProof.at(-1).trusted") == true,
          "upload change event comes from browser");
    eval("window.sameFileProof=[];for(const kind of ['change','cancel'])"
         "document.querySelector('#files').addEventListener(kind,e=>"
         "sameFileProof.push({kind:e.type,trusted:e.isTrusted}));true");
    call("form_upload", {{"selector", "#files"}, {"files", first.string()}});
    check(eval("sameFileProof.length===1&&sameFileProof[0].trusted") == true,
          "reselecting the same regular file completes on one trusted event");
    call("upload_file",
         {{"selector", "#files"},
          {"files", JsonDoc::array({first.string(), second.string()})}});
    check(eval("document.querySelector('#files').files.length") == 2,
          "legacy upload selects multiple files");
    check(eval("document.querySelector('#files').files[1].arrayBuffer().then(b="
               ">[...new Uint8Array(b)])") == JsonDoc::array({0, 1, 255, 65}),
          "binary file bytes survive native selection");
    rejects(
        [&] {
          call("form_upload",
               {{"selector", "#files"},
                {"files", JsonDoc::array({first.string(),
                                       (outside / "sentinel.txt").string()})}});
        },
        "mixed authorized and outside batch rejected");
    check(eval("document.querySelector('#files').files.length") == 2,
          "batch validation failure preserves previous selection");
    JsonDoc too_many = JsonDoc::array();
    for (int index = 0; index < 129; ++index)
      too_many.push_back(first.string());
    rejects(
        [&] {
          call("form_upload", {{"selector", "#files"}, {"files", too_many}});
        },
        "upload batch count bounded before browser mutation");
    rejects(
        [&] {
          call("form_upload", {{"selector", "#files"},
                               {"files", (allowed / "missing").string()}});
        },
        "missing file rejected before browser update");
    fs::create_symlink(outside / "sentinel.txt", allowed / "escape");
    rejects(
        [&] {
          call("form_upload", {{"selector", "#files"},
                               {"files", (allowed / "escape").string()}});
        },
        "upload outside symlink refused");
    fs::create_symlink(first, allowed / "inside");
    call("form_upload",
         {{"selector", "#files"}, {"files", (allowed / "inside").string()}});
    check(eval("document.querySelector('#files').files[0].text()") ==
              "Browser file 中文😀\n",
          "allowed symlink uses canonical source file");
    rejects(
        [&] {
          call("form_upload",
               {{"selector", "#person"}, {"files", first.string()}});
        },
        "non-file input rejected");
    eval("document.body.insertAdjacentHTML('beforeend','<input id=single "
         "type=file>');true");
    rejects(
        [&] {
          call("form_upload",
               {{"selector", "#single"},
                {"files", JsonDoc::array({first.string(), second.string()})}});
        },
        "multiple files refused by single-file control");
    eval("document.querySelector('#files').style.display='none';true");
    call("form_upload", {{"selector", "#files"}, {"files", second.string()}});
    check(eval("document.querySelector('#files').files[0].size") == 4,
          "hidden file input receives native selection");
    check(
        call("form_upload", {{"selector", "#files"}, {"files", JsonDoc::array()}})
                .at("uploaded") == 0,
        "empty file array clears native selection");
    check(eval("document.querySelector('#files').files.length") == 0,
          "file clearing visible in browser");
    fs::create_directories(allowed / "folder/nested");
    write(allowed / "folder/a.txt", "alpha");
    write(allowed / "folder/nested/b.txt", "beta");
    eval("document.body.insertAdjacentHTML('beforeend','<input id=directory "
         "type=file webkitdirectory>');true");
    call("form_upload", {{"selector", "#directory"},
                         {"files", (allowed / "folder").string()}});
    check(eval("[...document.querySelector('#directory').files].map(f=>f."
               "webkitRelativePath).sort()") ==
              JsonDoc::array({"folder/a.txt", "folder/nested/b.txt"}),
          "directory upload preserves nested relative paths");
    check(eval("Promise.all([...document.querySelector('#directory').files]."
               "map(f=>f.text())).then(values=>values.sort())") ==
              JsonDoc::array({"alpha", "beta"}),
          "browser reads actual directory member bytes");
    call("form_upload", {{"selector", "#directory"},
                         {"files", (allowed / "folder").string()}});
    check(eval("document.querySelector('#directory').files.length") == 2,
          "reselecting same directory completes");
    fs::create_symlink(outside / "sentinel.txt", allowed / "folder/escape");
    rejects(
        [&] {
          call("form_upload", {{"selector", "#directory"},
                               {"files", (allowed / "folder").string()}});
        },
        "directory upload rejects symlink members before browser selection");
    check(eval("document.querySelector('#directory').files.length") == 2,
          "rejected directory preserves earlier selection");
    // Directory enumeration completes after the native command acknowledges
    // the request. Both selections have identical names, sizes and counts.
    // A stale FileList must not satisfy the second selection's completion.
    const auto folder_a = allowed / "one/same", folder_b = allowed / "two/same";
    for (unsigned index = 0; index < 96; ++index) {
      const auto relative =
          fs::path(std::to_string(index)) / "nested/member.txt";
      fs::create_directories((folder_a / relative).parent_path());
      fs::create_directories((folder_b / relative).parent_path());
      write(folder_a / relative, "A:" + std::to_string(index));
      write(folder_b / relative, "B:" + std::to_string(index));
    }
    eval("window.directoryProof=[];for(const kind of ['change','cancel'])"
         "document.querySelector('#directory').addEventListener(kind,e=>"
         "directoryProof.push({kind:e.type,trusted:e.isTrusted,count:e.target."
         "files.length}));true");
    call("form_upload",
         {{"selector", "#directory"}, {"files", folder_a.string()}});
    check(eval("document.querySelector('#directory').files.length") == 96,
          "asynchronous directory selection waits for all nested members");
    check(eval("directoryProof.length===1&&directoryProof[0].trusted&&"
               "directoryProof[0].count===96") == true,
          "directory completion includes its one trusted selection event");
    const auto relative_paths =
        eval("[...document.querySelector('#directory').files].map(f=>f."
             "webkitRelativePath).sort()");
    call("form_upload",
         {{"selector", "#directory"}, {"files", folder_b.string()}});
    check(eval("[...document.querySelector('#directory').files].map(f=>f."
               "webkitRelativePath).sort()") == relative_paths,
          "replacement fixture really has the same relative names and count");
    check(eval("directoryProof.length===2&&directoryProof[1].trusted") == true,
          "same-count replacement waits for its own trusted event");
    check(eval("Promise.all([...document.querySelector('#directory').files]."
               "map(f=>f.text())).then(values=>values.length===96&&values."
               "every(v=>v.startsWith('B:')))") == true,
          "same-name same-size replacement exposes the new contents before "
          "return");
    call("form_upload",
         {{"selector", "#directory"}, {"files", folder_b.string()}});
    check(eval("directoryProof.length===3&&directoryProof[2].trusted") == true,
          "reselecting the same directory completes exactly one new selection");
    fs::create_directory(allowed / "empty-directory");
    call("form_upload", {{"selector", "#directory"},
                         {"files", (allowed / "empty-directory").string()}});
    check(eval("document.querySelector('#directory').files.length===0&&"
               "directoryProof.length===4") == true,
          "empty directory selection waits for its completion event");
    call("form_upload", {{"selector", "#directory"},
                         {"files", (allowed / "empty-directory").string()}});
    check(eval("directoryProof.length===5") == true,
          "reselecting an empty directory completes without waiting for files");
    // Block receipt delivery while preserving the native selection itself.
    // Deadlines and cancellation must not reissue the selecting command.
    eval("window.blockedSelection=[];window.blockFileReceipt=e=>{if(e.target."
         "id==='directory'&&e.isTrusted){blockedSelection.push(e.type);e."
         "stopImmediatePropagation()}};"
         "for(const kind of "
         "['change','cancel'])window.addEventListener(kind,blockFileReceipt,"
         "true);true");
    // This deadline covers filesystem validation, lookup and asynchronous
    // enumeration as well as the intentionally missing acknowledgement.
    // Give a shared CI worker time to reach the post-selection failure stage.
    call("browser_configure", {{"long_timeout", 3000}});
    const auto receipt_started = std::chrono::steady_clock::now();
    const auto receipt_failure = rejects(
        [&] {
          call("form_upload",
               {{"selector", "#directory"}, {"files", folder_a.string()}});
        },
        "missing native selection receipt reaches its existing deadline");
    const auto receipt_elapsed =
        std::chrono::duration_cast<MsDuration>(
            std::chrono::steady_clock::now() - receipt_started)
            .count();
    call("browser_configure", {{"long_timeout", 30000}});
    auto receipt_state =
        eval("({events:blockedSelection.slice(),count:document.querySelector('#"
             "directory')?.files.length??null})");
    receipt_state["error"] = receipt_failure;
    receipt_state["elapsed_ms"] = receipt_elapsed;
    check(receipt_failure == "Target did not acknowledge the input event "
                             "before the action deadline" &&
              receipt_state.at("events").size() == 1 &&
              receipt_state.at("count") == 96,
          "deadline failure must follow exactly one completed selection "
          "without replay; state=" +
              receipt_state.dump());
    check(eval("Object.getOwnPropertyNames(window).filter(k=>k.startsWith('__"
               "pagepilot_receipt_')).length") == 0,
          "timed-out upload removes its receipt binding");
    eval("blockedSelection=[];true");
    CdpChannel upload_observer(static_cast<unsigned>(std::stoul(argv[1])));
    const auto upload_session =
        upload_observer
            .call("Target.attachToTarget",
                  {{"targetId", runtime.browser().current_target()},
                   {"flatten", true}})
            .at("sessionId")
            .get<std::string>();
    std::stop_source stop_upload;
    std::exception_ptr stopping_error;
    std::jthread stopping([&] {
      try {
        const auto until =
            std::chrono::steady_clock::now() + MsDuration(3000);
        while (upload_observer
                   .call("Runtime.evaluate",
                         {{"expression", "blockedSelection.length===1"},
                          {"returnByValue", true}},
                         upload_session)
                   .at("result")
                   .at("value") != true) {
          if (std::chrono::steady_clock::now() >= until)
            throw BridgeError("upload observer did not see native selection");
          std::this_thread::sleep_for(MsDuration(5));
        }
        stop_upload.request_stop();
      } catch (...) {
        stopping_error = std::current_exception();
      }
    });
    bool upload_cancelled = false;
    {
      CancelScope scope(stop_upload.get_token());
      try {
        call("form_upload",
             {{"selector", "#directory"}, {"files", folder_b.string()}});
      } catch (const RequestAborted &) {
        upload_cancelled = true;
      }
    }
    stopping.join();
    if (stopping_error)
      std::rethrow_exception(stopping_error);
    check(upload_cancelled,
          "cancellation interrupts pending selection acknowledgement");
    check(eval("blockedSelection.length===1&&document.querySelector('#"
               "directory').files.length===96") == true,
          "cancelled upload never repeats native selection");
    check(eval("Object.getOwnPropertyNames(window).filter(k=>k.startsWith('__"
               "pagepilot_receipt_')).length") == 0,
          "cancelled upload removes its receipt binding");
    eval("for(const kind of "
         "['change','cancel'])window.removeEventListener(kind,blockFileReceipt,"
         "true);true");
    call("form_upload",
         {{"selector", "#directory"}, {"files", folder_a.string()}});
    check(
        eval(
            "Promise.all([...document.querySelector('#directory').files].map(f="
            ">f.text())).then(values=>values.every(v=>v.startsWith('A:')))") ==
            true,
        "a subsequent upload works after acknowledgement cancellation");
    eval("window.detachedSelection=0;window.removeFileInput=e=>{if(e.target.id="
         "=='directory'&&e.isTrusted){detachedSelection++;e.target.remove();e."
         "stopImmediatePropagation()}};"
         "window.addEventListener('change',removeFileInput,true);true");
    call("browser_configure", {{"long_timeout", 3000}});
    const auto detached_started = std::chrono::steady_clock::now();
    const auto detached_failure = rejects(
        [&] {
          call("form_upload",
               {{"selector", "#directory"}, {"files", folder_b.string()}});
        },
        "detached selection target fails instead of being reacquired");
    const auto detached_elapsed =
        std::chrono::duration_cast<MsDuration>(
            std::chrono::steady_clock::now() - detached_started)
            .count();
    call("browser_configure", {{"long_timeout", 30000}});
    auto detached_state = eval("({events:detachedSelection,removed:document."
                               "querySelector('#directory')===null})");
    detached_state["error"] = detached_failure;
    detached_state["elapsed_ms"] = detached_elapsed;
    check(detached_state.at("events") == 1 &&
              detached_state.at("removed") == true,
          "target detachment must occur once without input recreation; state=" +
              detached_state.dump());
    eval("window.removeEventListener('change',removeFileInput,true);true");
    call("page_navigate", {{"url", std::string(site) + "/capture.html"}});
    runtime.browser().page_call("Emulation.setDeviceMetricsOverride",
                                {{"width", 800},
                                 {"height", 600},
                                 {"deviceScaleFactor", 1},
                                 {"mobile", false}});
    auto capture_state = [&] {
      return JsonDoc{
          {"layout", runtime.browser().page_call("Page.getLayoutMetrics")},
          {"page", eval("({innerWidth,innerHeight,devicePixelRatio,"
                        "clientWidth:document.documentElement.clientWidth,"
                        "scrollWidth:document.documentElement.scrollWidth,"
                        "scrollbarWidth:getComputedStyle(document."
                        "documentElement).scrollbarWidth,visualViewport:"
                        "{width:visualViewport.width,height:visualViewport."
                        "height,scale:visualViewport.scale}})")}};
    };
    write(evidence / "capture-initial-layout.json", capture_state().dump(2));
    auto save = [&](const std::string &name, const JsonDoc &result) {
      if (result.contains("screenshot")) {
        auto image = decode_png(result.at("screenshot"));
        PathGuard({evidence})
            .output((evidence / name).string())
            .commit(image.bytes);
      }
      auto metadata = result;
      metadata.erase("screenshot");
      metadata["captureStateAfter"] = capture_state();
      write(evidence / (name + ".json"), metadata.dump(2));
    };
    const auto viewport = call("page_capture");
    save("viewport.png", viewport);
    check(viewport.at("width") == 800 && viewport.at("height") == 600,
          "viewport PNG has expected pixel dimensions; actual=" +
              viewport.at("width").dump() + "x" + viewport.at("height").dump());
    check(viewport.at("screenshot").get<std::string>().size() > 1000,
          "inline screenshot contains complete Base64 rather than a preview");
    const auto full = call("page_capture", {{"fullPage", true}});
    save("full.png", full);
    check(full.at("width") == 800 && full.at("height") == 1600,
          "full-page PNG includes entire document height; actual=" +
              full.at("width").dump() + "x" + full.at("height").dump());
    const auto element = call("page_capture", {{"selector", "#swatch"}});
    save("element.png", element);
    check(element.at("width") == 200 && element.at("height") == 120,
          "element capture crops exact visible box");
    eval("scrollTo(0,700);true");
    const auto scrolled = call("page_capture");
    save("scrolled.png", scrolled);
    check(scrolled.at("width") == 800 && scrolled.at("height") == 600,
          "scrolled viewport capture remains viewport sized");
    const auto far = call("page_capture", {{"selector", "#far"}});
    save("far.png", far);
    check(far.at("width") == 160 && far.at("height") == 90,
          "offscreen element scrolled and captured");
    const auto saved =
        call("screenshot", {{"selector", "#swatch"},
                            {"path", (evidence / "saved.png").string()}});
    save("saved.png", saved);
    check(saved.at("saved") == fs::canonical(evidence / "saved.png").string() &&
              !saved.contains("screenshot"),
          "legacy screenshot saves complete file and returns canonical "
          "destination");
    check(fs::file_size(evidence / "saved.png") == saved.at("size"),
          "saved file byte count matches result");
    rejects(
        [&] {
          call("page_capture", {{"path", (outside / "blocked.png").string()}});
        },
        "screenshot outside configured roots refused");
    check(!fs::exists(outside / "blocked.png"),
          "refused screenshot creates no outside file");
    call("browser_configure", {{"long_timeout", 150}});
    rejects(
        [&] {
          call("page_capture", {{"selector", "#hidden-never"},
                                {"path", (evidence / "failed.png").string()}});
        },
        "missing screenshot selector fails with bounded wait");
    call("browser_configure", {{"long_timeout", 30000}});
    check(!fs::exists(evidence / "failed.png"),
          "failed capture never leaves incomplete destination");
    write(allowed / "preserved.png", "original image placeholder");
    call("browser_configure", {{"long_timeout", 150}});
    rejects(
        [&] {
          call("page_capture",
               {{"selector", "#hidden-never"},
                {"path", (allowed / "preserved.png").string()}});
        },
        "failed capture does not commit over existing file");
    call("browser_configure", {{"long_timeout", 30000}});
    check(read_text(allowed / "preserved.png") == "original image placeholder",
          "existing destination survives capture failure");
    runtime.browser().page_call("Emulation.setDeviceMetricsOverride",
                                {{"width", 800},
                                 {"height", 600},
                                 {"deviceScaleFactor", 2},
                                 {"mobile", false}});
    eval("scrollTo(0,0);true");
    write(evidence / "capture-retina-layout.json", capture_state().dump(2));
    const auto retina = call("page_capture");
    save("retina.png", retina);
    check(retina.at("width") == 1600 && retina.at("height") == 1200,
          "device scale reflected in physical screenshot pixels");
    runtime.browser().page_call("Emulation.setDeviceMetricsOverride",
                                {{"width", 800},
                                 {"height", 600},
                                 {"deviceScaleFactor", 8},
                                 {"mobile", false}});
    rejects([&] { call("page_capture", {{"fullPage", true}}); },
            "oversized physical capture rejected before browser rasterization");
    runtime.browser().page_call("Emulation.clearDeviceMetricsOverride");
    call("page_navigate", {{"url", std::string(site) + "/frame-host.html"}});
    call("page_wait", {{"type", "function"},
                       {"expression", "readyFrames.length>=6"},
                       {"timeout", 5000}});
    call("frame_enter", {{"selector", "#crossFrame"}});
    eval("document.body.innerHTML='<input id=files type=file><div id=panel "
         "style=\"width:160px;height:90px;background:rgb(30,80,220)\"></"
         "div>';true");
    call("form_upload", {{"selector", "#files"}, {"files", first.string()}});
    check(eval("document.querySelector('#files').files[0].text()") ==
              "Browser file 中文😀\n",
          "native file selection works in OOP frame");
    const auto framed = call("page_capture", {{"selector", "#panel"}});
    save("oop-element.png", framed);
    check(framed.at("width").get<unsigned>() > 100 &&
              framed.at("height").get<unsigned>() > 60,
          "OOP element capture uses root projected bounds");
    call("frame_reset");
    check(eval("document.querySelector('#files')===null") == true,
          "frame upload does not create input in parent");
    call("tab_close");
    std::cout << passed << " live file/capture checks passed; " << failed
              << " failed\n";
    return failed ? 1 : 0;
  } catch (const std::exception &error) {
    std::cerr << step << "\n" << error.what() << '\n';
    return 1;
  }
}
