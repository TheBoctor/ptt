## Push-to-Talk Helper ##

Small utility I cobbled together using sample source code from various projects, designed to suit my personal preferences and use case. This is NOT production software for the general public, but may be useful to others.

This utility allows using push-to-talk to control sources via libinput events, **so long as you add your user to the `input` group**. XTEST/xdo is not used, there is no requirement for XWayland or any specific display server, and it should work on any distro with up-to-date packages.

This will allow push-to-talk behavior even on things like the Discord web client or Vencord, no XTEST or XWayland shims required. Simply set your application's mic policy to passive/auto and set a relatively low activation threshold.

Sound cues for mute toggle via SDL2 (SDL_mixer) are supported, though their paths are currently hardcoded.

Roadmap:
- [x] Config file/parser, standard XDG user .config dir
- [x] Ability to list audio sources when the preferred one is unset or unavailable
- [x] Keyboard binding support
- [ ] Keyboard binding support for chords, modifiers, or non-character-producing keys
- [ ] Argument to show a "push a button" prompt, show the result and confirm, and write it to config

Deps:
- Cmake
- Cmake Extra Modules
- Udev
- Libinput
- Pipewire
- SDL2
- SDL_mixer
- [Rohrkabel](https://github.com/Curve/rohrkabel)
- [Libconfig](https://github.com/hyperrealm/libconfig)
