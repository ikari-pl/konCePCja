/* konCePCja - Amstrad CPC Emulator
   Telnet Console — mirrors CPC text output and injects keyboard input
   over a persistent TCP connection on port 6544 (IPC+1).

   Output: Hooks TXT_OUTPUT (&BB5A) via the Z80 execution loop.
           Characters are pushed to a lock-free SPSC ring buffer
           and flushed to the TCP client by the server thread.

   Input:  Received bytes are buffered and fed to AutoTypeQueue
           each frame by the main loop, converting ANSI escape
           sequences to CPC special keys.
*/

#include "telnet_console.h"
#include "autotype.h"
#include "z80.h"
#include "log.h"

#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#endif

TelnetConsole g_telnet;

// ── Z80 hook callback ──────────────────────────────────

static void txt_output_hook(uint8_t ch)
{
   g_telnet.on_txt_output(ch);
}

// ── Ring buffer (SPSC) ─────────────────────────────────

void TelnetConsole::on_txt_output(uint8_t ch)
{
   int head = output_head.load(std::memory_order_relaxed);
   int next = (head + 1) % OUTPUT_BUF_SIZE;
   // If buffer full, drop character (server not draining fast enough)
   if (next == output_tail.load(std::memory_order_acquire)) return;
   output_buf[head] = ch;
   output_head.store(next, std::memory_order_release);
}

// ── Input drain (called from main loop each frame) ─────

void TelnetConsole::drain_input()
{
   std::string buf;
   {
      std::lock_guard<std::mutex> lock(input_mutex);
      if (pending_input.empty()) return;
      buf.swap(pending_input);
   }

   // Convert ANSI escape sequences to autotype ~KEY~ syntax.
   // Everything else passes through as literal characters.
   std::string autotype_text;
   for (size_t i = 0; i < buf.size(); i++) {
      uint8_t ch = static_cast<uint8_t>(buf[i]);

      // ANSI escape: \x1b[ followed by a letter
      if (ch == 0x1b && i + 2 < buf.size() && buf[i + 1] == '[') {
         char code = buf[i + 2];
         i += 2;
         switch (code) {
            case 'A': autotype_text += "~UP~"; continue;
            case 'B': autotype_text += "~DOWN~"; continue;
            case 'C': autotype_text += "~RIGHT~"; continue;
            case 'D': autotype_text += "~LEFT~"; continue;
            default: break; // unknown — fall through
         }
      }

      // Bare ESC (no [ follows, or end of buffer)
      if (ch == 0x1b) {
         autotype_text += "~ESC~";
         continue;
      }

      // DEL / Backspace
      if (ch == 0x7f || ch == 0x08) {
         autotype_text += "~DEL~";
         continue;
      }

      // Tab
      if (ch == 0x09) {
         autotype_text += "~TAB~";
         continue;
      }

      // Ctrl+C → ESC (common telnet interrupt)
      if (ch == 0x03) {
         autotype_text += "~ESC~";
         continue;
      }

      // CR or LF → newline (autotype maps \n to RETURN)
      if (ch == 0x0d) {
         autotype_text += '\n';
         // Skip a following LF (CR+LF pair)
         if (i + 1 < buf.size() && buf[i + 1] == 0x0a) i++;
         continue;
      }
      if (ch == 0x0a) {
         autotype_text += '\n';
         continue;
      }

      // Printable ASCII — pass through directly
      if (ch >= 0x20 && ch < 0x7f) {
         autotype_text += static_cast<char>(ch);
         continue;
      }

      // Everything else: skip
   }

   if (!autotype_text.empty()) {
      g_autotype_queue.enqueue(autotype_text);
   }
}

// ── Lifecycle ──────────────────────────────────────────

TelnetConsole::~TelnetConsole()
{
   stop();
}

void TelnetConsole::start(int base_port)
{
   if (running.load()) return;
   base_port_ = base_port;
   running.store(true);
   output_head.store(0);
   output_tail.store(0);
   z80_set_txt_output_hook(&txt_output_hook, 0xBB5A);
   z80_set_bdos_output_hook(&txt_output_hook);  // CP/M: BDOS C_WRITE (C=2, char in E)
   server_thread = std::thread(&TelnetConsole::run, this);
}

void TelnetConsole::stop()
{
   running.store(false);
   z80_set_txt_output_hook(nullptr, 0);
   z80_set_bdos_output_hook(nullptr);
   if (server_thread.joinable()) server_thread.join();
}

// ── Server thread ──────────────────────────────────────

#ifdef _WIN32

void TelnetConsole::run()
{
   WSADATA wsa;
   if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;

   SOCKET server_fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
   if (server_fd == INVALID_SOCKET) { WSACleanup(); return; }

   int opt = 1;
   setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
              reinterpret_cast<const char*>(&opt), sizeof(opt));

   sockaddr_in addr{};
   addr.sin_family = AF_INET;
   addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

   int bound_port = 0;
   for (int p = base_port_; p < base_port_ + 10; p++) {
      addr.sin_port = htons(static_cast<uint16_t>(p));
      if (bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != SOCKET_ERROR) {
         bound_port = p;
         break;
      }
   }
   if (bound_port == 0) {
      LOG_ERROR("Telnet: could not bind to any port in range "
                << base_port_ << "-" << (base_port_ + 9));
      closesocket(server_fd);
      WSACleanup();
      return;
   }

   if (listen(server_fd, 1) == SOCKET_ERROR) {
      closesocket(server_fd);
      WSACleanup();
      return;
   }

   actual_port.store(bound_port);
   LOG_INFO("Telnet console: listening on port " << bound_port);

   SOCKET client_fd = INVALID_SOCKET;

   while (running.load()) {
      fd_set readfds;
      FD_ZERO(&readfds);
      FD_SET(server_fd, &readfds);
      if (client_fd != INVALID_SOCKET)
         FD_SET(client_fd, &readfds);
      timeval tv{0, 50000}; // 50ms — 20Hz polling

      int ready = select(0, &readfds, nullptr, nullptr, &tv);

      // Accept new connection (replaces existing)
      if (ready > 0 && FD_ISSET(server_fd, &readfds)) {
         sockaddr_in peer{};
         int plen = sizeof(peer);
         SOCKET new_fd = accept(server_fd, reinterpret_cast<sockaddr*>(&peer), &plen);
         if (new_fd != INVALID_SOCKET) {
            if (client_fd != INVALID_SOCKET) closesocket(client_fd);
            client_fd = new_fd;
            client_connected.store(true);
            const char* banner = "konCePCja CPC Telnet Console\r\n---\r\n";
            ::send(client_fd, banner, static_cast<int>(strlen(banner)), 0);
         }
      }

      // Read input from client
      if (client_fd != INVALID_SOCKET && ready > 0 && FD_ISSET(client_fd, &readfds)) {
         char buf[256];
         int n = recv(client_fd, buf, sizeof(buf), 0);
         if (n <= 0) {
            closesocket(client_fd);
            client_fd = INVALID_SOCKET;
            client_connected.store(false);
         } else {
            std::lock_guard<std::mutex> lock(input_mutex);
            pending_input.append(buf, static_cast<size_t>(n));
         }
      }

      // Flush output ring buffer to client
      if (client_fd != INVALID_SOCKET) {
         int tail = output_tail.load(std::memory_order_relaxed);
         int head = output_head.load(std::memory_order_acquire);
         if (tail != head) {
            uint8_t flush_buf[OUTPUT_BUF_SIZE];
            int count = 0;
            while (tail != head && count < OUTPUT_BUF_SIZE - 1) {
               uint8_t ch = output_buf[tail];
               tail = (tail + 1) % OUTPUT_BUF_SIZE;
               // Convert CPC carriage return to telnet CR+LF
               if (ch == 0x0d) {
                  flush_buf[count++] = '\r';
                  if (count < OUTPUT_BUF_SIZE - 1) flush_buf[count++] = '\n';
               } else if (ch == 0x0a) {
                  flush_buf[count++] = '\n';
               } else if (ch >= 0x20 && ch < 0x7f) {
                  flush_buf[count++] = ch;
               } else if (ch == 0x07) {
                  flush_buf[count++] = '\a'; // bell
               }
               // Other control chars: skip
            }
            output_tail.store(tail, std::memory_order_release);
            if (count > 0) {
               int sent = ::send(client_fd, reinterpret_cast<const char*>(flush_buf),
                                 count, 0);
               if (sent < 0) {
                  closesocket(client_fd);
                  client_fd = INVALID_SOCKET;
                  client_connected.store(false);
               }
            }
         }
      }
   }

   if (client_fd != INVALID_SOCKET) closesocket(client_fd);
   closesocket(server_fd);
   WSACleanup();
}

#else // POSIX

void TelnetConsole::run()
{
   int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
   if (server_fd < 0) return;

   int opt = 1;
   setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

   sockaddr_in addr{};
   addr.sin_family = AF_INET;
   addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

   int bound_port = 0;
   for (int p = base_port_; p < base_port_ + 10; p++) {
      addr.sin_port = htons(static_cast<uint16_t>(p));
      if (bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
         bound_port = p;
         break;
      }
   }
   if (bound_port == 0) {
      LOG_ERROR("Telnet: could not bind to any port in range "
                << base_port_ << "-" << (base_port_ + 9));
      ::close(server_fd);
      return;
   }

   if (listen(server_fd, 1) < 0) {
      ::close(server_fd);
      return;
   }

   actual_port.store(bound_port);
   LOG_INFO("Telnet console: listening on port " << bound_port);

   int client_fd = -1;

   while (running.load()) {
      fd_set readfds;
      FD_ZERO(&readfds);
      FD_SET(server_fd, &readfds);
      int maxfd = server_fd;
      if (client_fd >= 0) {
         FD_SET(client_fd, &readfds);
         if (client_fd > maxfd) maxfd = client_fd;
      }
      timeval tv{0, 50000}; // 50ms

      int ready = select(maxfd + 1, &readfds, nullptr, nullptr, &tv);

      // Accept new connection (replaces existing)
      if (ready > 0 && FD_ISSET(server_fd, &readfds)) {
         sockaddr_in peer{};
         socklen_t plen = sizeof(peer);
         int new_fd = accept(server_fd, reinterpret_cast<sockaddr*>(&peer), &plen);
         if (new_fd >= 0) {
            if (client_fd >= 0) ::close(client_fd);
            client_fd = new_fd;
            client_connected.store(true);
            const char* banner = "konCePCja CPC Telnet Console\r\n---\r\n";
            (void)::write(client_fd, banner, strlen(banner));
         }
      }

      // Read input from client
      if (client_fd >= 0 && ready > 0 && FD_ISSET(client_fd, &readfds)) {
         char buf[256];
         ssize_t n = ::read(client_fd, buf, sizeof(buf));
         if (n <= 0) {
            ::close(client_fd);
            client_fd = -1;
            client_connected.store(false);
         } else {
            std::lock_guard<std::mutex> lock(input_mutex);
            pending_input.append(buf, static_cast<size_t>(n));
         }
      }

      // Flush output ring buffer to client
      if (client_fd >= 0) {
         int tail = output_tail.load(std::memory_order_relaxed);
         int head = output_head.load(std::memory_order_acquire);
         if (tail != head) {
            uint8_t flush_buf[OUTPUT_BUF_SIZE];
            int count = 0;
            while (tail != head && count < OUTPUT_BUF_SIZE - 1) {
               uint8_t ch = output_buf[tail];
               tail = (tail + 1) % OUTPUT_BUF_SIZE;
               // Convert CPC carriage return to telnet CR+LF
               if (ch == 0x0d) {
                  flush_buf[count++] = '\r';
                  if (count < OUTPUT_BUF_SIZE - 1) flush_buf[count++] = '\n';
               } else if (ch == 0x0a) {
                  flush_buf[count++] = '\n';
               } else if (ch >= 0x20 && ch < 0x7f) {
                  flush_buf[count++] = ch;
               } else if (ch == 0x07) {
                  flush_buf[count++] = '\a'; // bell
               }
               // Other control chars: skip
            }
            output_tail.store(tail, std::memory_order_release);
            if (count > 0) {
               ssize_t sent = ::write(client_fd, flush_buf,
                                      static_cast<size_t>(count));
               if (sent < 0) {
                  ::close(client_fd);
                  client_fd = -1;
                  client_connected.store(false);
               }
            }
         }
      }
   }

   if (client_fd >= 0) ::close(client_fd);
   ::close(server_fd);
}

#endif // _WIN32
