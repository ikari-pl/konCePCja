#pragma once

#include <cstdint>

#include "types.h"

union TCounter {
  struct {
    word Lo;
    word Hi;
  };
  dword Re;
};

union TNoise {
  struct {
    word Low;
    word Val;
  };
  dword Seed;
};

union TEnvelopeCounter {
  struct {
    dword Lo;
    dword Hi;
  };
  int64_t Re;
};
