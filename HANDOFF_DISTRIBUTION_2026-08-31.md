# DSR for MOD 재배포 인수인계 — v0.1.0-alpha.2

## 현재 상태

재배포 파이프라인 구현과 Task 5 재검토는 코드 HEAD `478a0b3`에서 승인됐다. 승인 시 검증은 관리형 435/435, 네이티브 15/15였으며, 이전 계획의 385개는 역사적 기준선이다. Task 6 배포 검토에서 단일 파일 호스트의 무허가 DrSwizzler 포함을 발견했고, fix commit `a77a7d2`가 TPF/DrSwizzler 및 사용하지 않는 BouncyCastle 런타임을 제외하는 프로젝트 소유 SoulsFormats subset, 구조적 bundle/deps 검사, deterministic corresponding-source builder를 추가했다. 독립 privacy gate에서 subset의 Release CodeView path를 추가로 거부해 fix commit `4497070`이 Release-only symbol suppression과 Debug-PDB 보존 검사를 추가했다. 대응 소스 재검토 뒤 fix commit `24db7b0`이 ZstdNet/Zstandard의 정확한 upstream tree를 고정하고, 세 submodule 전체를 소스 ZIP에 넣으며, main/submodule 바이너리-소스 동일성 검사를 fail-closed로 추가했다. 최종 빌드가 260자를 넘는 staging 경로의 Win32 file lease 문제를 드러냈고 fix commit `8bfdf8d`가 extended-path 검증과 회귀 검사를 추가했다. 현재 전체 기준은 관리형 445개와 네이티브 15개다.

배포 빌드는 저장소 루트에서 다음 한 경로로 만든다.

```powershell
pwsh -NoProfile -File packaging/build-release.ps1 -Version 0.1.0-alpha.2 -OutputPath artifacts
```

출력 파일은 다음 네 개이며 소스 관리에는 추가하지 않는다.

```text
artifacts/DSR-for-MOD-v0.1.0-alpha.2-win-x64.zip
artifacts/DSR-for-MOD-v0.1.0-alpha.2-win-x64.zip.sha256
artifacts/DSR-for-MOD-v0.1.0-alpha.2-source.zip
artifacts/DSR-for-MOD-v0.1.0-alpha.2-source.zip.sha256
```

빌드할 때마다 두 ZIP이 교체되므로 각각 함께 생성된 sidecar 체크섬만 해당 ZIP에 유효하다. 바이너리 ZIP은 정확히 12경로를 유지하고, 별도 소스 ZIP은 committed `HEAD`와 실제 고정 SoulsFormatsNEXT, ZstdNet, Zstandard tree로 만든다. 소스 ZIP의 `SOURCE_REVISIONS.json`은 해당 main commit과 세 submodule commit을 엄격한 스키마로 식별한다.

최종 검토에 사용할 두 archive의 크기/해시는 `.superpowers/sdd/2026-08-31-redistributable-release/task-6-report.md`에 기록한다. 추적 문서 안에 source ZIP hash를 넣으면 그 문서 자체가 source ZIP을 바꾸는 자기참조가 되므로 여기에는 source hash를 고정하지 않는다.

최종 검증은 바이너리를 새 고유 임시 디렉터리에 독립 추출해 12개 경로, 엄격한 네 속성 매니페스트, bridge/host hash, package validator, 금지 경로와 개인정보 byte를 검사한다. source ZIP도 별도로 경로 정렬/중복/상위 이동/fixed timestamp/세 upstream tree의 exact entry/필수 소스/금지 디렉터리/checksum을 확인하고, 추출한 소스에서 bridge host를 restore/build한다.

## 정확한 ZIP 경계

ZIP에는 다음 12개 경로만 들어간다.

```text
CHANGELOG.md
DSRForMod.Launcher.exe
INSTALL_KO.md
LICENSE
README.md
THIRD_PARTY_NOTICES.md
components/rmm-bridge/DSRRandomizer.RmmBridge.dll
components/rmm-bridge/DSRRandomizer.RmmBridgeHost.exe
components/rmm-bridge/deployment-manifest.json
config/compatibility-profiles.json
native/DSRRandomizer.Runtime.dll
native/DSRRandomizer.Runtime.dll.sha256
```

`HANDOFF_DISTRIBUTION_2026-08-31.md`와 `docs/superpowers`의 설계/계획 문서는 저장소 기록이며 ZIP 내용이 아니다.

## 포함되는 것과 공급 경계

포함되는 실행 구성요소는 프로젝트가 소유한 런처, 가드, RMM 브리지 DLL과 자체 포함형 호스트다. 런처는 패키지의 가드/프로필/브리지/호스트 정체성을 빌드 시 고정하고, 패키지 검증과 실행 직전 검증을 다시 수행한다.

다음 항목은 번들, 다운로드, 설치, 미러링하지 않는다.

- Item Randomizer
- Enemy Randomizer
- Enemy Randomizer 패키지의 호환 Mod Engine 포크
- `DS1HeapPatch.dll`
- Dark Souls Remastered 실행 파일/에셋/복사 런타임
- Steam ID, `.sl2`, `.rmm`, seed, spoiler, 로그, 프로필, 스테이징

수령자는 [Item Randomizer 공식 릴리스](https://github.com/HotPocketRemix/DarkSoulsItemRandomizer/releases)와 [Enemy Randomizer 공식 파일 페이지](https://www.nexusmods.com/darksoulsremastered/mods/922?tab=files)에서 직접 받고 `INSTALL_KO.md`의 정확한 복사 런타임 구조에 배치한다.

## 실행 경로

1. 수령자가 기존 새 로컬 폴더를 외부 루트로 선택하고 런처를 재시작한다.
2. Steam 정품 설치를 검증하고 별도 복사 런타임을 초기화한다.
3. 수령자가 Item Randomizer를 먼저, Enemy Randomizer를 마지막으로 실행한다.
4. Enemy Randomizer의 `Launch DS1`은 사용하지 않고 DSR for MOD의 `Launch modded copy`로 돌아온다.
5. 런처가 프로젝트 브리지/호스트만 외부 루트에 설치 또는 복구하고, 엄격한 네 속성 매니페스트와 해시를 검증한다.
6. 사용자 제공 `config_randomizer.toml`, Mod Engine, `DS1HeapPatch.dll`을 검증한 뒤 브리지 설정을 새로 만들고 Mod Engine을 시작한다.
7. 브리지 호스트가 GameParam 병합과 전용 `.rmm` 세션을 준비한 뒤에만 게임이 준비 상태가 된다.

Steam Offline Mode는 수령자가 직접 설정해야 한다. 런처는 네트워크를 차단하거나 Steam 상태를 증명하지 않는다.

## 구현·검증 핵심

- 프로젝트 브리지와 호스트는 새 외부 루트에도 세이브/프로필/Randomizer 선행 상태 없이 설치할 수 있다.
- 설치는 소스 해시와 엄격한 매니페스트를 확인하고, 임시 형제 파일을 내구성 있게 쓴 뒤 DLL/호스트를 교체하고 매니페스트를 마지막에 쓴다.
- 대상 루트/조상/자식의 재분석 지점과 경로 이탈을 거부하고, 설치 후 세 파일을 다시 열어 검증한다.
- 런처는 설치된 DLL과 호스트의 정체성을 다시 확인하고 두 파일의 교체를 막는 lease를 Mod Engine 시작까지 유지한다.
- 네이티브 통합 검증은 별도 이름의 테스트 전용 브리지와 실제 실행되는 합성 호출 지점을 사용한다. Production 브리지의 고정 게임 정체성 검증은 완화하지 않았다.
- 릴리스 빌더는 GUID가 붙은 fail-if-exists 작업/스테이징/추출 디렉터리만 사용하고, 물리적 루트와 자식을 고정·검증하며, 검증된 작업 디렉터리만 수동 정리한다.
- 브리지 호스트 Release 빌드는 private absolute PDB path를 넣지 않는다. Task 6의 byte-level privacy scan은 첫 빌드의 local PDB path를 거부했고, Debug build의 유용한 symbol 출력은 유지한다.
- bridge-host 전용 SoulsFormats subset은 `Formats/TPF` 전체와 DrSwizzler를 제외한다. 사용하지 않는 BouncyCastle runtime도 제외하며, 필요한 BND3/PARAM/DCX_DFLT 경로와 ZstdNet/libzstd는 유지한다. official host의 .NET v6 bundle manifest와 embedded deps JSON을 직접 파싱해 제외 항목을 검증한다.
- 공식 빌드 전에 main `HEAD`가 committed/clean인지 확인하고 nonignored untracked 입력을 거부한다. 재귀 submodule은 모두 초기화되고 각 gitlink와 정확히 일치하며 clean해야 한다. 바이너리 staging 뒤와 소스 archive 직전에도 동일 조건을 재검사하고, 네 산출물이 모두 성공하기 전에는 최종 경로로 게시하지 않는다. `artifacts`, `bin`, `obj` 같은 ignored 생성물은 이 검사에서 허용된다.
- package validator의 Win32 file lease는 extended path를 사용한다. 따라서 reparse/identity 검사를 완화하지 않고도 260자를 넘는 안전한 고유 staging 경로를 검증한다.
- staging과 새 ZIP 추출본 모두 패키지 validator를 통과해야 하며, ZIP 엔트리의 중복·루트 경로·역슬래시 별칭·점/상위 경로를 거부한다.

## 라이선스 주의

브리지 호스트는 commit `55b08a3c02a03777cf19958d8f6aa18d7af59da1`의 SoulsFormatsNEXT 소스를 수정된 subset으로 컴파일한다. 수정 고지는 `THIRD_PARTY_NOTICES.md`와 subset project에 있다. 12경로 바이너리 ZIP은 corresponding source가 아니므로 정확한 별도 source ZIP/checksum과 함께 전달하거나, GPL이 허용하는 same-place gratis source access를 유지해야 한다. source ZIP에는 단순 gitlink가 아니라 실제 고정 SoulsFormatsNEXT 전체, ZstdNet commit `c90152918f633e945f163652e6368001556784e7`의 managed source/project, Zstandard commit `b706286adbba780006a47ef92df0ad7a785666b6`의 native source/build 입력이 들어간다.

Windows에서 source ZIP을 재빌드할 때는 드라이브 루트나 시스템 임시 루트에 가까운 짧은 경로에 압축을 푼다. 깊은 경로는 컴파일 전에 레거시 MSBuild 경로 길이 제한에 걸릴 수 있다. 정확한 restore/build 명령은 source ZIP 안의 `README.md`에 있다.

## 남은 운영 위험

- 이 버전은 alpha이며 실제 수령자 PC의 Randomizer 패키지 구조와 Steam Offline Mode는 자동으로 공급하거나 증명하지 않는다.
- 자동화 검증은 실제 게임 플레이 성공을 대신하지 않는다. 수령자 환경에서는 설치 안내를 따른 제어된 오프라인 실행 확인이 여전히 필요하다.
- 외부 루트를 지우면 개인 `.rmm`, 생성 결과, 로그도 사라진다. 제거 전 사용자가 보관할 `.rmm`은 별도의 비공개 위치에 백업해야 한다.
- 생성된 외부 루트나 복사 런타임을 다시 압축해 배포하면 안 된다. 배포 대상은 공식 빌더가 만든 12경로 binary ZIP/checksum과 exact corresponding-source ZIP/checksum 묶음이다.
