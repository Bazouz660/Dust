# Automatic crash reports

Dust enables crash recording from **DustBoot**, before its effect plugins and
GUI initialize. It works during startup and save loading, with file logging
enabled or disabled.

After a crash, press **Win+R** and open:

```text
%LOCALAPPDATA%\Dust\crash_reports
```

Share the newest `DustCrash_*.zip` with your bug report. You can also reach this
folder from **Report a Bug → Open Crash Reports** after launching the game again.
Nothing is uploaded automatically. The five newest completed archives are kept.

Each archive contains:

- `crash.dmp`: the faulting thread and exception context, thread stacks, loaded
  modules and memory-region information. This is not a full game-memory dump.
- `report.txt`: exception code/address, UTC time, process/thread IDs and the last
  startup/effect-initialization phase reached.
- Dust/effect settings, the latest Dust and DustBoot logs when available, and
  selected Kenshi logs, graphics settings and mod load order. Log tails are capped.

Dumps can contain memory fragments and file paths; review archives before sharing.
The faulting module alone does not prove which mod caused a crash. If archiving
fails, the raw `.dmp` and `.txt` remain in the same directory when writable.

To disable automatic recording, add this to Dust's `Dust.ini` and restart Kenshi:

```ini
[Diagnostics]
CrashDumps=0
```

## Implementation and limits

`DustCrashReporter.exe` starts as a hidden helper alongside DustBoot and exits
when the game exits. It uses inherited handles limited to the game process and
its reporting channel. No service, registry change or debugger attachment is used.

The game exception handler copies its context into memory prepared at startup,
signals the helper, and waits up to 30 seconds for the dump. The helper calls
`MiniDumpWriteDump` outside the unstable game process, following
[Microsoft's recommendation](https://learn.microsoft.com/en-us/windows/win32/api/minidumpapiset/nf-minidumpapiset-minidumpwritedump).
Dump writing finishes before the game's existing handler is allowed to continue;
ZIP packaging then proceeds independently.

DustBoot keeps its recorder ahead of later `SetUnhandledExceptionFilter`
registrations and preserves the downstream handler chain. Handled exceptions
do not generate reports. Shutdown noise is excluded after Dust signals teardown.

This covers ordinary unhandled Windows exceptions, including tested access
violations on main/worker threads and stack overflow. It cannot guarantee dumps
for forced process termination, fail-fast paths that bypass exception handlers,
power loss, hangs, or failures before DustBoot's entry point runs. A missing or
blocked helper disables reporting without preventing the game from starting.

For analysis, keep the matching binaries and PDBs from the failing build. Release
CI preserves PDBs in the separate `Dust-symbols-*` artifact; they are not shipped
in the game mod. Open `crash.dmp` in WinDbg or Visual Studio with those symbols.

## Validation

`python tests/run_native_tests.py CrashReportTests` crashes disposable test
processes, never Kenshi. It verifies the ZIP and minidump exception/thread/module
streams, the target PID, startup capture without effects, disabled logging,
normal exit, handled exceptions, worker faults, stack overflow, later handler
chaining, opt-out, missing helper, unwritable output and shutdown suppression.
