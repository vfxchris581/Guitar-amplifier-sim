Guitar Amplifier Sim for macOS (Apple Silicon and Intel, macOS 11 or later)

INSTALL - choose one:

  A) Double-click "Install Guitar Amplifier Sim.pkg" and follow the steps.
     Installs to /Library/Audio/Plug-Ins/VST3 (all users).

  B) Drag "Guitar Amplifier Sim.vst3" onto "VST3 Plug-Ins folder".
     (Or copy it to ~/Library/Audio/Plug-Ins/VST3 to install for your user only.)

Then open your DAW and rescan plug-ins.

IF MACOS BLOCKS IT
This build is not yet signed with an Apple Developer ID, so macOS may say it
"can't be opened" or is "from an unidentified developer":
  - Installer: right-click (Control-click) the .pkg, choose Open, then Open again.
    On recent macOS: System Settings > Privacy & Security > "Open Anyway".
  - Plug-in your DAW refuses to load: in Terminal run
      xattr -dr com.apple.quarantine "/Library/Audio/Plug-Ins/VST3/Guitar Amplifier Sim.vst3"

UNINSTALL
Delete "Guitar Amplifier Sim.vst3" from /Library/Audio/Plug-Ins/VST3
(and ~/Library/Audio/Plug-Ins/VST3 if you copied it there). Your models, captures,
IRs and presets in Documents/Guitar Amplifier Sim are not removed.
