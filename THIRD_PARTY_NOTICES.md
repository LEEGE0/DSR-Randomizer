# Third-party notices

The release packaging script appends the exact license and third-party-notice files from the resolved .NET 8 Windows x64 runtime packages used by the self-contained build. Their package names and versions are read from the published dependency manifest; the notices are not taken from an unrelated system runtime.

The launcher uses the .NET runtime and Windows Desktop runtime under the MIT License, copyright .NET Foundation and contributors.

xUnit, Microsoft.NET.Test.Sdk, and coverlet are development/test dependencies and are not included in the release archive. Their respective license terms remain available from their official package repositories.

MinHook v1.3.4, pinned at commit `c3fcafdc10146beb5919319d0683e44e3c30d537`, is used by the native runtime under the BSD 2-Clause License:

Copyright (C) 2009-2017 Tsuda Kageyu. All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

Dark Souls, Dark Souls Remastered, and related marks and game content belong to their respective owners. This project does not redistribute game files.
