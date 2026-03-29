#ifndef PSG_TYPES_H
#define PSG_TYPES_H

#include "types.h"
#include <cstdint>

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

#endif // PSG_TYPES_H
