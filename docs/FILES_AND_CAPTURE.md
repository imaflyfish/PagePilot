# File selection and screenshots

`FileTools` performs uploads and captures through native Chrome sessions.
`PathGuard` owns path policy and filesystem handles; `CaptureSink` commits
complete output files; `decode_png` validates Base64 and PNG container integrity.
These modules are C++20. Pillow is used only by independent integration tests.

## File roots and output integrity

Default roots retain the old application's policy: the current account's home,
`/tmp`, `/var/tmp`, and the platform temporary directory, after canonicalization.
Repeated `--allow-root DIR` arguments **replace** those defaults. For example:

```sh
page-pilot --port 9222 --allow-root /path/to/uploads --allow-root /path/to/screenshots
```

Roots must already exist. File paths may be absolute or relative to the server's
working directory. Empty paths, null bytes, paths over 4096 bytes, dangling
symlinks, outside targets and prefix-sibling directories are refused. Existing
symlinks resolve to their real targets, which must remain inside an allowed root.
New output parents are created below the resolved allowed root.

Roots are pinned with directory descriptors. Each subsequent parent is opened
without following a replacement symlink. The visible parent is checked against
the pinned directory before output. Upload preparation holds file descriptors,
records device/inode/size/modification metadata and rechecks each path immediately
before the browser command. Directory uploads also validate every selected member
and refuse symlinks within the directory tree.

This is path policy and replacement detection, not an operating-system sandbox.
Chrome ultimately opens the canonical upload paths itself. A different local
process can still race the final check or modify a file after it is selected.
The tool preserves the baseline's direct-file lifetime: the original files must
remain available when the browser later reads/submits them. No temporary upload
copy is deleted on server disconnect.

Screenshot output is written to an exclusively created temporary file in the
destination directory, flushed, and renamed only after all bytes are written.
The final file uses mode 0600; newly created directories use 0700. Existing files
are preserved when capture fails. Temporary files are removed on write/commit
failure. This provides whole-file replacement, without claiming crash-durable
directory metadata or protection from every hostile local rename race.

## Upload semantics

`form_upload` (`upload_file` in compatibility mode) accepts one path, an array of
paths, or an empty array. A normal file input accepts regular files; a
`webkitdirectory` input accepts one directory. Hidden file inputs are supported.
Directory uploads retain Chrome's relative paths. An empty array clears the
selection. Multiple files require the input's `multiple` property. The entire
batch is validated before changing the browser selection.

Limits: 128 files and 1 GiB per batch; directory traversal also limits depth to
32 and visited entries to 4096. Oversized regular files are rejected from metadata
without reading their contents into the server. Native nonempty selection uses
`DOM.setFileInputFiles`, including OOP frame sessions, and produces trusted browser
events in the tested Chrome 152 and 153 builds. Chrome 153 did not clear an existing selection for
an empty CDP path array, so clearing explicitly sets the file input's empty value
and dispatches ordinary DOM input/change events. Those clearing events are
synthetic; the implementation does not claim otherwise.

The native command is issued once. Its reply can precede directory enumeration,
so a listener is installed on the original element before selection. Completion
requires that selection's trusted `change` or `cancel` event and an event-loop
turn after it, followed by a check of the resulting file count. Repeated selection
of the same files and empty directories are supported. A previous selection with
the same count or relative names cannot complete the new operation merely by
matching those values.

The observation retains its original session and action deadline. Missing event
delivery, cancellation or a detached target fails without reissuing the selection
or locating a replacement input. Listener, binding and remote-object cleanup is
bounded. Failure does not undo a selection Chrome already performed. Empty-array
clearing retains its separate synthetic-event path and does not wait for a
trusted native-selection event.

`uploaded` counts requested paths, matching the old handler; one directory thus
reports 1 even when it contains several selected files. This action selects
files in the input. It does not submit the site's form or claim a server upload.

## Screenshot semantics

`page_capture` (`screenshot` in compatibility mode) captures PNG. With no selector
it captures the selected tab's viewport or, for `fullPage: true`, the full page.
When a selector is supplied, that element is revealed in the selected document
and its bounds are projected through frame owners to the root page. Transformed
elements/frames produce an axis-aligned crop, including surrounding pixels in the
corners of a rotated box. Device scale is reflected in the PNG's physical size.
Viewport bounds use the browser's CSS content area, excluding any scrollbar.

Without `path`, `screenshot` contains the **complete** Base64 PNG. With a path,
`saved` contains the canonical committed destination. Both forms return byte
size, width, height and `mimeType: "image/png"`. No preview truncation is applied
to either MCP text content or structured content.

Capture bounds and decoded PNG dimensions are limited to 32768 per edge and
32 million pixels; physical size is checked before capture using page density.
The encoded/decoded image is bounded to 20 MiB of PNG bytes. Base64 framing,
padding, PNG signature, chunk extents/CRCs, first header and final ending are
validated. The C++ parser does not decode or prove semantic correctness of IDAT
pixels; test images are independently decoded with Pillow.

## Current evidence

`file_contract.cpp` exercises root/symlink/traversal rules, missing and special
files, pinned-root/parent/destination replacements, changed input metadata,
complete atomic writes, limits and corrupt image rejection. `files_live.cpp`
checks actual selected names and text/binary bytes, directory relative paths,
batch refusal without changing prior selection, OOP selection, clearing and
real screenshots. Its 96-member nested-directory regression also replaces a
selection with identical names and counts but different contents, repeats regular
files and empty directories, and blocks completion-event delivery to verify
deadline/cancellation cleanup without duplicate selection. Target detachment
does not recreate or reselect the input. `verify_pngs.py` decodes eight captures and checks known pixels,
including full-page, scrolled viewport, offscreen element, scale 2 and a rotated
OOP frame crop.
The owned capture fixture hides scrollbars with CSS while preserving scrolling,
so exact pixel expectations do not depend on the host's scrollbar preference.
The test evidence records layout, viewport and device-scale diagnostics.

`mcp_files.py` drives separate actual native processes using both supported MCP
versions and canonical/legacy names. Deterministic noisy canvas captures exceed
500,000 Base64 characters and are fully decoded with matching pixels and byte
counts. The modern text and structured results match exactly. Saved and inline
images match byte-for-byte. Explicit file roots also have real MCP refusal cases.

Linux/Windows file behavior and exhaustive concurrent mutation/animation
combinations are not validated.

Protocol reference: [official Chrome definitions](https://github.com/ChromeDevTools/devtools-protocol/blob/master/json/browser_protocol.json).
