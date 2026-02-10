#ifndef ASIC_DEBUG_H
#define ASIC_DEBUG_H

#include <string>

std::string asic_dump_dma();
std::string asic_dump_sprites();
std::string asic_dump_interrupts();
std::string asic_dump_palette();
std::string asic_dump_all();

#endif
