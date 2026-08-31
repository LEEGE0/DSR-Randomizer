# DSR for MOD 재배포 인수인계 — v0.1.0-alpha.2

## 현재 상태

재배포 파이프라인 구현과 Task 5 재검토는 코드 HEAD `478a0b3`에서 승인됐다. 승인 시 검증은 관리형 435/435, 네이티브 15/15였으며, 이전 계획에 적힌 관리형 385개는 역사적 기준선일 뿐 현재 수가 아니다. Task 6의 최종 전체 실행은 테스트 파일 변경 없이 관리형 436/436을 발견했고 네이티브는 계속 15/15였다. 최종 보고는 더 최근의 436개를 사용한다.

배포 빌드는 저장소 루트에서 다음 한 경로로 만든다.

```powershell
pwsh -NoProfile -File packaging/build-release.ps1 -Version 0.1.0-alpha.2 -OutputPath artifacts
```

출력 파일은 다음 두 개이며 소스 관리에는 추가하지 않는다.

```text
artifacts/DSR-for-MOD-v0.1.0-alpha.2-win-x64.zip
artifacts/DSR-for-MOD-v0.1.0-alpha.2-win-x64.zip.sha256
```

빌드할 때마다 ZIP이 교체되므로 함께 생성된 sidecar 체크섬만 그 ZIP에 유효하다.

Task 6 최종 빌드의 정확한 릴리스 증거는 다음과 같다.

```text
ZIP 크기: 110428304 bytes
ZIP SHA-256: a3377a6d9cf1bc72046ce8b39f44442111f1db9b7c221ff218078d3f0aed57cc
브리지 SHA-256: c2906e98a47fef145b24ff3a85840aa3c67eb3eef8c00b6624ba93dd25ebc2c0
호스트 SHA-256: 3d6a62b4be7a8ad1f8c6992f05e7ebd761e19358be37118fe484042679d12da8
```

새 고유 임시 디렉터리에 독립 추출해 12개 경로, 엄격한 네 속성 매니페스트, 브리지/호스트 해시, 패키지 validator, 금지 경로와 개인정보 바이트 0건을 확인한 뒤 그 검증용 추출본만 정리했다. sidecar도 위 ZIP SHA-256과 정확히 일치한다.

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
- 브리지 호스트 Release 빌드는 디버그 레코드를 넣지 않는다. Task 6의 바이트 단위 개인정보 검사는 첫 빌드에 포함된 로컬 PDB 경로를 거부했고, 이 설정으로 다시 만든 ZIP에서는 해당 바이트가 0건이었다.
- staging과 새 ZIP 추출본 모두 패키지 validator를 통과해야 하며, ZIP 엔트리의 중복·루트 경로·역슬래시 별칭·점/상위 경로를 거부한다.

## 라이선스 주의

브리지 호스트는 commit `55b08a3c02a03777cf19958d8f6aa18d7af59da1`의 SoulsFormatsNEXT 소스를 링크한다. 바이너리 ZIP은 완전한 corresponding-source 묶음이 아니다. ZIP을 다시 배포하는 사람은 GPL-3.0에 맞는 방식으로 정확한 빌드의 프로젝트 소스, 빌드 스크립트, 실제 고정 SoulsFormatsNEXT 소스와 고지문을 함께 제공하거나 제공 가능하게 해야 한다. 자세한 내용은 `THIRD_PARTY_NOTICES.md`에 있다.

## 남은 운영 위험

- 이 버전은 alpha이며 실제 수령자 PC의 Randomizer 패키지 구조와 Steam Offline Mode는 자동으로 공급하거나 증명하지 않는다.
- 자동화 검증은 실제 게임 플레이 성공을 대신하지 않는다. 수령자 환경에서는 설치 안내를 따른 제어된 오프라인 실행 확인이 여전히 필요하다.
- 외부 루트를 지우면 개인 `.rmm`, 생성 결과, 로그도 사라진다. 제거 전 사용자가 보관할 `.rmm`은 별도의 비공개 위치에 백업해야 한다.
- 생성된 외부 루트나 복사 런타임을 다시 압축해 배포하면 안 된다. 배포 대상은 공식 빌더가 만든 12경로 ZIP과 그 체크섬뿐이다.
