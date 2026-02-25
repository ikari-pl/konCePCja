#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <cstdint>

class TelnetConsole {
public:
   ~TelnetConsole();
   void start(int base_port = 6544);
   void stop();
   int port() const { return actual_port.load(); }
   bool has_client() const { return client_connected.load(); }

   // Called from Z80 execution loop when PC == TXT_OUTPUT address.
   // Must be extremely fast — lock-free ring buffer write.
   void on_txt_output(uint8_t ch);

   // Called from main loop each frame — feeds pending input to autotype queue.
   void drain_input();

private:
   void run();

   std::atomic<bool> running{false};
   std::atomic<int> actual_port{0};
   std::atomic<bool> client_connected{false};
   int base_port_ = 6544;
   std::thread server_thread;

   // Output ring buffer: Z80 hook writes (single producer), server thread reads (single consumer)
   static constexpr int OUTPUT_BUF_SIZE = 4096;
   uint8_t output_buf[OUTPUT_BUF_SIZE] = {};
   std::atomic<int> output_head{0};  // write position (Z80 thread)
   std::atomic<int> output_tail{0};  // read position (server thread)

   // Input buffer: server thread writes, main loop reads
   std::mutex input_mutex;
   std::string pending_input;
};

extern TelnetConsole g_telnet;
