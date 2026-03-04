# custom-windows-task-manager
Custom CLI based windows task manager
Features:
 *   - Process list: PID, name, CPU%, RAM (Working Set), Virtual Mem, state
 *   - Per-process memory map: base address, size, type, state, protect flags
 *   - GPU usage via DXGI Performance Counters (works NVIDIA/AMD/Intel)
 *   - Suspend / Resume a process (freezes ALL threads via NtSuspendProcess)
 *   - Timed resume: unfreeze after N seconds (background thread)
 *   - Kill with any Win32 exit code
 *   - Restart Process with new process ID
 *   - Detailed process info: owner, priority, handles, PEB address
 *
 * Commands:
 *   list                            refresh process list
 *   info   <pid>                    detailed process info + owner
 *   map    <pid>                    virtual memory map with hex addresses
 *   pause  <pid>                    suspend all threads (freeze)
 *   resume <pid>                    resume all threads
 *   resume <pid> <secs>             resume after N seconds
 *   kill   <pid>                    TerminateProcess (exit code 1)
 *   kill   <pid> <exitcode>         TerminateProcess with custom exit code
 *   priv                            Run in previlage mode --> access "SYSTEM" owned processes
 *   restart                         Restart a process
 *   gpu                             GPU / adapter usage via DXGI
 *   help                            show this list
 *   quit / exit                     exit
 */

Run the run.exe to test it
