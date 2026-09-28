# Native keyboard input

`keyboard_press` (`press_key`) accepts a key and optional modifier array;
`keyboard_chord` (`hotkey`) accepts a plus-separated chord. The native planner
resolves the complete chord before issuing input, presses each key in order,
and releases them in reverse order. A modifier array produces real modifier
events, not just flags on the final key. Modifiers include left/right Shift,
Control, Alt and Meta; `ControlOrMeta` selects Meta on macOS and Control on other
build hosts. `Ctrl` and `Command` are extra convenience aliases.

US physical key names and literal character names have distinct semantics:

| Input | Main key event |
|---|---|
| `a`, `A` | The specified character, without an implicit Shift event |
| `Shift+a` | Character `a` with Shift held |
| `Shift+KeyA` | Character `A` with Shift held |
| `Shift+Digit1` | Character `!` with Shift held |
| `Shift+1` | Character `1` with Shift held |
| `+`, `Control++` | Literal plus, optionally with Control held |
| `Numpad1`, `Shift+Numpad1` | Key `End` / `1`, keypad location 3, virtual key 35 |

The last row preserves the baseline's navigation virtual key. Actual Chrome on
macOS does not insert `1` for `Shift+Numpad1`, despite its key string. The old
server's editor effects were measured separately from its event traces. NumLock
state, IME composition and other keyboard layouts are not synthesized. Use
`element_type`/`element_fill` for arbitrary Unicode text; a chord's key names
must be recognized. Chords have a 1024-byte/16-key limit. Unknown keys or excess
length fail before any modifier is pressed.

The mapping supports 105 baseline physical names plus F13–F24. Actual Chrome
reports an empty `code` for AltGraph. Keypad release now retains location 3;
the baseline omitted the keypad flag on release and reported location 1. These
two function-key/keypad differences are intentional. Standard DOM/CDP names
remain standard; the owned implementation is `plan_keyboard` and
`BrowserSession::press_keyboard`.

macOS editing bindings accompany native events, so select-all, deletion and
cursor motion have real editor effects. The US layout and editing mapping data
were transformed from Playwright 1.50.0 and keep their Apache-2.0 copyright and
license. They are embedded data, with source hashes and transformation notices;
the executable has no Node/Playwright runtime dependency.

## Failure and cancellation

Each chord pins its original root CDP session, including when the selected
execution context is a child frame. Chrome routes native input to its focused
frame. No new page is attached midway through a chord. The action deadline
covers connection preparation, presses and releases.

A lost reply cannot prove that keyDown had no effect. The native executor tracks
potentially held keys before sending each down event. On failure/cancellation,
it attempts only their releases, in reverse order, on the original session.
Each cleanup call is bounded to 100 ms and ignores the cancelled action token;
at most 16 such calls can occur. Failure never causes a second keyDown. An
unconfirmed keyUp may be repeated during cleanup. A vanished page or broken
transport can prevent delivery; cleanup is bounded best effort, not rollback of
text or page shortcuts that already ran.

## Evidence and reproduction

The original server ran on an owned Chrome 153 profile with Playwright 1.50.0.
`tests/fixtures/keyboard-values.json` pins 30 observed key/chord traces. Native
real-browser tests compare trusted events including key/code/location/keyCode,
modifier flags and order; they exercise all 117 physical names, both Shift sides,
modifier arrays, plus literals, invalid-input atomicity and real macOS editing.
The real-browser suite has 169 checks. The separate owned socket peer has six
fault scenarios / 63 checks: missing down reply, cancellation triggered after
observing Shift down, down/up errors, lost session and timed-out cleanup. It
checks exact releases, original-session routing, late replies and fresh input.
Socket injection does not substitute for the separate real-browser event tests.

```sh
cmake --build --preset debug
python3 tests/integration/with_chrome.py --evidence build/keyboard-example build/debug/pilot-keyboard-tests
python3 tests/integration/keyboard_fixture.py --binary build/debug/pilot-keyboard-fault-tests --evidence build/keyboard-fault-example
```

Two host-dependent behaviors are worth knowing before reading a failure. A
keypad digit is a navigation key on some hosts rather than an inserted
character, so an editor test that expects the digit fails for reasons that have
nothing to do with this code. And a fault fixture that rejects Control down too
early never reaches an intended Shift fault, which reports the wrong scenario.
Both are fixture conditions, not product defects. How to run these suites is in
[validation/README.md](../validation/README.md).
