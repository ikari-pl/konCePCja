#include "silicon_disc.h"
#include <cstring>
#include <cstdio>

SiliconDisc g_silicon_disc;

void silicon_disc_init(SiliconDisc& sd) {
    if (sd.data) return; // already allocated
    sd.data = new uint8_t[SILICON_DISC_SIZE]();  // value-initialized (zero)
}

void silicon_disc_free(SiliconDisc& sd) {
    delete[] sd.data;
    sd.data = nullptr;
    sd.enabled = false;
}

void silicon_disc_clear(SiliconDisc& sd) {
    if (sd.data) {
        memset(sd.data, 0, SILICON_DISC_SIZE);
    }
}

bool silicon_disc_save(const SiliconDisc& sd, const std::string& path) {
    if (!sd.data) return false;
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    // Header: "KSDX" + version byte + 3 reserved bytes
    const char header[8] = {'K', 'S', 'D', 'X', 1, 0, 0, 0};
    if (fwrite(header, 1, 8, f) != 8) { fclose(f); return false; }
    if (fwrite(sd.data, 1, SILICON_DISC_SIZE, f) != SILICON_DISC_SIZE) {
        fclose(f);
        return false;
    }
    fclose(f);
    return true;
}

bool silicon_disc_load(SiliconDisc& sd, const std::string& path) {
    if (!sd.data) silicon_disc_init(sd);
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char header[8];
    if (fread(header, 1, 8, f) != 8 ||
        header[0] != 'K' || header[1] != 'S' || header[2] != 'D' || header[3] != 'X') {
        fclose(f);
        return false;
    }
    if (fread(sd.data, 1, SILICON_DISC_SIZE, f) != SILICON_DISC_SIZE) {
        fclose(f);
        return false;
    }
    fclose(f);
    return true;
}
