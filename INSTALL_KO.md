# DSR for MOD v0.1.0-alpha.2 설치 안내

이 문서는 배포 ZIP을 받은 사람이 자신의 Steam 정품 Dark Souls Remastered와 자신이 직접 받은 Randomizer를 사용해 별도의 모드 런타임을 만드는 절차입니다.

## 이 ZIP에 포함된 것과 포함되지 않은 것

ZIP에는 DSR for MOD 프로젝트가 소유한 런처, 네이티브 오프라인/저장 가드, 호환성 프로필, RMM 브리지 DLL, 자체 포함형 브리지 호스트, 배포 매니페스트와 문서만 들어 있습니다. 압축을 푼 뒤에는 `DSRForMod.Launcher.exe`, `native`, `config`, `components`의 상대 배치를 바꾸지 마십시오.

다음 타사 파일은 **번들되지 않으며, 런처가 다운로드하거나 설치하지도 않습니다.**

- Item Randomizer
- Enemy Randomizer
- Enemy Randomizer 패키지에 들어 있는 호환 Mod Engine 포크
- `DS1HeapPatch.dll`

각 사용자가 아래 공식 배포처에서 직접 받아야 합니다.

- Item Randomizer: [공식 GitHub 릴리스](https://github.com/HotPocketRemix/DarkSoulsItemRandomizer/releases)
- Enemy Randomizer: [공식 Nexus Mods 파일 페이지](https://www.nexusmods.com/darksoulsremastered/mods/922?tab=files)

Enemy Randomizer 압축은 받은 구조 그대로 보관하십시오. 그 패키지의 Mod Engine과 `DS1HeapPatch.dll`을 임의의 다른 버전으로 교체하지 마십시오.

## 준비

1. Windows x64 PC에서 Steam 정품 Dark Souls Remastered를 설치하고 Steam의 파일 무결성 검사를 완료합니다.
2. 게임 복사본을 둘 새 로컬 폴더를 만듭니다. 이 폴더가 `<external-root>`이며, Steam 설치 폴더와 서로 겹치지 않아야 합니다. 네트워크/UNC 경로, 심볼릭 링크, 정션, 재분석 지점은 사용하지 마십시오.
3. `<external-root>`에는 약 9 GB 이상의 여유 공간을 준비합니다. 세이브, 로그, 생성 결과도 모두 이 루트 아래에 쌓입니다.
4. 배포 ZIP과 같은 위치의 `.sha256` 파일이 있다면 압축을 풀기 전에 PowerShell에서 해시를 비교합니다.

   ```powershell
   Get-FileHash .\DSR-for-MOD-v0.1.0-alpha.2-win-x64.zip -Algorithm SHA256
   Get-Content .\DSR-for-MOD-v0.1.0-alpha.2-win-x64.zip.sha256
   ```

## 런처와 복사 런타임 초기화

1. ZIP을 비어 있는 새 폴더에 풉니다. Steam 게임 폴더나 기존 Randomizer 폴더에 덮어 풀지 마십시오.
2. 압축을 푼 폴더에서 다음 명령으로 12개 허용 파일과 내장 해시/매니페스트를 검사할 수 있습니다. 종료 코드가 0이어야 합니다.

   ```powershell
   .\DSRForMod.Launcher.exe --validate-package .
   ```

3. `DSRForMod.Launcher.exe`를 실행합니다. `External material root`에 새 `<external-root>`를, `Dark Souls Remastered installation`에 Steam 원본 설치 폴더를 입력합니다.
4. `Save external root`를 누르고 런처를 완전히 종료한 뒤 다시 실행합니다. 외부 루트를 바꾼 세션에서는 재시작 전까지 재료 작업이 비활성화됩니다.
5. `Verify installation`으로 지원되는 정품 설치인지 확인한 다음 `Create external runtime`을 누릅니다. 런처가 검증된 게임 파일만 `<external-root>\runtimes\runtime-<id>`에 복사하고 활성 런타임 매니페스트를 만듭니다.

Steam 원본 폴더는 읽기 전용 입력입니다. 프로젝트 네이티브 가드와 호환성 프로필은 압축을 푼 DSR for MOD 폴더에 그대로 남으며, 런처가 내장 SHA-256과 비교한 뒤 복사 게임을 시작할 때 사용합니다. 가드를 Steam 폴더나 Randomizer 폴더에 수동 복사하지 마십시오.

## 사용자가 준비한 Randomizer 배치

활성 런타임의 `Mods` 아래에는 `DS1EnemyRandomizer`라는 이름의 디렉터리가 정확히 하나 있어야 합니다. 이 디렉터리는 `Mods` 바로 아래 또는 한 개의 패키지 디렉터리 아래에 둘 수 있습니다.

```text
<external-root>\
  runtimes\
    runtime-<id>\
      DarkSoulsRemastered.exe
      Mods\
        [선택적 패키지 폴더]\
          DS1EnemyRandomizer\
            DarkSoulsItemRandomizer.exe
            DS1EnemyRandomizer.exe
            config_randomizer.toml
            dist1\Vanilla\GameParam.parambnd.dcx
            dist1\Defs\*.xml
            dist1\ModEngine\modengine2_launcher.exe
            dist1\ModEngine\modengine2\bin\modengine2.dll
            dist1\DLL\DS1HeapPatch.dll
            param\GameParam\GameParam.parambnd.dcx
```

- Item Randomizer 파일은 `DarkSoulsItemRandomizer.exe`가 위 `DS1EnemyRandomizer` 디렉터리에 오도록 사용자가 직접 배치합니다.
- Enemy Randomizer의 `dist1`, `param`, Mod Engine, `DS1HeapPatch.dll` 구조는 유지합니다.
- `Mods` 아래에 `DS1EnemyRandomizer.exe` 복사본이나 백업을 여러 개 두지 마십시오. 런처는 정확히 하나만 허용합니다.
- 이 배치는 사용자가 소유한 로컬 설치입니다. DSR for MOD ZIP에 합쳐서 재배포하면 안 됩니다.

## Randomizer 실행과 모드 시작

1. 런처의 `Launch Item Randomizer`를 눌러 활성 복사 런타임에 Item 결과를 먼저 내보냅니다.
2. 런처의 `Launch Enemy Randomizer`를 눌러 Enemy Randomizer를 마지막으로 실행합니다. `Merge files from game directory`를 켠 상태로 Randomize를 완료합니다.
3. Enemy Randomizer의 `Launch DS1` 버튼은 사용하지 않습니다. 작업을 마치고 Enemy Randomizer를 닫은 뒤 DSR for MOD 런처로 돌아옵니다.
4. Steam을 사용자가 직접 **Offline Mode**로 전환합니다. 런처는 Steam 상태를 확인하지 않고, 네트워크를 차단하거나 방화벽/어댑터 설정을 바꾸지 않습니다.
5. `Dedicated mod save`에서 자신의 정확한 Steam ID 프로필을 선택하고 `Launch modded copy`를 누릅니다.

모드 시작 직전에 런처는 패키지에 포함된 프로젝트 소유 RMM 브리지 DLL과 호스트만 `<external-root>\components\rmm-bridge`에 설치하거나 복구합니다. 소스와 설치 대상의 해시 및 정확히 네 속성인 배포 매니페스트를 다시 검증하고, 검증된 DLL/호스트 파일을 열린 상태로 유지한 뒤에만 사용자 제공 Mod Engine을 시작합니다. 이 과정은 Item/Enemy Randomizer, Mod Engine 또는 `DS1HeapPatch.dll`을 설치하지 않습니다.

브리지 호스트는 사용자 제공 Randomizer 결과와 Steam Overhaul을 읽어 `<external-root>\components\rmm-bridge\content` 아래에 병합 결과를 만들고, 게임 준비 완료 전에 검증합니다. 런처는 사용자 제공 `config_randomizer.toml`을 바탕으로 `<external-root>\staging\diagnostics\config-randomizer-bridged.toml`을 새로 만들며, 원본 Randomizer 설정을 배포 파일로 취급하지 않습니다.

## 세이브 격리

- 모드 세이브는 `<external-root>\saves\<SteamID>\DRAKS0005.rmm`만 사용합니다.
- 유효한 `.rmm`이 있으면 정상 `.sl2`를 열지 않고 그대로 재사용합니다.
- `.rmm`이 없을 때만 사용자가 선택한 정상 `DRAKS0005.sl2`를 읽기 전용으로 한 번 복사하고 검증한 뒤 원자적으로 `.rmm`으로 게시합니다.
- 정상 `.sl2`는 쓰기, 이름 변경, 교체, 삭제 대상이 아닙니다. 브리지 준비나 저장 격리 검증이 실패하면 게임 시작은 중단됩니다.

## 설치 확인과 문제 해결

- `DSRForMod.Launcher.exe --status`로 선택된 외부 루트와 활성 런타임 준비 상태를 확인할 수 있습니다.
- `--validate-package`가 실패하면 ZIP을 다시 받고 체크섬부터 확인하십시오. 개별 DLL/EXE를 인터넷에서 따로 구해 덮어쓰지 마십시오.
- `RMM_BRIDGE_BUNDLE_INVALID`는 압축을 푼 패키지의 브리지 소스 또는 매니페스트가 없거나 해시가 다르다는 뜻입니다.
- `RMM_BRIDGE_INSTALL_FAILED`는 선택한 외부 루트에 안전하게 설치할 수 없다는 뜻입니다. 권한, 남은 공간, 링크/정션 여부와 실행 중인 게임/호스트를 확인하십시오.
- `RMM_BRIDGE_INSTALL_TAMPERED`는 설치 후 재검증이 실패했다는 뜻입니다. 게임을 시작하지 않은 상태로 패키지 체크섬과 외부 루트를 다시 확인하십시오.
- `<external-root>\components\rmm-bridge`만 임의로 지워도 다음 모드 시작 때 런처가 프로젝트 브리지를 다시 설치합니다. 타사 파일은 복구하지 않습니다.

## 롤백과 제거

바닐라로 돌아가려면 게임과 브리지 호스트를 종료하고 Steam에서 원본 게임을 실행하면 됩니다. DSR for MOD는 Steam 원본을 수정하지 않습니다.

완전히 제거하려면 먼저 자신의 `.rmm` 중 보관할 파일을 비공개 위치에 백업한 뒤 다음 항목을 각각 정확히 확인해 삭제합니다.

1. 사용자가 선택한 `<external-root>` 전체
2. 압축을 푼 DSR for MOD 폴더
3. `%LOCALAPPDATA%\DSR-Randomizer`의 작은 외부 루트 선택 정보
4. 사용자가 직접 복사한 Item/Enemy Randomizer 로컬 파일

Steam의 Dark Souls Remastered 설치 폴더와 정상 세이브 폴더는 제거 대상으로 선택하지 마십시오. `<external-root>`를 삭제하면 그 안의 `.rmm`, 생성 결과, 로그와 스테이징도 함께 사라집니다.

## 절대 재배포하지 말아야 할 것

- 자신의 실제 Steam ID 또는 Steam ID가 들어간 디렉터리/설정/메타데이터
- `DRAKS0005.sl2`, `DRAKS0005.rmm` 및 다른 저장/백업 파일
- 생성한 seed, spoiler 또는 Randomizer 결과 폴더
- `logs`, `profile`, `saves`, `staging` 디렉터리와 그 내용
- `runtime-current.json`, 선택 프로필, 세션 상태처럼 개인 경로나 상태가 들어간 파일
- `DarkSoulsRemastered.exe`와 Steam 게임 데이터/에셋, 복사 런타임 전체
- Item Randomizer, Enemy Randomizer, 호환 Mod Engine 포크, `DS1HeapPatch.dll`

공유할 수 있는 것은 프로젝트가 만든 원본 배포 ZIP과 그 체크섬뿐입니다. 자신의 외부 루트나 초기화된 런타임을 다시 압축해 전달하지 마십시오.
