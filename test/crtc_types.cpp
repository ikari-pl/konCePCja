/* crtc_types.cpp — the CRTC type metadata lookups (crtc_types.h): which chip
 * ships in which CPC model, chip names, manufacturers. The per-type register
 * readback/quirk behaviour lives with the hw CRTC Device tests
 * (test/hw/crtc_test.cpp, test/hw/crtc_quirks_test.cpp). */

#include <gtest/gtest.h>

#include "crtc_types.h"

TEST(CrtcTypes, DefaultTypeForCPC464) { EXPECT_EQ(0, crtc_type_for_model(0)); }

TEST(CrtcTypes, DefaultTypeForCPC664) { EXPECT_EQ(0, crtc_type_for_model(1)); }

TEST(CrtcTypes, DefaultTypeForCPC6128) {
  EXPECT_EQ(1, crtc_type_for_model(2));
}

TEST(CrtcTypes, DefaultTypeForPlus) { EXPECT_EQ(3, crtc_type_for_model(3)); }

TEST(CrtcTypes, DefaultTypeForUnknownModel) {
  EXPECT_EQ(0, crtc_type_for_model(99));
}

TEST(CrtcTypes, ChipNameType0) {
  EXPECT_STREQ("HD6845S", crtc_type_chip_name(0));
}

TEST(CrtcTypes, ChipNameType1) {
  EXPECT_STREQ("UM6845R", crtc_type_chip_name(1));
}

TEST(CrtcTypes, ChipNameType2) {
  EXPECT_STREQ("MC6845", crtc_type_chip_name(2));
}

TEST(CrtcTypes, ChipNameType3) {
  EXPECT_STREQ("AMS40489", crtc_type_chip_name(3));
}

TEST(CrtcTypes, ManufacturerType0) {
  EXPECT_STREQ("Hitachi", crtc_type_manufacturer(0));
}

TEST(CrtcTypes, ManufacturerType1) {
  EXPECT_STREQ("UMC", crtc_type_manufacturer(1));
}

TEST(CrtcTypes, ManufacturerType2) {
  EXPECT_STREQ("Motorola", crtc_type_manufacturer(2));
}

TEST(CrtcTypes, ManufacturerType3) {
  EXPECT_STREQ("Amstrad", crtc_type_manufacturer(3));
}

TEST(CrtcTypes, ChipNameUnknown) {
  EXPECT_STREQ("Unknown", crtc_type_chip_name(99));
}

TEST(CrtcTypes, ManufacturerUnknown) {
  EXPECT_STREQ("Unknown", crtc_type_manufacturer(99));
}
