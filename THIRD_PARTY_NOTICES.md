# Third-party notices

## Bundled components and corresponding source

The `v0.1.0-alpha.2` binary release bundles the project-owned launcher, native guard, RMM bridge DLL, and self-contained RMM bridge host. The packaging script appends the exact license and third-party-notice files from the resolved .NET 8 Windows x64 runtime packages used by that self-contained build. Package names and versions are read from the exact published dependency manifest.

The launcher and bridge host use the .NET runtime and Windows Desktop runtime under the MIT License, copyright .NET Foundation and contributors. Their complete runtime notices are appended to this file inside the built binary archive.

SoulsFormatsNEXT, from https://github.com/soulsmods/SoulsFormatsNEXT.git, is compiled from source into the managed RMM bridge host at commit `55b08a3c02a03777cf19958d8f6aa18d7af59da1`. It is copyright Joseph Anderson and contributors and licensed under the GNU General Public License, version 3. Exact upstream source: https://github.com/soulsmods/SoulsFormatsNEXT/tree/55b08a3c02a03777cf19958d8f6aa18d7af59da1.

**Modified by DSR for MOD on 2026-09-01 to omit TPF/DrSwizzler support for the bridge-host build.** The project-owned subset project excludes every `Formats/TPF` source file. It also satisfies two unused BouncyCastle `using` directives with empty compile-time namespace declarations, so neither DrSwizzler nor BouncyCastle code, assembly, or package reference enters the published host. The pinned upstream checkout itself remains unmodified.

The corresponding source is emitted separately as:

```text
DSR-for-MOD-v0.1.0-alpha.2-source.zip
DSR-for-MOD-v0.1.0-alpha.2-source.zip.sha256
```

That deterministic archive is made from the committed main-repository tree and the actual contents of all three pinned upstream trees, not merely their submodule pointers:

- SoulsFormatsNEXT commit `55b08a3c02a03777cf19958d8f6aa18d7af59da1`: https://github.com/soulsmods/SoulsFormatsNEXT/tree/55b08a3c02a03777cf19958d8f6aa18d7af59da1
- ZstdNet commit `c90152918f633e945f163652e6368001556784e7`: https://github.com/skbkontur/ZstdNet/tree/c90152918f633e945f163652e6368001556784e7
- Zstandard commit `b706286adbba780006a47ef92df0ad7a785666b6`: https://github.com/facebook/zstd/tree/b706286adbba780006a47ef92df0ad7a785666b6

It includes the project solution, the subset project and modification notice, release/build scripts, applicable license files, the complete pinned SoulsFormatsNEXT source, the ZstdNet managed wrapper source/project files, and the Zstandard native source/build inputs. Its strict `SOURCE_REVISIONS.json` records the exact main commit and all three submodule commits. It excludes Git metadata, build outputs, generated release artifacts, and private working data.

The 12-path binary ZIP is not itself a corresponding-source archive. Convey the binary ZIP and checksum together with the exact source ZIP and checksum, or provide equivalent gratis access to that exact source at the same place, for as long as the GPL requires. Do not redistribute the binary alone unless you independently satisfy GPL-3.0 section 6 for the exact distributed build.

## ZstdNet 1.4.5

The bridge-host bundle includes `ZstdNet.dll` 1.4.5 and its x64/x86 `libzstd.dll` native payloads because the retained SoulsFormats DCX implementation supports Zstandard-compressed data. The exact signed NuGet package contains the following two notices.

Primary sources:

- ZstdNet wrapper: https://raw.githubusercontent.com/skbkontur/ZstdNet/c90152918f633e945f163652e6368001556784e7/LICENSE
- bundled Zstandard 1.4.5: https://raw.githubusercontent.com/facebook/zstd/b706286adbba780006a47ef92df0ad7a785666b6/LICENSE
- ZstdNet exact source tree: https://github.com/skbkontur/ZstdNet/tree/c90152918f633e945f163652e6368001556784e7
- Zstandard exact source tree: https://github.com/facebook/zstd/tree/b706286adbba780006a47ef92df0ad7a785666b6

### ZstdNet wrapper license

BSD License

For ZstdNet software

Copyright (c) 2016-present, SKB Kontur. All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

 * Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

 * Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

 * Neither the name SKB Kontur nor the names of its contributors may be used to
   endorse or promote products derived from this software without specific
   prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

### Zstandard license

BSD License

For Zstandard software

Copyright (c) 2016-present, Facebook, Inc. All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

 * Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

 * Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

 * Neither the name Facebook nor the names of its contributors may be used to
   endorse or promote products derived from this software without specific
   prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

`System.Text.Encoding.CodePages` is supplied in the published host by the .NET runtime pack already covered by the appended .NET notices. No separate CodePages, System.ValueTuple, or System.Runtime.CompilerServices.Unsafe NuGet package DLL is embedded. DrSwizzler and BouncyCastle are not bundled.

## MinHook 1.3.4

MinHook v1.3.4, pinned at commit `c3fcafdc10146beb5919319d0683e44e3c30d537`, is used by the native runtime under the BSD 2-Clause License:

Copyright (C) 2009-2017 Tsuda Kageyu. All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

xUnit, Microsoft.NET.Test.Sdk, and coverlet are development/test dependencies and are not included in the binary release archive.

## Recipient-supplied third-party tools

Item Randomizer, Enemy Randomizer, the compatible Mod Engine fork distributed with Enemy Randomizer, and `DS1HeapPatch.dll` are not included, downloaded, installed, mirrored, or relicensed by DSR for MOD. Recipients obtain and place them themselves using the [official Item Randomizer releases](https://github.com/HotPocketRemix/DarkSoulsItemRandomizer/releases) and [official Enemy Randomizer files](https://www.nexusmods.com/darksoulsremastered/mods/922?tab=files).

Dark Souls, Dark Souls Remastered, related marks, game executables, assets, and saves belong to their respective owners. This project does not redistribute game files or personal game data.
