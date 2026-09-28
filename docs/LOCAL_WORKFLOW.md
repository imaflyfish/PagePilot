# Try a local page through MCP

This example fills a field, clicks a counter once and reads the result. It uses
the bundled page, so you can inspect both its HTML and the browser effects.
Complete [build and MCP host setup](GETTING_STARTED.md) first.

From the source checkout, serve the fixtures on loopback in a separate terminal:

```sh
python3 -m http.server 8000 --bind 127.0.0.1 --directory tests/fixtures
```

In your configured MCP host, call `tab_create` with `{}` to select a new tab.
Then call `workflow_steps` with these arguments:

```json
{
  "retry_on_fail": false,
  "return_intermediate": true,
  "steps": [
    {"tool": "page_navigate", "args": {"url": "http://127.0.0.1:8000/page.html"}},
    {"tool": "element_fill", "args": {"selector": "#person", "text": "Local example"}},
    {"tool": "element_click", "args": {"selector": "#count-button"}},
    {"tool": "element_read", "args": {"selector": "#clicks", "type": "text"}}
  ]
}
```

The workflow should report `succeeded: 4`, `failed: 0`, and
`last_result.text: "1"`. The visible field should contain `Local example` and
the counter should show `1`. You can independently read the field with
`element_read` arguments `{"selector":"#person","type":"value"}` and inspect
the page with `page_snapshot`. Call `tab_close` with `{}` when finished, and
stop the fixture server with Ctrl-C. This closes the tab created for the example;
the Chrome process remains running.

Retries are disabled here because repeating a click would change the counter.
Workflows are sequential actions, not transactions: earlier effects remain if
a later step fails. See [WORKFLOWS.md](WORKFLOWS.md) for those semantics.

For a host-independent check of the same navigation/fill/click/read path, the
existing integration client sends real MCP messages to the installed executable:

```sh
python3 tests/integration/with_chrome.py \
  --binary "$PWD/dist/PagePilot-macos-arm64/bin/page-pilot" \
  --evidence build/local-mcp-example \
  tests/integration/mcp_live.py
```

This command starts its own temporary headless Chrome and local fixture server;
it does not use the manually started browser or require the terminal server
above. It reports 55 checks covering the basic path, both protocol versions,
compatibility names and frame operations. The evidence directory contains MCP
request results and the owned-browser cleanup receipt. To include the existing
53-check workflow suite, append `tests/integration/mcp_workflows.py` to the command.
These clients test the stdio protocol; they are not a claim of validation through
every MCP host application, personal browser profile or arbitrary website.
