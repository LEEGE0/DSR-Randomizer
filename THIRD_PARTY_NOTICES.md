# Third-party notices

## Bundled components

The `v0.1.0-alpha.2` binary release bundles the project-owned launcher, native guard, RMM bridge DLL, and self-contained RMM bridge host. The packaging script appends the exact license and third-party-notice files from the resolved .NET 8 Windows x64 runtime packages used by that self-contained build. Package names and versions are read from the exact published dependency manifest, not from an unrelated system runtime.

The launcher and bridge host use the .NET runtime and Windows Desktop runtime under the MIT License, copyright .NET Foundation and contributors.

SoulsFormatsNEXT, from https://github.com/soulsmods/SoulsFormatsNEXT.git, is linked from source into the managed RMM bridge host at commit `55b08a3c02a03777cf19958d8f6aa18d7af59da1`. It is copyright Joseph Anderson and contributors and licensed under the GNU General Public License, version 3. The repository source tree pins it under `third_party/SoulsFormatsNEXT` and includes the GPL-3.0 license text.

The binary release ZIP is not a corresponding-source archive. Anyone redistributing the bridge-host binary must also provide, or make available by a GPL-3.0-compliant method, the complete corresponding source for the exact distributed build: this project's relevant source and build scripts, the pinned SoulsFormatsNEXT source rather than only a submodule pointer, and the applicable license notices. Do not redistribute the binary ZIP alone if you cannot satisfy that obligation.

For the selected `netstandard2.1` target, the pinned SoulsFormatsNEXT restore graph resolves these runtime NuGet dependencies: BouncyCastle.Cryptography 2.4.0, DrSwizzler 1.1.1, System.Text.Encoding.CodePages 4.7.0, System.ValueTuple 4.5.0, and ZstdNet 1.4.5, plus transitive System.Runtime.CompilerServices.Unsafe 4.7.0. Their respective license terms remain available in their NuGet packages and official package repositories. The release builder also appends the exact .NET runtime third-party notices that apply to the published self-contained executable.

MinHook v1.3.4, pinned at commit `c3fcafdc10146beb5919319d0683e44e3c30d537`, is used by the native runtime under the BSD 2-Clause License:

Copyright (C) 2009-2017 Tsuda Kageyu. All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

xUnit, Microsoft.NET.Test.Sdk, and coverlet are development/test dependencies and are not included in the release archive. Their respective license terms remain available from their official package repositories.

## Recipient-supplied third-party tools

Item Randomizer, Enemy Randomizer, the compatible Mod Engine fork distributed with Enemy Randomizer, and `DS1HeapPatch.dll` are not included, downloaded, installed, mirrored, or relicensed by DSR for MOD. Recipients obtain and place them themselves using the [official Item Randomizer releases](https://github.com/HotPocketRemix/DarkSoulsItemRandomizer/releases) and [official Enemy Randomizer files](https://www.nexusmods.com/darksoulsremastered/mods/922?tab=files). Their absence from this notice's bundled-component list is intentional.

Dark Souls, Dark Souls Remastered, related marks, game executables, assets, and saves belong to their respective owners. This project does not redistribute game files or personal game data.
