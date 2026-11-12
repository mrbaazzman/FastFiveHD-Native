# ANGLE (x64)

Local GLES2 backend used by the native Windows build.

- `libEGL.dll`, `libGLESv2.dll` are the x64 ANGLE binaries shipped with the installed
  Command Code/Chromium runtime on this machine.
- ANGLE version reported at runtime: `2.1.28027`, backend `Direct3D 11`
  (`GL_VERSION OpenGL ES 3.0 (ANGLE 2.1.28027 ...)`).
- `d3dcompiler_47.dll` is provided by Windows (`C:\Windows\System32`), so it is not vendored.

These files are a graphics driver/runtime dependency, not an emulator. The port creates a real
Win32 window and a real GLES2 context through them; all game logic and translation remain ours.
