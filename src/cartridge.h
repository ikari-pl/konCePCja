#pragma once

// konCePCja — loading of Plus-range cartridge files (.cpr).
//
// The container is RIFF with an "AMS!" form type; each chunk is one 16 KiB
// cartridge page. References:
//  - http://www.cpcwiki.eu/index.php/Format:CPR_CPC_Plus_cartridge_file_format
//  - https://en.wikipedia.org/wiki/Resource_Interchange_File_Format

#include <cstdio>
#include <string>

// Releases the cartridge image and page table.
void cpr_eject();

// Load a .cpr file. Returns 0 on success or an ERR_* code from errors.h.
// A file that is refused leaves the loaded cartridge as it was. One that is
// accepted frees the old image: the board reads it through a raw pointer, so
// load into a built machine through cpc_cartridge_insert() (koncepcja.h).
int cpr_load(const std::string& filename);
int cpr_load(std::FILE* pfile);
