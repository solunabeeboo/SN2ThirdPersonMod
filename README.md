# SN2ThirdPersonMod



SN2ThirdPersonMod/

├── CMakeLists.txt                   ← root build (fetches ImGui, MinHook, wires UE4SS includes)

├── enabled.txt                      ← UE4SS mod enable marker

├── Scripts/

│   └── main.lua                     ← Lua polling / flag bridge

└── SN2ThirdPersonSettings/

&#x20;   ├── CMakeLists.txt               ← mod target build + deploy step

&#x20;   └── dllmain.cpp                  ← all C++ source







Source: MinHook (src/hook.c; the Freeze/Unfreeze functions).

When MinHook installs or removes a vtable hook, it must patch memory safely. The standard technique:



&#x20;   CreateToolhelp32Snapshot(TH32CS\_SNAPTHREAD) ; enumerate every thread in the process

&#x20;   SuspendThread each thread that isn't the current one

&#x20;   SetThreadContext on each suspended thread; if a thread's instruction pointer is sitting inside the bytes being patched, it gets nudged to a safe location

&#x20;   Patch the memory

&#x20;   ResumeThread everything



This is textbook safe-hooking and is documented in MinHook's own source. Every hooking library (Detours, PolyHook, MinHook) does some version of this. It only runs at hook install/uninstall time, not per-frame.

ShellExecuteW



Source: UE4SS.lib (the static library). The dllmain.cpp has zero calls to it. Search the source for ShellExecute; it isn't there. The static lib pulls it in transitively, likely for UE4SS's own logging/crash-handler path that opens a file browser or URL on fatal errors.

Summary

API	Source	My code calls it?	When

CreateToolhelp32Snapshot	MinHook	No; MinHook internal	Hook install only

SuspendThread	MinHook	No; MinHook internal	Hook install only

SetThreadContext	MinHook	No; MinHook internal	Hook install only

ShellExecuteW	UE4SS.lib (transitive)	No	Never from our code



If you want to verify the MinHook side yourself, open C:\\Users\\rafa\\Documents\\SN2ModDev\\build\\\_deps\\minhook-src\\src\\hook.c and search for Freeze; you'll see all three APIs used together in that function.

