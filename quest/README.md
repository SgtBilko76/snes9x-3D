# Snes9x 3D for Meta Quest

An OpenXR frontend that puts the SNES picture on a curved screen in front of
the player. This is step one of the VR roadmap; the stereo work builds on top
of it.

## Pico

Nothing here is Meta-specific by design. The app is a `NativeActivity` talking
OpenXR, so a Pico runs the same binary; what it needs is its own manifest
entries, which sit alongside Meta's because each vendor ignores the other's:

- `pvr.app.type` and `pvr.sdk.version` on the application
- `com.picovr.intent.category.VRAPP` on the launcher intent
- controller bindings suggested for
  `/interaction_profiles/bytedance/pico4_controller` as well as Touch; a
  runtime that does not know a profile rejects it, which is logged and
  otherwise harmless

**None of this has been tested on a Pico.** There is no Pico here to test with,
so the following are the parts most likely to need work, in the order they
would fail:

1. **The OpenXR loader.** This bundles the Khronos loader, which finds a
   runtime through the Android broker. PICO OS registers one on recent
   versions; on an older one the loader will not find a runtime and
   `xrCreateInstance` fails. The fix is to ship Pico's own
   `libopenxr_loader.so` instead. Symptom: `xrCreateInstance failed` in
   logcat and the app exits at once.
2. **The cylinder layer.** `XR_KHR_composition_layer_cylinder` may not be
   there. The app already falls back to a flat quad and says so in logcat, so
   this degrades rather than breaks.
3. **Refresh rate.** Pico 4 offers 72 and 90 Hz, so a 60 fps cartridge cannot
   get a whole multiple and will judder a little, the way PAL does on a Quest.
   `XR_FB_display_refresh_rate` is a Meta extension; without it the rate is
   left alone.
4. **Controller mapping.** If the Pico profile is accepted but the buttons sit
   wrongly, the `Controls...` page remaps them without a rebuild.

To check it, `adb logcat -s Snes9xVR:V` says which of these applied.

## Building

Needs the Android SDK with an NDK and build-tools installed. No Gradle and no
Android Studio project: the app is a `NativeActivity` with no Java at all, so
`aapt2` and `apksigner` are enough to package it.

```sh
./build.sh          # fetch dependencies, build, package, install
./build.sh build    # build and package only
./build.sh run      # install and launch
./build.sh release  # build a signed release APK
```

`release` signs with a key of its own at `~/.android/snes9xvr-release.keystore`,
created on first use. Android refuses to install an update signed by a
different key, so that file has to survive: losing it means every later version
needs the app uninstalled first, which takes the ROMs and saves with it.
Debug builds keep using the usual debug key, so moving between the two also
needs an uninstall.

`build.sh` downloads the Khronos OpenXR loader AAR on first run and caches the
headers and `libopenxr_loader.so` under `third_party/openxr/`.

## Running

ROMs live in the app's own external files directory, which needs no runtime
storage permission. Zip files work as they are:

```sh
adb push "game.zip" /sdcard/Android/data/com.snes9x.vr/files/roms/
```

The app loads the first ROM it finds there, alphabetically. Launch it once
before pushing: it creates the directories itself, and a directory created by
`adb shell mkdir` belongs to the shell user, which the app cannot read.

There is a test ROM generator that needs no copyrighted material:

```sh
python3 tools/make_test_rom.py /tmp/test.sfc
adb push /tmp/test.sfc /sdcard/Android/data/com.snes9x.vr/files/roms/
```

It draws two backgrounds scrolling at different speeds, which is also the
material the stereo parallax work needs.

## Controls

Touch controllers have exactly enough inputs for a SNES pad once the left
thumbstick becomes the d-pad:

| SNES        | Touch              |
|-------------|--------------------|
| D-pad       | left thumbstick    |
| Y / X       | left `X` / left `Y`|
| B / A       | right `A` / right `B` |
| L / R       | left / right trigger |
| Select      | left grip          |
| Start       | right grip         |
| —           | left menu button: options |

That is the default; `Controls...` in the menu changes it. Every physical
input gets an OpenXR action of its own and the mapping to a SNES button is
kept on our side, because suggested bindings are fixed once the session
starts and cannot be rebound at runtime. Left and right on a row cycles
through the SNES buttons and back round through "none", and the mapping is
remembered in the config.

Two things stay put whatever the mapping: the left thumbstick is always the
d-pad, and right `A` always selects in the menu, so no arrangement can leave
the menu unusable. Rebinding an input releases whatever it was holding, or
that button would stick down.

A Bluetooth gamepad works too, and can be used at the same time.

## Where ROMs live

`ROM folder` in the menu opens a folder browser that can go anywhere on the
device. Pick a folder, choose `[ Use this folder ]`, and it is remembered in
`files/vr_screen.cfg`. The default is the app's own `files/roms`, which works
with no permissions at all.

Anywhere else needs **all-files access**, because scoped storage hides
non-media files in folders the app does not own. The symptom is a folder that
opens but looks empty, which is why the menu shows `Storage access` next to
the folder: selecting it opens the system panel where the player grants it.
Over adb it is

```sh
adb shell appops set --uid com.snes9x.vr MANAGE_EXTERNAL_STORAGE allow
```

Be careful when testing this by listing directories: subdirectories stay
visible without the permission and only files disappear, so a probe that
counts directory entries will report that everything is readable when it is
not.

If the configured folder has no readable ROMs the app still starts, with no
cartridge and the menu open. It has to: otherwise a bad setting would leave no
way in to correct it.

Battery saves and save states always stay in the app's own directory, whatever
the ROM folder is set to, so a read-only ROM folder is fine.

## Loading ROMs

`Load ROM...` in the menu opens a browser over the ROM folder, listing every
`.sfc`, `.smc`, `.fig`, `.swc` and `.zip` in there. The list is rescanned each
time the menu opens, so ROMs added while the app is running show up without a
restart. Long names are shortened from the middle rather than the end, because
ROM names differ in their region and revision suffixes as often as in their
titles.

Switching cartridges happens on the emulation thread, like the save states.
The outgoing game's battery save is written first, while the filename is still
derived from it; `Memory::LoadROM` resets the machine itself.

Nothing is loaded on startup. The app opens on the ROM list instead, because
picking a game is the player's first move rather than something to guess at
alphabetically. With no ROMs at all it is the same screen, which is also where
storage access is granted and the folder is changed, so a bad setting cannot
lock the app up.

## Save states

Ten slots, per ROM, saved and loaded from the menu at any time. The core names
them after the loaded cartridge, so slots never collide between games, and
they land in `files/snapshots/`.

Requests are carried out on the emulation thread between frames. Freezing
halfway through one would capture the CPU and the PPU disagreeing about where
they are.

## Options menu

The left menu button opens it, anywhere. The left thumbstick moves between
rows and changes the value on the selected row; `A` activates. While it is
open the SNES pad is left alone, and anything it was holding is released, so
opening the menu mid-game cannot leave a button stuck down.

It carries the save slot and save/load actions, the ROM browser, the screen
distance, the
screen width in degrees, the stereo depth in millimetres, the depth mode, the
smoothing, and a recenter action. All of it persists in
`files/vr_screen.cfg`, and the menu opens by itself on a first run so the
controls are not something you have to already know about.

The panel is drawn on the CPU into an RGB565 buffer using the core's own
`var8x10font`, and shown as a quad layer floating between the player and the
screen. It is 1024x992, with the options drawn at four times the font's own
size and the ROM list at twice, so a long file name still fits across the
panel while the options stay comfortable to read.

## How it fits together

| File            | Role                                                        |
|-----------------|-------------------------------------------------------------|
| `main.cpp`      | OpenXR lifecycle, frame loop, cylinder layer submission      |
| `renderer.cpp`  | Uploads the SNES frame and blits it into the swapchain image |
| `emu.cpp`       | Emulation thread, frame triple buffer, core integration      |
| `audio.cpp`     | AAudio output and the ring buffer that paces emulation       |
| `input.cpp`     | OpenXR action set, Touch and gamepad mapping                 |
| `port_glue.cpp` | The `S9x*` callbacks the core expects from a frontend        |

Emulation runs on its own thread, paced by how fast the audio ring drains, so
the 60.1 Hz SNES refresh never stalls the 72/90 Hz compositor. Finished frames
go into a triple buffer; the frame loop picks up whatever is newest and lets
the runtime's timewarp absorb the rate mismatch.

Two layers go out each frame: a projection layer holding a dim neutral
backdrop, and the screen itself as an `XrCompositionLayerCylinderKHR` on top,
so the runtime composites the picture at full resolution on a curved surface
rather than us resampling it onto geometry. The backdrop is not decoration --
submitting only a cylinder leaves the Quest shell sitting on its loading
environment, because the compositor wants a projection layer describing the
scene. For the same reason the frame loop never submits an empty layer list:
if head tracking is not up yet the screen is parked at the space origin and
corrected once a real pose arrives. ## Stereo

`emu::Frame` carries the core's per-pixel priority buffer (`GFX.ZBuffer`)
alongside the pixels: the core writes a priority value for every pixel as it
composites the layers, so layer ordering comes for free. Each eye is rendered
from that same frame with a horizontal displacement per layer, and goes out as
its own cylinder layer with `XR_EYE_VISIBILITY_LEFT`/`RIGHT`.

Two things about that are worth knowing before changing it.

**Priority values are an ordering, not a distance.** Two adjacent layers differ
by one, wherever in the range they happen to sit -- a scene using BG1 and BG2
in mode 1 gives priorities 43 and 42. Normalising that across the possible
range yields a fraction of a pixel of separation. So the renderer ranks the
values the scene actually uses and spreads them evenly instead. The ranking is
decayed over time rather than rebuilt per frame, because re-ranking the instant
a sprite layer appears would jump the whole scene in depth.

**Strength is in metres, not pixels.** What matters is how the separation
compares to the eyes themselves: at one interpupillary distance the far layer
sits at infinity, and beyond that the eyes would have to diverge, which nobody
can fuse. Three SNES pixels sounds modest and is 73 mm on a 3.2 m screen --
already past an average IPD. The strength is a distance, converted to a
fraction of the screen width at render time, so it stays right when the screen
is resized, and it is capped at half an IPD.

The frontmost layer sits on the screen plane and everything else is pushed
behind it, so nothing ever protrudes past the screen edge.

There are two ways of getting there, switchable under **Depth mode**.

**Warp** displaces the finished picture per pixel, resolving the displacement
by backward search. It needs nothing from the core, but there is only one
picture: where a layer moves aside there is nothing behind it, so it smears
along layer boundaries.

**Layers** is the real thing, and the default. The core renders each
background and the sprites into buffers of their own alongside the normal
composite, so each layer moves as a whole and whatever sits behind it shows
through. That is the only change outside `quest/`, and it is small:
`RenderScreen` already derives `BGActive` from `$212C`, and both the sprite
block and the whole `switch (PPU.BGMode)` key off that one mask, so isolating
a layer is a matter of masking it to a single bit and pointing `GFX.S` and
`GFX.DB` at that layer's own buffers. The existing `DO_BG` machinery is
untouched. It is safe because `REGMATH::Calc` takes the tile's own colour and
the subscreen and never reads back the composited pixel, so colour math
behaves the same either way.

The GPU picks, per pixel, whichever layer has the highest priority there,
which is the same rule the core's own compositing uses -- so with the stereo
strength at zero the result is the core's composite, unchanged.

## Frame pacing

The SNES runs at 60 frames a second, or 50 on a PAL cartridge, and cannot be
made to run at anything else: the games depend on that timing, so speeding it
up speeds up the game and pitches up the sound. Showing 60 fps content on a
72 Hz display therefore repeats frames on an uneven 1,1,2,3,3,4 cadence --
twelve duplicated frames a second, which is invisible on a still screen and
obvious the moment anything scrolls.

So the display is matched to the cartridge rather than the other way round.
`XR_FB_display_refresh_rate` reports what the headset offers (72, 80, 90 and
120 on a Quest 3) and the rate closest to a whole multiple of the cartridge
wins, with a tie broken towards the faster one because it reprojects head
motion better:

| cartridge | chosen | frames each |
|---|---|---|
| 60 fps (NTSC) | 120 Hz | exactly 2 |
| 50 fps (PAL) | 90 Hz | 1.8 |

NTSC is exact, so scrolling is as smooth as the hardware was. PAL cannot be:
no rate the headset offers is a multiple of 50, and 90 Hz is merely the least
bad. A PAL game will still judder a little, and the fix for that is an NTSC
copy of the game, not a change here.

The runtime accepts the request and applies it when it is ready rather than
immediately, and can move it back again, so the rate is re-checked every
couple of seconds and re-requested if it has drifted. Asking for a rate that
is already set costs nothing.

## Smoothing and resolution

**Smoothing** in the menu picks how the picture is magnified:

| | |
|---|---|
| `Pixels` | no interpolation; a hard pixel grid |
| `Sharp` | blends across one screen pixel at each texel edge, which takes the stair-steps off while the grid still reads as a grid (default) |
| `Smooth` | the same blend, widened |
| `Softer` | wider again |
| `Soft` | ordinary bilinear |

The three middle settings are one mechanism at different widths: the blend is
confined to a band around each texel edge, and widening that band walks
towards bilinear. Past a point it *is* bilinear, so `Soft` is its own mode
rather than an ever-larger number.

A layer's buffer holds nothing outside its own coverage, so an ordinary
bilinear tap near a layer edge would blend the picture with whatever was last
left there. Each tap is therefore weighted by its coverage, which keeps the
filtering and drops the contributions that are not real. Boundaries *between*
layers stay hard, which is correct: two layers at different depths have no
business blending into each other.

Note for anyone editing the shader: the filter needs `fwidth`, and derivatives
are only defined where every fragment in a quad takes the same path. The
sampling happens inside branches that differ per pixel, so the texel scale is
worked out once at the top of `main()` and used from there. Calling `fwidth`
inside those branches gives a black screen on the Quest 3 driver, with no
compile error to explain it.

The eyes render at 2048x1536, about twice what the headset resolves across the
default screen size, so the compositor downsamples and that does the
antialiasing. Measured against 1024x768 and 1536x1152 on a Quest 3: the same
72 Hz and the same emulation rate, so the extra resolution is free.

## Mode 7

Everything above places a layer at *a* depth. Mode 7 is the one case where the
SNES tells us an actual distance, so it gets treated properly.

A Mode 7 ground plane is drawn by changing the transform matrix every
scanline, and snes9x already keeps all of them in `LineMatrixData`. The matrix
maps screen pixels to texture coordinates, so the length of one screen pixel's
step across the texture, `sqrt(A^2 + C^2)`, is how much ground that row covers
-- which is proportional to how far away it is. Disparity goes as
1/distance, so the reciprocal of that step, normalised over the rows the frame
uses, is the depth directly. `GFX.LineBGMode` records which scanlines were
Mode 7, because games switch modes mid-frame for their HUDs.

The result is a road that recedes rather than a flat picture hung at one
depth. Measured on Super Mario Kart's two-player attract demo, disparity falls
from 5 px at the horizon to 1 px at the near edge -- and then *restarts* at the
second viewport's horizon, because each row's depth comes from its own matrix.

A scene with the same matrix on every line, like a rotating title screen,
comes out flat, which is correct: that is a picture being spun, not a plane
being looked along.

## Sprite depth

Sprites standing on a Mode 7 plane take the distance of the ground under their
feet: a sprite's base row gives its depth, and the whole sprite carries that
one value, which is what keeps it standing upright instead of leaning away
with the ground. Sprites whose base is not on a Mode 7 row, such as a status
bar, stay on the screen plane.

Measured on Super Mario Kart's attract demo, nine karts came out at nine
distinct depths, ordered strictly by how far down the screen each one stands.

### Why not the DSP-1

The plan was to read the depth from the DSP-1, which is the one place a SNES
game hands over real 3D coordinates: `DSP1_Project` takes world X/Y/Z and
returns a screen position plus a scale factor `M` that is inversely
proportional to distance -- exactly what disparity needs. Super Mario Kart
does use it, around 2100 projections in the first minute, and
`DSP1Projections` captures them.

They are not used yet, because the chip is only told about positions, not
about which sprite each one belongs to. The game takes `H` and `V`, adds its
own offsets, and writes the result into OAM itself, so tying a projection back
to a sprite means recovering that offset -- solvable by voting on the
translation that best aligns the two sets of points, but not needed for
anything standing on the ground.

What the DSP-1 route would add is objects that are *not* on the ground: a kart
in the middle of a jump currently takes the depth of the road beneath it
rather than its own. The capture is in place for whenever that is worth doing.

Note for measuring any of this: Super Mario Kart makes no projections at all
on its title screen, so a capture taken a couple of seconds after boot shows
an empty list and suggests, wrongly, that the game never uses the chip. The
`dump7` marker exists to hold captures back until the game is actually in
Mode 7.

## Debugging without wearing the headset

```sh
adb shell touch /sdcard/Android/data/com.snes9x.vr/files/dump
adb shell am force-stop com.snes9x.vr
adb shell am start -n com.snes9x.vr/android.app.NativeActivity
adb pull /sdcard/Android/data/com.snes9x.vr/files/frame.ppm
```

That writes a frame out as a PPM, along with its priority buffer
(`frame_depth.pgm`) and, with the layer split on, each layer's colour and
coverage (`frame_layerN.ppm`, `frame_layerN_z.pgm`). Emulation normally
suspends when the headset is off the head; a pending dump keeps it running so
the picture can be checked from a desk.

The same trigger also captures what each eye actually renders, read back from
the GPU as `eye_left.ppm` and `eye_right.ppm`, together with the composite of
the very frame they were drawn from as `eye_frame.ppm`. Comparing an eye
against anything captured separately is meaningless -- the picture has usually
moved on by the next frame -- so they are deliberately taken from one frame.

Two checks worth repeating after changing any of this:

- Recompose the dumped layers on the CPU, picking the highest priority at each
  pixel, and it should equal `frame.ppm` exactly. That is the core half.
- With the stereo strength at zero, an eye render should equal `eye_frame.ppm`
  at every source texel centre. That is the GPU half. Convert with rounding,
  not truncation: `(c * 255 + 15) / 31`, because that is what the GPU does, and
  truncating makes every second pixel look like a mismatch by one.

The capture repeats every 600 frames rather than firing once, because a game
usually needs to be left alone for a while before it shows the thing worth
looking at. A `dump7` marker alongside `dump` holds it back until the game is
actually in Mode 7:

```sh
adb shell touch /sdcard/Android/data/com.snes9x.vr/files/dump7
```
