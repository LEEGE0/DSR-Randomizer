# DSR for MOD 설치 안내

이 배포 파일은 Windows x64용 DSR for MOD 런처와 프로젝트 자체 런타임 구성 요소만 포함합니다. Dark Souls Remastered 게임 파일, 개인 저장 파일, Steam ID, Item Randomizer, Enemy Randomizer, Mod Engine, `DS1HeapPatch.dll`은 포함하지 않습니다.

## 준비물

1. Steam에서 정품 Dark Souls Remastered를 설치하고 파일 무결성 검사를 완료합니다.
2. Item Randomizer는 [공식 GitHub 릴리스](https://github.com/HotPocketRemix/DarkSoulsItemRandomizer/releases)에서 직접 받습니다.
3. Enemy Randomizer는 [공식 Nexus Mods 파일 페이지](https://www.nexusmods.com/darksoulsremastered/mods/922?tab=files)에서 직접 받습니다.
4. Enemy Randomizer 폴더는 압축을 푼 구조 그대로 유지합니다. 그 폴더에 동봉된 호환 Mod Engine과 `DS1HeapPatch.dll`을 다른 버전으로 바꾸거나 따로 떼어 놓지 마십시오.
5. 게임 복사본을 저장할 새 외부 루트를 준비합니다. 원본 게임과 별도의 위치여야 하며 약 9 GB 이상의 여유 공간이 필요합니다.

## 설치 순서

1. ZIP을 새 폴더에 풀고 `DSRForMod.Launcher.exe`를 실행합니다.
2. 런처에서 새 외부 루트를 선택한 뒤, Steam의 Dark Souls Remastered 설치 경로를 검증하고 복사 런타임 초기화를 실행합니다. 원본 설치 폴더를 외부 루트로 선택하지 마십시오.
3. 초기화된 활성 런타임의 `Mods` 폴더 아래에 사용자가 받은 Randomizer 파일을 둡니다. 런처는 `DS1EnemyRandomizer.exe`가 들어 있는 폴더를 하나만 찾아야 하며, 다음 파일이 같은 Enemy Randomizer 배포 구조 안에 있어야 합니다.

   ```text
   <외부 루트>\runtimes\runtime-...\Mods\<Randomizer 폴더>\
     DarkSoulsItemRandomizer.exe
     DS1EnemyRandomizer.exe
     config_randomizer.toml
     dist1\ModEngine\modengine2_launcher.exe
     dist1\ModEngine\modengine2\bin\modengine2.dll
     dist1\DLL\DS1HeapPatch.dll
   ```

4. 필요하면 런처에서 Item Randomizer와 Enemy Randomizer를 실행해 사용자 자신의 복사 런타임에 설정과 결과를 생성합니다.
5. Steam을 사용자가 직접 오프라인 모드로 전환합니다. 런처는 Steam 상태나 네트워크 차단을 대신 확인하지 않습니다.
6. 자신의 Steam 저장 프로필을 명시적으로 선택하고 모드 런타임을 실행합니다. 런처는 패키지에 포함된 RMM 브리지와 호스트를 외부 루트에 설치하고 해시를 검증한 뒤에만 Mod Engine을 시작합니다.

## 저장 데이터 및 공유 금지

- 다른 사람의 `.sl2`, `.rmm`, Steam ID 폴더를 복사하거나 공유하지 마십시오.
- 생성된 seed와 spoiler 파일에는 플레이 정보가 포함될 수 있으므로 배포 ZIP에 넣거나 다른 사용자 설치에 복사하지 마십시오.
- 정상 저장인 `.sl2`는 원본 입력으로 보호됩니다. 모드 실행은 외부 루트의 전용 `.rmm`을 사용합니다.
- 이 프로젝트의 ZIP에는 타사 Randomizer 실행 파일, Enemy Randomizer의 Mod Engine 포크, `DS1HeapPatch.dll`이 번들되지 않으며 자동으로 다운로드되지도 않습니다. 반드시 위 공식 링크에서 사용자가 직접 받아야 합니다.
