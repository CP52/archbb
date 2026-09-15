#pragma once
// Stub minimo di NimBLE-Arduino 1.4.x — solo cio' che ArchBB usa.
#include <Arduino.h>
#include <cstdint>
#include <string>
struct ble_gap_conn_desc { uint16_t conn_handle; uint16_t conn_itvl; uint16_t conn_latency; };
namespace NIMBLE_PROPERTY {
  static const uint32_t READ = 1, WRITE = 2, NOTIFY = 4, INDICATE = 8,
                        WRITE_NR = 16, BROADCAST = 32, READ_ENC = 64,
                        WRITE_ENC = 128;
}
// Livelli di potenza radio (esp_bt.h nel mondo vero).
enum esp_power_level_t { ESP_PWR_LVL_N12, ESP_PWR_LVL_N9, ESP_PWR_LVL_N6,
                         ESP_PWR_LVL_N3, ESP_PWR_LVL_N0, ESP_PWR_LVL_P3,
                         ESP_PWR_LVL_P6, ESP_PWR_LVL_P9 };
class NimBLECharacteristic;
class NimBLECharacteristicCallbacks { public:
  enum Status { SUCCESS_INDICATE, SUCCESS_NOTIFY, ERROR_INDICATE_DISABLED,
                ERROR_NOTIFY_DISABLED, ERROR_GATT, ERROR_NO_CLIENT,
                ERROR_INDICATE_TIMEOUT, ERROR_INDICATE_FAILURE };
  virtual ~NimBLECharacteristicCallbacks() {}
  virtual void onRead(NimBLECharacteristic*) {}
  virtual void onWrite(NimBLECharacteristic*) {}
  virtual void onStatus(NimBLECharacteristic*, Status, int) {}
  virtual void onSubscribe(NimBLECharacteristic*, ble_gap_conn_desc*, uint16_t) {}
};
// NimBLEAttValue: nella 1.4.x getValue() ritorna questo, e .data() da' un
// const uint8_t*. Uno std::string non andrebbe bene (data() -> const char*):
// lo stub deve rispecchiare il tipo VERO, altrimenti il banco darebbe errori
// che sul dispositivo non esistono — o peggio, non ne darebbe dove ci sono.
class NimBLEAttValue { public:
  const uint8_t* data() const; size_t length() const; size_t size() const;
  const char* c_str() const;
};
class NimBLECharacteristic { public:
  void setValue(const uint8_t*, size_t); void setValue(uint8_t*, size_t);
  void setValue(const char*); void notify(bool is_notification = true);
  void setCallbacks(NimBLECharacteristicCallbacks*);
  NimBLEAttValue getValue();
  uint8_t* getData(); size_t getDataLength();
};
class NimBLEService { public:
  NimBLECharacteristic* createCharacteristic(const char*, uint32_t);
  bool start();
};
class NimBLEServerCallbacks { public:
  virtual ~NimBLEServerCallbacks() {}
  virtual void onConnect(class NimBLEServer*) {}
  virtual void onConnect(class NimBLEServer*, ble_gap_conn_desc*) {}
  virtual void onDisconnect(class NimBLEServer*) {}
  virtual void onMTUChange(uint16_t, ble_gap_conn_desc*) {}
};
class NimBLEAdvertising { public:
  void addServiceUUID(const char*); void setScanResponse(bool); bool start();
  void setMinPreferred(uint16_t); void setMaxPreferred(uint16_t);
};
class NimBLEServer { public:
  NimBLEService* createService(const char*);
  void setCallbacks(NimBLEServerCallbacks*);
  NimBLEAdvertising* getAdvertising();
  size_t getConnectedCount(); uint16_t getPeerMTU(uint16_t);
  void disconnect(uint16_t);
  bool updateConnParams(uint16_t handle, uint16_t minItvl, uint16_t maxItvl,
                        uint16_t latency, uint16_t timeout);
};
class NimBLEDevice { public:
  static void init(const char*); static void deinit(bool clearAll = false);
  static NimBLEServer* createServer(); static NimBLEAdvertising* getAdvertising();
  static bool startAdvertising(); static bool stopAdvertising();
  static void setMTU(uint16_t); static uint16_t getMTU();
  static void setPower(esp_power_level_t, int type = 0); static bool getInitialized();
};
