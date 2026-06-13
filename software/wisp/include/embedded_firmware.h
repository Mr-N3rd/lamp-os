// embedded_firmware.h — declares the signed lamp firmware blob the wisp
// distributes over the mesh.
//
// When scripts/embed_firmware.py has run (as the PIO pre-build hook
// does on every wisp build), the generated header
// `embedded_firmware_generated.h` (gitignored) contains the real
// signed lamp firmware. We pick it up via __has_include so a fresh
// checkout without the generated file still compiles against the
// committed stub.
//
// Either header MUST define (in namespace wisp):
//   constexpr uint8_t kEmbeddedFirmware[N];
//   constexpr size_t  kEmbeddedFirmwareLen;
//
// FirmwareCarrier consumes these symbols and parses the trailing LSIG
// footer at runtime. The stub deliberately omits the LSIG magic so the
// distributor stays Disabled (no false offers, no surprise OTAs).

#pragma once

#if __has_include("embedded_firmware_generated.h")
#include "embedded_firmware_generated.h"
#else
#include "embedded_firmware.h.stub"
#endif
