/* konCePCja — Serial Interface configuration (the [peripheral] serial_* keys).
 *
 * Split from serial_interface.h so the host config (t_CPC in koncepcja.h) can
 * hold one without pulling in the DART, timer and backend classes.
 */

#pragma once

#include <cstdint>
#include <string>

// Serial backend types
enum class SerialBackendType : std::uint8_t {
  Null,        // Drop all data
  File,        // Input/output files
  HostSerial,  // Physical serial port
  NullModem,   // Loopback
  TcpSocket,   // TCP client
  Plotter      // HP-GL plotter (HP 7470A)
};

// Serial configuration
struct SerialConfig {
  bool enabled = false;
  SerialBackendType backend_type = SerialBackendType::Null;

  // File backend
  std::string input_file;
  std::string output_file;

  // HostSerial backend
  std::string device_path;

  // TcpSocket backend
  std::string tcp_host = "127.0.0.1";
  uint16_t tcp_port = 23;

  // Common settings
  uint32_t baud_rate = 9600;

  friend bool operator==(const SerialConfig& lhs, const SerialConfig& rhs) {
    return lhs.enabled == rhs.enabled && lhs.backend_type == rhs.backend_type &&
           lhs.input_file == rhs.input_file &&
           lhs.output_file == rhs.output_file &&
           lhs.device_path == rhs.device_path && lhs.tcp_host == rhs.tcp_host &&
           lhs.tcp_port == rhs.tcp_port && lhs.baud_rate == rhs.baud_rate;
  }
  friend bool operator!=(const SerialConfig& lhs, const SerialConfig& rhs) {
    return !(lhs == rhs);
  }
};
