# TRDP Simulator

TRDP Simulator is a cross-platform command line tool that sends and receives TRDP process data (PD) and message data (MD) using the [TCNopen TRDP](https://www.tcnopen.eu) stack. A single XML configuration file drives the behaviour of the simulator, enabling rapid scripting of TRDP scenarios for lab validation or integration testing on Windows, Linux, and Raspberry Pi targets.

## Features

- Pure C++17 implementation with a portable CMake build system.
- Optional integration with the official TCNopen TRDP stack for real PD/MD communication.
- XML based configuration describing network interfaces, PD publishers/subscribers, and MD senders/listeners.
- Built-in payload helpers (hex, text, or file sourced) and periodic transmission scheduling.
- Graceful shutdown handling and structured logging to console and/or file.
- Configuration validation catches duplicate names, invalid scheduling intervals, and incomplete auto-reply definitions before the
  simulator starts.

## Repository layout

```
.
├── CMakeLists.txt
├── docs/
│   └── configuration.example.xml
├── include/trdp_simulator/
├── src/
└── third_party/
    └── tinyxml2/
```

## Building

1. **Install dependencies**
   - A C++17 capable compiler (MSVC 2019+, GCC 10+, or Clang 10+).
   - CMake 3.16 or newer.
   - Optional: the TCNopen TRDP stack libraries and headers.

2. **Configure and build**

   ```bash
   cmake -S . -B build -DTRDPSimulator_ENABLE_TRDP=ON -DTRDP_ROOT=/path/to/trdp
   cmake --build build
   ```

   If the TRDP stack is not available on the build machine, omit `-DTRDPSimulator_ENABLE_TRDP=ON`. The simulator will then fall back to a stubbed adapter that performs loop-back testing but does not emit real network traffic.

3. **Install (optional)**

   ```bash
   cmake --install build --prefix /opt/trdp-simulator
   ```

## Running

Provide the XML configuration file with the `--config` option:

```bash
./build/trdp-simulator --config docs/configuration.example.xml
```

Press `Ctrl+C` to stop the simulator. The stub adapter echoes PD and MD payloads locally so that configuration and logging can be validated without live TRDP traffic.

## Configuration file

A single XML file controls every aspect of the simulator. See [`docs/configuration.example.xml`](docs/configuration.example.xml) for a detailed sample. At a glance:

- `<network>` — interface name, host IP, gateway, VLAN, and TTL defaults.
- `<logging>` — log level, console enable/disable, and optional log file path.
- `<pd>` — define any number of `<publisher>` and `<subscriber>` entries with COMIDs, dataset IDs, cycle times, and payload definitions.
- `<md>` — configure `<sender>` and `<listener>` elements for message data with reply expectations and automatic responses.
- MD senders with `cycleTimeMs="0"` transmit a single request at startup instead of running a periodic loop.

Payloads accept three formats:

- `format="hex"` — space-free hexadecimal string (`deadbeef`).
- `format="text"` — UTF-8 text payload.
- `format="file"` — path to a binary file to load at runtime.

## Integrating the TCNopen TRDP stack

To exercise real TRDP traffic the simulator must be linked against the official TCNopen TRDP stack. Provide the installation location through `TRDP_ROOT` or the standard `CMAKE_PREFIX_PATH`. When found the build automatically enables the high-fidelity adapter located in `src/trdp_stack_adapter_real.cpp`, which maps the simulator operations to `tlc_*` and `tlm_*` APIs from the stack.

> **Note:** The TRDP stack is distributed separately by TCNopen. Consult their licensing terms and download portal to obtain the latest stable release. The simulator expects headers such as `trdp_if_light.h` and `trdp_mdcom.h` to be reachable by the compiler and the corresponding libraries (`libtrdp`) to be linkable.

## Cross-platform notes

- **Linux / Raspberry Pi** — Build directly on the target using GCC or Clang. Ensure that the user has access to the network interfaces defined in the configuration.
- **Windows** — Configure with Visual Studio generator (`cmake -G "Visual Studio 17 2022"`). Provide the TRDP headers/libs compiled for Windows if real traffic is required.
- **Containers** — The stub adapter is useful for CI environments without direct network access. Real TRDP integration requires raw socket permissions; run with appropriate capabilities (`CAP_NET_RAW`).

## License

This project bundles [tinyxml2](https://github.com/leethomason/tinyxml2) under the zlib license in `third_party/tinyxml2`. The simulator source itself is distributed under the MIT license.
