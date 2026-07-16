/* crtc_types.h — CRTC type metadata (Gate C Wave 1, S2 — replacement-ledger).
 *
 * Static facts about the four CRTC variants (which chip ships in which CPC
 * model, chip names, manufacturers). Pure lookup tables consumed by the
 * Machine menu, the IPC server, and machine configuration — independent of
 * either emulation core, so they outlive the legacy crtc.cpp/h. */

#pragma once

unsigned char crtc_type_for_model(unsigned int cpc_model);
const char* crtc_type_chip_name(unsigned char crtc_type);
const char* crtc_type_manufacturer(unsigned char crtc_type);
