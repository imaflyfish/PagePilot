# Security reporting

For a sensitive report, use GitHub's [private vulnerability reporting](https://github.com/imaflyfish/PagePilot/security/advisories/new).
The private reporting channel is enabled for this repository. Ordinary correctness
bugs can be filed as public issues with private information removed. If the private
form is unavailable, open a minimal issue asking for a reporting channel without
including sensitive details.

Reports are handled on a best-effort basis; no response deadline or maintained
binary-release support window is promised. Identify the exact commit and compare
with the current default branch where practical.

PagePilot connects trusted MCP callers to a loopback Chrome DevTools endpoint.
Browser operations can act through the connected profile. Upload/screenshot roots
control those file operations; they do not sandbox page evaluation or a logged-in
browser session. The caller is responsible for Chrome's debugging-port exposure.

Reports about file-policy bypasses, unintended target/session changes, protocol
handling errors or browser content causing an unrequested operation are welcome.
Include the exact commit, OS/Chrome versions, redacted MCP arguments and a minimal
owned page that reproduces the behavior. Prefer a temporary browser profile.

Do not attach session cookies, tokens, personal browser profiles, private pages or
unredacted screenshots. The test suites drive owned pages and temporary profiles,
not arbitrary sites. See [getting started](../docs/GETTING_STARTED.md) and the
[validation scope](../validation/README.md).
