TONE3000 for Linux
==================

Install (recommended)
---------------------
  ./install.sh

This installs the VST3 to ~/.vst3, the LV2 to ~/.lv2, the CLAP to ~/.clap
and the standalone app to ~/.local/bin (with a desktop entry and icon so it
shows up in your applications menu), checking for required system libraries
first and offering to install any that are missing. Factory presets land in
~/.config/TONE3000/Presets/Factory.

  ./install.sh --check       check dependencies only, install nothing
  ./install.sh --uninstall   remove a previous install

Raspberry Pi kiosk
------------------
  ./install.sh --kiosk

Installs the standalone and its supervisor (tone3000-autostart) in
~/.local/bin, then makes TONE3000 start with the labwc desktop session
so the panel boots straight into the pedalboard UI. It also merges an
undecorated, maximized window rule and a touchscreen rule into
~/.config/labwc/rc.xml (starting from /etc/xdg/labwc/rc.xml when you
have no rc.xml of your own).

The launcher waits for the USB audio interface (MOTU M2 by default;
override with T3K_AUDIO_DEVICE, or set it empty to skip the wait) and
sets T3K_OSK_HEIGHT=220. Logs: ~/.local/state/tone3000/.

If your panel is not covered by the distro touch rules, pin it at
install time:

  T3K_TOUCH_DEVICE="wch.cn USB2IIC_CTP_CONTROL (USB 3-2)" \
  T3K_TOUCH_OUTPUT=HDMI-A-1 ./install.sh --kiosk

Log out and back in (or reboot) after installing. Run
./install.sh --uninstall to remove the launcher and the kiosk rules.

Manual install
--------------
Copy TONE3000.vst3 to ~/.vst3/, TONE3000.lv2 to ~/.lv2/, TONE3000.clap to
~/.clap/ and (optionally) the TONE3000 binary anywhere on your PATH. Install
only the formats your DAW uses; one is enough.

Runtime dependencies
--------------------
Required: libcurl (tone downloads), ALSA, FreeType. Present on almost
every desktop install; `./install.sh --check` verifies and offers to
install anything missing.

  Ubuntu / Debian:  sudo apt install libcurl4 libasound2 libfreetype6
  Fedora:           sudo dnf install libcurl alsa-lib freetype
  Arch:             sudo pacman -S curl alsa-lib freetype2
  openSUSE:         sudo zypper install libcurl4 alsa libfreetype6

Troubleshooting
---------------
Log file: ~/.config/TONE3000/TONE3000.log
Unresolved libraries: ldd ./TONE3000 | grep "not found"
