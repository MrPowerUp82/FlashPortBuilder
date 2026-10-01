# Graph Report - FlashPortBuilder  (2026-10-01)

## Corpus Check
- 56 files · ~100,896 words
- Verdict: corpus is large enough that graph structure adds value.

## Summary
- 2360 nodes · 5122 edges · 119 communities (117 shown, 1 thin omitted)
- Extraction: 92% EXTRACTED · 8% INFERRED · 0% AMBIGUOUS · INFERRED: 425 edges (avg confidence: 0.83)
- Token cost: 0 input · 0 output

## Graph Freshness
- Built from commit: `ce56efef`
- Run `git rev-parse HEAD` and compare to check if the graph is stale.
- Run `graphify update .` after code changes (no API cost).

## Community Hubs (Navigation)
- EventData
- writeMoviePack
- SWFSummary
- Player
- VM
- DisplayObject
- Font
- FillStyle
- Clip
- Reader
- AVM1.cpp
- ABCFile.hpp
- MoviePackReport
- AVM2.cpp
- SoundPlay
- SWFDocument
- EditText
- Trait
- Renderer
- VM
- AVM2Method
- ScriptFunction
- Options
- BitReader
- Object
- Frame
- Movie
- Seg
- AVM1Clip.cpp
- DisasmReport
- AVM2Code.cpp
- ABCFile
- main
- FlashRuntime.cpp
- ByteReader
- string
- AbcFile
- In
- installBuiltinsImpl
- Value
- Cursor
- PCMSound
- analyzeBuffer
- AVM2Flash.cpp
- Class
- Disassembler.cpp
- AVM1Function
- MorphRecord
- installBuiltins
- Audio
- AssetExtractor.cpp
- AVM1Action
- Decoder
- unpackLoader
- FrameScriptInfo
- PlaceObject
- installFlashImpl
- Interval
- AssetReport
- Tessellator.cpp
- Voice
- ImageRGBA
- ABCMethodBody
- Shape.cpp
- FlashPortBuilder v0.5.0
- PlaceCmd
- AVM1Source
- ABCTrait
- loadSWFDocument
- Matrix
- EditTextDef
- MeshVertex
- runtime/Audio.cpp
- Context
- Timer
- ABCFile.cpp
- .count
- extractFrameScripts
- SlabFiller
- SoundPlayDef
- FontDef
- Vertex
- DecodedBitmap
- Edge
- FlashRuntime.hpp
- vector
- parsePlaceObject
- Character
- decodePNG
- Painter
- Inverse
- SWFStructures.hpp
- FrameDef
- ColorTransform
- encodePNG
- ABCInstance
- extractAssets
- tessellateShape
- MethodInfo
- SoundDef
- run
- AudioPack
- as3MouseEvent
- AVM1Code.cpp
- ABCMethod
- ABCReader::analyze
- ButtonRecord
- ClipAction
- Matrix
- BitmapDataNative
- characterForClass
- ChannelData
- Button
- AVM2Xml.cpp
- TextFieldObject
- TimerData
- Budget
- DomainData

## God Nodes (most connected - your core abstractions)
1. `string` - 260 edges
2. `VM` - 123 edges
3. `DisplayObject` - 97 edges
4. `Clip` - 81 edges
5. `Player` - 77 edges
6. `VM` - 65 edges
7. `ABCFile` - 52 edges
8. `Movie` - 48 edges
9. `installFlashImpl()` - 46 edges
10. `SWFDocument` - 42 edges

## Surprising Connections (you probably didn't know these)
- `writeMoviePack()` --calls--> `phase`  [INFERRED]
  src/generator/MoviePack.cpp → runtime/AVM2Flash.cpp
- `ABCFile::multinameName()` --references--> `ABCNamespace`  [INFERRED]
  src/avm2/ABCFile.cpp → include/flashport/ABCFile.hpp
- `ABCFile::multinameName()` --calls--> `namespaceName`  [INFERRED]
  src/avm2/ABCFile.cpp → include/flashport/ABCFile.hpp
- `parseTags()` --calls--> `analyze`  [INFERRED]
  src/swf/SWFReader.cpp → include/flashport/ABCReader.hpp
- `cleanJPEG()` --references--> `DecodedBitmap`  [INFERRED]
  src/assets/AssetExtractor.cpp → include/flashport/AssetExtractor.hpp

## Import Cycles
- None detected.

## Communities (119 total, 1 thin omitted)

### Community 0 - "EventData"
Cohesion: 0.12
Nodes (18): map, shared_ptr, defaultTextDef(), DisplayNative, d, frameScripts, EventData, bubbles (+10 more)

### Community 1 - "writeMoviePack"
Cohesion: 0.26
Nodes (14): collectAudio(), int16_t, int32_t, uint8_t, deflate(), le16(), PackWriter, buf (+6 more)

### Community 2 - "SWFSummary"
Cohesion: 0.06
Nodes (36): ABCSummary, bytecodeBytes, classes, methodBodies, methods, name, scripts, BinaryDataSummary (+28 more)

### Community 3 - "Player"
Cohesion: 0.06
Nodes (38): as3ClipRemoved(), as3PreAllocate(), as3Start(), set, unique_ptr, LoadedSwf, base, domain (+30 more)

### Community 4 - "VM"
Cohesion: 0.04
Nodes (53): ClassBuilder, c, Code, int32_t, ClassPtr, deque, uint64_t, unique_ptr (+45 more)

### Community 5 - "DisplayObject"
Cohesion: 0.05
Nodes (41): enable_shared_from_this<DisplayObject>, addChildAt, detach, setChildIndex, map, shared_ptr, DisplayObject, as3 (+33 more)

### Community 6 - "Font"
Cohesion: 0.07
Nodes (31): Font, advances, ascent, bold, codes, descent, emSquare, glyphs (+23 more)

### Community 7 - "FillStyle"
Cohesion: 0.06
Nodes (38): FillStyle, bitmapId, color, focal, interpolation, matrix, repeat, smooth (+30 more)

### Community 8 - "Clip"
Cohesion: 0.08
Nodes (31): PlaceCmd, as3FrameEntered(), applyProperties(), Clip, advance, bornTick_, childAt, childAtIndex (+23 more)

### Community 9 - "Reader"
Cohesion: 0.12
Nodes (20): Code, int16_t, size_t, uint8_t, vector, load, readDataFile, Reader (+12 more)

### Community 10 - "AVM1.cpp"
Cohesion: 0.14
Nodes (34): getOwn, setOwn, int32_t, Value, vector, VM, isIndex(), call (+26 more)

### Community 11 - "ABCFile.hpp"
Cohesion: 0.10
Nodes (25): ABCClass, cinit, traits, ABCMetadata, items, name, ABCMultiname, kind (+17 more)

### Community 12 - "MoviePackReport"
Cohesion: 0.06
Nodes (31): HeavyShape, id, ms, triangles, uint16_t, uint64_t, vector, MoviePackReport (+23 more)

### Community 13 - "AVM2.cpp"
Cohesion: 0.11
Nodes (61): arrayIndex(), Args, ArrayObject, ClassPtr, NativeFn, Object, ObjectPtr, shared_ptr (+53 more)

### Community 14 - "SoundPlay"
Cohesion: 0.12
Nodes (18): uint32_t, Env, left, pos, right, SoundPlay, env, flags (+10 more)

### Community 15 - "SWFDocument"
Cohesion: 0.10
Nodes (26): EmbeddedBinary, bytes, characterId, size_t, uint16_t, uint32_t, uint8_t, vector (+18 more)

### Community 16 - "EditText"
Cohesion: 0.07
Nodes (29): EditText, align, autoSize, border, bounds, color, fontId, height (+21 more)

### Community 17 - "Trait"
Cohesion: 0.07
Nodes (33): NsKind, Definition, domain, ns, script, value, Kind, Namespace (+25 more)

### Community 18 - "Renderer"
Cohesion: 0.11
Nodes (32): ChildIt, ColorTransform, int32_t, Matrix, SDL_BlendMode, SDL_Texture, SDL_BlendMode, SDL_Renderer (+24 more)

### Community 19 - "VM"
Cohesion: 0.07
Nodes (30): ArrayObject, ClipObject, Fn, ObjectPtr, shared_ptr, uint64_t, VM, arrayProto (+22 more)

### Community 20 - "AVM2Method"
Cohesion: 0.07
Nodes (36): AVM2Block, end, firstInstr, handler, instrCount, scopeIn, stackIn, start (+28 more)

### Community 21 - "ScriptFunction"
Cohesion: 0.07
Nodes (28): Code, pair, size_t, uint16_t, uint32_t, uint8_t, weak_ptr, ScriptFunction (+20 more)

### Community 22 - "Options"
Cohesion: 0.11
Nodes (20): uint64_t, vector, Options, audioOut, capture, captureAfter, dump, ignoreStops (+12 more)

### Community 23 - "BitReader"
Cohesion: 0.16
Nodes (29): BitReader, bitPos_, data_, size_, int32_t, size_t, uint16_t, uint32_t (+21 more)

### Community 24 - "Object"
Cohesion: 0.07
Nodes (20): ArrayObject, items, keys, enable_shared_from_this<Object>, map, ObjectPtr, Type, Object (+12 more)

### Community 25 - "Frame"
Cohesion: 0.10
Nodes (22): ActionBlock, length, offset, ButtonCondAction, code, conditions, FrameAction, size_t (+14 more)

### Community 26 - "Movie"
Cohesion: 0.08
Nodes (27): SDL_Renderer, map, Movie, abcBlocks, background, bitmaps, buttons, domainBases (+19 more)

### Community 27 - "Seg"
Cohesion: 0.18
Nodes (11): Seg, wind, x0, x1, y0, y1, X, a (+3 more)

### Community 28 - "AVM1Clip.cpp"
Cohesion: 0.16
Nodes (23): VM::VM(), arg(), clipGotoFrame(), ClipObject::getOwn(), ClipObject::keys(), ClipObject::setOwn(), clipValue(), copyInit() (+15 more)

### Community 29 - "DisasmReport"
Cohesion: 0.09
Nodes (21): DisasmReport, abcBlocks, avm1Actions, avm1ActionUse, avm1Blocks, avm1Functions, avm1Sources, avm1SourcesWithErrors (+13 more)

### Community 30 - "AVM2Code.cpp"
Cohesion: 0.12
Nodes (21): array, AVM2Operands, AVM2OpInfo, name, operands, deque, set, buildOpTable() (+13 more)

### Community 31 - "ABCFile"
Cohesion: 0.10
Nodes (20): ABCFile, bodies, classes, doubles, floats, instances, ints, major (+12 more)

### Community 32 - "main"
Cohesion: 0.16
Nodes (22): SWFReader, analyzeBytes, analyzeFile, abcBodies(), abcBytes(), abcMethods(), pair, path (+14 more)

### Community 33 - "FlashRuntime.cpp"
Cohesion: 0.17
Nodes (25): ObjectPtr, uint32_t, VM, decodeUtf8(), forEachClip(), hasHandler(), domainOf, notifyListeners() (+17 more)

### Community 34 - "ByteReader"
Cohesion: 0.19
Nodes (9): ByteReader, int32_t, size_t, uint32_t, uint8_t, vector, int32_t, decodeAVM2Body() (+1 more)

### Community 35 - "string"
Cohesion: 0.14
Nodes (14): string, ProjectGenerator, generate, lower(), queueHandler, resolvePath, setMember, setVariable (+6 more)

### Community 36 - "AbcFile"
Cohesion: 0.03
Nodes (67): AbcFile, bodies, classes, classObjects, domain, doubles, floats, instances (+59 more)

### Community 37 - "In"
Cohesion: 0.22
Nodes (11): int32_t, size_t, uint16_t, uint32_t, uint8_t, unique_ptr, vector, In (+3 more)

### Community 38 - "installBuiltinsImpl"
Cohesion: 0.20
Nodes (20): arg(), asArray(), compareValues(), Args, ArrayObject, shared_ptr, size_t, uint32_t (+12 more)

### Community 39 - "Value"
Cohesion: 0.04
Nodes (51): ArrayObject, items, DynamicProps, entries, index, Entry, alive, key (+43 more)

### Community 40 - "Cursor"
Cohesion: 0.21
Nodes (8): int16_t, size_t, uint16_t, uint32_t, Cursor, d_, end_, utf8_

### Community 41 - "PCMSound"
Cohesion: 0.12
Nodes (18): int16_t, size_t, vector, PCMSound, channels, rate, samples, size_t (+10 more)

### Community 42 - "analyzeBuffer"
Cohesion: 0.21
Nodes (13): ABCReader, analyze, uint16_t, analyzeBuffer(), size_t, uint16_t, uint8_t, vector (+5 more)

### Community 43 - "AVM2Flash.cpp"
Cohesion: 0.27
Nodes (17): as3ClipCreated(), as3KeyEvent(), as3PollSounds(), as3Step(), classDerivesFrom(), ObjectPtr, Value, disp() (+9 more)

### Community 44 - "Class"
Cohesion: 0.11
Nodes (18): Class, abc, allocator, callAsFunction, iinit, index, instanceTraits, interfaces (+10 more)

### Community 45 - "Disassembler.cpp"
Cohesion: 0.20
Nodes (15): AVM2Writer, printed_, where_, ostream, path, set, uint32_t, vector (+7 more)

### Community 46 - "AVM1Function"
Cohesion: 0.12
Nodes (18): AVM1Block, actionCount, end, firstAction, start, successors, AVM1Function, actions (+10 more)

### Community 47 - "MorphRecord"
Cohesion: 0.11
Nodes (17): int32_t, Point, x, y, uint32_t, MorphRecord, control, curved (+9 more)

### Community 48 - "installBuiltins"
Cohesion: 0.13
Nodes (26): arg(), asArray(), Args, ArrayObject, Fn, Object, ObjectPtr, shared_ptr (+18 more)

### Community 49 - "Audio"
Cohesion: 0.11
Nodes (17): Audio, advance, callback, device_, deviceRate_, finished_, masterVolume, mix (+9 more)

### Community 50 - "AssetExtractor.cpp"
Cohesion: 0.38
Nodes (15): ImageKind, cleanJPEG(), convertSound(), size_t, uint8_t, vector, decodeBitmapTag(), decodeLossless() (+7 more)

### Community 51 - "AVM1Action"
Cohesion: 0.12
Nodes (18): AVM1Action, code, function, ints, length, offset, push, strings (+10 more)

### Community 52 - "Decoder"
Cohesion: 0.20
Nodes (11): AVM1Program, errors, functions, uint8_t, decodeAVM1(), Decoder, d_, prog_ (+3 more)

### Community 53 - "unpackLoader"
Cohesion: 0.30
Nodes (13): binaryDataPayloads(), looksLikeSWF(), alchemyConstantArrays(), int32_t, optional, size_t, uint8_t, vector (+5 more)

### Community 54 - "FrameScriptInfo"
Cohesion: 0.12
Nodes (17): FrameAction, frame, kind, label, FrameScriptInfo, actions, avm1Scripts, avm2Scripts (+9 more)

### Community 55 - "PlaceObject"
Cohesion: 0.12
Nodes (17): vector, PlaceObject, blendMode, cacheAsBitmap, characterId, className, clipActions, clipDepth (+9 more)

### Community 56 - "installFlashImpl"
Cohesion: 0.21
Nodes (17): arg(), clipOfValue(), ColorXform, add, mul, Args, size_t, VM (+9 more)

### Community 57 - "Interval"
Cohesion: 0.13
Nodes (12): Fn, vector, Interval, args, fn, method, next, once (+4 more)

### Community 58 - "AssetReport"
Cohesion: 0.11
Nodes (17): AssetReport, bitmaps, bitmapsFailed, bitmapsRaw, problems, shapes, shapesFailed, skipped (+9 more)

### Community 59 - "Tessellator.cpp"
Cohesion: 0.20
Nodes (17): addQuad(), clipPoly(), size_t, vector, flatten(), polyline(), Pt, x (+9 more)

### Community 60 - "Voice"
Cohesion: 0.12
Nodes (16): size_t, Voice, data, def, end, env, envIdx, frames (+8 more)

### Community 61 - "ImageRGBA"
Cohesion: 0.29
Nodes (6): uint8_t, vector, ImageRGBA, height, pixels, width

### Community 62 - "ABCMethodBody"
Cohesion: 0.13
Nodes (15): ABCException, excType, from, target, to, varName, ABCMethodBody, code (+7 more)

### Community 63 - "Shape.cpp"
Cohesion: 0.20
Nodes (20): ostringstream, int32_t, map, Matrix, hexColor(), lerp(), lerpC(), lerpFill() (+12 more)

### Community 64 - "FlashPortBuilder v0.5.0"
Cohesion: 0.12
Nodes (15): Analyze a SWF, Build FlashPortBuilder, Compare several games, Compatibility examples, Disassemble AVM1/AVM2 code, Extract assets, FlashPortBuilder v0.5.0, Generate an SDL2 port project (+7 more)

### Community 65 - "PlaceCmd"
Cohesion: 0.12
Nodes (18): ClipActionDef, code, events, keyCode, uint8_t, PlaceCmd, blend, character (+10 more)

### Community 66 - "AVM1Source"
Cohesion: 0.25
Nodes (8): AVM1Source, frame, label, length, offset, spriteId, size_t, uint16_t

### Community 67 - "ABCTrait"
Cohesion: 0.18
Nodes (11): ABCTrait, attrs, index, kind, metadata, name, slotId, typeName (+3 more)

### Community 68 - "loadSWFDocument"
Cohesion: 0.39
Nodes (10): collectTags(), size_t, uint16_t, uint8_t, vector, decompressSWF(), decompressZWS(), loadSWFDocument() (+2 more)

### Community 69 - "Matrix"
Cohesion: 0.10
Nodes (16): ButtonRecord, character, cxform, depth, matrix, states, ColorTransform, add (+8 more)

### Community 70 - "EditTextDef"
Cohesion: 0.14
Nodes (14): EditTextDef, align, bounds, color, flags, font, height, indent (+6 more)

### Community 71 - "MeshVertex"
Cohesion: 0.15
Nodes (13): int32_t, uint8_t, vector, Mesh, indices, texture, vertices, MeshVertex (+5 more)

### Community 72 - "runtime/Audio.cpp"
Cohesion: 0.32
Nodes (13): lock, play, playing, playingSound, positionMs, setVoice, stop, stopAll (+5 more)

### Community 73 - "Context"
Cohesion: 0.12
Nodes (15): ClipObject, clip, getOwn, keys, setOwn, Context, depth, locals (+7 more)

### Community 74 - "Timer"
Cohesion: 0.15
Nodes (13): Args, weak_ptr, Task, frame, target, Timer, args, fn (+5 more)

### Community 75 - "ABCFile.cpp"
Cohesion: 0.21
Nodes (13): multinameLocal, namespaceName, ABCFile::multinameLocal(), ABCFile::multinameName(), ABCFile::namespaceName(), ABCFile::string(), TraitKind, uint32_t (+5 more)

### Community 76 - ".count"
Cohesion: 0.21
Nodes (13): attach, setupChild, clipAlive(), uint16_t, Geometry, characterBounds, characterHit, clipBounds (+5 more)

### Community 77 - "extractFrameScripts"
Cohesion: 0.31
Nodes (11): act(), avm1Actions(), avm2Actions(), FrameAction, Kind, optional, uint32_t, vector (+3 more)

### Community 78 - "SlabFiller"
Cohesion: 0.23
Nodes (8): Key, Mesh, Open, bottom, top, SlabFiller, evenOdd_, open_

### Community 79 - "SoundPlayDef"
Cohesion: 0.15
Nodes (14): Env, left, pos, right, uint16_t, uint32_t, uint8_t, vector (+6 more)

### Community 80 - "FontDef"
Cohesion: 0.13
Nodes (15): FontDef, ascent, byCode, descent, emSquare, glyphs, leading, GlyphDef (+7 more)

### Community 81 - "Vertex"
Cohesion: 0.33
Nodes (6): Vertex, rgba, u, v, x, y

### Community 82 - "DecodedBitmap"
Cohesion: 0.20
Nodes (10): DecodedBitmap, decoded, error, height, image, original, originalExtension, width (+2 more)

### Community 83 - "Edge"
Cohesion: 0.29
Nodes (9): Edge, control, curved, from, to, buildContours(), buildPolylines(), vector (+1 more)

### Community 84 - "FlashRuntime.hpp"
Cohesion: 0.24
Nodes (4): vector, map, SDL_Renderer, saveCapture()

### Community 85 - "vector"
Cohesion: 0.10
Nodes (21): BitmapDef, failed, height, texture, width, zlib, FrameAction, frame (+13 more)

### Community 86 - "parsePlaceObject"
Cohesion: 0.29
Nodes (9): clipEventNames(), size_t, uint32_t, uint8_t, vector, findAVM1Sources(), parsePlaceObject(), readCString() (+1 more)

### Community 87 - "Character"
Cohesion: 0.18
Nodes (11): Character, bounds, file, frames, hasBounds, height, kind, names (+3 more)

### Community 88 - "decodePNG"
Cohesion: 0.38
Nodes (9): be32(), size_t, uint32_t, uint8_t, vector, decodeGIF(), decodePNG(), inflateStream() (+1 more)

### Community 89 - "Painter"
Cohesion: 0.17
Nodes (12): int32_t, Painter, color, inv, linear, mirror, texture, tile (+4 more)

### Community 90 - "Inverse"
Cohesion: 0.17
Nodes (10): Matrix, Inverse, a, b, c, d, ok, tx (+2 more)

### Community 91 - "SWFStructures.hpp"
Cohesion: 0.22
Nodes (6): optional, uint8_t, vector, UnpackResult, method, swf

### Community 92 - "FrameDef"
Cohesion: 0.11
Nodes (20): AbcBlock, bytes, flags, name, ButtonDef, actions, records, sound (+12 more)

### Community 93 - "ColorTransform"
Cohesion: 0.20
Nodes (10): ColorTransform, aAdd, aMul, bAdd, bMul, gAdd, gMul, rAdd (+2 more)

### Community 94 - "encodePNG"
Cohesion: 0.57
Nodes (7): chunk(), uint32_t, uint8_t, vector, decodeJPEG(), encodePNG(), put32()

### Community 95 - "ABCInstance"
Cohesion: 0.25
Nodes (8): ABCInstance, flags, iinit, interfaces, name, protectedNs, superName, traits

### Community 96 - "extractAssets"
Cohesion: 0.25
Nodes (9): path, uint16_t, extractAssets(), jsonEscape(), soundFormatName(), timelineName(), writeFile(), writeText() (+1 more)

### Community 97 - "tessellateShape"
Cohesion: 0.17
Nodes (15): BitmapInfo, file, height, width, GradientAtlas, byKey_, nextId_, textureFor (+7 more)

### Community 98 - "MethodInfo"
Cohesion: 0.15
Nodes (13): VM, DictionaryObject, entries, indexOf, pair, MethodInfo, body, flags (+5 more)

### Community 99 - "SoundDef"
Cohesion: 0.22
Nodes (8): int16_t, SoundDef, channels, frames, loaded, pcm, rate, z

### Community 100 - "run"
Cohesion: 0.24
Nodes (10): Code, size_t, uint16_t, uint32_t, uint8_t, Payload, p, queueClipEvent (+2 more)

### Community 101 - "AudioPack"
Cohesion: 0.22
Nodes (9): AudioPack, buttons, sounds, starts, ButtonSounds, button, play, sound (+1 more)

### Community 102 - "as3MouseEvent"
Cohesion: 0.25
Nodes (7): BlendMode, as3MouseEvent(), blendPixel(), uint32_t, SoundData, soundId, soundOf()

### Community 103 - "AVM1Code.cpp"
Cohesion: 0.60
Nodes (4): avm1ActionName(), vector, formatAVM1Action(), quote()

### Community 104 - "ABCMethod"
Cohesion: 0.22
Nodes (9): ABCMethod, body, flags, name, optionals, paramNames, paramTypes, returnType (+1 more)

### Community 105 - "ABCReader::analyze"
Cohesion: 0.32
Nodes (7): ABCReader::analyze(), uint16_t, uint32_t, uint8_t, vector, skipConstantPool(), skipTraits()

### Community 106 - "ButtonRecord"
Cohesion: 0.22
Nodes (8): ButtonRecord, character, cxform, depth, matrix, states, ColorTransform, Matrix

### Community 107 - "ClipAction"
Cohesion: 0.29
Nodes (7): ClipAction, actionLength, actionOffset, events, keyCode, uint32_t, uint8_t

### Community 108 - "Matrix"
Cohesion: 0.29
Nodes (7): Matrix, a, b, c, d, tx, ty

### Community 109 - "BitmapDataNative"
Cohesion: 0.29
Nodes (7): BitmapDataNative, h, px, transparent, w, bitmapOf(), vector

### Community 110 - "characterForClass"
Cohesion: 0.33
Nodes (5): characterForClass(), ClassPtr, Kind, uint16_t, VM::classForCharacter()

### Community 111 - "ChannelData"
Cohesion: 0.33
Nodes (6): ChannelData, handle, pan, soundId, volume, channelOf()

### Community 112 - "Button"
Cohesion: 0.40
Nodes (5): Button, actions, records, trackAsMenu, vector

### Community 113 - "AVM2Xml.cpp"
Cohesion: 0.08
Nodes (69): enable_shared_from_this<XNode>, NodePtr, Multiname, anyName, attribute, name, nss, rtName (+61 more)

### Community 114 - "TextFieldObject"
Cohesion: 0.40
Nodes (4): TextFieldObject, field, getOwn, setOwn

### Community 115 - "TimerData"
Cohesion: 0.40
Nodes (5): TimerData, currentCount, delay, repeatCount, timerId

### Community 116 - "Budget"
Cohesion: 0.67
Nodes (3): Budget, left, uint64_t

## Knowledge Gaps
- **1028 isolated node(s):** `kind`, `name`, `kind`, `name`, `ns` (+1023 more)
  These have ≤1 connection - possible missing edges or undocumented components. (Counts symbols only; 1255 node(s) total have ≤1 connection when file, concept and rationale nodes are included.)
- **1 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **Why does `string` connect `string` to `EventData`, `writeMoviePack`, `SWFSummary`, `Player`, `VM`, `DisplayObject`, `Font`, `Clip`, `Reader`, `AVM1.cpp`, `ABCFile.hpp`, `MoviePackReport`, `AVM2.cpp`, `SWFDocument`, `EditText`, `Trait`, `Renderer`, `VM`, `AVM2Method`, `ScriptFunction`, `Options`, `Object`, `Frame`, `Movie`, `AVM1Clip.cpp`, `DisasmReport`, `AVM2Code.cpp`, `ABCFile`, `main`, `FlashRuntime.cpp`, `ByteReader`, `AbcFile`, `In`, `installBuiltinsImpl`, `Value`, `Cursor`, `PCMSound`, `analyzeBuffer`, `AVM2Flash.cpp`, `Class`, `Disassembler.cpp`, `AVM1Function`, `installBuiltins`, `Audio`, `AssetExtractor.cpp`, `AVM1Action`, `Decoder`, `unpackLoader`, `FrameScriptInfo`, `PlaceObject`, `installFlashImpl`, `Interval`, `AssetReport`, `ImageRGBA`, `Shape.cpp`, `PlaceCmd`, `AVM1Source`, `loadSWFDocument`, `EditTextDef`, `Context`, `ABCFile.cpp`, `.count`, `extractFrameScripts`, `DecodedBitmap`, `FlashRuntime.hpp`, `vector`, `parsePlaceObject`, `Character`, `decodePNG`, `SWFStructures.hpp`, `FrameDef`, `encodePNG`, `extractAssets`, `tessellateShape`, `MethodInfo`, `run`, `AVM1Code.cpp`, `ABCReader::analyze`, `characterForClass`, `AVM2Xml.cpp`?**
  _High betweenness centrality (0.705) - this node is a cross-community bridge._
- **Why does `VM` connect `VM` to `string`, `AbcFile`, `DisplayObject`, `Player`, `Value`, `Timer`, `AVM2.cpp`, `Trait`, `AVM2Code.cpp`?**
  _High betweenness centrality (0.117) - this node is a cross-community bridge._
- **Why does `Player` connect `Player` to `FlashRuntime.cpp`, `string`, `VM`, `DisplayObject`, `as3MouseEvent`, `Clip`, `Reader`, `AVM2Flash.cpp`, `.count`, `characterForClass`, `FontDef`, `Audio`, `Renderer`, `VM`, `FlashRuntime.hpp`, `AVM1Action`, `Movie`, `AVM1Clip.cpp`?**
  _High betweenness centrality (0.049) - this node is a cross-community bridge._
- **Are the 5 inferred relationships involving `DisplayObject` (e.g. with `installFlashImpl()` and `place`) actually correct?**
  _`DisplayObject` has 5 INFERRED edges - model-reasoned connections that need verification._
- **What connects `kind`, `name`, `kind` to the rest of the system?**
  _1028 weakly-connected nodes found - possible documentation gaps or missing edges._
- **Should `EventData` be split into smaller, more focused modules?**
  _Cohesion score 0.11764705882352941 - nodes in this community are weakly interconnected._
- **Should `SWFSummary` be split into smaller, more focused modules?**
  _Cohesion score 0.06190476190476191 - nodes in this community are weakly interconnected._