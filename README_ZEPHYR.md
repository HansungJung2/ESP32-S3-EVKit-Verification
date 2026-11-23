# ESP32-S3-WROOM-1 (N16R8) Zephyr RTOS 검증 프로젝트

본 브랜치(`branch_zephyr`)는 **ESP32-S3 (Xtensa Dual-Core LX7)**를 위한 공식 **Zephyr RTOS** 기반 펌웨어 프로젝트입니다.

---

## 1. 프로젝트 주요 기능

* **기능 지원 및 마이그레이션 가이드**: [ZEPHYR_FEATURE_SUPPORT_GUIDE.md](file:///d:/00. Code/AI분석/ESP32-S3-EVKit-Verification/ZEPHYR_FEATURE_SUPPORT_GUIDE.md) (Web Server, BLE, Wi-Fi, LED, MMAP, OTA 상세 비교)
* **Zephyr Kernel 멀티스레딩**
  * `heartbeat_thread`: 1초 주기로 시스템 런타임 상태 모니터링
  * `worker_thread`: `k_msgq` 메시지 큐를 통한 비동기 이벤트 처리
* **대화형 Zephyr Shell (UART CLI)**
  * 시리얼 콘솔(115200 bps) 기반 셸 지원
  * 전용 애플리케이션 명령어:
    * `app sysinfo`: 칩셋, 아키텍처, 커널 버전, CPU 클럭, 업타임 확인
    * `app threads`: 실행 중인 스레드 목록, 우선순위, 스택 사용량 분석 (`thread_analyzer`)
    * `app event <val>`: 워커 스레드로 큐 메시지 송신 테스트
    * `app reboot`: Zephyr 커널 레벨 시스템 리셋 (`sys_reboot`)
* **장치 트리 (Device Tree Overlay)**
  * `boards/esp32s3_devkitm.overlay`: GPIO 및 주변장치 매핑

---

## 2. 개발 환경 준비

PC에 이미 설치된 도구:
* **Python 3.12**
* **West v1.5.0** (`pip install west`)
* **CMake v3.20+ & Ninja** (`pip install cmake ninja`)

추가 필요 구성요소 (Zephyr SDK):
* [Zephyr SDK Releases](https://github.com/zephyrproject-rtos/sdk-ng/releases)에서 Windows용 Zephyr SDK(최신버전 또는 v0.16.x)를 다운로드하여 `C:\zephyr-sdk` 등에 압축 해제 후 `setup.cmd`를 실행합니다.

---

## 3. 빌드 및 다운로드 방법

PowerShell 터미널에서 다음 명령어를 실행합니다:

### 1) Zephyr 워크스페이스 동기화 (최초 1회)
```powershell
# 상위 디렉터리에서 워크스페이스 초기화 및 모듈 다운로드
west init -l .
west update
```

### 2) 빌드 (Build)
```powershell
west build -b esp32s3_devkitm
```
*(또는 최신 Zephyr v3.7+ 타겟: `west build -b esp32s3_devkitm/esp32s3/procpu`)*

### 3) 보드 플래싱 (Upload)
ESP32-S3가 연결된 시리얼 포트(예: `COM3`)로 플래싱합니다:
```powershell
west flash --esp-device COM3
```

### 4) 시리얼 모니터 (Serial Console)
```powershell
west espressif monitor -p COM3
```
*(종료: `Ctrl + ]` 또는 일반 터미널 모니터 `115200` bps 사용)*
