# Bouncemasters — Nintendo Switch port (Unity 6000.0.58f2 / IL2CPP wrapper)
 
This is a native wrapper / loader that runs the original ARM64 Android build of
Bouncemasters on Switch homebrew. It contains **no game code and no game
assets** — it loads the game's own libraries and recreates, natively, the thin
Android/JNI layer the Unity engine expects.
 
## Install & run
 
You need files from Bouncemasters Epic Games version 1.0.0 (`com.joybits.bouncemasters`).
 
Put the `.nro` in any folder under `sdmc:/switch/` and place your game files next
to it — the loader finds its folder at runtime, so the name is up to you:
 
```
sdmc:/switch/bouncemasters_nx
├── bouncemasters_nx.nro
├── libmain.so  libunity.so  libil2cpp.so   <- from your APK: lib/arm64-v8a/
├── cursor.png                              <- optional
└── assets/                                 <- the APK's assets/ folder, whole
```
 
`
 Launch via **title override** (hold R while starting an installed game) or a
forwarder. Applet mode will not work — the port needs the full application
memory pool.
 
Optionally drop a `cursor.png` (up to 64×64, transparency respected, top-left
pixel is the hotspot) in the same folder to replace the on-screen cursor with
your own.
 
## Controls
 
 | Input | Action |
|---|---|
| Touchscreen | Tap / drag (handheld) |
| `+` | Toggle the on-screen cursor |
| `–` | Toggle gyro pointing (tilt/turn the controller to aim) |
| Left stick | Move the cursor |
| `L` / `R` | Recenter the cursor to the middle of the screen (helps gyro aiming) |
| `A` / `ZR` / `ZL` | Tap / confirm (ZL and ZR let you play one-handed) |
| D-pad up / down | Adjust sensitivity of whatever is driving the cursor |
 
Your stick, mouse and gyro sensitivities are remembered in `pointer.cfg`
automatically after in-game adjustment.
 
## Languages
 
 `language.txt` is written next to the `.nro` on first launch, listing every
option with `auto` selected. Change the one word to pick a specific language,
including the seven that can't be selected with auto (Polish, Turkish, Vietnamese,
Thai, Hindi, Indonesian, Malay).
 
## Save data
 
Progress lives in `prefs.kv` beside the `.nro`, with `prefs.kv.bak` kept one save
behind. 

## Building
 
Requires devkitPro with the `switch-dev` group plus these portlibs:
 
```sh
pacman -S switch-dev
pacman -S switch-mesa switch-libdrm_nouveau switch-sdl2 switch-libpng switch-zlib
 
export DEVKITPRO=/opt/devkitpro
make                                    # -> bouncemasters_nx.nro
```

## Credits
 
The loader/shim infrastructure (`so_util`, `libc_shim`, `jni_fake`, `unity_jni`,
`opensles`, `nx_pointer`, diagnostics) derives from the open-source Switch
`.so`-loader lineage — Andy Nguyen, fgsfds and ChanseyIsTheBest, building on
TheOfficialFloW's Vita/Switch loader tradition — reaching this project via the
Zookeeper DX, PvZ Fusion and Killer Bean Unleashed ports, with the Clay Jam,
Hitman Sniper, CloverPit, Data Defense and Ticket to Earth ports as references
for Unity 6, FMOD audio and save handling. All MIT-licensed. Thanks to everyone
in that lineage for making this approach possible.
