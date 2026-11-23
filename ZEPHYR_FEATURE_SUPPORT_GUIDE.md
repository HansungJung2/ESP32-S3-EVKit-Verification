# ESP32-S3 Zephyr RTOS 기능 지원 및 마이그레이션 가이드
> **대상 칩셋**: ESP32-S3-WROOM-1 (N16R8: 16MB Flash, 8MB Octal PSRAM)  
> **비교 브랜치**: `main` (Arduino / ESP-IDF) vs `branch_zephyr` (Zephyr RTOS v3.7.0 LTS)  
> **작성일**: 2026-11-23

---

## 1. 개요 및 배경

기존 `main` 브랜치에서 운영되었던 검증 펌웨어(Web Server, BLE, Wi-Fi, WS2812 RGB LED, MMAP, Dual OTA)와 현재 `branch_zephyr` 브랜치에 구축된 **Zephyr RTOS** 간의 기능 지원 현황과 아키텍처 차이점을 정리한 문서입니다.

현재 `branch_zephyr`에 배포된 펌웨어는 Zephyr RTOS의 **기본 커널(Kernel), 멀티스레딩, IPC(메시지 큐), 대화형 Shell CLI 동작을 검증하기 위한 베이스라인(Baseline)** 상태입니다. 

**결론적으로, 기존의 모든 기능(Wi-Fi, BLE, 웹서버, LED, MMAP/PSRAM, OTA)은 Zephyr RTOS에서 공식 지원하며 완벽하게 구현 가능합니다.**

---

## 2. 주요 기능별 지원 현황 및 아키텍처 비교

| 기능 영역 | `main` 브랜치 (Arduino / ESP-IDF) | `branch_zephyr` 브랜치 (Zephyr RTOS) | Zephyr 공식 지원 여부 및 핵심 구현체 |
| :--- | :--- | :--- | :--- |
| **Wi-Fi 통신** | `WiFi.h` (Station / SoftAP) | `CONFIG_WIFI=y`<br>`CONFIG_WIFI_ESP32=y` | **공식 지원**<br>Espressif 바이너리 Wi-Fi 드라이버(`hal_espressif`) + Zephyr L2/L3 네트워크 스택 연동 |
| **BLE 5.0** | `BLEDevice.h` (Nordic UART Service) | `CONFIG_BT=y`<br>`CONFIG_BT_ESP32=y` | **강력 지원 (Zephyr 핵심 강점)**<br>Zephyr 네이티브 블루투스 호스트 스택, NUS(Nordic UART Service) 표준 내장 |
| **Web Server** | `WebServer.h` (HTTP 동기/비동기 서버) | `CONFIG_HTTP_SERVER=y`<br>또는 Zephyr BSD Socket API | **지원**<br>Zephyr HTTP Server 서브시스템 및 REST 핸들러 제공 |
| **RGB LED** | `FastLED` 라이브러리 | `CONFIG_LED_STRIP=y`<br>`CONFIG_WS2812_STRIP=y` | **지원**<br>DeviceTree 바인딩 및 RMT/SPI 기반 Zephyr 표준 LED Strip 드라이버로 WS2812 제어 |
| **MMAP / PSRAM** | `esp_partition_mmap()`<br>`BOARD_HAS_PSRAM` | `CONFIG_FLASH_MAP=y`<br>`CONFIG_ESP_SPIRAM=y` | **지원**<br>8MB Octal PSRAM 하드웨어 매핑 및 Flash 파티션 가상주소 매핑 API 지원 |
| **OTA 무선 업데이트** | `Update.h` (app0/app1 A/B 뱅크) | **MCUboot** + **MCUmgr**<br>(`CONFIG_BOOTLOADER_MCUBOOT=y`) | **엔터프라이즈급 표준 지원**<br>임베디드 업계 표준인 **MCUboot** 부트로더 연동, 무선(BLE/Wi-Fi/HTTP) 펌웨어 전송 및 롤백 보호 |

---

## 3. 기능별 상세 구현 방안 및 Zephyr 설정

### 3.1 Wi-Fi (802.11 b/g/n 2.4GHz)
* **개념**: Zephyr는 `hal_espressif` 모듈에 포함된 Espressif 공식 Wi-Fi 펌웨어 블롭(`libnet80211.a`, `libpp.a` 등)을 사용하여 하드웨어 무선 MAC/PHY를 제어합니다.
* **Kconfig 설정 (`prj.conf`)**:
  ```ini
  CONFIG_NETWORKING=y
  CONFIG_NET_L2_WIFI_MGMT=y
  CONFIG_WIFI=y
  CONFIG_WIFI_ESP32=y
  CONFIG_NET_IPV4=y
  CONFIG_NET_DHCPV4=y
  CONFIG_NET_SHELL=y
  CONFIG_NET_L2_WIFI_SHELL=y
  ```
* **동작 방식**: 
  - Zephyr Shell에서 `wifi scan`, `wifi connect <SSID> -p <PW>` 명령어로 즉시 접속 가능.
  - C 코드에서는 `net_mgmt(NET_REQUEST_WIFI_CONNECT, ...)` API를 호출하여 연결 관리.

---

### 3.2 BLE 5.0 (Nordic UART Service - NUS)
* **개념**: Zephyr의 Bluetooth 스택은 업계 최고 수준의 완성도와 메모리 효율을 자랑합니다. ESP32-S3의 블루투스 컨트롤러를 VHCI(Virtual HCI)로 연결하여 동작합니다.
* **Kconfig 설정 (`prj.conf`)**:
  ```ini
  CONFIG_BT=y
  CONFIG_BT_PERIPHERAL=y
  CONFIG_BT_DEVICE_NAME="ESP32-S3-Zephyr"
  CONFIG_BT_NUS=y
  ```
* **동작 방식**: 
  - 스마트폰의 `Serial Bluetooth Terminal` 또는 `nRF Connect` 앱으로 페어링 없이 즉시 연결.
  - BLE를 통해 수신된 텍스트를 Zephyr Shell 입력으로 파이프라인 연결(Wireless Shell CLI) 가능.

---

### 3.3 HTTP Web Server
* **개념**: Arduino에서는 C++ 기반의 `WebServer` 객체를 사용했으나, Zephyr에서는 자체 HTTP Server 서브시스템 또는 POSIX/BSD Socket API를 사용합니다.
* **Kconfig 설정 (`prj.conf`)**:
  ```ini
  CONFIG_HTTP_SERVER=y
  CONFIG_NET_TCP=y
  CONFIG_NET_SOCKETS=y
  ```
* **동작 방식**: 
  - LittleFS 파일시스템과 연동하여 HTML/JS/CSS 정적 파일 서빙.
  - REST API 엔드포인트를 등록하여 시스템 정보(CPU, 힙, 스레드 상태, 센서 데이터)를 JSON으로 반환.

---

### 3.4 온보드 WS2812B RGB LED (GPIO 48)
* **개념**: ESP32-S3 WROOM-1 내장 네오픽셀 LED(GPIO 48)는 Zephyr의 표준 `led_strip` 드라이버를 통해 제어합니다.
* **DeviceTree 오버레이 (`boards/esp32s3_devkitm.overlay`)**:
  ```dts
  / {
      aliases {
          led-strip = &led_strip;
      };
  };

  &pinctrl {
      ws2812_pins: ws2812_pins {
          pinmux = <ESP32_PINMUX(48, ESP_NOSIG, ESP_NOSIG)>;
      };
  };

  /* SPI 또는 RMT 기반 WS2812 노드 정의 */
  ```
* **Kconfig 설정 (`prj.conf`)**:
  ```ini
  CONFIG_LED_STRIP=y
  CONFIG_WS2812_STRIP=y
  ```
* **동작 방식**: 
  - `led_strip_update_rgb(strip_dev, &pixel, 1)` 함수로 RGB 색상 및 밝기 제어.

---

### 3.5 MMAP (Flash 메모리 매핑 & 8MB Octal PSRAM)
* **개념**: ESP32-S3의 MMU(메모리 관리 장치)를 활용하여 외부 SPI Flash 및 PSRAM을 Xtensa CPU 주소 공간에 0-Copy로 매핑합니다.
* **Flash MMAP**:
  - Zephyr의 Flash Map 서브시스템(`CONFIG_FLASH_MAP=y`)을 활성화하여 특정 파티션 주소를 직접 접근하거나, Espressif HAL의 `esp_partition_mmap()`을 호출하여 영구 저장소 구현.
* **8MB PSRAM (Octal/OPI)**:
  - **Kconfig 설정**:
    ```ini
    CONFIG_ESP_SPIRAM=y
    CONFIG_ESP32S3_SPIRAM_MODE_OCT=y
    ```
  - 활성화 시 8MB PSRAM이 시스템 힙 메모리(`k_malloc`) 풀로 확장되거나 전용 버퍼 풀로 할당 가능.

---

### 3.6 OTA 무선 펌웨어 업데이트 (MCUboot & MCUmgr)
* **개념**: Arduino의 `Update.h`보다 훨씬 강력하고 안전한 업계 표준 **MCUboot** 부트로더를 사용합니다.
* **주요 특징**:
  1. **Dual Bank A/B Swap**: `slot0_partition`(실행 영역)과 `slot1_partition`(다운로드 영역) 간의 안전 교체.
  2. **자동 롤백(Rollback)**: 새 펌웨어로 부팅 후 일정 시간 내에 확인(Confirm)하지 않으면 이전 정상 펌웨어로 자동 복구.
  3. **MCUmgr 무선 전송**: BLE, Wi-Fi(UDP/HTTP), UART를 통해 스마트폰 또는 PC CLI(`mcumgr-cli`)로 무선 업데이트 수행.
* **Kconfig 설정**:
  ```ini
  CONFIG_BOOTLOADER_MCUBOOT=y
  CONFIG_MCUMGR=y
  CONFIG_MCUMGR_TRANSPORT_BT=y
  CONFIG_MCUMGR_TRANSPORT_SHELL=y
  ```

---

## 4. 브랜치 운영 가이드

* **`main` 브랜치 (Arduino Framework / PlatformIO)**
  - **목적**: 빠른 하드웨어 기능 검증, 풀스택 웹 대시보드 및 BLE NUS CLI 즉시 운영.
  - **장점**: Arduino 라이브러리 생태계(FastLED, WebServer, ArduinoOTA 등)를 100% 활용한 완성도 높은 GUI/Web 환경.
  - **전환 방법**: `git checkout main`

* **`branch_zephyr` 브랜치 (Zephyr RTOS)**
  - **목적**: 고신뢰성 멀티스레드 RTOS 기반 임베디드 제품 개발 및 엔터프라이즈 아키텍처 구축.
  - **장점**: 엄격한 스레드 스케줄링, 모듈형 드라이버/서브시스템, 업계 표준 MCUboot OTA, 상용급 저전력 및 보안성.
  - **전환 방법**: `git checkout branch_zephyr`

---

## 5. Zephyr 포팅 권장 로드맵

Zephyr RTOS에 기존 기능을 단계별로 추가할 경우 권장하는 순서는 다음과 같습니다:

1. **Step 1: WS2812 RGB LED (GPIO 48) 연동**
   - DeviceTree 및 `led_strip` 드라이버 추가 ➔ Shell 커맨드(`app led 255 0 0`)로 색상 제어
2. **Step 2: Wi-Fi L2 스택 및 Shell CLI 활성화**
   - Espressif Wi-Fi 드라이버 활성화 ➔ `wifi scan`, `wifi connect` 명령어로 네트워크 접속
3. **Step 3: BLE 5.0 Nordic UART Service (NUS) 탑재**
   - Bluetooth 서브시스템 활성화 ➔ 스마트폰 무선 터미널과 UART Shell 브릿지 연결
4. **Step 4: 8MB PSRAM 활성화 및 LittleFS 연동**
   - Octal PSRAM 힙 확장 ➔ Flash LittleFS 마운트 및 파일 입출력 검증
5. **Step 5: HTTP Web Server & MCUboot OTA 완성**
   - 경량 Web 대시보드 서빙 ➔ MCUboot 기반 무선 펌웨어 업데이트 파이프라인 구축
