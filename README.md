## Push-to-Talk Helper ##

Note: This is mostly an opinion, skip this soapbox if you just want the program details. I am considering rewriting this to use XDG Global Shortcut, but the portal's implementation status in major DEs is currently painful to use, the point the input group hack and corresponding security hole are still better. On GNOME I was unable to get global shortcuts to work at all, and on Plasma, held ones are unusable because releasing any key will clobber them. Between Xorg and Wayland, you're picking an unmaintainable security nightmare or a beautiful and high-performance accessibility nightmare.


Small utility I cobbled together using sample source code from various projects, designed to suit my personal preferences and use case. This is NOT production software for the general public, but may be useful to others.

This utility allows using push-to-talk to control sources via libinput events, **so long as the user belongs to the `input` group**. XTEST/xdo is not used, there is no requirement for XWayland nor any specific display server, DE, or compositor. Xlib is only used to resolve human-readable button names.

This will allow push-to-talk behavior even on things like the Discord web client or Vencord/Vesktop. Simply set a VoIP application's mic policy to passive/auto and set a relatively low activation threshold. **In Discord web clients, your microphone may be quieter and require extra amplification. The desktop version seems to have C++ modules to amplify audio input, but the web client lacks this.**

Roadmap:
- [x] Config file/parser, standard XDG user .config dir
- [x] Ability to list audio sources when the preferred one is unset or unavailable
- [x] Keyboard binding support
- [x] Ability to prefer keyboard/mouse/both even when a key name is defined in ptt.conf
- [ ] Keyboard binding support for more chords
- [ ] Argument to show a "push a button" prompt, show the result and confirm, and write it to config
- [ ] Fix hotplugs causing rohrkabel crashes

Deps:
- Cmake
- Udev (in `systemd-devel` on Red Hat distros)
- Libinput
- Xlib
- Xkbcommon
- Pipewire
- [Rohrkabel](https://github.com/Curve/rohrkabel)
- [Libconfig](https://github.com/hyperrealm/libconfig)
