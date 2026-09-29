// beads-lly6: host Disc Tools / IPC disk family must see and update the live
// FDC medium, not a stale driveA/driveB snapshot from load time.

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "disk_file_editor.h"
#include "disk_format.h"
#include "flux_encode_util.h"  // fluxtest::build_standard_dsk
#include "hw/fdc.h"
#include "hw/flux_synth.h"  // fluxsynth::amsdos_content / scp_from_sectors
#include "koncepcja.h"
#include "m4board.h"
#include "silicon_disc.h"
#include "slotshandler.h"
#include "subcycle_bridge.h"
#include "symbiface.h"

extern t_drive driveA;
extern t_drive driveB;
extern t_CPC CPC;

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

  ASSERT_EQ("", disk_write_file(&driveA, "WORLD.BIN", std::vector<uint8_t>{'W'},
                                true));
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

// beads-csl7.4: the push guard added in 13db3b7c. With a writable flux disc in
// drive A, the FDC serves clean tracks from the SCP and written tracks from a
// synthesized DSK overlay. A host edit is pushed by copying into that overlay,
// which only works when the sizes match; anything else used to fall through
// to insert_disk(), which replaces the medium and silently drops the SCP (and
// with it every weak/protection bit on the clean tracks). The push must be
// refused instead, with the flux backing still attached.
class FluxPushGuardTest : public testing::Test {
 protected:
  void SetUp() override {
    saved_rom_path_ = CPC.rom_path;
    saved_model_ = CPC.model;
    saved_ram_size_ = CPC.ram_size;
    saved_drive_a_file_ = CPC.driveA.file;
    saved_drive_b_file_ = CPC.driveB.file;
    saved_m4_ = g_m4board.enabled;
    saved_silicon_disc_ = g_silicon_disc.enabled;
    saved_symbiface_ = g_symbiface.enabled;
    CPC.rom_path = "rom";
    CPC.model = 2;
    CPC.ram_size = 128;
    // stop() writes dirty discs, the Silicon Disc and IDE images back to
    // wherever these point; whatever an earlier test left here must not be
    // attached, or written to.
    CPC.driveA.file.clear();
    CPC.driveB.file.clear();
    g_m4board.enabled = false;
    g_silicon_disc.enabled = false;
    g_symbiface.enabled = false;
    dsk_eject_host(&driveA);
    started_ = subcycle_bridge_start();
  }

  void TearDown() override {
    if (started_) subcycle_bridge_stop();
    dsk_eject_host(&driveA);
    CPC.rom_path = saved_rom_path_;
    CPC.model = saved_model_;
    CPC.ram_size = saved_ram_size_;
    CPC.driveA.file = saved_drive_a_file_;
    CPC.driveB.file = saved_drive_b_file_;
    g_m4board.enabled = saved_m4_;
    g_silicon_disc.enabled = saved_silicon_disc_;
    g_symbiface.enabled = saved_symbiface_;
  }

  // Attach a writable 2-cylinder flux disc to drive A, now.
  void attach_writable_flux() {
    subcycle_bridge_insert_media(
        fluxsynth::scp_from_sectors(fluxsynth::amsdos_content(2)),
        /*flux=*/true, 0);
    subcycle_bridge_apply_pending_media();
  }

  static const uint8_t* scp() {
    size_t len = 0;
    return fdc_media_flux_scp(subcycle_bridge_fdc(), len);
  }
  static size_t overlay_size() {
    size_t len = 0;
    return fdc_media_image_unit(subcycle_bridge_fdc(), 0, len) ? len : 0;
  }

  bool started_ = false;
  std::string saved_rom_path_;
  unsigned int saved_model_ = 0;
  unsigned int saved_ram_size_ = 0;
  std::string saved_drive_a_file_;
  std::string saved_drive_b_file_;
  bool saved_m4_ = false;
  bool saved_silicon_disc_ = false;
  bool saved_symbiface_ = false;
};

TEST_F(FluxPushGuardTest, SameSizeEditLandsInTheOverlayAndKeepsTheScp) {
  ASSERT_TRUE(started_) << "bridge did not start from rom/";
  attach_writable_flux();
  ASSERT_NE(nullptr, scp()) << "the synthetic SCP did not attach";
  ASSERT_GT(overlay_size(), 0u) << "flux attached read-only (no DSK overlay)";

  // The Disc Tools path: pull the live overlay, edit, push back.
  ASSERT_TRUE(subcycle_bridge_pull_drive_view(0));
  ASSERT_EQ("", disk_write_file(&driveA, "HI.BIN", {'H', 'I'}, true));
  EXPECT_TRUE(subcycle_bridge_push_drive_view(0));
  EXPECT_NE(nullptr, scp()) << "an in-place overlay edit dropped the SCP";
}

TEST_F(FluxPushGuardTest, DifferentSizeHostDiscIsRefusedAndKeepsTheScp) {
  ASSERT_TRUE(started_) << "bridge did not start from rom/";
  attach_writable_flux();
  const uint8_t* const scp_before = scp();
  ASSERT_NE(nullptr, scp_before) << "the synthetic SCP did not attach";
  size_t const overlay_before = overlay_size();
  ASSERT_GT(overlay_before, 0u) << "flux attached read-only (no DSK overlay)";

  // A 3-track host disc against the 2-cylinder overlay. Loaded straight
  // into the host view: disk_format_drive() would push on its own (and is
  // refused by the same guard — it reports "could not update the live FDC
  // medium").
  std::vector<std::vector<fluxtest::SectorSpec>> tracks(3);
  for (int t = 0; t < 3; ++t) {
    for (int r = 0; r < 9; ++r) {
      fluxtest::SectorSpec sec;
      sec.chrn[0] = static_cast<uint8_t>(t);
      sec.chrn[2] = static_cast<uint8_t>(0xC1 + r);
      sec.payload = fluxtest::make_payload(t, r, 512);
      tracks[t].push_back(sec);
    }
  }
  std::vector<uint8_t> host = fluxtest::build_standard_dsk(tracks);
  ASSERT_EQ(0, dsk_load_bytes(host.data(), host.size(), &driveA));
  std::vector<uint8_t> serialized;
  ASSERT_EQ(0, dsk_to_bytes(&driveA, serialized));
  ASSERT_NE(overlay_before, serialized.size());

  EXPECT_FALSE(subcycle_bridge_push_drive_view(0));
  EXPECT_EQ(scp_before, scp()) << "the push replaced the flux backing";
  EXPECT_EQ(overlay_before, overlay_size()) << "the overlay was replaced";
}

}  // namespace
