# Third-party notices

The release packaging script appends the exact license and third-party-notice files from the resolved .NET 8 Windows x64 runtime packages used by the self-contained build. Their package names and versions are read from the published dependency manifest; the notices are not taken from an unrelated system runtime.

The launcher uses the .NET runtime and Windows Desktop runtime under the MIT License, copyright .NET Foundation and contributors.

xUnit, Microsoft.NET.Test.Sdk, and coverlet are development/test dependencies and are not included in the release archive. Their respective license terms remain available from their official package repositories.

Dark Souls, Dark Souls Remastered, and related marks and game content belong to their respective owners. This project does not redistribute game files.
