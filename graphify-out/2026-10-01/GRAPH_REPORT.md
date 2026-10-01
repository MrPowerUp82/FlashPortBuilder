# Graph Report - FlashPortBuilder  (2026-10-01)

## Corpus Check
- 54 files · ~88,684 words
- Verdict: corpus is large enough that graph structure adds value.

## Summary
- 2217 nodes · 4704 edges · 113 communities
- Extraction: 91% EXTRACTED · 9% INFERRED · 0% AMBIGUOUS · INFERRED: 425 edges (avg confidence: 0.83)
- Token cost: 0 input · 0 output

## Community Hubs (Navigation)
- installFlashImpl
- writeMoviePack
- SWFSummary
- FlashRuntime.cpp
- VM
- DisplayObject
- Font
- FillStyle
- Clip
- Reader
- VM
- uint32_t
- MoviePackReport
- AVM2.cpp
- SoundPlay
- SWFDocument
- EditText
- Trait
- Renderer
- execute
- AVM2Method
- ScriptFunction
- Options
- BitReader
- Object
- Frame
- Movie
- SlabFiller
- AVM1Clip.cpp
- DisasmReport
- AVM2Code.cpp
- ABCFile
- main
- Shape.cpp
- ByteReader
- Object
- MethodBody
- In
- installBuiltinsImpl
- Value
- Cursor
- PCMSound
- analyzeBuffer
- AbcFile
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
- AVM2.hpp
- TraitInfo
- AssetReport
- Tessellator.cpp
- Voice
- ImageRGBA
- ABCMethodBody
- string
- FlashPortBuilder v0.5.0
- PlaceCmd
- AVM1Source
- ABCTrait
- loadSWFDocument
- FlashRuntime.hpp
- EditTextDef
- MeshVertex
- runtime/Audio.cpp
- FunctionObject
- Timer
- uint32_t
- Matrix
- extractFrameScripts
- BinaryDataSummary
- SoundPlayDef
- FontDef
- uint8_t
- DecodedBitmap
- RGBA
- Rect
- vector
- Matrix
- Character
- decodePNG
- Painter
- Inverse
- UnpackResult
- FrameDef
- ColorTransform
- encodePNG
- ABCInstance
- vector
- GradientAtlas
- MethodInfo
- SoundDef
- run
- ABCSummary
- BitmapDef
- disassembleAVM1
- ABCMethod
- tessellateShape
- ButtonRecord
- InstanceInfo
- Matrix
- MoviePack.cpp
- initSlots
- PlaceCmd
- Multiname

## God Nodes (most connected - your core abstractions)
1. `string` - 234 edges
2. `VM` - 110 edges
3. `DisplayObject` - 92 edges
4. `Clip` - 80 edges
5. `VM` - 65 edges
6. `Player` - 65 edges
7. `ABCFile` - 52 edges
8. `installFlashImpl()` - 47 edges
9. `Movie` - 45 edges
10. `SWFDocument` - 42 edges

## Surprising Connections (you probably didn't know these)
- `main()` --calls--> `analyzeFile`  [INFERRED]
  src/main.cpp → include/flashport/SWFReader.hpp
- `main()` --calls--> `analyzeBytes`  [INFERRED]
  src/main.cpp → include/flashport/SWFReader.hpp
- `writeMoviePack()` --calls--> `phase`  [INFERRED]
  src/generator/MoviePack.cpp → runtime/AVM2Flash.cpp
- `ABCFile::multinameName()` --references--> `ABCNamespace`  [INFERRED]
  src/avm2/ABCFile.cpp → include/flashport/ABCFile.hpp
- `ABCFile::multinameName()` --calls--> `namespaceName`  [INFERRED]
  src/avm2/ABCFile.cpp → include/flashport/ABCFile.hpp

## Import Cycles
- None detected.

## Communities (113 total, 0 thin omitted)

### Community 0 - "installFlashImpl"
Cohesion: 0.06
Nodes (71): map, NativeData, listeners, arg(), as3ClipCreated(), as3KeyEvent(), as3MouseEvent(), as3PollSounds() (+63 more)

### Community 1 - "writeMoviePack"
Cohesion: 0.30
Nodes (8): int16_t, int32_t, PackWriter, buf, writeAudio(), writeBitmap(), writeMoviePack(), writeSoundPlay()

### Community 2 - "SWFSummary"
Cohesion: 0.11
Nodes (19): map, uint8_t, vector, SWFSummary, abcBlocks, actionScript3, actualFileLength, binaryData (+11 more)

### Community 3 - "FlashRuntime.cpp"
Cohesion: 0.06
Nodes (57): VM::VM(), as3ClipRemoved(), as3FrameEntered(), as3PreAllocate(), as3Start(), childAt, indexOf, swapDepths (+49 more)

### Community 4 - "VM"
Cohesion: 0.05
Nodes (40): ClassBuilder, c, ClassPtr, deque, uint64_t, unique_ptr, VM, abcs_ (+32 more)

### Community 5 - "DisplayObject"
Cohesion: 0.06
Nodes (37): enable_shared_from_this<DisplayObject>, addChildAt, detach, setChildIndex, map, shared_ptr, DisplayObject, as3 (+29 more)

### Community 6 - "Font"
Cohesion: 0.14
Nodes (14): Font, advances, ascent, bold, codes, descent, emSquare, glyphs (+6 more)

### Community 7 - "FillStyle"
Cohesion: 0.06
Nodes (45): FillStyle, bitmapId, color, focal, interpolation, matrix, repeat, smooth (+37 more)

### Community 8 - "Clip"
Cohesion: 0.12
Nodes (24): PlaceCmd, targetPath, applyProperties(), Clip, advance, attach, bornTick_, childAtIndex (+16 more)

### Community 9 - "Reader"
Cohesion: 0.11
Nodes (23): Code, int16_t, int32_t, SDL_Texture, size_t, uint32_t, uint8_t, vector (+15 more)

### Community 10 - "VM"
Cohesion: 0.06
Nodes (72): ArrayObject, getOwn, items, keys, setOwn, Budget, left, ArrayObject (+64 more)

### Community 11 - "uint32_t"
Cohesion: 0.15
Nodes (15): ABCMultiname, kind, name, ns, nsSet, typeBase, typeParams, ABCNamespace (+7 more)

### Community 12 - "MoviePackReport"
Cohesion: 0.07
Nodes (29): HeavyShape, id, ms, triangles, uint16_t, uint64_t, vector, MoviePackReport (+21 more)

### Community 13 - "AVM2.cpp"
Cohesion: 0.10
Nodes (42): ClassPtr, Code, int32_t, NativeFn, uint32_t, Value, VM, indexOf (+34 more)

### Community 14 - "SoundPlay"
Cohesion: 0.09
Nodes (28): AudioPack, buttons, sounds, starts, ButtonSounds, button, play, sound (+20 more)

### Community 15 - "SWFDocument"
Cohesion: 0.11
Nodes (24): EmbeddedBinary, bytes, characterId, size_t, uint16_t, uint32_t, uint8_t, vector (+16 more)

### Community 16 - "EditText"
Cohesion: 0.09
Nodes (23): EditText, align, autoSize, border, bounds, color, fontId, height (+15 more)

### Community 17 - "Trait"
Cohesion: 0.08
Nodes (30): NsKind, Kind, Namespace, kind, owner, uri, nsMatches(), Trait (+22 more)

### Community 18 - "Renderer"
Cohesion: 0.13
Nodes (15): SDL_Renderer, uint64_t, Renderer, height_, layerDepth_, layers_, maskBlend_, masksEnabled (+7 more)

### Community 19 - "execute"
Cohesion: 0.23
Nodes (14): Args, ArrayObject, ObjectPtr, shared_ptr, vector, call, callProperty, construct (+6 more)

### Community 20 - "AVM2Method"
Cohesion: 0.06
Nodes (40): AVM2Operands, AVM2Block, end, firstInstr, handler, instrCount, scopeIn, stackIn (+32 more)

### Community 21 - "ScriptFunction"
Cohesion: 0.07
Nodes (28): Code, pair, size_t, uint16_t, uint32_t, uint8_t, weak_ptr, ScriptFunction (+20 more)

### Community 22 - "Options"
Cohesion: 0.10
Nodes (24): SDL_Renderer, uint64_t, vector, dumpClip(), main(), Options, audioOut, capture (+16 more)

### Community 23 - "BitReader"
Cohesion: 0.13
Nodes (25): BitReader, bitPos_, data_, size_, int32_t, size_t, uint32_t, uint8_t (+17 more)

### Community 24 - "Object"
Cohesion: 0.04
Nodes (38): ClipObject, clip, getOwn, keys, setOwn, deque, enable_shared_from_this<Object>, Fn (+30 more)

### Community 25 - "Frame"
Cohesion: 0.12
Nodes (17): ActionBlock, length, offset, ButtonCondAction, code, conditions, FrameAction, size_t (+9 more)

### Community 26 - "Movie"
Cohesion: 0.08
Nodes (25): SDL_Renderer, map, Movie, abcBlocks, background, bitmaps, buttons, editTexts (+17 more)

### Community 27 - "SlabFiller"
Cohesion: 0.13
Nodes (19): Key, vector, Open, bottom, top, Seg, wind, x0 (+11 more)

### Community 28 - "AVM1Clip.cpp"
Cohesion: 0.17
Nodes (22): arg(), clipGotoFrame(), ClipObject::getOwn(), ClipObject::keys(), ClipObject::setOwn(), clipValue(), copyInit(), Args (+14 more)

### Community 29 - "DisasmReport"
Cohesion: 0.09
Nodes (21): DisasmReport, abcBlocks, avm1Actions, avm1ActionUse, avm1Blocks, avm1Functions, avm1Sources, avm1SourcesWithErrors (+13 more)

### Community 30 - "AVM2Code.cpp"
Cohesion: 0.18
Nodes (17): array, set, buildOpTable(), uint32_t, uint8_t, decodeAVM2Body(), Effect, pop (+9 more)

### Community 31 - "ABCFile"
Cohesion: 0.08
Nodes (31): ABCFile, bodies, classes, doubles, floats, instances, ints, major (+23 more)

### Community 32 - "main"
Cohesion: 0.23
Nodes (16): abcBodies(), abcBytes(), abcMethods(), pair, uint16_t, uint64_t, vector, findRuntimeDir() (+8 more)

### Community 33 - "Shape.cpp"
Cohesion: 0.22
Nodes (22): uint16_t, Matrix, uint16_t, uint8_t, lerp(), lerpC(), lerpFill(), lerpI() (+14 more)

### Community 34 - "ByteReader"
Cohesion: 0.13
Nodes (15): ByteReader, int32_t, size_t, uint32_t, uint8_t, vector, ABCReader::analyze(), uint16_t (+7 more)

### Community 35 - "Object"
Cohesion: 0.12
Nodes (16): ArrayObject, items, DictionaryObject, entries, enable_shared_from_this<Object>, pair, shared_ptr, vector (+8 more)

### Community 36 - "MethodBody"
Cohesion: 0.11
Nodes (20): int32_t, uint8_t, Instr, a, b, op, target, targets (+12 more)

### Community 37 - "In"
Cohesion: 0.22
Nodes (11): int32_t, size_t, uint16_t, uint32_t, uint8_t, unique_ptr, vector, In (+3 more)

### Community 38 - "installBuiltinsImpl"
Cohesion: 0.20
Nodes (20): arg(), asArray(), compareValues(), Args, ArrayObject, shared_ptr, size_t, uint32_t (+12 more)

### Community 39 - "Value"
Cohesion: 0.11
Nodes (11): ObjectPtr, T, Type, ScriptException, value, Value, b, n (+3 more)

### Community 40 - "Cursor"
Cohesion: 0.21
Nodes (8): int16_t, size_t, uint16_t, uint32_t, Cursor, d_, end_, utf8_

### Community 41 - "PCMSound"
Cohesion: 0.12
Nodes (18): int16_t, size_t, vector, PCMSound, channels, rate, samples, size_t (+10 more)

### Community 42 - "analyzeBuffer"
Cohesion: 0.21
Nodes (13): ABCReader, analyze, uint16_t, analyzeBuffer(), size_t, uint16_t, uint8_t, vector (+5 more)

### Community 43 - "AbcFile"
Cohesion: 0.12
Nodes (16): AbcFile, bodies, classes, classObjects, doubles, floats, instances, ints (+8 more)

### Community 44 - "Class"
Cohesion: 0.10
Nodes (21): Class, abc, allocator, callAsFunction, iinit, index, instanceTraits, interfaces (+13 more)

### Community 45 - "Disassembler.cpp"
Cohesion: 0.17
Nodes (16): TraitKind, traitKindName(), AVM2Writer, printed_, where_, ostream, path, set (+8 more)

### Community 46 - "AVM1Function"
Cohesion: 0.12
Nodes (18): AVM1Block, actionCount, end, firstAction, start, successors, AVM1Function, actions (+10 more)

### Community 47 - "MorphRecord"
Cohesion: 0.10
Nodes (19): int32_t, Point, x, y, size_t, uint32_t, MorphRecord, control (+11 more)

### Community 48 - "installBuiltins"
Cohesion: 0.13
Nodes (26): arg(), asArray(), Args, ArrayObject, Fn, Object, ObjectPtr, shared_ptr (+18 more)

### Community 49 - "Audio"
Cohesion: 0.11
Nodes (17): Audio, advance, callback, device_, deviceRate_, finished_, masterVolume, mix (+9 more)

### Community 50 - "AssetExtractor.cpp"
Cohesion: 0.22
Nodes (24): ImageKind, cleanJPEG(), convertSound(), path, size_t, uint16_t, uint8_t, vector (+16 more)

### Community 51 - "AVM1Action"
Cohesion: 0.12
Nodes (18): AVM1Action, code, function, ints, length, offset, push, strings (+10 more)

### Community 52 - "Decoder"
Cohesion: 0.20
Nodes (11): AVM1Program, errors, functions, uint8_t, decodeAVM1(), Decoder, d_, prog_ (+3 more)

### Community 53 - "unpackLoader"
Cohesion: 0.35
Nodes (11): alchemyConstantArrays(), int32_t, optional, size_t, uint8_t, vector, describeKey(), parseAllABC() (+3 more)

### Community 54 - "FrameScriptInfo"
Cohesion: 0.12
Nodes (17): FrameAction, frame, kind, label, FrameScriptInfo, actions, avm1Scripts, avm2Scripts (+9 more)

### Community 55 - "PlaceObject"
Cohesion: 0.12
Nodes (17): vector, PlaceObject, blendMode, cacheAsBitmap, characterId, className, clipActions, clipDepth (+9 more)

### Community 56 - "AVM2.hpp"
Cohesion: 0.14
Nodes (13): DynamicProps, entries, index, Entry, alive, key, value, size_t (+5 more)

### Community 57 - "TraitInfo"
Cohesion: 0.22
Nodes (9): TraitInfo, attrs, index, kind, name, slotId, typeName, vindex (+1 more)

### Community 58 - "AssetReport"
Cohesion: 0.11
Nodes (17): AssetReport, bitmaps, bitmapsFailed, bitmapsRaw, problems, shapes, shapesFailed, skipped (+9 more)

### Community 59 - "Tessellator.cpp"
Cohesion: 0.22
Nodes (11): Mesh, addQuad(), uint32_t, flatten(), GradientAtlas::textureFor(), polyline(), Pt, x (+3 more)

### Community 60 - "Voice"
Cohesion: 0.12
Nodes (16): size_t, Voice, data, def, end, env, envIdx, frames (+8 more)

### Community 61 - "ImageRGBA"
Cohesion: 0.29
Nodes (6): uint8_t, vector, ImageRGBA, height, pixels, width

### Community 62 - "ABCMethodBody"
Cohesion: 0.13
Nodes (15): ABCException, excType, from, target, to, varName, ABCMethodBody, code (+7 more)

### Community 63 - "string"
Cohesion: 0.16
Nodes (19): string, ProjectGenerator, generate, ostringstream, lower(), int32_t, map, hexColor() (+11 more)

### Community 64 - "FlashPortBuilder v0.5.0"
Cohesion: 0.13
Nodes (14): Analyze a SWF, Build FlashPortBuilder, Compare several games, Compatibility examples, Disassemble AVM1/AVM2 code, Extract assets, FlashPortBuilder v0.5.0, Generate an SDL2 port project (+6 more)

### Community 65 - "PlaceCmd"
Cohesion: 0.18
Nodes (11): PlaceCmd, character, clipActions, clipDepth, cxform, depth, flags, matrix (+3 more)

### Community 66 - "AVM1Source"
Cohesion: 0.14
Nodes (15): AVM1Source, frame, label, length, offset, spriteId, ClipAction, actionLength (+7 more)

### Community 67 - "ABCTrait"
Cohesion: 0.18
Nodes (11): ABCTrait, attrs, index, kind, metadata, name, slotId, typeName (+3 more)

### Community 68 - "loadSWFDocument"
Cohesion: 0.24
Nodes (15): path, forEachSWF(), binaryDataPayloads(), collectTags(), size_t, uint16_t, uint8_t, vector (+7 more)

### Community 69 - "FlashRuntime.hpp"
Cohesion: 0.15
Nodes (12): ButtonRecord, character, cxform, depth, matrix, states, ClipObject, ColorTransform (+4 more)

### Community 70 - "EditTextDef"
Cohesion: 0.14
Nodes (14): EditTextDef, align, bounds, color, flags, font, height, indent (+6 more)

### Community 71 - "MeshVertex"
Cohesion: 0.15
Nodes (13): int32_t, uint8_t, vector, Mesh, indices, texture, vertices, MeshVertex (+5 more)

### Community 72 - "runtime/Audio.cpp"
Cohesion: 0.32
Nodes (13): lock, play, playing, playingSound, positionMs, setVoice, stop, stopAll (+5 more)

### Community 73 - "FunctionObject"
Cohesion: 0.15
Nodes (11): FunctionObject, boundThis, declaring, isMethodClosure, method, scope, NativeFn, MethodRef (+3 more)

### Community 74 - "Timer"
Cohesion: 0.15
Nodes (13): Args, weak_ptr, Task, frame, target, Timer, args, fn (+5 more)

### Community 75 - "uint32_t"
Cohesion: 0.17
Nodes (12): ExceptionInfo, from, target, to, typeName, varName, uint32_t, ScriptEntry (+4 more)

### Community 76 - "Matrix"
Cohesion: 0.24
Nodes (20): ChildIt, ColorTransform, Matrix, uint16_t, Geometry, bounds, characterBounds, characterHit (+12 more)

### Community 77 - "extractFrameScripts"
Cohesion: 0.35
Nodes (11): act(), avm1Actions(), avm2Actions(), FrameAction, Kind, optional, uint32_t, vector (+3 more)

### Community 78 - "BinaryDataSummary"
Cohesion: 0.25
Nodes (8): BinaryDataSummary, characterId, embeddedSwf, magic, payloadBytes, shared_ptr, size_t, uint16_t

### Community 79 - "SoundPlayDef"
Cohesion: 0.15
Nodes (14): Env, left, pos, right, uint16_t, uint32_t, uint8_t, vector (+6 more)

### Community 80 - "FontDef"
Cohesion: 0.13
Nodes (15): FontDef, ascent, byCode, descent, emSquare, glyphs, leading, GlyphDef (+7 more)

### Community 81 - "uint8_t"
Cohesion: 0.18
Nodes (11): FrameAction, frame, kind, label, uint8_t, Vertex, rgba, u (+3 more)

### Community 82 - "DecodedBitmap"
Cohesion: 0.15
Nodes (11): DecodedBitmap, decoded, error, height, image, original, originalExtension, width (+3 more)

### Community 83 - "RGBA"
Cohesion: 0.17
Nodes (14): Edge, control, curved, from, to, RGBA, a, b (+6 more)

### Community 84 - "Rect"
Cohesion: 0.20
Nodes (9): int32_t, Rect, xmax, xmin, ymax, ymin, SWFReader, analyzeBytes (+1 more)

### Community 85 - "vector"
Cohesion: 0.28
Nodes (9): int32_t, vector, Mesh, indices, texture, vertices, ShapeDef, bounds (+1 more)

### Community 86 - "Matrix"
Cohesion: 0.17
Nodes (7): Matrix, a, b, c, d, tx, ty

### Community 87 - "Character"
Cohesion: 0.18
Nodes (11): Character, bounds, file, frames, hasBounds, height, kind, names (+3 more)

### Community 88 - "decodePNG"
Cohesion: 0.38
Nodes (9): be32(), size_t, uint32_t, uint8_t, vector, decodeGIF(), decodePNG(), inflateStream() (+1 more)

### Community 89 - "Painter"
Cohesion: 0.20
Nodes (10): int32_t, Painter, color, inv, linear, texture, uOffset, uScale (+2 more)

### Community 90 - "Inverse"
Cohesion: 0.20
Nodes (10): Matrix, Inverse, a, b, c, d, ok, tx (+2 more)

### Community 91 - "UnpackResult"
Cohesion: 0.40
Nodes (5): uint8_t, vector, UnpackResult, method, swf

### Community 92 - "FrameDef"
Cohesion: 0.09
Nodes (24): AbcBlock, bytes, flags, name, ButtonDef, actions, records, sound (+16 more)

### Community 93 - "ColorTransform"
Cohesion: 0.20
Nodes (10): ColorTransform, aAdd, aMul, bAdd, bMul, gAdd, gMul, rAdd (+2 more)

### Community 94 - "encodePNG"
Cohesion: 0.57
Nodes (7): chunk(), uint32_t, uint8_t, vector, decodeJPEG(), encodePNG(), put32()

### Community 95 - "ABCInstance"
Cohesion: 0.25
Nodes (8): ABCInstance, flags, iinit, interfaces, name, protectedNs, superName, traits

### Community 96 - "vector"
Cohesion: 0.15
Nodes (12): ABCClass, cinit, traits, ABCMetadata, items, name, ABCScript, init (+4 more)

### Community 97 - "GradientAtlas"
Cohesion: 0.29
Nodes (7): GradientAtlas, byKey_, nextId_, textureFor, textures_, map, uint32_t

### Community 98 - "MethodInfo"
Cohesion: 0.25
Nodes (8): MethodInfo, body, flags, name, optionals, paramCount, paramTypes, returnType

### Community 99 - "SoundDef"
Cohesion: 0.20
Nodes (8): int16_t, SoundDef, channels, frames, loaded, pcm, rate, z

### Community 100 - "run"
Cohesion: 0.09
Nodes (25): Context, depth, locals, original, pool, registers, scope, target (+17 more)

### Community 101 - "ABCSummary"
Cohesion: 0.22
Nodes (9): ABCSummary, bytecodeBytes, classes, methodBodies, methods, name, scripts, uint32_t (+1 more)

### Community 102 - "BitmapDef"
Cohesion: 0.29
Nodes (7): BitmapDef, failed, height, texture, width, zlib, SDL_Texture

### Community 103 - "disassembleAVM1"
Cohesion: 0.53
Nodes (5): avm1ActionName(), vector, formatAVM1Action(), quote(), disassembleAVM1()

### Community 104 - "ABCMethod"
Cohesion: 0.22
Nodes (9): ABCMethod, body, flags, name, optionals, paramNames, paramTypes, returnType (+1 more)

### Community 105 - "tessellateShape"
Cohesion: 0.36
Nodes (8): BitmapInfo, file, height, width, map, uint16_t, makePainter(), tessellateShape()

### Community 106 - "ButtonRecord"
Cohesion: 0.15
Nodes (15): Button, actions, records, trackAsMenu, ButtonRecord, character, cxform, depth (+7 more)

### Community 107 - "InstanceInfo"
Cohesion: 0.25
Nodes (8): InstanceInfo, flags, iinit, interfaces, name, protectedNs, superName, traits

### Community 108 - "Matrix"
Cohesion: 0.29
Nodes (7): Matrix, a, b, c, d, tx, ty

### Community 109 - "MoviePack.cpp"
Cohesion: 0.43
Nodes (6): collectAudio(), ostream, le16(), parseButton(), printMoviePackReport(), readSoundInfo()

### Community 110 - "initSlots"
Cohesion: 0.67
Nodes (3): Object, initSlots, traitsOf

### Community 112 - "PlaceCmd"
Cohesion: 0.50
Nodes (4): PlaceCmd, depth, place, type

### Community 113 - "Multiname"
Cohesion: 0.16
Nodes (19): arrayIndex(), size_t, Definition, ns, script, value, Multiname, anyName (+11 more)

## Knowledge Gaps
- **986 isolated node(s):** `kind`, `name`, `kind`, `name`, `ns` (+981 more)
  These have ≤1 connection - possible missing edges or undocumented components. (Counts symbols only; 1204 node(s) total have ≤1 connection when file, concept and rationale nodes are included.)

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **Why does `string` connect `string` to `installFlashImpl`, `writeMoviePack`, `SWFSummary`, `FlashRuntime.cpp`, `VM`, `DisplayObject`, `Font`, `FillStyle`, `Clip`, `Reader`, `VM`, `MoviePackReport`, `AVM2.cpp`, `SWFDocument`, `EditText`, `Trait`, `execute`, `AVM2Method`, `ScriptFunction`, `Options`, `BitReader`, `Object`, `Frame`, `Movie`, `AVM1Clip.cpp`, `DisasmReport`, `AVM2Code.cpp`, `ABCFile`, `main`, `ByteReader`, `In`, `installBuiltinsImpl`, `Value`, `Cursor`, `PCMSound`, `analyzeBuffer`, `AbcFile`, `Class`, `Disassembler.cpp`, `AVM1Function`, `installBuiltins`, `Audio`, `AssetExtractor.cpp`, `AVM1Action`, `Decoder`, `unpackLoader`, `FrameScriptInfo`, `PlaceObject`, `AVM2.hpp`, `AssetReport`, `ImageRGBA`, `PlaceCmd`, `AVM1Source`, `loadSWFDocument`, `FlashRuntime.hpp`, `EditTextDef`, `MeshVertex`, `Matrix`, `extractFrameScripts`, `BinaryDataSummary`, `uint8_t`, `DecodedBitmap`, `Rect`, `Character`, `decodePNG`, `UnpackResult`, `FrameDef`, `encodePNG`, `vector`, `GradientAtlas`, `MethodInfo`, `SoundDef`, `run`, `ABCSummary`, `disassembleAVM1`, `tessellateShape`, `Multiname`?**
  _High betweenness centrality (0.696) - this node is a cross-community bridge._
- **Why does `VM` connect `VM` to `installFlashImpl`, `Object`, `FlashRuntime.cpp`, `DisplayObject`, `Value`, `Timer`, `AbcFile`, `uint32_t`, `AVM2.cpp`, `Class`, `initSlots`, `Multiname`, `Trait`, `execute`, `AVM2.hpp`, `AVM2Code.cpp`, `string`?**
  _High betweenness centrality (0.098) - this node is a cross-community bridge._
- **Why does `Movie` connect `Movie` to `SoundDef`, `FlashRuntime.cpp`, `FlashRuntime.hpp`, `BitmapDef`, `EditTextDef`, `Reader`, `Matrix`, `FontDef`, `uint8_t`, `Renderer`, `vector`, `Matrix`, `Options`, `FrameDef`, `string`?**
  _High betweenness centrality (0.068) - this node is a cross-community bridge._
- **Are the 5 inferred relationships involving `DisplayObject` (e.g. with `installFlashImpl()` and `place`) actually correct?**
  _`DisplayObject` has 5 INFERRED edges - model-reasoned connections that need verification._
- **Are the 2 inferred relationships involving `Clip` (e.g. with `installFlashImpl()` and `main()`) actually correct?**
  _`Clip` has 2 INFERRED edges - model-reasoned connections that need verification._
- **What connects `kind`, `name`, `kind` to the rest of the system?**
  _986 weakly-connected nodes found - possible documentation gaps or missing edges._
- **Should `installFlashImpl` be split into smaller, more focused modules?**
  _Cohesion score 0.056576576576576575 - nodes in this community are weakly interconnected._