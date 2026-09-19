# PandaDesk – ESP32-C6 PlatformIO Implementation Plan

## 1. Goal

PandaDesk is designed as a universal ESP32-C6 controller for height-adjustable desks. The firmware must not assume one fixed desk wiring. Instead, the PCB exposes eight generic desk lines (`DESK1..DESK8`) and the selected desk profile decides at runtime which lines are used as RX, TX, GPIO, open-drain, inputs, outputs, or require 5 V pull-ups.

The firmware architecture should keep hardware, protocol logic, application logic, networking, and safety clearly separated so additional desk families can be added without modifying the core system.

The current hardware supports multiple physical connector families, including:

- RJ12 Jiecang
- RJ45 Jiecang
- RJ45 Loctek
- direct 8-pin terminal access
- passthrough operation

Automatic desk detection is not required for the first versions. The user selects the correct desk profile.

---

## 2. Platform

Use:

```text
PlatformIO
ESP32-C6
ESP-IDF
C++17
```

ESP-IDF is preferred over Arduino because PandaDesk requires low-level control over:

- GPIO direction
- open-drain operation
- UART routing
- UART inversion if needed
- I²C
- NVS
- FreeRTOS tasks
- watchdogs
- OTA
- native USB
- networking
- future 802.15.4 support

Example starting `platformio.ini`:

```ini
[platformio]
default_envs = pandadesk

[env:pandadesk]
platform = espressif32
board = esp32-c6-devkitc-1
framework = espidf

monitor_speed = 115200
monitor_filters =
    esp32_exception_decoder

build_flags =
    -std=gnu++17
    -DPANDADESK_HW_REV=1

board_build.partitions = partitions.csv
```

For the final PCB, create a custom PlatformIO board definition matching the exact ESP32-C6-MINI-1 flash configuration instead of permanently using the DevKitC target.

---

## 3. Firmware Architecture

The firmware should be split into the following layers:

```text
Application
│
├── CommandManager
├── DeskState
├── SafetyManager
│
├── Networking
│   ├── Wi-Fi
│   ├── REST
│   ├── WebSocket
│   ├── MQTT
│   └── Home Assistant
│
├── Protocol Layer
│   ├── Jiecang
│   ├── Loctek
│   └── future protocols
│
├── Desk Profile Layer
│
└── Hardware Layer
    ├── DeskBus
    ├── PullupController
    ├── TCA9535
    ├── UART
    └── I²C
```

The most important rule is:

> Higher layers must never access raw ESP32 GPIO numbers or TCA9535 pins directly.

---

# 4. First Firmware Milestone: Hardware Abstraction

Do not begin with a desk protocol.

The first milestone should verify the PandaDesk hardware.

The firmware should initially provide:

```text
ESP32-C6
 │
 ├── HardwareConfig
 │
 ├── DeskBus
 │     ├── DESK1
 │     ├── DESK2
 │     ├── ...
 │     └── DESK8
 │
 ├── PullupController
 │      └── TCA9535
 │
 ├── ConfigStore
 │      └── NVS
 │
 └── Diagnostics
```

Required functionality:

1. Boot reliably.
2. Initialize I²C.
3. Detect the TCA9535.
4. Force PU1..PU8 OFF.
5. Configure all DESK GPIOs as high impedance.
6. Toggle individual desk pull-ups.
7. Read individual desk lines.
8. Change each desk line between input, output, and open-drain.
9. Expose diagnostics over USB serial.

This stage becomes the PCB validation firmware.

---

# 5. GPIO Allocation

Keep the logical desk line numbering simple.

Recommended mapping where compatible with the final schematic:

```cpp
namespace Pins {

constexpr gpio_num_t DESK1 = GPIO_NUM_0;
constexpr gpio_num_t DESK2 = GPIO_NUM_1;
constexpr gpio_num_t DESK3 = GPIO_NUM_2;
constexpr gpio_num_t DESK4 = GPIO_NUM_3;
constexpr gpio_num_t DESK5 = GPIO_NUM_4;
constexpr gpio_num_t DESK6 = GPIO_NUM_5;
constexpr gpio_num_t DESK7 = GPIO_NUM_6;
constexpr gpio_num_t DESK8 = GPIO_NUM_7;

constexpr gpio_num_t I2C_SDA = GPIO_NUM_18;
constexpr gpio_num_t I2C_SCL = GPIO_NUM_19;

constexpr gpio_num_t USB_D_MINUS = GPIO_NUM_12;
constexpr gpio_num_t USB_D_PLUS  = GPIO_NUM_13;

constexpr gpio_num_t BOOT = GPIO_NUM_9;

}
```

The exact mapping must follow the final PCB schematic.

Prefer a direct mapping:

```text
DESK1 -> GPIO0
DESK2 -> GPIO1
...
DESK8 -> GPIO7
```

Avoid using boot/strapping pins for desk communication unless required.

---

# 6. DeskBus Hardware Abstraction

The rest of the firmware should never work directly with raw GPIO numbers.

Create:

```cpp
class DeskBus {
public:
    void initialize();

    void setInput(uint8_t line);
    void setOutput(uint8_t line);
    void setOpenDrain(uint8_t line);

    void write(uint8_t line, bool level);
    bool read(uint8_t line);

    void setPullup(uint8_t line, bool enabled);

    void makeSafe();
};
```

Logical line numbering:

```text
0 = DESK1
1 = DESK2
...
7 = DESK8
```

Internal mapping:

```cpp
static constexpr gpio_num_t deskGpios[8] = {
    GPIO_NUM_0,
    GPIO_NUM_1,
    GPIO_NUM_2,
    GPIO_NUM_3,
    GPIO_NUM_4,
    GPIO_NUM_5,
    GPIO_NUM_6,
    GPIO_NUM_7
};
```

This class forms the boundary between the PCB and the desk protocol implementation.

---

# 7. Safe Boot Behaviour

Safe startup is critical because PandaDesk is electrically connected to another controller.

Boot sequence:

```text
ESP32 reset
   │
   ▼
Desk GPIOs -> INPUT / high impedance
   │
   ▼
Initialize I²C
   │
   ▼
Detect TCA9535
   │
   ▼
PU1..PU8 -> LOW
   │
   ▼
Configure P00..P07 as outputs
   │
   ▼
Load configuration
   │
   ▼
Load selected desk profile
   │
   ▼
Validate profile
   │
   ▼
Configure required desk lines
   │
   ▼
Start protocol driver
```

There must never be a boot state where an unknown desk line is actively driven HIGH.

Implement:

```cpp
DeskBus::makeSafe();
```

It should:

```text
all DESK lines -> input/high impedance
all pull-ups -> off
UART TX -> detached
protocol driver -> stopped
```

Call it:

- before applying another profile
- on invalid configuration
- after protocol initialization failure
- if the TCA9535 disappears
- before reboot
- after serious internal errors

---

# 8. TCA9535 Driver

The TCA9535 controls the firmware-selectable 5 V pull-ups.

Mapping:

```text
P00 -> PU1
P01 -> PU2
P02 -> PU3
P03 -> PU4
P04 -> PU5
P05 -> PU6
P06 -> PU7
P07 -> PU8
```

Create:

```cpp
class Tca9535 {
public:
    esp_err_t begin();

    bool available() const;

    esp_err_t setDirection(uint8_t pin, bool output);
    esp_err_t write(uint8_t pin, bool value);
    esp_err_t writePort(uint16_t value);

private:
    uint16_t outputState_;
};
```

Then wrap it in:

```cpp
class PullupController {
public:
    esp_err_t begin();
    esp_err_t disableAll();
    esp_err_t set(uint8_t deskLine, bool enabled);
};
```

Protocol code should call:

```cpp
deskBus.setPullup(DESK3, true);
```

and never access TCA9535 pins directly.

---

# 9. Desk Profiles

Profiles are the central concept of PandaDesk.

A profile determines:

- connector type
- protocol family
- line roles
- RX line
- TX line
- input lines
- output lines
- open-drain lines
- required 5 V pull-ups
- UART baud rate
- UART inversion if needed
- protocol-specific configuration

Suggested types:

```cpp
enum class DeskProtocol {
    JIECANG,
    LOCTEK
};

enum class LineMode {
    UNUSED,
    INPUT,
    OUTPUT,
    OPEN_DRAIN,
    UART_RX,
    UART_TX
};

struct DeskLineConfig {
    uint8_t line;
    LineMode mode;
    bool pullup5V;
};

struct DeskProfile {
    const char* id;
    const char* manufacturer;
    const char* model;
    const char* connector;

    DeskProtocol protocol;

    std::array<DeskLineConfig, 8> lines;

    uint32_t baudRate;
    bool uartInverted;
};
```

Initial profiles:

```text
Jiecang RJ12
Jiecang RJ45
Loctek RJ45
```

The exact line mappings are filled from confirmed measurements/protocol documentation.

---

# 10. Separate Connector Mapping from Protocol Mapping

Physical connector wiring and communication protocol should not be treated as the same thing.

Use two layers.

## ConnectorProfile

```cpp
struct ConnectorProfile {
    ConnectorType connector;
    std::array<uint8_t, 8> connectorToDesk;
};
```

Example:

```text
RJ45 pin 1 -> DESK3
RJ45 pin 2 -> DESK1
...
```

## Protocol Profile

Defines logical roles:

```text
DESK3 = RX
DESK5 = TX
baud = ...
protocol = Jiecang
```

Then combine them into a desk model profile.

This allows:

```text
Jiecang protocol + RJ12 wiring
Jiecang protocol + RJ45 wiring
Loctek protocol + RJ45 wiring
```

without duplicating protocol implementations.

---

# 11. Protocol Interface

Create a common interface:

```cpp
class DeskProtocolDriver {
public:
    virtual ~DeskProtocolDriver() = default;

    virtual esp_err_t begin(const DeskProfile& profile) = 0;
    virtual void stop() = 0;

    virtual void loop() = 0;

    virtual bool moveUp() = 0;
    virtual bool moveDown() = 0;
    virtual bool stopMovement() = 0;

    virtual bool moveTo(float heightCm) = 0;

    virtual std::optional<float> currentHeight() const = 0;
    virtual bool isMoving() const = 0;
};
```

Initial implementations:

```text
DeskProtocolDriver
       │
       ├── JiecangProtocol
       └── LoctekProtocol
```

Future:

```text
MaidesiteProtocol
KaidiProtocol
GenericProtocol
```

The application layer should never know manufacturer-specific packet bytes.

---

# 12. UART Abstraction

UART must be dynamically routed according to the selected desk profile.

Create:

```cpp
class DeskUart {
public:
    esp_err_t attach(
        uint8_t rxDeskLine,
        uint8_t txDeskLine,
        uint32_t baud,
        bool inverted = false
    );

    void detach();

    int read(uint8_t* buffer, size_t size);
    int write(const uint8_t* buffer, size_t size);
};
```

Example:

```text
Profile:
DESK2 = UART RX
DESK5 = UART TX

             ↓

GPIO1 -> UART RX
GPIO4 -> UART TX
```

Changing the desk profile should not require recompiling the firmware.

---

# 13. Open-Drain Operation

PandaDesk must support both:

```text
Push-pull
Open-drain
```

ESP-IDF provides native open-drain GPIO mode.

Example:

```cpp
gpio_config_t config {
    .pin_bit_mask = ...,
    .mode = GPIO_MODE_OUTPUT_OD,
    ...
};
```

For open-drain:

```text
0 -> actively LOW
1 -> released
```

If the selected profile requires it:

```cpp
deskBus.setPullup(line, true);
```

activates the external 4.7 kΩ pull-up to 5 V.

Never emulate open-drain by actively driving HIGH.

---

# 14. Profile Validator

Before applying a profile:

```cpp
ProfileValidator::validate(profile)
```

Reject profiles containing:

- duplicate incompatible line assignments
- RX and TX assigned to the same line
- TX configured as input-only
- invalid pull-up assignments
- unsupported line numbers
- invalid baud rate
- missing protocol type
- unsupported connector
- invalid protocol-specific parameters

This is especially important if profiles become editable later.

---

# 15. FreeRTOS Runtime Architecture

Use independent tasks instead of one large loop.

```text
Main
 │
 ├── Protocol Task
 │      desk RX/TX
 │      packet parser
 │      height updates
 │
 ├── Control Task
 │      commands
 │      movement supervision
 │      safety
 │
 ├── Network Task
 │      Wi-Fi
 │      REST
 │      WebSocket
 │      MQTT
 │
 └── System
        NVS
        OTA
        watchdog
        diagnostics
```

Use:

- queues
- event groups
- mutexes

Avoid shared mutable global state.

---

# 16. Desk State Model

Maintain one canonical desk state.

```cpp
struct DeskState {
    bool connected;
    bool moving;
    MovementDirection direction;

    float currentHeight;
    std::optional<float> targetHeight;

    uint32_t lastPacketMs;
    uint32_t lastMovementMs;

    ProtocolStatus protocolStatus;
};
```

Consumers include:

- Web UI
- REST
- WebSocket
- MQTT
- Home Assistant
- diagnostics

No subsystem should independently poll the desk when the protocol layer already knows the state.

---

# 17. Command Model

Normalize commands before protocol translation.

```cpp
enum class DeskCommandType {
    MOVE_UP,
    MOVE_DOWN,
    STOP,
    MOVE_TO,
    PRESET
};

struct DeskCommand {
    DeskCommandType type;
    float value;
};
```

Command sources may include:

```text
Web UI
REST
MQTT
Home Assistant
future physical controls
```

All feed into:

```text
DeskCommandQueue
```

Only the protocol driver converts these into manufacturer-specific packets.

---

# 18. Configuration System

Use ESP-IDF NVS.

Persist:

```text
system.hostname
system.deviceName

wifi.ssid
wifi.password

desk.profile
desk.minHeight
desk.maxHeight

mqtt.enabled
mqtt.host
mqtt.port
mqtt.username
mqtt.password

api.authentication

system.firstBoot
```

Do not persist temporary runtime state such as:

- current height
- current movement
- connection state

unless protocol calibration specifically requires it.

Suggested abstraction:

```cpp
class ConfigManager {
public:
    esp_err_t begin();

    SystemConfig load();
    esp_err_t save(const SystemConfig& config);

    esp_err_t factoryReset();
};
```

---

# 19. First-Boot Provisioning

First startup flow:

```text
No configuration
      │
      ▼
Start AP
      │
      ▼
PandaDesk-XXXX
      │
      ▼
Web setup
      │
      ├── Wi-Fi
      ├── desk manufacturer
      ├── desk model
      └── MQTT optional
      │
      ▼
Save NVS
      │
      ▼
Restart
```

Automatic desk detection should not be required initially.

---

# 20. Web Interface

Suggested frontend stack:

```text
Vite
TypeScript
HTML/CSS
```

Build output can be embedded into a filesystem/storage partition.

Suggested endpoints:

```text
/
 /assets/*
 /api/v1/*
 /ws
```

WebSocket state example:

```json
{
  "height": 72.4,
  "moving": false,
  "direction": "stopped",
  "connected": true
}
```

REST examples:

```text
POST /api/v1/desk/up
POST /api/v1/desk/down
POST /api/v1/desk/stop
```

Move-to:

```text
POST /api/v1/desk/height
```

```json
{
  "height": 75.0
}
```

---

# 21. MQTT and Home Assistant

MQTT should be designed in from the beginning even if it remains disabled by default.

Suggested topics:

```text
pandadesk/<id>/state
pandadesk/<id>/height
pandadesk/<id>/moving
pandadesk/<id>/availability

pandadesk/<id>/command/up
pandadesk/<id>/command/down
pandadesk/<id>/command/stop
pandadesk/<id>/command/height
```

Support Home Assistant MQTT discovery.

Potential entities:

- desk height
- desk connected
- movement status
- up
- down
- stop
- target height

---

# 22. Logging

Use structured logging categories:

```text
SYSTEM
NETWORK
PROFILE
DESK_BUS
UART
PROTOCOL
MQTT
WEB
SAFETY
```

Levels:

```text
ERROR
WARN
INFO
DEBUG
TRACE
```

Optional protocol trace:

```text
RX  12:31:02.412  F1 F1 01 00 ...
TX  12:31:02.519  F1 F1 02 00 ...
```

Raw frame dumps should only be enabled in debug mode.

---

# 23. Protocol Sniffer Mode

Add a dedicated passive diagnostic mode early in development.

Possible configuration:

```text
Sniffer mode
  DESK1 input
  DESK2 input
  ...
```

Expose captured traffic via USB.

Important rule:

> Sniffer mode must never actively drive desk signals.

All 5 V desk pull-ups should remain OFF unless explicitly required for a controlled diagnostic test.

---

# 24. Safety Manager

Commands must not go directly from network interfaces into the protocol driver.

Use:

```text
API / MQTT / UI
       │
       ▼
CommandManager
       │
       ▼
SafetyManager
       │
       ▼
DeskProtocol
```

SafetyManager should enforce:

- configured minimum height
- configured maximum height
- maximum continuous movement time
- valid desk connection
- valid protocol state
- communication timeout
- one movement command at a time
- emergency stop behaviour

Example:

```cpp
constexpr auto MAX_CONTINUOUS_MOVEMENT =
    std::chrono::seconds(45);
```

The exact value can later become profile-specific.

---

# 25. Watchdog Behaviour

The protocol/control tasks should feed a watchdog.

If a task hangs while the desk is moving:

```text
watchdog
   ↓
system reset
   ↓
boot
   ↓
all desk lines high impedance
   ↓
all desk pull-ups off
```

Safe hardware startup therefore becomes part of the overall safety architecture.

---

# 26. Network Isolation

Wi-Fi, MQTT, WebSocket, or Home Assistant failure must not interfere with local desk communication.

The desk subsystem must continue operating correctly when:

```text
Wi-Fi disconnected
MQTT unavailable
Home Assistant offline
```

Desk communication must never depend on network timing.

---

# 27. OTA Architecture

Design OTA support from the beginning.

Suggested partition layout:

```text
nvs
otadata
phy_init
factory
ota_0
ota_1
storage
```

OTA flow:

```text
download firmware
   ↓
verify
   ↓
write inactive partition
   ↓
reboot
   ↓
self-test
   ↓
mark valid
```

If startup fails, ESP-IDF OTA rollback should restore the previous working image.

---

# 28. Firmware Versioning

Expose build information:

```cpp
struct FirmwareInfo {
    const char* version;
    const char* gitSha;
    const char* buildDate;
    uint8_t hardwareRevision;
};
```

Example:

```text
PandaDesk
Firmware: 0.2.0
Commit: af31c4e
Hardware: REV1
ESP-IDF: 5.x
```

Expose it through:

- USB console
- Web UI
- REST
- MQTT diagnostics

---

# 29. Recommended Project Structure

```text
PandaDesk/
│
├── platformio.ini
├── partitions.csv
├── sdkconfig.defaults
│
├── include/
│   └── pandadesk/
│       ├── version.h
│       └── build_config.h
│
├── src/
│   ├── main.cpp
│   │
│   ├── app/
│   │   ├── PandaDeskApp.cpp
│   │   ├── PandaDeskApp.h
│   │   ├── DeskState.h
│   │   ├── DeskCommand.h
│   │   └── CommandManager.cpp
│   │
│   ├── hardware/
│   │   ├── Pins.h
│   │   ├── DeskBus.cpp
│   │   ├── DeskBus.h
│   │   ├── Tca9535.cpp
│   │   ├── Tca9535.h
│   │   ├── PullupController.cpp
│   │   └── PullupController.h
│   │
│   ├── profiles/
│   │   ├── DeskProfile.h
│   │   ├── ProfileRegistry.cpp
│   │   ├── ProfileRegistry.h
│   │   ├── ProfileValidator.cpp
│   │   └── builtin/
│   │       ├── JiecangRJ12.cpp
│   │       ├── JiecangRJ45.cpp
│   │       └── LoctekRJ45.cpp
│   │
│   ├── protocol/
│   │   ├── DeskProtocol.h
│   │   ├── DeskUart.cpp
│   │   ├── DeskUart.h
│   │   │
│   │   ├── jiecang/
│   │   │   ├── JiecangProtocol.cpp
│   │   │   ├── JiecangProtocol.h
│   │   │   ├── JiecangParser.cpp
│   │   │   └── JiecangPackets.h
│   │   │
│   │   └── loctek/
│   │       ├── LoctekProtocol.cpp
│   │       ├── LoctekProtocol.h
│   │       ├── LoctekParser.cpp
│   │       └── LoctekPackets.h
│   │
│   ├── safety/
│   │   ├── SafetyManager.cpp
│   │   └── SafetyManager.h
│   │
│   ├── config/
│   │   ├── ConfigManager.cpp
│   │   ├── ConfigManager.h
│   │   └── ConfigSchema.h
│   │
│   ├── network/
│   │   ├── WifiManager.cpp
│   │   ├── MqttManager.cpp
│   │   └── Mdns.cpp
│   │
│   ├── web/
│   │   ├── WebServer.cpp
│   │   ├── RestApi.cpp
│   │   └── WebSocket.cpp
│   │
│   ├── ota/
│   │   ├── OtaManager.cpp
│   │   └── OtaManager.h
│   │
│   └── diagnostics/
│       ├── Diagnostics.cpp
│       ├── ProtocolLogger.cpp
│       └── Sniffer.cpp
│
├── frontend/
│
├── test/
│   ├── test_profiles/
│   ├── test_jiecang/
│   ├── test_loctek/
│   └── test_config/
│
└── docs/
    ├── PROTOCOLS.md
    ├── PROFILES.md
    ├── HARDWARE.md
    └── DEVELOPMENT.md
```

---

# 30. Implementation Phases

## Phase 1 – PlatformIO Skeleton

Deliverables:

- ESP32-C6 boots
- USB logging works
- build/version information available
- NVS available

No desk control.

## Phase 2 – Board Support Package

Implement:

- Pins
- I²C
- TCA9535
- PullupController
- DeskBus

Validate every hardware signal.

## Phase 3 – Safety Layer

Implement:

- safe boot
- `makeSafe()`
- profile validation
- watchdog
- movement timeout infrastructure

This must exist before transmitting movement commands.

## Phase 4 – Profile Engine

Implement:

- DeskProfile
- ConnectorProfile
- ProfileRegistry
- ProfileValidator

Create initial profile definitions:

- Jiecang RJ12
- Jiecang RJ45
- Loctek RJ45

## Phase 5 – UART Abstraction

Verify that UART can dynamically move between desk lines without recompiling the firmware.

Examples:

```text
DESK1 RX / DESK2 TX
DESK6 RX / DESK3 TX
```

## Phase 6 – Jiecang Passive Decoding

Receive only.

Decode:

- raw frames
- current height
- movement state
- status

Do not transmit initially.

## Phase 7 – Jiecang Commands

Implement:

- UP
- DOWN
- STOP

After that:

- MOVE_TO
- presets

## Phase 8 – Loctek Passive Decoding

Follow the same sequence as Jiecang.

Electrical compatibility must be verified before connecting any desk with lines above 5 V.

## Phase 9 – Networking

Implement:

- Wi-Fi
- AP provisioning
- mDNS
- persistent configuration

## Phase 10 – REST and WebSocket

Expose desk state and commands.

## Phase 11 – Web UI

Create the PandaDesk frontend.

## Phase 12 – MQTT and Home Assistant

Add discovery, commands, state reporting, and availability.

## Phase 13 – OTA

Enable dual-partition OTA and rollback.

## Phase 14 – Additional Desk Profiles

Add further:

- Jiecang controllers
- Loctek controllers
- Maidesite
- Kaidi
- other compatible desks

without modifying the central application architecture.

---

# 31. Testing Strategy

## Host-Side Unit Tests

Protocol parsers should be testable without hardware.

Example:

```text
Input packet
    ↓
parser
    ↓
height
movement
checksum state
```

This is particularly useful while reverse-engineering protocols.

## Hardware-in-the-Loop Tests

Use PandaDesk plus a second microcontroller acting as a simulated desk.

Test:

- UART RX
- UART TX
- all eight desk GPIOs
- open-drain
- all eight pull-ups
- profile switching
- I²C errors
- TCA9535 disappearance
- reboot during communication
- watchdog behaviour

## Real Desk Tests

For every supported profile maintain a compatibility test sheet.

Test:

- connection
- startup
- passive RX
- height
- up
- down
- stop
- move-to
- original handset passthrough
- reboot while idle
- reboot during movement
- Wi-Fi loss
- MQTT loss

---

# 32. Compatibility Database

Desk compatibility metadata should be explicit.

Example:

```cpp
{
    .id = "jiecang_xxx_rj12",
    .manufacturer = "Jiecang",
    .testedModels = {
        "..."
    },
    .connector = ConnectorType::RJ12_JIECANG,
    .status = ProfileStatus::STABLE
}
```

Possible status values:

```text
EXPERIMENTAL
TESTED
STABLE
```

The WebUI can then warn the user when a profile is experimental.

---

# 33. One Firmware Image

Do not build separate firmware images per desk manufacturer.

Avoid:

```text
pandadesk-jiecang.bin
pandadesk-loctek.bin
```

Use:

```text
pandadesk.bin
```

with the selected desk profile stored in NVS.

Changing desks should be a configuration change, not a firmware reflash.

---

# 34. Final Dependency Direction

The intended firmware dependency flow is:

```text
              WebUI
                │
 REST ─ MQTT ─ Commands
                │
                ▼
          CommandManager
                │
                ▼
          SafetyManager
                │
                ▼
        DeskProtocol API
          │           │
          │           │
      Jiecang       Loctek
          │           │
          └─────┬─────┘
                ▼
             DeskBus
         ┌──────┴──────┐
         ▼             ▼
      Desk GPIO     Pull-ups
                       │
                    TCA9535
```

Rules:

- Nothing above `DeskProtocol` knows manufacturer packet bytes.
- Nothing above `DeskBus` knows raw GPIO numbers.
- Protocol drivers never access network services.
- Network services never manipulate hardware directly.

---

# 35. Definition of PandaDesk Firmware v0.1.0

The first usable release should include:

```text
ESP32-C6 / PandaDesk REV1
PlatformIO
safe startup
NVS configuration
TCA9535 support
PU1..PU8 control
DESK1..8 hardware abstraction

Jiecang RJ12 profile
Jiecang RJ45 profile
Loctek RJ45 profile

height reading
up
down
stop

Wi-Fi provisioning
Web UI
REST
WebSocket

MQTT
Home Assistant discovery

OTA
factory reset
diagnostics
```

Postpone initially:

```text
automatic desk detection
Zigbee
Thread
BLE control
user-created protocol profiles
cloud functionality
```

---

# 36. Recommended First Implementation Order

The first production-quality implementation should focus on these core pieces:

```text
DeskBus
        +
PullupController
        +
DeskProfile
        +
ProfileValidator
        +
SafeState
```

Only after these are stable should the first active desk protocol implementation be allowed to transmit movement commands.

This architecture preserves the core PandaDesk concept: one universal hardware platform whose behaviour is defined almost entirely by software profiles.
