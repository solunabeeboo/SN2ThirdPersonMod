# SN2ThirdPersonMod



SN2ThirdPersonMod/

├── CMakeLists.txt                   ← root build (fetches ImGui, MinHook, wires UE4SS includes)

├── enabled.txt                      ← UE4SS mod enable marker

├── Scripts/

│   └── main.lua                     ← Lua polling / flag bridge

└── SN2ThirdPersonSettings/

&#x20;   ├── CMakeLists.txt               ← mod target build + deploy step

&#x20;   └── dllmain.cpp                  ← all C++ source

