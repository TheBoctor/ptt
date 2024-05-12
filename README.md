## Push-to-Talk Helper ##

Small utility I cobbled together using sample source code from various projects, for personal use only. This is NOT commercial software. This allows using push-to-talk to control sources with buttons, so long as you add your user to the `input` group. XTEST/xdo is not used, there is no requirement for XWayland or any specific display server, and it should work on any distro with up-to-date packages.

This will allow push-to-talk behavior even on things like the Discord web client or Vencord, no X11 required. Simply set the microphone to passive and set a relatively low activation threshold.

Sound cues for mute toggle via SDL2 (SDL_mixer) are supported, though their paths are currently hardcoded.

Roadmap:
- [ ] Config file/parser, standard XDG user .config dir
- [ ] Keyboard support
- [ ] Argument to show a friendly list of sources and write chosen one to config
- [ ] Argument to show a "push a button" prompt, show the result and confirm, and write it to config

Deps:
- Cmake
- Cmake Extra Modules
- Udev
- Libinput
- Pipewire
- SDL2
- SDL_mixer
- [Rohrkabel](https://github.com/Curve/rohrkabel) (Modern C++ wrapper for Pipewire)
