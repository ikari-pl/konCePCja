// beads-lly6: host Disc Tools / IPC disk family must see and update the live
// FDC medium, not a stale driveA/driveB snapshot from load time.

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "disk_file_editor.h"
#include "disk_format.h"
#include "hw/fdc.h"
#include "koncepcja.h"
#include "slotshandler.h"
#include "subcycle_bridge.h"

extern t_drive driveA;
extern t_drive driveB;

namespace {

class DiskMediaSyncTest : public testing::Test {
 protected:
  void SetUp() override {
    dsk_eject_host(&driveA);
    dsk_eject_host(&driveB);
  }

  void TearDown() override {
    dsk_eject_host(&driveA);
    dsk_eject_host(&driveB);
  }
};

TEST_F(DiskMediaSyncTest, LoadBytesRoundTripsExtendedDsk) {
  ASSERT_EQ("", disk_format_drive('A', "data"));
  std::vector<uint8_t> bytes;
  ASSERT_EQ(0, dsk_to_bytes(&driveA, bytes));
  ASSERT_FALSE(bytes.empty());

  dsk_eject_host(&driveA);
  EXPECT_EQ(0u, driveA.tracks);

  ASSERT_EQ(0, dsk_load_bytes(bytes.data(), bytes.size(), &driveA));
  EXPECT_GT(driveA.tracks, 0u);

  std::string err;
  auto files = disk_list_files(&driveA, err);
  EXPECT_EQ("", err);
  EXPECT_TRUE(files.empty());
}

TEST_F(DiskMediaSyncTest, HostEditsReachLiveFdcMedium) {
  // Format a blank DATA disc in the host view and attach it to an FDC —
  // the same split Disc Tools/IPC used to leave unsynced (beads-lly6).
  ASSERT_EQ("", disk_format_drive('A', "data"));
  std::vector<uint8_t> dsk;
  ASSERT_EQ(0, dsk_to_bytes(&driveA, dsk));

  std::vector<uint8_t> mem(fdc_state_size());
  Device dev = fdc_init(mem.data());
  ASSERT_EQ(0, fdc_attach_disk(&dev, dsk.data(), dsk.size(), 0));

  size_t len = 0;
  const uint8_t* image = fdc_media_image(&dev, len);
  ASSERT_NE(nullptr, image);
  ASSERT_EQ(dsk.size(), len);

  // Pull live bytes into the host view, add a file, push bytes back onto the
  // FDC image (the Disc Tools / IPC commit path).
  std::vector<uint8_t> live(image, image + len);
  ASSERT_EQ(0, dsk_load_bytes(live.data(), live.size(), &driveA));
  const std::vector<uint8_t> payload = {'H', 'I'};
  ASSERT_EQ("", disk_write_file(&driveA, "HELLO.BIN", payload, true));

  std::vector<uint8_t> edited;
  ASSERT_EQ(0, dsk_to_bytes(&driveA, edited));
  ASSERT_EQ(0, fdc_attach_disk(&dev, edited.data(), edited.size(), 0));
  fdc_media_mark_dirty_unit(&dev, 0);
  EXPECT_NE(0, fdc_media_dirty(&dev));

  size_t out_len = 0;
  const uint8_t* out = fdc_media_image(&dev, out_len);
  ASSERT_NE(nullptr, out);
  const uint8_t name[11] = {'H', 'E', 'L', 'L', 'O', ' ',
                            ' ', ' ', 'B', 'I', 'N'};
  bool found = false;
  for (size_t off = 0; off + 11 <= out_len && !found; ++off) {
    found = std::memcmp(out + off, name, 11) == 0;
  }
  EXPECT_TRUE(found) << "HELLO.BIN must appear in the live FDC image";
}

TEST_F(DiskMediaSyncTest, PullSeesMachineMutationsHostMissed) {
  ASSERT_EQ("", disk_format_drive('A', "data"));
  std::vector<uint8_t> dsk;
  ASSERT_EQ(0, dsk_to_bytes(&driveA, dsk));

  // Host view stays at the blank load-time snapshot.
  dsk_eject_host(&driveB);
  ASSERT_EQ(0, dsk_load_bytes(dsk.data(), dsk.size(), &driveB));

  // Mutate a separate copy the way the FDC mutates Bridge::media in place.
  std::vector<uint8_t> machine_dsk = dsk;
  ASSERT_EQ(0, dsk_load_bytes(machine_dsk.data(), machine_dsk.size(), &driveA));
  ASSERT_EQ("", disk_write_file(&driveA, "FROMCPC.BIN",
                                std::vector<uint8_t>{1, 2, 3}, true));
  ASSERT_EQ(0, dsk_to_bytes(&driveA, machine_dsk));

  // Stale host view (driveB) does not see the CPC file yet.
  std::string err;
  auto stale = disk_list_files(&driveB, err);
  ASSERT_EQ("", err);
  EXPECT_TRUE(stale.empty());

  // Pull = dsk_load_bytes from the live medium.
  ASSERT_EQ(0, dsk_load_bytes(machine_dsk.data(), machine_dsk.size(), &driveB));
  auto fresh = disk_list_files(&driveB, err);
  ASSERT_EQ("", err);
  ASSERT_EQ(1u, fresh.size());
  EXPECT_EQ("FROMCPC.BIN", fresh[0].display_name);
}

TEST_F(DiskMediaSyncTest, LoadBytesFailedParseKeepsExistingTracks) {
  ASSERT_EQ("", disk_format_drive('A', "data"));
  const unsigned int tracks = driveA.tracks;
  ASSERT_GT(tracks, 0u);

  const uint8_t garbage[] = {'N', 'O', 'T', 'A', 'D', 'I', 'S', 'K'};
  EXPECT_NE(0, dsk_load_bytes(garbage, sizeof(garbage), &driveA));
  EXPECT_EQ(tracks, driveA.tracks);
}

TEST_F(DiskMediaSyncTest, RollbackHostViewRestoresSnapshotWhenBridgeInactive) {
  ASSERT_EQ("", disk_format_drive('A', "data"));
  ASSERT_EQ("", disk_write_file(&driveA, "HELLO.BIN",
                                std::vector<uint8_t>{'H', 'I'}, true));
  std::vector<uint8_t> snapshot;
  ASSERT_EQ(0, dsk_to_bytes(&driveA, snapshot));

  ASSERT_EQ("", disk_write_file(&driveA, "WORLD.BIN",
                                std::vector<uint8_t>{'W'}, true));
  std::string err;
  auto mutated = disk_list_files(&driveA, err);
  ASSERT_EQ("", err);
  ASSERT_EQ(2u, mutated.size());

  ASSERT_FALSE(subcycle_bridge_active());
  subcycle_bridge_rollback_host_view(0, snapshot);

  auto restored = disk_list_files(&driveA, err);
  ASSERT_EQ("", err);
  ASSERT_EQ(1u, restored.size());
  EXPECT_EQ("HELLO.BIN", restored[0].display_name);
}

TEST_F(DiskMediaSyncTest, RollbackHostViewEjectsWhenSnapshotEmpty) {
  ASSERT_EQ("", disk_format_drive('A', "data"));
  ASSERT_GT(driveA.tracks, 0u);
  ASSERT_FALSE(subcycle_bridge_active());
  subcycle_bridge_rollback_host_view(0, {});
  EXPECT_EQ(0u, driveA.tracks);
}

TEST(DiskMediaSyncBridge, PullPushNoopWhenInactive) {
  EXPECT_FALSE(subcycle_bridge_active());
  EXPECT_FALSE(subcycle_bridge_pull_drive_view(0));
  EXPECT_FALSE(subcycle_bridge_push_drive_view(0));
}

}  // namespace
