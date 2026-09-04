# Darkest Dungeon full forensic capture

This package replaces the old passive session logger. A capture is called
`COMPLETE` only when it contains a finalized Microsoft Time Travel Debugging
trace plus the surrounding evidence needed to interpret it.

## Coverage contract

A complete capture contains:

- full instruction-level TTD recording for the `Darkest.exe` process tree;
- memory and control-flow history replayable in WinDbg;
- a native binary index of every bridged named-effect entry/exit, every
  per-target application/pass/cancel, symbolic primitive request, caller,
  stack, argument, and bounded raw-memory sample, regardless of Lua interest;
- a pre-run copy of the complete game/mod installation tree;
- pre-run and post-run copies of the Darkest Dungeon Steam save tree;
- copies and hashes of every loaded module observed during the run;
- WPR/ETW system telemetry;
- 250 ms process and thread samples plus two-second module samples;
- complete and session-delta game/DDLua logs;
- Windows application events and a SHA-256 artifact manifest.

The native `.bin` trace is copied to `native_diagnostics` and automatically
decoded into JSON Lines plus an event-count summary. A missing or malformed
native trace makes the entire capture incomplete.

If TTD, WPR, snapshots, trace finalization, or required artifacts fail, the
session is labeled `FAILED_INCOMPLETE`. It must never be described as a full
trace.

The scope is the target process tree and the local inputs/state it uses. No
recording technology can preserve facts that never entered that scope, such as
external physical events or data held only by an unavailable remote service.
TTD traces can also contain sensitive in-process data; keep captures local and
do not upload or share them casually.

## One-time setup

Run the following once:

```powershell
.\Accept-TtdEula.ps1
```

Microsoft TTD displays its license. The user must read and accept it personally.
The framework does not silently accept third-party license terms.

Then verify readiness. A non-elevated shell reports that recording will require
elevation; the actual recorder requests UAC automatically:

```powershell
.\Test-DDForensicCapture.ps1
```

Validate real TTD and WPR file creation once after setup:

```powershell
.\Test-TtdRecording.ps1
```

## Recording

Darkest Dungeon must be closed. Run:

```powershell
.\Start-DDForensicCapture.ps1
```

The script snapshots all inputs, starts WPR, launches the game under TTD, and
waits until the game exits. If necessary it first opens a UAC prompt and restarts
itself with the administrator token required by TTD/WPR. Full instruction tracing has substantial CPU,
storage, and frame-rate cost. Do not terminate the recorder or shut down Windows
before finalization finishes.

The result is written beneath `DDLuaFramework/forensic_captures/<timestamp>`.
Check `capture_status.json`; only `state: COMPLETE` satisfies this package's
coverage contract. The `.run` file can be opened in WinDbg for reverse execution
and retrospective queries.
