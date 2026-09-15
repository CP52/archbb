#pragma once
#include <FS.h>
#include <SPI.h>
class SDClass : public FSbase { public:
  bool begin(uint8_t cs = 0, SPIClass& spi = *(SPIClass*)nullptr, uint32_t freq = 4000000);
  void end(); uint64_t cardSize(); uint64_t totalBytes(); uint64_t usedBytes();
  uint8_t cardType();
};
extern SDClass SD;
#define CARD_NONE 0
#define CARD_MMC 1
#define CARD_SD 2
#define CARD_SDHC 3
