# Validation scope

This directory describes how to verify a checkout of PagePilot. It publishes no
recorded run: counts and transcripts belong to the machine that ran them, so run
the suites yourself and read your own output.

## Toolchain probe

Configuration compiles **and links** a `std::stop_source` / `std::stop_token`
probe before the main build. Accepting `-std=c++20` is not sufficient: the
standard library and SDK must provide C++20 cancellation. If the probe fails,
upgrade the compiler together with its standard library/SDK and configure a fresh
build directory.

## Suites

```sh
cmake --preset debug   && cmake --build --preset debug   && ctest --preset debug --verbose
cmake --preset release && cmake --build --preset release && ctest --preset release --verbose
cmake --preset sanitize && cmake --build --preset sanitize && ctest --preset sanitize --verbose
```

Contract tests need no browser. The owned-browser and MCP programs drive a real
Chrome on loopback with a **separate throwaway profile**, and the socket-fault
programs drive a local fixture server. `tests/integration/run_suite.py` starts
Chrome, runs the programs and removes the temporary profile; a failing program
still fails the run even though the harness collects the remaining results while
the browser is alive.

Both supported MCP protocol versions, the 54 canonical tool names and the 75
compatibility names are exercised.

## Limits

Browser results depend on the Chrome build in use; version-specific layout and
scheduling differences are real and show up as timing or geometry failures rather
than as code defects. Live tests require a browser you own — never point them at
a profile holding real credentials. Linux and Windows execution are unverified;
Windows additionally needs a port of the POSIX file and stdin code.
