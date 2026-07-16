/* crtc_types.cpp — CRTC type metadata (see crtc_types.h). */

#include "crtc_types.h"

unsigned char crtc_type_for_model(unsigned int cpc_model) {
  // CPC464=0, CPC664=1 -> Type 0 (HD6845S/UM6845)
  // CPC6128=2 -> Type 1 (UM6845R)
  // Plus/6128+=3 -> Type 3 (AMS40489/ASIC)
  switch (cpc_model) {
    case 0:
      return 0;  // CPC 464
    case 1:
      return 0;  // CPC 664
    case 2:
      return 1;  // CPC 6128
    case 3:
      return 3;  // 6128+
    default:
      return 0;
  }
}

const char* crtc_type_chip_name(unsigned char crtc_type) {
  switch (crtc_type) {
    case 0:
      return "HD6845S";
    case 1:
      return "UM6845R";
    case 2:
      return "MC6845";
    case 3:
      return "AMS40489";
    default:
      return "Unknown";
  }
}

const char* crtc_type_manufacturer(unsigned char crtc_type) {
  switch (crtc_type) {
    case 0:
      return "Hitachi";
    case 1:
      return "UMC";
    case 2:
      return "Motorola";
    case 3:
      return "Amstrad";
    default:
      return "Unknown";
  }
}
