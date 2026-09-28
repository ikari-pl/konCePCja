// Full I2C bus scan, the way i2cdetect does it: a zero-length write probes for
// an ACK without assuming the device has readable registers. The earlier probe
// did a register read at guessed addresses, which a device can legitimately
// NAK even when present.
#define _GNU_SOURCE
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>

int main(void) {
  for (int bus = 0; bus <= 1; bus++) {
    char path[32];
    snprintf(path, sizeof(path), "/dev/i2c-%d", bus);
    int fd = open(path, O_RDWR);
    if (fd < 0) { printf("%s: open failed\n", path); continue; }
    printf("%s:", path);
    int found = 0;
    for (int a = 0x03; a <= 0x77; a++) {
      // A zero-length write is the classic i2cdetect probe, but the Synopsys
      // DesignWare adapter on this SoC rejects zero-length transfers, so every
      // address reads as absent -- including devices known to be present. A
      // one-byte read is supported and still only needs an ACK.
      unsigned char b = 0;
      struct i2c_msg msg = { .addr = a, .flags = I2C_M_RD, .len = 1, .buf = &b };
      struct i2c_rdwr_ioctl_data x = { .msgs = &msg, .nmsgs = 1 };
      if (ioctl(fd, I2C_RDWR, &x) >= 0) { printf(" 0x%02x", a); found++; }
    }
    printf("%s\n", found ? "" : "  (nothing responded)");
    close(fd);
  }
  return 0;
}
