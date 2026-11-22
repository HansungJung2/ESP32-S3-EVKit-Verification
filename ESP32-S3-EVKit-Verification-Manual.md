# ESP32-S3-WROOM-1 (N16R8) EV-Kit 검증 펌웨어 및 개발 매뉴얼

본 문서는 **ESP32-S3-WROOM-1 (N16R8)** 모듈을 처음 사용하는 개발자를 위한 하드웨어 사양 요약, 개발 환경 구축(컴파일러/툴체인), 빌드 및 펌웨어 다운로드 가이드, 그리고 본 검증 프로젝트에 구현된 기능 설명서입니다.

---

## 1. ESP32-S3 (N16R8) 하드웨어 자원 및 사양 정리

ESP32-S3는 Espressif사에서 출시한 고성능 2.4GHz Wi-Fi + BLE 5.0 듀얼 모드 MCU로, 특히 **AI 벡터 연산 가속(Vector Instructions)**과 **대용량 메모리 인터페이스(Octal SPI)**가 특징입니다.

### 📌 칩셋 및 모듈 스펙표
| 항목 | 사양 (Specification) | 상세 설명 |
| :--- | :--- | :--- |
| **MCU 칩셋** | **ESP32-S3** (Xtensa® 32-bit LX7 Dual-Core) | 최대 **240 MHz** 클럭, 7단계 파이프라인 |
| **내장 메모리 (SRAM)** | **512 KB SRAM** (384KB 내부 + 128KB RTC/캐시) | 칩 내장 초고속 내부 메모리 |
| **ROM** | 384 KB | 부트로더 및 기본 런타임 저장 |
| **Flash 메모리 (N16)** | **16 MB Quad/Octal SPI Flash** | 프로그램 펌웨어 및 파일시스템 영역 |
| **외장 RAM (R8)** | **8 MB Octal SPI (OPI) PSRAM** | 고용량 버퍼, 이미지 처리, 음성/AI 연산용 |
| **무선 통신 (Wi-Fi)** | **Wi-Fi 4 (802.11 b/g/n)** | 2.4 GHz 대역 (최대 150 Mbps), SoftAP / Station 동시 지원 |
| **무선 통신 (Bluetooth)**| **Bluetooth 5 (LE) + Mesh** | 2 Mbps High Speed, Coded PHY (Long Range) |
| **AI 벡터 가속** | Vector Instructions (SIMD) 내장 | 신경망 연산, 오디오 FFT, 머신러닝 가속 |
| **주요 주변장치 (I/O)** | GPIO, UART, SPI, I2C, I2S, LCD, DVP Camera, USB OTG | 총 45개 프로그래머블 GPIO |
| **내장 USB 컨트롤러** | USB 1.1 Full-Speed OTG + USB Serial/JTAG | 내장 USB CDC 및 JTAG 디버깅 기본 지원 |
| **온도 센서** | 내장 온칩 온도 센서 (`temperatureRead()`) | 실시간 칩 내부 다이(Die) 온도 측정 |

---

## 2. 개발 환경 & 컴파일러 종류

ESP32-S3를 개발할 때 선택할 수 있는 대표적인 툴체인은 3가지가 있습니다.

### ① PlatformIO Core / IDE (본 프로젝트 적용 / 가장 추천 ⭐)
* **컴파일러**: `xtensa-esp-elf-gcc` / `g++` (Xtensa 아키텍처 전용 GCC 14.x)
* **빌드 시스템**: SCons 기반 자동 의존성 관리
* **장점**: 
  - 보드별 라이브러리(`lib_deps`)와 툴체인을 프로젝트 단위로 격리 및 자동 다운로드.
  - CLI(`pio`) 및 VS Code GUI 확장 모두 지원하여 자동화 및 재현성이 뛰어남.

### ② ESP-IDF (Espressif IoT Development Framework)
* **컴파일러**: `xtensa-esp-elf-gcc` (CMake + Ninja 빌드 시스템)
* **장점**: Espressif 공식 네이티브 C/FreeRTOS 프레임워크. 가장 정밀한 하드웨어 레지스터/전력 제어가 가능하나 초보자에게 진입장벽이 높음.

### ③ Arduino IDE
* **컴파일러**: 동일하게 Xtensa GCC 사용
* **장점**: GUI가 간단하나, 라이브러리 버전 관리 및 대형 프로젝트 관리가 상대적으로 불편함.

---

## 3. 필수 설치 프로그램 리스트

### 필수 설치 항목
1. **Python 3.10 ~ 3.13** (설치 시 `Add Python to PATH` 반드시 체크)
2. **PlatformIO Core (CLI)**
   ```powershell
   pip install platformio
   ```
3. **USB-UART 드라이버 (CH343 / CH340)**
   - 본 개발 보드는 USB 통신 칩으로 **WCH CH343** 칩을 사용합니다.
   - 장치 관리자에서 포트가 `USB-Enhanced-SERIAL CH343(COMx)`로 인식되어야 합니다. (드라이버가 없을 경우 WCH 공식 홈페이지에서 CH343 VCP 드라이버 설치)
4. *(선택/권장)* **VS Code (Visual Studio Code)**
   - 확장 프로그램: `PlatformIO IDE` 설치 시 마우스 클릭으로 빌드/업로드 가능

---

## 4. 컴파일(빌드) 및 보드 다운로드(업로드) 방법

터미널(PowerShell 또는 CMD)을 열고 프로젝트 폴더로 이동합니다:

```powershell
cd "d:\AI_Code\ESP32-S3-WROOM-1_(N16R8)\ESP32-S3-EVKit-Verfication"
```

### 1) 빌드 (Compile Only)
```powershell
pio run
```
* 최초 실행 시 Xtensa GCC 툴체인 및 프레임워크가 자동 다운로드됩니다.
* 빌드 성공 시 Flash / RAM 사용량 퍼센트가 표시됩니다.

### 2) 보드 다운로드 (Upload / Flash)
```powershell
pio run --target upload
```
* 보드가 연결된 포트(`COM6`)로 펌웨어를 자동 플래싱합니다.
* **⚠️ 주의**: 다른 터미널에서 시리얼 모니터가 켜져 있으면 `PermissionError: 포트 사용 중` 에러가 발생하므로, 업로드 전에는 시리얼 모니터를 `Ctrl+C`로 종료해야 합니다.
* **부트로더 수동 진입 (필요 시)**:
  - 간혹 포트 인식이 안 될 경우 보드의 `BOOT` 버튼을 누른 상태에서 `EN(RST)` 버튼을 1회 누르고 `BOOT` 버튼을 놓으면 다운로드 모드로 강제 진입합니다.

### 3) 시리얼 모니터 확인
```powershell
pio device monitor
```
* 115200 보드레이트로 ESP32-S3와 시리얼 통신을 진행합니다. (종료: `Ctrl + C`)

---

## 5. 프로젝트 소스 코드 구조

```
ESP32-S3-EVKit-Verfication/
├── platformio.ini                    # 보드 설정, 플래시(16MB)/PSRAM(8MB OPI) 활성화, 라이브러리 의존성 정의
├── partitions_16MB.csv               # 16MB 플래시 파티션 맵 (Dual OTA 4MBx2 + 7MB LittleFS + 128KB MMAP)
├── ESP32-S3-EVKit-Verification-Manual.md # 본 매뉴얼 문서
└── src/
    ├── main.cpp                     # setup(), loop(), WebServer, FastLED, OTA/FS 웹 핸들러
    ├── debug_cli.h                  # CLI 셸 인터페이스 및 커맨드 구조체 정의
    ├── debug_cli.cpp                # 3중 채널(USB/BLE/Web) CLI 엔진 및 명령어 핸들러
    ├── mmap_storage.h               # 128KB Flash MMAP 비휘발성 스토리지 헤더
    ├── mmap_storage.cpp             # esp_partition_mmap 기반 0-copy 읽기 및 플래시 쓰기 드라이버
    ├── fs_manager.h                 # 7MB LittleFS 파일 시스템 관리자 헤더
    └── fs_manager.cpp               # LittleFS 마운트, 파일 열람/다운로드/업로드/포맷 드라이버
```

---

## 6. 구현된 기능 상세 설명

본 프로젝트는 단순 Hello World를 넘어, ESP32-S3의 모든 인터페이스(USB / 무선 BLE / Wi-Fi Web)를 동시에 검증할 수 있는 통합 프레임워크로 구현되었습니다.

### 1) STM32 스타일 CLI 셸 (3중 동시 채널 지원)
* **USB 시리얼**, **스마트폰 BLE 무선**, **웹 브라우저 터미널**에서 동일한 명령어를 입력하고 제어할 수 있습니다.
* 링 버퍼(Ring Buffer) 기반 논블로킹 수신, `argc`/`argv` 토큰 분리 파싱, 에코 및 백스페이스(`\b \b`) 기능 탑재.

| 명령어 | 형식 / 예시 | 동작 설명 |
| :--- | :--- | :--- |
| **`help`** | `help` | 등록된 모든 CLI 명령어 목록 출력 |
| **`top`** | `top` | **CPU 클럭, 칩 온도, SRAM 사용량, 8MB PSRAM 사용량, Wi-Fi RSSI 실시간 통계 출력** |
| **`fs`** | `fs list`<br>`fs cat <file>`<br>`fs rm <file>`<br>`fs write <file> <str>`<br>`fs sample`<br>`fs format` | **7MB LittleFS 파일 시스템 관리 (파일 목록/내용출력/삭제/기록/샘플생성/포맷)** |
| **`ota`** | `ota` 또는 `ota status` | **Dual OTA 펌웨어 상태 (현재 실행 파티션 `app0`/`app1`, 다음 타겟, 스케치 크기)** |
| **`psram`** | `psram status`<br>`psram alloc <MB>`<br>`psram free`<br>`psram speed` | **8MB PSRAM 제어 및 벤치마크 (메모리 동적 할당, 해제, Read/Write 전송 속도 측정)** |
| **`mmap`** | `mmap status`<br>`mmap write <string>`<br>`mmap dump [len]`<br>`mmap erase` | **128KB Flash MMAP 비휘발성 메모리 관리 (가상주소 매핑, 영구 데이터 기록/덤프)** |
| **`sysinfo`** | `sysinfo` | 칩셋 스펙, MAC 주소, IP 주소 및 상태 정보 출력 |
| **`led`** | `led <r> <g> <b>` | GPIO48 RGB LED를 수동 색상(0~255)으로 설정 (예: `led 255 0 0`) |
| **`rgb`** | `rgb <rainbow\|pulse\|off>` | LED 프리셋 모드(무지개 순환 / 블루 숨쉬기 / 소등)로 전환 |
| **`bright`** | `bright <0-255>` | LED 전체 밝기 변경 |
| **`wifi`** | `wifi scan`<br>`wifi connect <SSID> <PW>` | 주변 와이파이 검색 및 집/사무실 공유기 연결 |
| **`hello`** | `hello [count]` | 통신 테스트용 문자열 출력 |
| **`reboot`** | `reboot` | ESP32-S3 소프트웨어 재부팅 |

---

### 2) 온보드 WS2812B RGB LED 제어 (GPIO48)
* ESP32-S3 모듈의 내장 네오픽셀 LED(GPIO48)를 FastLED 라이브러리로 제어합니다.
* 50fps(20ms 주기)의 부드러운 HSV 무지개 회전 및 호흡 효과(Pulse) 지원.

---

### 3) BLE 5.0 Nordic UART Service (NUS 무선 CLI)
* 별도 페어링 절차 없이 스마트폰 앱(`Serial Bluetooth Terminal` 등)으로 즉시 연결 가능.
* 디바이스 이름: **`ESP32-S3-CLI`**
* 스마트폰에서 CLI 명령어를 전송하고 즉시 결과를 수신.

---

### 4) 자체 Wi-Fi AP + 실시간 반응형 Web 리소스 대시보드
* **자체 공유기 모드(SoftAP)**가 기본 실행되어 외부 공유기가 없어도 스마트폰/PC에서 즉시 접속 가능.
  - **Wi-Fi SSID**: `ESP32-S3-WIFI`
  - **비밀번호**: `12345678`
  - **접속 주소**: `http://192.168.4.1`
* **주요 웹 대시보드 기능**:
  - **📊 실시간 메모리 게이지 바**: SRAM Heap, 8MB Octal PSRAM, 16MB Flash, 7MB LittleFS 사용량(KB/MB) 및 퍼센트(%) 표시
  - **📈 30초 실시간 시계열 그래프**: 1초마다 Heap/PSRAM 사용률 및 칩 내부 온도(°C)를 Canvas로 동적 렌더링
  - **📁 7MB LittleFS 파일 관리자 (txt, json, csv)**:
    - 웹 브라우저에서 플래시 내 파일 목록(용량) 실시간 확인
    - **파일 다운로드**: `.txt`, `.json`, `.csv` 파일을 클릭 한 번으로 내 PC/스마트폰으로 다운로드
    - **파일 업로드**: PC의 텍스트/데이터 파일을 웹 폼을 통해 플래시로 무선 전송
    - **실시간 내용 미리보기**: 텍스트 및 JSON, CSV 파일을 브라우저 화면에서 즉시 열람
    - 파일 개별 삭제, 기본 샘플 파일 생성(`config.json`, `sensor_log.csv`, `readme.txt`), 전체 포맷 지원
  - **🚀 Dual OTA 무선 펌웨어 업데이트**:
    - 웹 화면에서 컴파일된 새 펌웨어 바이너리(`.bin`)를 선택하고 전송하면 Wi-Fi 무선으로 업데이트 완료 후 자동 재부팅
    - 현재 실행 파티션(`app0`/`app1`) 및 다음 롤백/업데이트 타겟 자동 관리 (A/B 뱅크 안전 보호)
  - **⚡ 8MB PSRAM 제어 및 벤치마크**: 웹 버튼 클릭 한 번으로 PSRAM 1MB/4MB 할당/해제 및 R/W 속도(MB/s) 측정
  - **💾 128KB Flash MMAP 영구 저장소**: 
    - ESP32-S3 MMU 가상 주소 매핑(`esp_partition_mmap()`)을 통한 0-Copy 읽기
    - 웹 UI에서 메모를 입력하고 플래시에 영구 저장 (전원을 껐다 켜거나 재부팅해도 보존)
    - 부팅 횟수(`boot_count`), 기록 횟수(`write_count`) 및 실시간 128KB Flash Hex Dump 뷰어 탑재
  - **🎨 원터치 컬러 피커 & 슬라이더**: 웹에서 색상을 고르면 GPIO48 LED가 즉각 반응
  - **💻 웹 CLI 터미널**: 브라우저에서 `fs`, `ota`, `top`, `psram`, `mmap`, `help`, `wifi scan` 등을 직접 입력하고 실행 결과를 화면에서 확인

