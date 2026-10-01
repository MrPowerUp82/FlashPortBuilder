# Graph Report - FlashPortBuilder  (2026-10-01)

## Corpus Check
- 50 files · ~83,667 words
- Verdict: corpus is large enough that graph structure adds value.

## Summary
- 2060 nodes · 4400 edges · 115 communities (114 shown, 1 thin omitted)
- Extraction: 91% EXTRACTED · 9% INFERRED · 0% AMBIGUOUS · INFERRED: 397 edges (avg confidence: 0.83)
- Token cost: 0 input · 0 output

## Community Hubs (Navigation)
- installFlashImpl
- writeMoviePack
- SWFSummary
- Player
- VM
- DisplayObject
- Font
- FillStyle
- Clip
- Reader
- VM
- ABCFile.hpp
- MoviePackReport
- AVM2.cpp
- Object
- SWFDocument
- EditText
- Trait
- Renderer
- Multiname
- AVM2Method
- ScriptFunction
- Options
- BitReader
- Value
- Frame
- Movie
- Seg
- AVM1Clip.cpp
- DisasmReport
- AVM2Code.cpp
- ABCFile
- main
- NativeData
- ByteReader
- Object
- MethodBody
- In
- installBuiltinsImpl
- Value
- Cursor
- AVM2Stats
- analyzeBuffer
- AbcFile
- Class
- Disassembler.cpp
- AVM1Function
- MorphRecord
- installBuiltins
- newClass
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
- FlashRuntime.hpp
- decodeBitmapTag
- ABCMethodBody
- string
- FlashPortBuilder v0.5.0
- PlaceCmd
- ABCFile.cpp
- ABCTrait
- loadSWFDocument
- ButtonRecord
- EditTextDef
- MeshVertex
- Payload
- FunctionObject
- Timer
- uint32_t
- Matrix
- extractFrameScripts
- BinaryDataSummary
- Interval
- FontDef
- uint8_t
- DecodedBitmap
- Edge
- .pub
- vector
- Matrix
- Character
- decodePNG
- Painter
- Inverse
- UnpackResult
- ClipActionDef
- MsbBits
- encodePNG
- ABCInstance
- vector
- tessellateShape
- MethodInfo
- ABCReader::analyze
- Context
- ABCSummary
- BitmapDef
- disassembleAVM1
- Task
- SlabFiller
- ButtonRecord
- AVM1.hpp
- HeavyShape
- ClipObject
- NativeFunction
- Button
- PlaceCmd
- Definition
- ProjectGenerator::generate

## God Nodes (most connected - your core abstractions)
1. `string` - 228 edges
2. `VM` - 110 edges
3. `DisplayObject` - 91 edges
4. `Clip` - 79 edges
5. `VM` - 62 edges
6. `Player` - 60 edges
7. `ABCFile` - 52 edges
8. `installFlashImpl()` - 44 edges
9. `Movie` - 43 edges
10. `SWFDocument` - 41 edges

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

## Communities (115 total, 1 thin omitted)

### Community 0 - "installFlashImpl"
Cohesion: 0.09
Nodes (51): arg(), as3ClipCreated(), as3KeyEvent(), as3Start(), as3Step(), classDerivesFrom(), clipOfValue(), Args (+43 more)

### Community 1 - "writeMoviePack"
Cohesion: 0.23
Nodes (10): int16_t, int32_t, uint32_t, deflate(), le16(), PackWriter, buf, parseButton() (+2 more)

### Community 2 - "SWFSummary"
Cohesion: 0.09
Nodes (25): map, uint8_t, vector, SWFSummary, abcBlocks, actionScript3, actualFileLength, binaryData (+17 more)

### Community 3 - "Player"
Cohesion: 0.06
Nodes (44): as3ClipRemoved(), as3FrameEntered(), as3PreAllocate(), ObjectPtr, VM, forEachClip(), hasHandler(), set (+36 more)

### Community 4 - "VM"
Cohesion: 0.05
Nodes (46): ClassBuilder, c, ClassPtr, deque, uint64_t, unique_ptr, VM, abcs_ (+38 more)

### Community 5 - "DisplayObject"
Cohesion: 0.06
Nodes (37): enable_shared_from_this<DisplayObject>, addChildAt, detach, setChildIndex, map, shared_ptr, DisplayObject, as3 (+29 more)

### Community 6 - "Font"
Cohesion: 0.08
Nodes (25): Font, advances, ascent, bold, codes, descent, emSquare, glyphs (+17 more)

### Community 7 - "FillStyle"
Cohesion: 0.06
Nodes (35): FillStyle, bitmapId, color, focal, interpolation, matrix, repeat, smooth (+27 more)

### Community 8 - "Clip"
Cohesion: 0.11
Nodes (34): PlaceCmd, as3MouseEvent(), applyProperties(), Clip, advance, attach, bornTick_, childAt (+26 more)

### Community 9 - "Reader"
Cohesion: 0.11
Nodes (21): Code, int16_t, int32_t, SDL_Texture, size_t, uint8_t, vector, load (+13 more)

### Community 10 - "VM"
Cohesion: 0.05
Nodes (82): ArrayObject, getOwn, items, keys, setOwn, Budget, left, ArrayObject (+74 more)

### Community 11 - "ABCFile.hpp"
Cohesion: 0.09
Nodes (27): ABCClass, cinit, traits, ABCMethod, body, flags, name, optionals (+19 more)

### Community 12 - "MoviePackReport"
Cohesion: 0.08
Nodes (24): vector, MoviePackReport, abcBlocks, actionBlocks, bitmaps, buttons, editTexts, frameActions (+16 more)

### Community 13 - "AVM2.cpp"
Cohesion: 0.16
Nodes (31): arrayIndex(), int32_t, size_t, Value, VM, indexOf, numberIndex(), buildTraits (+23 more)

### Community 14 - "Object"
Cohesion: 0.20
Nodes (5): enable_shared_from_this<Object>, map, Object, props, proto

### Community 15 - "SWFDocument"
Cohesion: 0.09
Nodes (31): EmbeddedBinary, bytes, characterId, size_t, uint16_t, uint32_t, uint8_t, vector (+23 more)

### Community 16 - "EditText"
Cohesion: 0.07
Nodes (29): EditText, align, autoSize, border, bounds, color, fontId, height (+21 more)

### Community 17 - "Trait"
Cohesion: 0.08
Nodes (28): NsKind, Kind, Namespace, kind, owner, uri, Trait, classIndex (+20 more)

### Community 18 - "Renderer"
Cohesion: 0.14
Nodes (14): SDL_Renderer, Renderer, height_, layerDepth_, layers_, maskBlend_, masksEnabled, premultipliedBlend_ (+6 more)

### Community 19 - "Multiname"
Cohesion: 0.14
Nodes (20): Object, Args, Multiname, anyName, attribute, name, nss, rtName (+12 more)

### Community 20 - "AVM2Method"
Cohesion: 0.10
Nodes (26): AVM2Block, end, firstInstr, handler, instrCount, scopeIn, stackIn, start (+18 more)

### Community 21 - "ScriptFunction"
Cohesion: 0.11
Nodes (16): pair, size_t, uint16_t, uint8_t, ScriptFunction, base, code, end (+8 more)

### Community 22 - "Options"
Cohesion: 0.11
Nodes (21): SDL_Renderer, uint64_t, vector, Options, capture, captureAfter, dump, ignoreStops (+13 more)

### Community 23 - "BitReader"
Cohesion: 0.16
Nodes (29): BitReader, bitPos_, data_, size_, int32_t, size_t, uint16_t, uint32_t (+21 more)

### Community 24 - "Value"
Cohesion: 0.11
Nodes (11): ObjectPtr, Type, SuperObject, ctor, thisv, Value, b, n (+3 more)

### Community 25 - "Frame"
Cohesion: 0.12
Nodes (18): ActionBlock, length, offset, ButtonCondAction, code, conditions, FrameAction, size_t (+10 more)

### Community 26 - "Movie"
Cohesion: 0.09
Nodes (20): SDL_Renderer, Movie, abcBlocks, background, bitmaps, buttons, editTexts, exports (+12 more)

### Community 27 - "Seg"
Cohesion: 0.18
Nodes (12): vector, Seg, wind, x0, x1, y0, y1, X (+4 more)

### Community 28 - "AVM1Clip.cpp"
Cohesion: 0.16
Nodes (22): arg(), clipGotoFrame(), ClipObject::getOwn(), ClipObject::keys(), ClipObject::setOwn(), clipValue(), copyInit(), Args (+14 more)

### Community 29 - "DisasmReport"
Cohesion: 0.09
Nodes (21): DisasmReport, abcBlocks, avm1Actions, avm1ActionUse, avm1Blocks, avm1Functions, avm1Sources, avm1SourcesWithErrors (+13 more)

### Community 30 - "AVM2Code.cpp"
Cohesion: 0.18
Nodes (18): array, buildOpTable(), int32_t, uint32_t, uint8_t, decodeAVM2Body(), Effect, pop (+10 more)

### Community 31 - "ABCFile"
Cohesion: 0.10
Nodes (20): ABCFile, bodies, classes, doubles, floats, instances, ints, major (+12 more)

### Community 32 - "main"
Cohesion: 0.26
Nodes (13): abcBodies(), abcBytes(), abcMethods(), path, uint64_t, findRuntimeDir(), forEachSWF(), main() (+5 more)

### Community 33 - "NativeData"
Cohesion: 0.15
Nodes (12): map, Listener, capture, fn, priority, NativeData, listeners, TimerData (+4 more)

### Community 34 - "ByteReader"
Cohesion: 0.24
Nodes (5): ByteReader, int32_t, size_t, uint8_t, vector

### Community 35 - "Object"
Cohesion: 0.13
Nodes (15): ArrayObject, items, DictionaryObject, entries, enable_shared_from_this<Object>, pair, vector, Object (+7 more)

### Community 36 - "MethodBody"
Cohesion: 0.10
Nodes (21): int32_t, shared_ptr, uint8_t, Instr, a, b, op, target (+13 more)

### Community 37 - "In"
Cohesion: 0.22
Nodes (11): int32_t, size_t, uint16_t, uint32_t, uint8_t, unique_ptr, vector, In (+3 more)

### Community 38 - "installBuiltinsImpl"
Cohesion: 0.20
Nodes (20): arg(), asArray(), compareValues(), Args, ArrayObject, shared_ptr, size_t, uint32_t (+12 more)

### Community 39 - "Value"
Cohesion: 0.12
Nodes (9): ObjectPtr, T, Type, Value, b, n, o, s (+1 more)

### Community 40 - "Cursor"
Cohesion: 0.20
Nodes (8): int16_t, size_t, uint16_t, uint32_t, Cursor, d_, end_, utf8_

### Community 41 - "AVM2Stats"
Cohesion: 0.13
Nodes (14): AVM2Operands, AVM2OpInfo, name, operands, AVM2Stats, blocks, bodies, bodiesWithErrors (+6 more)

### Community 42 - "analyzeBuffer"
Cohesion: 0.26
Nodes (12): ABCReader, analyze, analyzeBuffer(), size_t, uint16_t, uint8_t, vector, isSwfMagic() (+4 more)

### Community 43 - "AbcFile"
Cohesion: 0.12
Nodes (16): AbcFile, bodies, classes, classObjects, doubles, floats, instances, ints (+8 more)

### Community 44 - "Class"
Cohesion: 0.11
Nodes (19): Class, abc, allocator, callAsFunction, iinit, index, instanceTraits, interfaces (+11 more)

### Community 45 - "Disassembler.cpp"
Cohesion: 0.19
Nodes (14): AVM2Writer, printed_, where_, ostream, path, set, uint32_t, vector (+6 more)

### Community 46 - "AVM1Function"
Cohesion: 0.12
Nodes (19): AVM1Block, actionCount, end, firstAction, start, successors, AVM1Function, actions (+11 more)

### Community 47 - "MorphRecord"
Cohesion: 0.11
Nodes (17): int32_t, Point, x, y, uint32_t, MorphRecord, control, curved (+9 more)

### Community 48 - "installBuiltins"
Cohesion: 0.21
Nodes (17): arg(), asArray(), Args, ArrayObject, Fn, ObjectPtr, shared_ptr, size_t (+9 more)

### Community 49 - "newClass"
Cohesion: 0.24
Nodes (10): ArrayObject, ObjectPtr, shared_ptr, uint32_t, vector, nsMatches(), findDefinition, newArray (+2 more)

### Community 50 - "AssetExtractor.cpp"
Cohesion: 0.25
Nodes (17): cleanJPEG(), convertSound(), ostream, path, uint16_t, uint8_t, vector, decodeADPCM() (+9 more)

### Community 51 - "AVM1Action"
Cohesion: 0.12
Nodes (17): AVM1Action, code, function, ints, length, offset, push, strings (+9 more)

### Community 52 - "Decoder"
Cohesion: 0.23
Nodes (10): AVM1Program, errors, functions, uint8_t, decodeAVM1(), Decoder, d_, prog_ (+2 more)

### Community 53 - "unpackLoader"
Cohesion: 0.35
Nodes (11): alchemyConstantArrays(), int32_t, optional, size_t, uint8_t, vector, describeKey(), parseAllABC() (+3 more)

### Community 54 - "FrameScriptInfo"
Cohesion: 0.12
Nodes (17): FrameAction, frame, kind, label, FrameScriptInfo, actions, avm1Scripts, avm2Scripts (+9 more)

### Community 55 - "PlaceObject"
Cohesion: 0.04
Nodes (49): AVM1Source, frame, label, length, offset, spriteId, ClipAction, actionLength (+41 more)

### Community 56 - "AVM2.hpp"
Cohesion: 0.16
Nodes (11): DynamicProps, entries, index, Entry, alive, key, value, size_t (+3 more)

### Community 57 - "TraitInfo"
Cohesion: 0.12
Nodes (17): InstanceInfo, flags, iinit, interfaces, name, protectedNs, superName, traits (+9 more)

### Community 58 - "AssetReport"
Cohesion: 0.12
Nodes (15): AssetReport, bitmaps, bitmapsFailed, bitmapsRaw, problems, shapes, shapesFailed, skipped (+7 more)

### Community 59 - "Tessellator.cpp"
Cohesion: 0.26
Nodes (10): addQuad(), uint32_t, flatten(), GradientAtlas::textureFor(), polyline(), Pt, x, y (+2 more)

### Community 60 - "FlashRuntime.hpp"
Cohesion: 0.15
Nodes (13): ClipObject, FrameDef, actions, cmds, initScripts, label, scripts, map (+5 more)

### Community 61 - "decodeBitmapTag"
Cohesion: 0.22
Nodes (14): ImageKind, uint8_t, vector, ImageRGBA, height, pixels, width, size_t (+6 more)

### Community 62 - "ABCMethodBody"
Cohesion: 0.13
Nodes (15): ABCException, excType, from, target, to, varName, ABCMethodBody, code (+7 more)

### Community 63 - "string"
Cohesion: 0.13
Nodes (25): string, map, ProjectGenerator, generate, ostringstream, lower(), int32_t, map (+17 more)

### Community 64 - "FlashPortBuilder v0.5.0"
Cohesion: 0.13
Nodes (14): Analyze a SWF, Build FlashPortBuilder, Compare several games, Compatibility examples, Disassemble AVM1/AVM2 code, Extract assets, FlashPortBuilder v0.5.0, Generate an SDL2 port project (+6 more)

### Community 65 - "PlaceCmd"
Cohesion: 0.13
Nodes (14): ColorTransform, add, mul, PlaceCmd, character, clipActions, clipDepth, cxform (+6 more)

### Community 66 - "ABCFile.cpp"
Cohesion: 0.21
Nodes (13): multinameLocal, namespaceName, ABCFile::multinameLocal(), ABCFile::multinameName(), ABCFile::namespaceName(), ABCFile::string(), TraitKind, uint32_t (+5 more)

### Community 67 - "ABCTrait"
Cohesion: 0.14
Nodes (14): ABCScript, init, traits, ABCTrait, attrs, index, kind, metadata (+6 more)

### Community 68 - "loadSWFDocument"
Cohesion: 0.25
Nodes (14): uint16_t, uint32_t, binaryDataPayloads(), collectTags(), size_t, uint16_t, uint8_t, vector (+6 more)

### Community 69 - "ButtonRecord"
Cohesion: 0.12
Nodes (17): ButtonDef, actions, records, trackAsMenu, ButtonRecord, character, cxform, depth (+9 more)

### Community 70 - "EditTextDef"
Cohesion: 0.14
Nodes (14): EditTextDef, align, bounds, color, flags, font, height, indent (+6 more)

### Community 71 - "MeshVertex"
Cohesion: 0.15
Nodes (13): int32_t, uint8_t, vector, Mesh, indices, texture, vertices, MeshVertex (+5 more)

### Community 72 - "Payload"
Cohesion: 0.31
Nodes (6): uint16_t, uint32_t, uint8_t, Payload, p, queueClipEvent

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
Cohesion: 0.17
Nodes (23): ChildIt, ColorTransform, Matrix, uint16_t, uint32_t, decodeUtf8(), Geometry, bounds (+15 more)

### Community 77 - "extractFrameScripts"
Cohesion: 0.35
Nodes (11): act(), avm1Actions(), avm2Actions(), FrameAction, Kind, optional, uint32_t, vector (+3 more)

### Community 78 - "BinaryDataSummary"
Cohesion: 0.17
Nodes (11): BinaryDataSummary, characterId, embeddedSwf, magic, payloadBytes, shared_ptr, size_t, uint16_t (+3 more)

### Community 79 - "Interval"
Cohesion: 0.25
Nodes (8): Interval, args, fn, method, next, once, period, thisv

### Community 80 - "FontDef"
Cohesion: 0.22
Nodes (9): FontDef, ascent, byCode, descent, emSquare, glyphs, leading, int16_t (+1 more)

### Community 81 - "uint8_t"
Cohesion: 0.18
Nodes (11): FrameAction, frame, kind, label, uint8_t, Vertex, rgba, u (+3 more)

### Community 82 - "DecodedBitmap"
Cohesion: 0.20
Nodes (10): DecodedBitmap, decoded, error, height, image, original, originalExtension, width (+2 more)

### Community 83 - "Edge"
Cohesion: 0.11
Nodes (23): BitmapInfo, file, height, width, Edge, control, curved, from (+15 more)

### Community 84 - ".pub"
Cohesion: 0.21
Nodes (11): ClassPtr, Code, NativeFn, classOf, defineGlobal, defineGlobalFunction, defineNativeClass, findClass (+3 more)

### Community 85 - "vector"
Cohesion: 0.28
Nodes (9): int32_t, vector, Mesh, indices, texture, vertices, ShapeDef, bounds (+1 more)

### Community 86 - "Matrix"
Cohesion: 0.22
Nodes (7): Matrix, a, b, c, d, tx, ty

### Community 87 - "Character"
Cohesion: 0.20
Nodes (10): Character, bounds, file, frames, hasBounds, height, kind, names (+2 more)

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

### Community 92 - "ClipActionDef"
Cohesion: 0.20
Nodes (10): AbcBlock, bytes, flags, name, ClipActionDef, code, events, keyCode (+2 more)

### Community 93 - "MsbBits"
Cohesion: 0.22
Nodes (6): uint32_t, le32(), MsbBits, d_, n_, pos_

### Community 94 - "encodePNG"
Cohesion: 0.47
Nodes (8): chunk(), uint32_t, uint8_t, vector, decodeJPEG(), encodePNG(), haveJPEGDecoder(), put32()

### Community 95 - "ABCInstance"
Cohesion: 0.25
Nodes (8): ABCInstance, flags, iinit, interfaces, name, protectedNs, superName, traits

### Community 96 - "vector"
Cohesion: 0.24
Nodes (6): ABCMetadata, items, name, pair, vector, optional

### Community 97 - "tessellateShape"
Cohesion: 0.23
Nodes (11): GradientAtlas, byKey_, nextId_, textureFor, textures_, map, uint32_t, map (+3 more)

### Community 98 - "MethodInfo"
Cohesion: 0.25
Nodes (8): MethodInfo, body, flags, name, optionals, paramCount, paramTypes, returnType

### Community 99 - "ABCReader::analyze"
Cohesion: 0.32
Nodes (7): ABCReader::analyze(), uint16_t, uint32_t, uint8_t, vector, skipConstantPool(), skipTraits()

### Community 100 - "Context"
Cohesion: 0.20
Nodes (10): Context, depth, locals, original, pool, registers, scope, target (+2 more)

### Community 101 - "ABCSummary"
Cohesion: 0.22
Nodes (9): ABCSummary, bytecodeBytes, classes, methodBodies, methods, name, scripts, uint32_t (+1 more)

### Community 102 - "BitmapDef"
Cohesion: 0.29
Nodes (7): BitmapDef, failed, height, texture, width, zlib, SDL_Texture

### Community 103 - "disassembleAVM1"
Cohesion: 0.36
Nodes (7): set, avm1ActionName(), vector, formatAVM1Action(), isTerminator(), quote(), disassembleAVM1()

### Community 104 - "Task"
Cohesion: 0.22
Nodes (9): Code, uint32_t, weak_ptr, Task, classInit, clipEvent, code, handler (+1 more)

### Community 105 - "SlabFiller"
Cohesion: 0.25
Nodes (8): Key, Mesh, Open, bottom, top, SlabFiller, evenOdd_, open_

### Community 106 - "ButtonRecord"
Cohesion: 0.25
Nodes (8): ButtonRecord, character, cxform, depth, matrix, states, ColorTransform, Matrix

### Community 107 - "AVM1.hpp"
Cohesion: 0.29
Nodes (5): deque, TextFieldObject, field, getOwn, setOwn

### Community 108 - "HeavyShape"
Cohesion: 0.33
Nodes (6): HeavyShape, id, ms, triangles, uint16_t, uint64_t

### Community 109 - "ClipObject"
Cohesion: 0.33
Nodes (5): ClipObject, clip, getOwn, keys, setOwn

### Community 110 - "NativeFunction"
Cohesion: 0.40
Nodes (3): Fn, NativeFunction, fn

### Community 111 - "Button"
Cohesion: 0.40
Nodes (5): Button, actions, records, trackAsMenu, vector

### Community 112 - "PlaceCmd"
Cohesion: 0.40
Nodes (5): uint8_t, PlaceCmd, depth, place, type

### Community 113 - "Definition"
Cohesion: 0.50
Nodes (4): Definition, ns, script, value

## Knowledge Gaps
- **906 isolated node(s):** `kind`, `name`, `kind`, `name`, `ns` (+901 more)
  These have ≤1 connection - possible missing edges or undocumented components. (Counts symbols only; 1107 node(s) total have ≤1 connection when file, concept and rationale nodes are included.)
- **1 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **Why does `string` connect `string` to `installFlashImpl`, `writeMoviePack`, `SWFSummary`, `Player`, `VM`, `DisplayObject`, `Font`, `Clip`, `Reader`, `VM`, `ABCFile.hpp`, `MoviePackReport`, `AVM2.cpp`, `Object`, `SWFDocument`, `EditText`, `Trait`, `Multiname`, `AVM2Method`, `ScriptFunction`, `Options`, `Value`, `Frame`, `Movie`, `AVM1Clip.cpp`, `DisasmReport`, `AVM2Code.cpp`, `ABCFile`, `main`, `NativeData`, `ByteReader`, `In`, `installBuiltinsImpl`, `Value`, `Cursor`, `AVM2Stats`, `analyzeBuffer`, `AbcFile`, `Class`, `Disassembler.cpp`, `AVM1Function`, `installBuiltins`, `AssetExtractor.cpp`, `AVM1Action`, `Decoder`, `unpackLoader`, `FrameScriptInfo`, `PlaceObject`, `AVM2.hpp`, `AssetReport`, `FlashRuntime.hpp`, `decodeBitmapTag`, `PlaceCmd`, `ABCFile.cpp`, `loadSWFDocument`, `EditTextDef`, `MeshVertex`, `Matrix`, `extractFrameScripts`, `BinaryDataSummary`, `Interval`, `uint8_t`, `DecodedBitmap`, `Edge`, `.pub`, `Character`, `decodePNG`, `UnpackResult`, `ClipActionDef`, `encodePNG`, `vector`, `tessellateShape`, `MethodInfo`, `ABCReader::analyze`, `Context`, `ABCSummary`, `disassembleAVM1`, `Task`, `AVM1.hpp`, `ProjectGenerator::generate`?**
  _High betweenness centrality (0.709) - this node is a cross-community bridge._
- **Why does `VM` connect `VM` to `Player`, `DisplayObject`, `AVM2.cpp`, `Trait`, `Multiname`, `NativeData`, `Object`, `MethodBody`, `Value`, `AbcFile`, `Class`, `newClass`, `AVM2.hpp`, `string`, `Timer`, `uint32_t`, `.pub`, `disassembleAVM1`, `Definition`?**
  _High betweenness centrality (0.097) - this node is a cross-community bridge._
- **Why does `DisplayObject` connect `DisplayObject` to `installFlashImpl`, `PlaceCmd`, `Player`, `VM`, `ButtonRecord`, `EditTextDef`, `Clip`, `VM`, `AVM1.hpp`, `Matrix`, `AVM1Clip.cpp`, `FlashRuntime.hpp`, `uint8_t`, `vector`, `Matrix`, `ClipActionDef`, `string`?**
  _High betweenness centrality (0.090) - this node is a cross-community bridge._
- **Are the 5 inferred relationships involving `DisplayObject` (e.g. with `installFlashImpl()` and `place`) actually correct?**
  _`DisplayObject` has 5 INFERRED edges - model-reasoned connections that need verification._
- **Are the 2 inferred relationships involving `Clip` (e.g. with `installFlashImpl()` and `main()`) actually correct?**
  _`Clip` has 2 INFERRED edges - model-reasoned connections that need verification._
- **What connects `kind`, `name`, `kind` to the rest of the system?**
  _906 weakly-connected nodes found - possible documentation gaps or missing edges._
- **Should `installFlashImpl` be split into smaller, more focused modules?**
  _Cohesion score 0.08563134978229318 - nodes in this community are weakly interconnected._