# Installing Vinyl Tracer

English | [日本語](INSTALL_ja.md)

- [Windows (VST3)](#windows)
- [macOS (VST3 / AU)](#macos)

## Requirements

- Windows 10 / 11 (64-bit), or macOS 11 or later (Apple silicon / Intel)
- A DAW that supports VST3 or Audio Units (tested on Windows 11 + Ableton Live 12 and macOS 27 + Ableton Live 12)
- Serato CV02 / CV02.5 Control Vinyl and a turntable
- An audio interface that can send the turntable signal to your computer in stereo

## Windows

### Install

1. Extract the ZIP.
2. Copy the `Vinyl Tracer.vst3` folder inside it, **as a whole folder**, to the following location (allow administrator access if asked):

   ```
   C:\Program Files\Common Files\VST3\
   ```

3. Start your DAW and rescan plug-ins.
   - Ableton Live: open `Options` → `Settings` (`Preferences` in Live 11 and earlier) → `Plug-Ins`, turn on `Use VST3 Plug-In System Folders`, and press `Rescan`.
4. You're done when `Yamamoto Works` → `Vinyl Tracer` appears in the plug-in list.

### Update

Delete the old `Vinyl Tracer.vst3` in `C:\Program Files\Common Files\VST3\`, then copy in the new one. Settings saved in your Live Sets carry over.

### Uninstall

Delete the `C:\Program Files\Common Files\VST3\Vinyl Tracer.vst3` folder.

## macOS

The installer (`VinylTracer-<version>-macOS.pkg`) installs both VST3 and Audio Unit (AU). The installer and the plug-ins are notarized by Apple.

### Install

1. Double-click `VinylTracer-<version>-macOS.pkg` and follow the instructions (you'll be asked for an administrator password at the end).
   - The plug-ins go to the locations below. To install only VST3 or only AU, choose `Customize` during installation.

   | Format | Location |
   | --- | --- |
   | VST3 | `/Library/Audio/Plug-Ins/VST3/Vinyl Tracer.vst3` |
   | AU | `/Library/Audio/Plug-Ins/Components/Vinyl Tracer.component` |

2. Start your DAW and rescan plug-ins.
   - Ableton Live: open `Live` → `Settings` → `Plug-Ins`, turn on `Use VST3 Plug-In System Folders` for VST3 or `Use Audio Units` for AU, and press `Rescan`.
3. You're done when `Yamamoto Works` → `Vinyl Tracer` appears in the plug-in list.

Troubleshooting:

- **No input signal**: in `System Settings` → `Privacy & Security` → `Microphone`, allow your DAW (Ableton Live, etc.).
- **The AU doesn't show up**: log out and back in, or restart your Mac, then rescan.
- **Vinyl Tracer shows up twice**: a copy you placed by hand in `~/Library/Audio/Plug-Ins/` (the Library in your home folder) is still there. Delete it.

### Update

Run the installer for the new version; it overwrites the old one. Settings saved in your Live Sets carry over.

### Uninstall

In the Finder, choose `Go` → `Go to Folder…` (⇧⌘G) and delete these two items:

- `/Library/Audio/Plug-Ins/VST3/Vinyl Tracer.vst3`
- `/Library/Audio/Plug-Ins/Components/Vinyl Tracer.component`

## First-time setup (Ableton Live)

1. Create an audio track and set its input to the channel your turntable is connected to (stereo). Set Monitor to **In**.
2. Insert `Vinyl Tracer` on the track.
3. Press `Load Sample...`, or drag and drop an audio file onto the window, to load a sound.
4. Set `Timecode` to the side of the record you're using (Side A / Side B).
5. Play the record. You're ready when a circle appears in the scope on the left and the status below reads `SIGNAL ok / CODE ok / ABS ok`.
   - Adjust the input gain on your audio interface so the input level meter stays in the green.
6. A buffer size of 64–128 samples is recommended in Live. Latency is mostly set by the buffer size.

For details on each parameter and on cue points, see "Ableton Live での使い方" (Using it in Ableton Live) in [README.md](README.md) (currently in Japanese only).

## License

Vinyl Tracer is distributed under the GNU Affero General Public License v3.0 or any later version (`AGPL-3.0-or-later`). The full text is in [LICENSE.md](LICENSE.md), and the licenses for the embedded fonts are in the `fonts/` folder. The source code is available at https://github.com/yamamo2shun1/vinyl-tracer.
