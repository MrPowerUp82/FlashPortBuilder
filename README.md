# FlashPortBuilder v0.5.0

Early SWF -> C++/SDL2 static port builder / recompiler experiment.

The goal is not to embed a Flash Player. The long-term pipeline is:

```text
SWF -> container/tags -> AVM1 or AVM2 IR -> C++ -> FlashRuntimeSDL2 -> native executable
```

## Implemented

- FWS (uncompressed), CWS (zlib) and ZWS (LZMA, needs liblzma) parsing.
- SWF RECT, stage size, FPS and frame count.
- Recursive SWF tag scanning, including `DefineSprite` timelines.
- `FileAttributes` AS3 detection.
- AVM1 detection through `DoAction` / `DoInitAction`.
- `SymbolClass` document-class detection.
- `DoABC` / AVM2 ABC structural parsing.
- Counts methods, classes, scripts, method bodies and bytecode size.
- Detects `DefineBinaryData` payloads.
- Automatically recognizes and analyzes an FWS/CWS SWF embedded directly in `DefineBinaryData`.
- Compatibility-matrix command for testing several games against the same parser.
- Generates a C++17 + SDL2 project that plays the game's timelines from a movie pack
  (tessellated shapes, textures, display-list commands, masks; see "Timeline runtime").
- Full ABC model: constant pools, multinames, methods, metadata, classes, traits, bodies.
- AVM2 decoder: reachability-driven decoding (robust to junk after jumps), basic blocks,
  CFG successors, exception handlers and a stack/scope-depth verifier.
- AVM1 decoder for `DoAction`, `DoInitAction`, `DefineButton`/`DefineButton2` actions and
  `PlaceObject2/3` clip events, including nested `DefineFunction`/`DefineFunction2`, `Try`,
  `With`, typed `Push` values and constant pools, with per-function basic blocks.
- Loader unpacking: plain SWFs in `DefineBinaryData`, Alchemy "XOR merge" packers (the
  XOR key is recovered automatically from the Alchemy FSM bytecode) and simple repeating
  XOR payloads with 1-3 byte keys; candidates are validated by fully parsing the result.
- ABC 47.16+ (Harman AIR) float constant pools and `pushfloat`/`convert_f`/`unplus`.
- Asset extraction: `DefineBits*`/JPEGTables/JPEG2-4 (alpha planes merged; PNG and GIF
  payloads decoded by built-in decoders), lossless bitmaps (8/15/32-bit, premultiplied
  alpha undone) to PNG; `DefineShape1-4` to SVG
  (solid, linear/radial/focal gradients, bitmap fills, LINESTYLE2); `DefineSound` and
  stream sounds to MP3/WAV (SWF ADPCM decoded); `manifest.json` with characters, bounds,
  sprite frame counts and symbol names.

## Compatibility examples

These three real games are our initial regression suite:

| Game | Outer SWF | Script model | Important architecture case |
|---|---:|---|---|
| Mike Shadow: I Paid For It | v9, 780x450, 24 FPS | AVM2 / AS3 | Large direct AS3 game: 427 ABC blocks, 2,373 methods, 987 sprites |
| Comic Stars Fighting 3.6 | v10, 550x400, 34 FPS | AVM2 / AS3 preloader | Packed/protected loader with two `DefineBinaryData` payloads; main payload is not a plain SWF |
| Dragon Ball Fierce Fighting 2.8 | v15, 550x400, 36 FPS | AS3 wrapper -> embedded AVM1 game | Direct nested CWS in `DefineBinaryData`; inner game is 550x400, 30 FPS, 4,310 `DoAction`, 1,123 sprites |

The Dragon Ball sample is important because it proves the builder should support both AVM generations. We can unwrap the lightweight AS3 loader and port the actual AVM1 game inside it.

The Comic Stars sample establishes a separate loader/unpacker problem: its large binary payload is opaque at the SWF-container level and is loaded through AS3 `ByteArray` / `Loader.loadBytes` behavior. This must be handled as a packing/decryption stage before normal SWF recompilation.

## Build FlashPortBuilder

Requires a C++17 compiler, CMake and zlib. libmpg123 is optional but needed for sound:
MP3 sounds are decoded to PCM while the movie pack is built (without it they stay silent).
libjpeg-turbo is optional (without it JPEG
bitmaps are copied as `.jpg` and JPEG3/4 alpha planes are not merged); liblzma (xz) is
optional and needed only for ZWS (LZMA-compressed) SWFs.

```bash
cmake -S . -B build
cmake --build build
```

On Windows with MSYS2 (UCRT64):

```bash
pacman -S mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,zlib,SDL2,libjpeg-turbo,xz,mpg123}
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Analyze a SWF

```bash
./build/flashport analyze game.swf
```

If a plain SWF is embedded in `DefineBinaryData`, its analysis is printed recursively.

## Compare several games

```bash
./build/flashport matrix \
  mike-shadow-i-paid-for-it.swf \
  comic-stars-fighting-3-6.swf \
  dragon-ball-fierce-fighting-2-8.swf
```

## Unpack a protected loader

```bash
./build/flashport unpack comic-stars-fighting-3-6.swf --output inner.swf
```

## Disassemble AVM1/AVM2 code

```bash
./build/flashport disasm game.swf --output listings/game
```

Nested SWFs are followed automatically (`embedded_<id>/`, `unpacked/`).

## Extract assets

```bash
./build/flashport extract game.swf --output assets/game
```

## Generate an SDL2 port project

```bash
./build/flashport build game.swf --output generated/game
```

The project contains `assets/` (converted characters + `manifest.json`), `code/`
(AVM1/AVM2 listings), `movie.pack` and the SDL2 runtime (copied from `runtime/`). Stage
metadata comes from the innermost SWF, i.e. the real game rather than its wrapper or packer.

```bash
cmake -S generated/game -B generated/game/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build generated/game/build
generated/game/build/flash_game
```

`flashport pack game.swf --output movie.pack` writes only the movie pack.

## Runtime

The generated executable plays the game from its movie pack:

- Shapes are tessellated at build time (scanline trapezoids with vertical merging,
  even-odd/nonzero, curves flattened by error). Gradients and bitmap fills become
  textures with affine UVs; strokes become quads. Drawing uses `SDL_RenderGeometry`.
- Morph shapes (shape tweens, `DefineMorphShape`/`2`) are interpolated and tessellated at
  build time for every ratio their timelines use; the runtime draws the closest ratio.
- Static text (DefineText) is laid out at build time with the embedded font outlines and
  packed as shapes. Dynamic text fields (DefineEditText) are laid out at run time from
  tessellated glyphs, bound to their variables, and scriptable (`field.text`).
- The display list follows Flash semantics: PlaceObject/RemoveObject per frame, nested
  MovieClips, character replacement, colour transforms, buttons (up/over/down/hit
  states), and masks (clipDepth) composited through offscreen layers. `goto` and looping
  rebuild the display list and keep unchanged instances, like the Flash Player.
- **AVM1 movies run their scripts** in an interpreter (`runtime/AVM1*.cpp`): all SWF 5-8
  actions, `DefineFunction`/`DefineFunction2` with registers and closures, paths
  (`_root.a.b`, `_parent`, `/a:b`), the Flash action queue (frame scripts, DoInitAction,
  `onClipEvent`, `onEnterFrame`, button conditions, `setInterval`), MovieClip properties
  and methods (`attachMovie`, `duplicateMovieClip`, `hitTest`, `swapDepths`, ...), `Math`,
  `Key`, `Mouse`, `Array`, `String`, `SharedObject` (in memory) and `Sound` (silent stub).
  AS2 classes work: `extends`, `super()` / `super.method()` (bound to the caller's `this`) and
  `Object.registerClass` (class applied to every instance of the symbol once the frame's
  `DoInitAction` blocks have run). Text fields count in hit tests and can be button records.
- **AVM2 movies run their ActionScript 3** (`runtime/AVM2*.cpp`): ABC classes loaded lazily
  per script, namespaces (private/protected/internal), slots typed like the AVM (`int`
  truncates), getters/setters, `super`, closures, activations, exceptions, `for..in`,
  and a native Flash API: display list (DisplayObject .. MovieClip/Sprite/SimpleButton/
  Stage, `addFrameScript`, timeline instances bound to their symbol classes and parent
  slots), events with capture/bubbling, mouse events, Timer/setTimeout, TextField,
  geometry, Dictionary/ByteArray, SharedObject (in memory). Network, ads and sound are
  stubs: loads fail with `ioError` like an offline player; sounds are silent.

- **Sound** (all three script models): DefineSound (PCM, ADPCM, MP3), timeline `StartSound`
  (loops, in/out points, volume envelopes, "stop" and "no multiple" flags), stream sounds
  (started with their timeline, stopped when it is destroyed) and `DefineButtonSound`
  (roll over / out, press, release) are decoded at build time and mixed by the runtime through
  SDL (44.1 kHz stereo, linear resampling, soft limiter). AVM1 `Sound` (`attachSound`, `start`
  with offset/loops, `stop`, `setVolume`, `setPan`, `duration`, `position`, `onSoundComplete`,
  `stopAllSounds`) and AS3 `Sound` / `SoundChannel` / `SoundTransform` / `SoundMixer`
  (embedded sound classes, `play(start, loops, transform)`, `soundComplete`) play pack sounds.
  Nellymoser and Speex streams and sounds loaded from the network stay silent.

Keyboard and mouse go to the game. Debug keys: F2 pause, F3 step, F4 restart,
F5 ignore `stop()`, F6 toggle masks, F7 symbol viewer (PgDn/PgUp browse), Esc quit.
For automated checks: `flash_game [movie.pack] [--play-all] [--view <n>] [--dump]
[--input "tick:click:x,y;tick:down:keycode;..."] --after <ticks> --capture out.bmp`.

Status per game. Debug env vars: `FP_TRACE_PROP=_x` logs every
assignment of a MovieClip property, `FP_TRACE_CALL=name` logs a method's calls and results,
`FP_TRACE_AUDIO=1` logs every started sound with its level, `FP_DUMP_DEPTH`/`FP_DUMP_BUTTONS`
refine `--dump`. In scripted runs `--audio-out file.wav` also saves the mixed audio.
- **Dragon Ball Fierce Fighting 2.8** (AVM1): playable - menus, difficulty, fights with
  W/A/S/D + J/K/L/U/I/O, combo counter.
- **Mike Shadow: I Paid For It** (AS3): playable - preloader (the "Go!" button appears
  after the ad timer), intro (SKIP), main menu, fights by clicking skills, HUD.
- **Toon Cup 2010** (AVM1/AS2): playable - menus, country and character select, a full
  match with players, ball, referee, score and clock (needed `super` and text-in-button fixes).
- **Superman Returns: Save Metropolis** (AVM1/AS2): playable - title, countdown, scrolling
  buildings, falling debris, height meter (needed `Object.registerClass`).
- **Mortal Kombat Karnage** (AVM1): playable - menu, mode and fighter select, difficulty,
  fight tower, fights with arrows + S/A/D.
- **Comic Stars Fighting 3.6** (AS3, unpacked from its Alchemy loader): playable - title
  ("start"), New Game, player stats, Single-Player, character select (then "Battle Mode"),
  level select, fights with W/A/S/D + J/K/L/U/I/O, combo counter.

## Pipeline status on the regression suite

| Game | Unpack | AVM2 decode | AVM1 decode | Assets |
|---|---|---|---|---|
| Mike Shadow | n/a | 2,373 bodies, 80,376 instr, 0 errors | n/a | 78 bitmaps, 11,158 shapes, 10 sounds + 1 stream, 0 failures |
| Comic Stars 3.6 | Alchemy XOR loader -> CWS v12 AS3 game | 4,645 bodies, 186,222 instr, 0 errors | n/a | 2,536 bitmaps, 3,028 shapes, 19 sounds, 0 failures |
| Dragon Ball FF 2.8 | plain embedded SWF | 24 bodies (wrapper), 0 errors | 5,956 sources, 189,720 actions, 0 errors | 1,907 bitmaps, 2,813 shapes, 48 sounds, 0 failures |

All AVM2 bodies also pass the stack/scope verifier with no warnings.

Movie packs: Mike Shadow 53 MB (4.1M triangles), Dragon Ball 25 MB, Comic Stars 15 MB.

The SWF test corpus of the JPEXS Free Flash Decompiler (97 files, used only as test input)
also goes through `analyze`/`disasm`/`extract`/`pack`: everything decodes except a Harman
AIR-encrypted SWF; a few hand-made/cross-compiled ABC bodies get genuine verifier warnings.

## Next implementation layers

1. Shared Flash IR and dynamic `FlashValue` runtime.
2. C++ emission for AVM1/AVM2 method bodies from the decoded CFGs.
3. EventDispatcher, keyboard/mouse mapping and button interaction.
4. Fonts and text fields, morph shapes, filters/blend modes, sound playback, video.

## Not implemented yet

- Script execution / C++ code generation from AVM1/AVM2.
- Filters, blend modes, video; morph shape ratios set only by scripts use the closest
  tessellated ratio.
- Nellymoser/Speex decoding; per-sound loudness is not normalised.
- Anti-aliasing of tessellated shapes.
- Protected-SWF schemes other than the Alchemy XOR loader.
