Push-to-Talk Helper

This is a small utility I cobbled together using sample source code from various projects, for personal use only. This is NOT commercial software.

It toggles mute on a specified Pipewire source (Microphone) as with a conventional "push to talk" button, without caring what display protocols or clients are involved.
Sound cues via SDL2 (SDL_mixer) are supported, the examples here mimic those used in popular VoIP program Discord, and are not my work.

Configuration is at compile time with static definitions, I have not yet chosen or implemented a config parser, but friendlier configuration is planned.

Keyboard support is not yet implemented, but is also planned.

Deps:
- Cmake
- Cmake Extra Modules
- Udev
- Libinput
- Pipewire
- SDL2
- SDL_mixer
- Rohrkabel (Modern C++ wrapper for Pipewire)
