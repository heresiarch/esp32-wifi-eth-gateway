# Requirements Document

## Introduction

The ESP32 WiFi-Ethernet bridge currently runs one TCP proxy instance. `main.c` calls `proxy_start()` once with `CONFIG_SERVER_LISTEN_PORT`, `CONFIG_PROXY_TARGET_ADDRESS` and `CONFIG_PROXY_TARGET_PORT`. This feature adds a second, independent proxy instance that listens on a second port (default 502, the Modbus TCP port) and forwards traffic to a configurable target. The second instance is configured through ESP-IDF Kconfig in `main/Kconfig.projbuild` and `main/Kconfig.projbuild.example`, in the same style as the existing proxy settings. The second instance is optional: a Kconfig switch `SECOND_PROXY_ENABLE` (default enabled) turns it on or off, and when it is off only the Primary_Instance runs.

## Glossary

- **Second_Proxy_Switch**: The Kconfig bool option `SECOND_PROXY_ENABLE` (`CONFIG_SECOND_PROXY_ENABLE` in C) that enables or disables the Secondary_Instance.
- **Second_Port_Options**: The Kconfig options `SECOND_SERVER_LISTEN_PORT`, `SECOND_PROXY_TARGET_ADDRESS` and `SECOND_PROXY_TARGET_PORT`.
- **README**: The file `README.md` in the repository root.

- **Bridge**: The ESP32 firmware application that connects the Ethernet interface to the Wi-Fi interface.
- **Proxy**: The TCP forwarding component implemented in `main/proxy.c` and `main/proxy.h`.
- **Proxy_Instance**: One running Proxy task with its own listen port, target address and target port.
- **Primary_Instance**: The existing Proxy_Instance configured by `CONFIG_SERVER_LISTEN_PORT`, `CONFIG_PROXY_TARGET_ADDRESS` and `CONFIG_PROXY_TARGET_PORT`.
- **Secondary_Instance**: The new Proxy_Instance added by this feature.
- **Kconfig_File**: The file `main/Kconfig.projbuild`, which defines the menuconfig options.
- **Kconfig_Example_File**: The file `main/Kconfig.projbuild.example`, the template with placeholder values.
- **Listen_Port**: The local TCP port on which a Proxy_Instance accepts client connections.
- **Build_System**: The ESP-IDF build toolchain (`idf.py build`) that compiles the firmware.
- **Target**: The remote IP address (or domain name) and TCP port to which a Proxy_Instance forwards traffic.

## Requirements

### Requirement 1: Kconfig options for the second port

**User Story:** As a developer, I want to configure the second proxy port through menuconfig, so that I can set it up the same way as the existing port.

#### Acceptance Criteria

1. THE Kconfig_File SHALL define an integer option `SECOND_SERVER_LISTEN_PORT` in the "Ethernet & Proxy Settings" menu with a default of 502, an inclusive range of 1 to 65535 and the dependency `depends on SECOND_PROXY_ENABLE`.
2. THE Kconfig_File SHALL define a string option `SECOND_PROXY_TARGET_ADDRESS` in the "Ethernet & Proxy Settings" menu with a default value matching the default of the existing `PROXY_TARGET_ADDRESS` option and the dependency `depends on SECOND_PROXY_ENABLE`.
3. THE Kconfig_File SHALL define an integer option `SECOND_PROXY_TARGET_PORT` in the "Ethernet & Proxy Settings" menu with a default of 502, an inclusive range of 1 to 65535 and the dependency `depends on SECOND_PROXY_ENABLE`.
4. THE Kconfig_File SHALL provide a non-empty prompt string and a non-empty help text for each of the Second_Port_Options, with each help text stating the purpose of the option and, for the port options, the valid range 1 to 65535.
5. THE Kconfig_Example_File SHALL contain the Second_Proxy_Switch and the Second_Port_Options with names, types, defaults, ranges, dependencies, prompt strings and help texts identical to those in the Kconfig_File, except that deployment-specific values (such as the target address) may be placeholders.
6. IF a value outside the range 1 to 65535 is entered for `SECOND_SERVER_LISTEN_PORT` or `SECOND_PROXY_TARGET_PORT` in menuconfig, THEN THE Kconfig_File SHALL cause menuconfig to reject the value and retain the previously configured value.

### Requirement 2: Default target for the second port

**User Story:** As a developer, I want a sensible default target for the second port, so that the firmware builds and runs without extra configuration.

#### Acceptance Criteria

1. WHERE the Kconfig_File defines `SECOND_PROXY_TARGET_ADDRESS`, THE Kconfig_File SHALL declare its default value as the string "10.0.0.2", identical to the default of `PROXY_TARGET_ADDRESS` in the Kconfig_File.
2. WHERE the Kconfig_Example_File defines `SECOND_PROXY_TARGET_ADDRESS`, THE Kconfig_Example_File SHALL declare its default value as the string "192.168.4.2", identical to the default of `PROXY_TARGET_ADDRESS` in the Kconfig_Example_File.
3. WHEN the firmware is built without a user-supplied value for `SECOND_PROXY_TARGET_ADDRESS`, THE Build_System SHALL complete the build with exit code 0 and use the default value declared in the Kconfig file in use.

### Requirement 3: Parallel proxying on two ports

**User Story:** As a user of the bridge, I want a second port to be proxied in parallel, so that two services can be reached at the same time.

#### Acceptance Criteria

1. WHEN the Bridge starts, THE Bridge SHALL start the Primary_Instance with `CONFIG_SERVER_LISTEN_PORT`, `CONFIG_PROXY_TARGET_ADDRESS` and `CONFIG_PROXY_TARGET_PORT`, and the Primary_Instance SHALL accept client connections on its Listen_Port within 5 seconds after the Bridge has obtained a network connection.
2. WHERE the Second_Proxy_Switch is enabled, WHEN the Bridge starts, THE Bridge SHALL start the Secondary_Instance with `CONFIG_SECOND_SERVER_LISTEN_PORT`, `CONFIG_SECOND_PROXY_TARGET_ADDRESS` and `CONFIG_SECOND_PROXY_TARGET_PORT`, and the Secondary_Instance SHALL accept client connections on its Listen_Port within 5 seconds after the Bridge has obtained a network connection.
3. WHERE the Second_Proxy_Switch is enabled, WHILE the Primary_Instance serves a client connection, THE Secondary_Instance SHALL accept and forward client connections on its Listen_Port without any change in the throughput or the connection state of the client connection served by the Primary_Instance.
4. WHERE the Second_Proxy_Switch is enabled, WHILE the Secondary_Instance serves a client connection, THE Primary_Instance SHALL accept and forward client connections on its Listen_Port without any change in the throughput or the connection state of the client connection served by the Secondary_Instance.
5. WHEN a client connects to the Listen_Port of a Proxy_Instance, THE Proxy_Instance SHALL open a connection to the Target of that Proxy_Instance and forward data bidirectionally between the client and the Target, preserving byte order and content without loss or modification, until either side closes the connection.
6. IF the Proxy_Instance cannot establish a connection to its Target within 10 seconds, THEN THE Proxy_Instance SHALL close the client connection, keep listening on its Listen_Port, and leave the other Proxy_Instance and its client connections unaffected.

### Requirement 4: Instance isolation

**User Story:** As a developer, I want the two proxy instances to be isolated, so that a failure on one port does not affect the other.

#### Acceptance Criteria

1. THE Proxy SHALL allocate a separate task, a separate transfer buffer and a separate configuration structure for each Proxy_Instance, such that no task, buffer or configuration structure is shared between the two Proxy_Instances.
2. IF a Proxy_Instance fails to allocate its resources, bind its Listen_Port or start listening, THEN THE Proxy SHALL log one error message that names the affected Listen_Port and the failed step (allocation, bind or listen), release all resources already allocated by that Proxy_Instance, and keep the other Proxy_Instance running and accepting connections.
3. WHEN a client connection of one Proxy_Instance closes or times out, THE Proxy SHALL close and release the client socket and the upstream socket of that connection within 1000 ms, and SHALL leave the sockets, Listen_Port and open connections of the other Proxy_Instance unchanged.
4. IF a Proxy_Instance task terminates because of an unrecoverable error, THEN THE Proxy SHALL keep the other Proxy_Instance task running and continue to forward data on its existing connections and accept new connections on its Listen_Port.
5. THE Proxy SHALL include the decimal Listen_Port number of the owning Proxy_Instance in every log message emitted by that Proxy_Instance, so that each log line can be attributed to exactly one of the two Proxy_Instances.

### Requirement 5: No regression for the first port

**User Story:** As a developer, I want the existing behavior of the first port preserved, so that this change causes no regression.

#### Acceptance Criteria

1. THE Kconfig_File SHALL keep the names, types, defaults and ranges of `SERVER_LISTEN_PORT`, `PROXY_TARGET_ADDRESS` and `PROXY_TARGET_PORT` unchanged.
2. THE Proxy SHALL keep the signature of `proxy_start()` unchanged as `void proxy_start(uint16_t listen_port, const char *remote_ip, uint16_t remote_port)`.
3. WHEN the Primary_Instance is the only instance receiving traffic, THE Proxy SHALL forward data with the same parameters as before this feature: a 30-second inactivity timeout, a 5-second connect timeout to the target, a 5-second limit for a stalled write, TCP keep-alive with 10 seconds idle time, 2 seconds probe interval and 3 probes, and a forwarding buffer of 1460 bytes.
4. WHEN the Primary_Instance is the only instance receiving traffic, THE Proxy SHALL serve one client at a time on `SERVER_LISTEN_PORT` and forward all bytes in both directions to `PROXY_TARGET_ADDRESS`:`PROXY_TARGET_PORT` without loss, reordering or modification.
5. IF the Primary_Instance fails to connect to the target, or either side closes the connection, or the inactivity timeout expires, THEN THE Proxy SHALL close both sockets of that connection and resume accepting new clients on `SERVER_LISTEN_PORT` within 1 second, as before this feature.

### Requirement 6: Port conflict detection

**User Story:** As a developer, I want invalid port configurations to be caught early, so that the two instances do not conflict.

#### Acceptance Criteria

1. WHERE the Second_Proxy_Switch is enabled, IF `CONFIG_SECOND_SERVER_LISTEN_PORT` equals `CONFIG_SERVER_LISTEN_PORT`, THEN THE Bridge SHALL abort the build with a non-zero exit status, produce no firmware binary, and emit a compile-time error message stating that both Listen_Ports must differ.
2. WHERE the Second_Proxy_Switch is enabled, IF `CONFIG_SECOND_SERVER_LISTEN_PORT` differs from `CONFIG_SERVER_LISTEN_PORT` and both values are within the range 1 to 65535, THEN THE Bridge SHALL complete the build without a port-conflict error.
3. WHERE the Second_Proxy_Switch is enabled, IF `CONFIG_SECOND_SERVER_LISTEN_PORT` or `CONFIG_SERVER_LISTEN_PORT` is outside the range 1 to 65535, THEN THE Bridge SHALL abort the build with a non-zero exit status and emit a compile-time error message identifying the out-of-range Listen_Port setting.
4. WHERE the Second_Proxy_Switch is disabled, THE Bridge SHALL evaluate no compile-time check that references a Second_Port_Option, and the build SHALL complete without requiring any Second_Port_Option to be defined.

### Requirement 7: Optional second proxy

**User Story:** As a developer, I want to switch the second proxy on or off in menuconfig, so that deployments that need only one port do not run an unused instance.

#### Acceptance Criteria

1. THE Kconfig_File SHALL define a bool option `SECOND_PROXY_ENABLE` in the "Ethernet & Proxy Settings" menu with a default of `y`, a non-empty prompt string and a non-empty help text stating that it enables the second proxy instance.
2. THE Kconfig_Example_File SHALL define `SECOND_PROXY_ENABLE` with the same name, type, default, prompt string and help text as the Kconfig_File.
3. WHILE the Second_Proxy_Switch is not set, THE menuconfig SHALL hide the Second_Port_Options.
4. WHILE the Second_Proxy_Switch is set, THE menuconfig SHALL show the Second_Port_Options.
5. WHERE the Second_Proxy_Switch is enabled, THE Bridge SHALL call `proxy_start()` for the Secondary_Instance exactly once, after the call for the Primary_Instance.
6. WHERE the Second_Proxy_Switch is disabled, THE Bridge SHALL start only the Primary_Instance, create no task, buffer or configuration structure for a Secondary_Instance, and open no socket on `CONFIG_SECOND_SERVER_LISTEN_PORT`.
7. WHERE the Second_Proxy_Switch is disabled, THE Primary_Instance SHALL behave as specified in Requirement 5.
8. THE README SHALL document the `SECOND_PROXY_ENABLE` option, its default value `y`, and the three Second_Port_Options that become visible when it is set.
