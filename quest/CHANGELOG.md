# Snes9x VR

## Unreleased

- **Super FX games no longer show a black screen.** `Settings` is zeroed before
  the defaults are applied, and `SuperFXClockMultiplier` is a percentage the
  chip's cycle budget is scaled by, so at zero Star Fox ran perfectly while
  being handed no cycles to draw with. The same omission left `OneClockCycle`,
  `OneSlowClockCycle` and `TwoClockCycles` at zero, which is every CPU timing
  decision the core makes, and the sound interpolation at none.
- **Controls are remappable** from a `Controls...` page in the menu. Each
  physical input carries whichever SNES button you put on it; the defaults are
  unchanged.
- **Two more smoothing levels** between `Sharp` and `Soft`.
- **No ROM is loaded on startup**; the app opens on the ROM list.
- **The screen sits closer** by default, 3 m rather than 4. The apparent size
  comes from the width angle, so this changes where it is without changing how
  big it looks.

## 0.1 — first beta

Meta Quest port by Sgt. Bilko.

A port of Snes9x to the Meta Quest that puts the SNES on a curved screen and
reconstructs depth from what the console was already telling the renderer.

### The screen

- Curved screen as an OpenXR cylinder layer, composited by the runtime at full
  resolution rather than resampled onto geometry, over a dim backdrop.
- Rendered at 2048x1536 per eye, about twice what the headset resolves across
  the default screen size, so the compositor's downsample does the
  antialiasing.
- Smoothing: `Pixels`, `Sharp` (default) or `Soft`.
- Screen distance, width and position adjustable, and remembered.

### Depth

The SNES has no 3D to read back, so depth is reconstructed three ways, in
increasing order of how real it is:

- **Layer order.** The core writes a priority value per pixel as it composites,
  which gives the layer ordering for free. Those values are an ordering rather
  than a distance, so the layers a scene actually uses are ranked and spread
  evenly.
- **Split layers.** Each background and the sprites render into buffers of
  their own as well as the composite, so a displaced layer uncovers the one
  behind it instead of smearing. At zero separation the result is the core's
  own composite, pixel for pixel.
- **Mode 7.** A Mode 7 ground plane changes its matrix every scanline, and that
  matrix says how far away each row is. So a road recedes properly instead of
  hanging flat, and sprites standing on it take the distance of the ground
  under their feet.

Stereo strength is a distance in millimetres, capped at half an
interpupillary distance, because past one IPD the eyes would have to diverge.

### Frame pacing

The display rate is matched to the cartridge rather than the other way round:
120 Hz for a 60 fps NTSC game, which is exactly two display frames each. PAL
gets 90 Hz, the closest the headset offers to a multiple of 50.

### Everything else

- Reads `.sfc`, `.smc`, `.fig`, `.swc` and `.zip`.
- In-headset ROM browser, and a folder browser for choosing where ROMs live.
- Ten save-state slots per ROM, plus battery saves.
- Touch controllers, and Bluetooth gamepads alongside them.

### Known limits

- PAL games still judder slightly: no rate the headset offers divides 50.
- Sprites take the depth of the ground beneath them, so an airborne object is
  placed as though it had landed. The DSP-1 projections that would fix this are
  captured but not yet associated with sprites.
- Super FX games (Star Fox) get layer-order depth only. There is no transform
  stage to hook: the geometry lives in ROM code as ordinary integer
  arithmetic. They also boot to a black screen for a good fifteen seconds
  before the first picture, which is the game, not the port.
- A ROM folder outside the app's own directory needs all-files access, granted
  once from the menu.
