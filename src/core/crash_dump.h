// DockForge - crash minidump writer (T-062).
//
// On any unhandled exception a minidump lands in the DumpsDir() directory
// (%LOCALAPPDATA%\DockForge\dumps) and the process then dies promptly
// (DoD: "dump 生成且不阻塞退出"; the T-211 budget is 2 seconds). Feasibility
// was measured first in spikes/t062_dump:
// MiniDumpWriteDump under MinGW g++ 15.2 links clean and took 62ms on a real
// 0xC0000005 with a second thread alive.
//
// Filter-chain contract: SetUnhandledExceptionFilter REPLACES whatever was
// installed before (there is no API to read it back), so InstallCrashDumper()
// must run AFTER core::InstallTaskbarCrashRecovery() (T-061) and forwards to
// the filter it replaced, keeping taskbar recovery alive. main.cpp wires them
// in that order.
//
// Inside the filter the process is one instruction from death: the heap, the
// logger and the CRT may all be mid-failure (T-061 learned this the hard way).
// Only raw Win32 runs there - CreateFileW / MiniDumpWriteDump / QPC / WriteFile.
// The dump timing and verdict are appended to logs\crash-dump.txt so the probe
// can prove the path ran without trusting the (possibly dead) logger.
#pragma once

#include <string>

namespace dockforge::core {

// Installs the minidump writer as the LAST unhandled-exception filter. Returns
// false when the dumps directory cannot be created (the filter is still
// installed - it will just skip the write - and false is reported, never
// silently swallowed).
bool InstallCrashDumper();

// The directory dumps are written to (paths.cpp DumpsDir(), kept as a separate
// seam so tests can point the module at a scratch directory).
void SetDumpsDirectoryForTest(const std::wstring& dir);

}  // namespace dockforge::core
