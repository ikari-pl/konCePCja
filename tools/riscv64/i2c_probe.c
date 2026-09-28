// Probe for the TCA8418 keyboard controller on the K230's I2C buses.
// No i2c-tools on the board, and the chip is not instantiated as a kernel
// client, so the launcher must drive it raw. This does a single register read
// (0x01 = CFG) at each candidate address on each bus.
#define _GNU_SOURCE
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>

static int probe(int fd, int addr, int reg, unsigned char* out) {
  struct i2c_msg msgs[2];
  unsigned char r = (unsigned char)reg;
  msgs[0].addr = addr; msgs[0].flags = 0;        msgs[0].len = 1; msgs[0].buf = &r;
  msgs[1].addr = addr; msgs[1].flags = I2C_M_RD; msgs[1].len = 1; msgs[1].buf = out;
  struct i2c_rdwr_ioctl_data x = { .msgs = msgs, .nmsgs = 2 };
  return ioctl(fd, I2C_RDWR, &x) < 0 ? -1 : 0;
}

int main(void) {
  int const addrs[] = {0x34, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
                       0x6b, 0x55, 0x38, -1};
  for (int bus = 0; bus <= 1; bus++) {
    char path[32];
    snprintf(path, sizeof(path), "/dev/i2c-%d", bus);
    int fd = open(path, O_RDWR);
    if (fd < 0) { printf("%s: cannot open\n", path); continue; }
    printf("%s:", path);
    for (int i = 0; addrs[i] >= 0; i++) {
      unsigned char v = 0;
      if (probe(fd, addrs[i], 0x01, &v) == 0) printf("  0x%02x(reg01=0x%02x)", addrs[i], v);
    }
    printf("\n");
    close(fd);
  }
  return 0;
}
