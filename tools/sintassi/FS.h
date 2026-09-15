#pragma once
#include <Arduino.h>
#define FILE_READ  "r"
#define FILE_WRITE "w"
#define FILE_APPEND "a"
class File { public:
  operator bool() const; void close(); size_t size(); bool isDirectory();
  int read(); size_t read(uint8_t*,size_t); size_t write(const uint8_t*,size_t);
  size_t write(uint8_t); int printf(const char*,...); size_t println(const char*);
  size_t print(const char*); void flush(); bool seek(uint32_t);
  const char* name(); File openNextFile();
  int available(); String readStringUntil(char); size_t position();
  size_t println(); size_t print(int); size_t print(unsigned long);
};
class FSbase { public:
  File open(const char*, const char* mode = FILE_READ);
  bool exists(const char*); bool mkdir(const char*); bool remove(const char*);
  bool rmdir(const char*);
};
