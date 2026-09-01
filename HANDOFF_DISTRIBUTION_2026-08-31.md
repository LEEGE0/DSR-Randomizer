# DSR for MOD 재배포 인수인계 — v0.1.0-alpha.2

## 현재 상태

재배포 파이프라인 구현과 Task 5 재검토는 코드 HEAD `478a0b3`에서 승인됐다. 승인 시 검증은 관리형 435/435, 네이티브 15/15였으며, 이전 계획의 385개는 역사적 기준선이다. Task 6 검토 과정에서 DrSwizzler 제거, 정확한 Zstd 대응 소스, main/submodule 동일성, 다중 표현 privacy 검사, 긴 경로 검증을 추가했다. 여러 loose 산출물을 transaction으로 게시하는 설계는 반복 검토에서 불필요하게 넓은 crash/ownership 표면을 드러냈다. 구조 전환 fix commit `7429093`은 해당 state machine과 journal을 제거하고, 검증된 내부 바이너리/소스 ZIP과 `SHA256SUMS.txt`를 포함하는 하나의 authoritative outer ZIP만 게시하도록 바꿨다. 후속 fix commit `bfa6440`은 pending/final 객체를 열린 Windows handle에 결속하고 legacy cleanup 입력을 검증된 version 하나로 제한했다. 최종 단순화 fix commit `3530676`은 불필요한 whole backup/rollback/failed-canonical 상태를 모두 제거하고, 완전히 검증된 pending handle의 atomic rename 하나만 commit point로 정했다. 현재 전체 기준은 관리형 447개와 네이티브 15개다.

배포 빌드는 저장소 루트에서 다음 한 경로로 만든다.

```powershell
pwsh -NoProfile -File packaging/build-release.ps1 -Version 0.1.0-alpha.2 -OutputPath artifacts
```

최종 출력 파일은 다음 하나이며 소스 관리에는 추가하지 않는다.

```text
artifacts/DSR-for-MOD-v0.1.0-alpha.2-redistributable.zip
```

외부 ZIP에는 fixed timestamp/ordinal order로 `DSR-for-MOD-v0.1.0-alpha.2-source.zip`, `DSR-for-MOD-v0.1.0-alpha.2-win-x64.zip`, `SHA256SUMS.txt` 세 경로만 들어간다. `SHA256SUMS.txt`는 두 내부 ZIP의 exact byte hash와 파일명을 묶는다. 외부 ZIP의 sidecar는 만들지 않으며 최종 SHA-256은 report에 별도로 기록한다. 내부 바이너리 ZIP은 정확히 12경로를 유지하고, 내부 소스 ZIP은 committed `HEAD`와 실제 고정 SoulsFormatsNEXT, ZstdNet, Zstandard tree로 만든다. `SOURCE_REVISIONS.json`은 해당 main commit과 세 submodule commit을 엄격한 스키마로 식별한다.

최종 검토에 사용할 outer/inner archive의 크기와 해시는 `.superpowers/sdd/2026-08-31-redistributable-release/task-6-report.md`에 기록한다. 추적 package 문서 안에 source ZIP hash를 넣으면 그 문서 자체가 source ZIP을 바꾸는 자기참조가 되므로 여기에는 source hash를 고정하지 않는다.

최종 검증은 먼저 outer ZIP의 정확한 세 entry/순서/timestamp/manifest hash를 검사한다. 그 exact inner 바이너리를 새 고유 임시 디렉터리에 독립 추출해 12개 경로, 엄격한 네 속성 매니페스트, bridge/host hash, package validator, 금지 경로와 개인정보 byte를 검사한다. exact inner source도 경로 정렬/중복/상위 이동/fixed timestamp/세 upstream tree의 exact entry/필수 소스/금지 디렉터리/hash를 확인하고, 추출한 소스에서 bridge host를 restore/build한다.

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
- 공식 빌드 전에 main `HEAD`가 committed/clean인지 확인하고 nonignored untracked 입력을 거부한다. 재귀 submodule은 모두 초기화되고 각 gitlink와 정확히 일치하며 clean해야 한다. 바이너리 staging 뒤와 소스 archive 직전에도 동일 조건을 재검사하고, 두 내부 ZIP과 outer 검증이 모두 성공하기 전에는 최종 경로로 게시하지 않는다. `artifacts`, `bin`, `obj` 같은 ignored 생성물은 이 검사에서 허용된다.
- 소스와 바이너리 ZIP의 모든 엔트리는 알려진 reviewed profile/account marker를 plain, JSON-escaped, forward-slash/URI, UTF-8, UTF-16LE, UTF-16BE 형태로 검사한다. 프로젝트 소유 테스트 fixture는 중립 합성 값만 사용하며, 세 upstream submodule은 읽기 전용으로 별도 검사한다.
- 마지막 package/privacy/source gate 뒤 두 내부 ZIP의 expected SHA-256을 고정하고, 두 exact leased byte와 strict LF-only `SHA256SUMS.txt`로 deterministic outer ZIP을 만든다. 게시 완료까지 canonical output root의 빈 `.dsr-release-publication.lock`을 배타적으로 유지하고, 경쟁 publisher는 canonical 출력에 손대지 않은 채 `PUBLICATION_IN_PROGRESS`로 실패한다. 게시자는 gated outer를 안정된 lease로 열고 같은 volume의 pending file을 READ/DELETE 권한과 write/delete 공유 거부를 가진 handle로 생성한다. 그 한 handle로 byte 복사와 `Flush(true)`를 끝낸다. `BeforeHandleRename` hook 뒤 즉시 같은 handle의 regular/non-reparse/single-link identity, expected SHA-256, exact outer semantics를 다시 확인한다. 그 뒤 `SetFileInformationByHandle(FileRenameInfo)`가 성공하면 fallible path 조회 전에 그 사실을 기록하며, 그 native 성공이 유일한 commit point다. 그 전 실패는 기존 canonical을 byte-exact하게 보존하거나 첫 게시의 부재를 유지한다. Commit 뒤에는 whole backup, rollback candidate, failed-canonical 이름, transaction directory, journal이 모두 없다. Rename된 final handle을 성공 반환 경계까지 열어 write/delete 교체를 막는다. 비싼 SHA-256/outer 검사를 먼저 수행한 뒤 regular/non-reparse/single-link와 exact canonical path identity를 마지막 fallible 검사로 수행한다. 따라서 byte scan 중 5 ms 지연으로 추가된 hostile hard-link alias도 마지막 검사에서 거부하고, arbitrary pathname rollback 없이 같은 handle로 canonical link만 제거해 alias-mutable 출력을 승인하지 않는다. 비필수 post-commit 실패는 committed-new로 보고한 뒤에도 이 마지막 검사를 수행한다. Final handle을 dispose한 뒤 동일 사용자 프로세스가 alias를 만드는 경우는 이 좁은 live-success 보증 밖이며, 그 시점부터 같은 권한의 프로세스가 산출물을 바꿀 수 있다. 성공 뒤 legacy cleanup은 caller path/name을 받지 않고 검증된 version에서 과거 네 loose ZIP/sidecar exact path만 도출하며 regular/single-link/non-reparse일 때만 삭제한다.
- package validator의 Win32 file lease는 extended path를 사용한다. 따라서 reparse/identity 검사를 완화하지 않고도 260자를 넘는 안전한 고유 staging 경로를 검증한다.
- staging과 새 ZIP 추출본 모두 패키지 validator를 통과해야 하며, ZIP 엔트리의 중복·루트 경로·역슬래시 별칭·점/상위 경로를 거부한다.

## 라이선스 주의

브리지 호스트는 commit `55b08a3c02a03777cf19958d8f6aa18d7af59da1`의 SoulsFormatsNEXT 소스를 수정된 subset으로 컴파일한다. 수정 고지는 `THIRD_PARTY_NOTICES.md`와 subset project에 있다. 12경로 inner 바이너리 ZIP은 corresponding source가 아니므로 정확한 inner source ZIP과 이를 함께 묶은 authoritative outer 전체를 전달해야 한다. Source ZIP에는 단순 gitlink가 아니라 실제 고정 SoulsFormatsNEXT 전체, ZstdNet commit `c90152918f633e945f163652e6368001556784e7`의 managed source/project, Zstandard commit `b706286adbba780006a47ef92df0ad7a785666b6`의 native source/build 입력이 들어간다.

Windows에서 source ZIP을 재빌드할 때는 드라이브 루트나 시스템 임시 루트에 가까운 짧은 경로에 압축을 푼다. 깊은 경로는 컴파일 전에 레거시 MSBuild 경로 길이 제한에 걸릴 수 있다. 정확한 restore/build 명령은 source ZIP 안의 `README.md`에 있다.

## 남은 운영 위험

- 이 버전은 alpha이며 실제 수령자 PC의 Randomizer 패키지 구조와 Steam Offline Mode는 자동으로 공급하거나 증명하지 않는다.
- 자동화 검증은 실제 게임 플레이 성공을 대신하지 않는다. 수령자 환경에서는 설치 안내를 따른 제어된 오프라인 실행 확인이 여전히 필요하다.
- 외부 루트를 지우면 개인 `.rmm`, 생성 결과, 로그도 사라진다. 제거 전 사용자가 보관할 `.rmm`은 별도의 비공개 위치에 백업해야 한다.
- 생성된 외부 루트나 복사 런타임을 다시 압축해 배포하면 안 된다. 배포 대상은 공식 빌더가 만든 `DSR-for-MOD-v0.1.0-alpha.2-redistributable.zip` 한 파일뿐이다.
