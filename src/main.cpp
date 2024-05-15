#include <hkAuthContext.h>
#include <HomeKey.h>
#include <pb_encode.h>
#include <pb_decode.h>
#include <HomeKeyData.pb.h>
#include <utils.h>
#include <HomeSpan.h>
#include <PN532_SPI.h>
#include "PN532.h"
#include <HAP.h>
#include <chrono>
#include <HK_HomeKit.h>
#include <config.h>
#include <mqtt_client.h>
#include <esp_ota_ops.h>

const char* TAG = "MAIN";

enum lockStates
{
  UNLOCKED,
  LOCKED,
  JAMMED,
  UNKNOWN,
  UNLOCKING,
  LOCKING
};

#define PN532_SS (5)
PN532_SPI pn532spi;
PN532 nfc(pn532spi);

nvs_handle savedData;
HomeKeyData_ReaderData readerData = HomeKeyData_ReaderData_init_zero;
uint8_t ecpData[18] = { 0x6A, 0x2, 0xCB, 0x2, 0x6, 0x2, 0x11, 0x0 };
KeyFlow hkFlow = KeyFlow::kFlowFAST;
SpanCharacteristic* lockCurrentState;
SpanCharacteristic* lockTargetState;
esp_mqtt_client_handle_t client = nullptr;

bool save_to_nvs() {
  uint8_t *buffer = (uint8_t *)malloc(HomeKeyData_ReaderData_size);
  pb_ostream_t ostream = pb_ostream_from_buffer(buffer, HomeKeyData_ReaderData_size);
  bool encodeStatus = pb_encode(&ostream, &HomeKeyData_ReaderData_msg, &readerData);
  LOG(I, "PB ENCODE STATUS: %d", encodeStatus);
  LOG(I, "PB BYTES WRITTEN: %d", ostream.bytes_written);
  esp_err_t set_nvs = nvs_set_blob(savedData, "READERDATA", buffer, ostream.bytes_written);
  esp_err_t commit_nvs = nvs_commit(savedData);
  LOG(D, "NVS SET STATUS: %s", esp_err_to_name(set_nvs));
  LOG(D, "NVS COMMIT STATUS: %s", esp_err_to_name(commit_nvs));
  free(buffer);
  return !set_nvs && !commit_nvs;
}

struct LockManagement : Service::LockManagement
{
  SpanCharacteristic* lockControlPoint;
  SpanCharacteristic* version;
  const char* TAG = "LockManagement";

  LockManagement() : Service::LockManagement() {

    LOG(D,"Configuring LockManagement"); // initialization message

    lockControlPoint = new Characteristic::LockControlPoint();
    version = new Characteristic::Version();

  } // end constructor

}; // end LockManagement

// Function to calculate CRC16
void crc16a(unsigned char* data, unsigned int size, unsigned char* result) {
  unsigned short w_crc = 0x6363;

  for (unsigned int i = 0; i < size; ++i) {
    unsigned char byte = data[i];
    byte = (byte ^ (w_crc & 0x00FF));
    byte = ((byte ^ (byte << 4)) & 0xFF);
    w_crc = ((w_crc >> 8) ^ (byte << 8) ^ (byte << 3) ^ (byte >> 4)) & 0xFFFF;
  }

  result[0] = static_cast<unsigned char>(w_crc & 0xFF);
  result[1] = static_cast<unsigned char>((w_crc >> 8) & 0xFF);
}

// Function to append CRC16 to data
void with_crc16(unsigned char* data, unsigned int size, unsigned char* result) {
  crc16a(data, size, result);
}

struct LockMechanism : Service::LockMechanism
{
  const char* TAG = "LockMechanism";

  LockMechanism() : Service::LockMechanism() {
    LOG(I, "Configuring LockMechanism"); // initialization message
    lockCurrentState = new Characteristic::LockCurrentState(1, true);
    lockTargetState = new Characteristic::LockTargetState(1, true);
    memcpy(ecpData + 8, readerData.reader_group_id, sizeof(readerData.reader_group_id));
    with_crc16(ecpData, 16, ecpData + 16);
  } // end constructor

  boolean update() {
    int targetState = lockTargetState->getNewVal();
    int currentState = lockCurrentState->getVal();
    LOG(I, "New LockState=%d, Current LockState=%d", targetState, lockCurrentState->getVal());
    if (client != nullptr) {
      if (targetState != currentState) {
          esp_mqtt_client_publish(client, MQTT_STATE_TOPIC, targetState == lockStates::UNLOCKED ? std::to_string(lockStates::UNLOCKING).c_str() : std::to_string(lockStates::LOCKING).c_str(), 1, 1, true);
        }
      else {
        esp_mqtt_client_publish(client, MQTT_STATE_TOPIC, std::to_string(currentState).c_str(), 1, 1, true);
      }
      if (MQTT_CUSTOM_STATE_ENABLED) {
        if (targetState == lockStates::UNLOCKED) {
          esp_mqtt_client_publish(client, MQTT_CUSTOM_STATE_TOPIC, std::to_string(customLockActions::UNLOCK).c_str(), 0, 0, false);
        }
        else if(targetState == lockStates::LOCKED) {
          esp_mqtt_client_publish(client, MQTT_CUSTOM_STATE_TOPIC, std::to_string(customLockActions::LOCK).c_str(), 0, 0, false);
        }
      }
    } else LOG(W, "MQTT Client not initialized, cannot publish message");

    return (true);
  }

  void loop() {
    uint8_t res[4];
    uint16_t resLen = 4;
    nfc.writeRegister(0x633d, 0, true);
    nfc.inCommunicateThru(ecpData, sizeof(ecpData), res, &resLen, 100, true);
    uint8_t uid[16];
    uint8_t uidLen = 0;
    uint16_t atqa[1];
    uint8_t sak[1];
    bool passiveTarget = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, atqa, sak, 500, true, true);
    if (passiveTarget) {
      LOG(D, "ATQA: %s", utils::bufToHexString(atqa, 1).c_str());
      LOG(D, "SAK: %s", utils::bufToHexString(sak, 1).c_str());
      LOG(D, "UID: %s", utils::bufToHexString(uid, uidLen).c_str());
      LOG(I, "*** PASSIVE TARGET DETECTED ***");
      auto startTime = std::chrono::high_resolution_clock::now();
      uint8_t data[13] = { 0x00, 0xA4, 0x04, 0x00, 0x07, 0xA0, 0x00, 0x00, 0x08, 0x58, 0x01, 0x01, 0x0 };
      uint8_t selectCmdRes[32];
      uint16_t selectCmdResLength = 32;
      LOG(D, "SELECT HomeKey Applet, APDU: %s", utils::bufToHexString(data, sizeof(data)).c_str());
      nfc.inDataExchange(data, sizeof(data), selectCmdRes, &selectCmdResLength);
      LOG(D, "SELECT HomeKey Applet, Response: %s, Length: %d", utils::bufToHexString(selectCmdRes, selectCmdResLength).c_str(), selectCmdResLength);
      if (selectCmdRes[selectCmdResLength - 2] == 0x90 && selectCmdRes[selectCmdResLength - 1] == 0x00) {
        LOG(D, "*** SELECT HOMEKEY APPLET SUCCESSFUL ***");
        LOG(D, "Reader Private Key: %s", utils::bufToHexString((const uint8_t*)readerData.reader_pk, sizeof(readerData.reader_pk)).c_str());
        HKAuthenticationContext authCtx(nfc, readerData, savedData);
        auto authResult = authCtx.authenticate(hkFlow);
        if (std::get<2>(authResult) != KeyFlow::kFlowFailed) {
          json payload;
          payload["issuerId"] = utils::bufToHexString(std::get<0>(authResult), 8, true);
          payload["endpointId"] = utils::bufToHexString(std::get<1>(authResult), 6, true);
          payload["homekey"] = true;
          std::string payloadStr = payload.as<std::string>();
          if (client != nullptr) {
            esp_mqtt_client_publish(client, MQTT_AUTH_TOPIC, payloadStr.c_str(), payloadStr.length(), 0, false);
            if (MQTT_HOMEKEY_ALWAYS_UNLOCK) {
              lockCurrentState->setVal(lockStates::UNLOCKED);
              lockTargetState->setVal(lockStates::UNLOCKED);
              esp_mqtt_client_publish(client, MQTT_STATE_TOPIC, std::to_string(lockStates::UNLOCKED).c_str(), 1, 1, true);
              if (MQTT_CUSTOM_STATE_ENABLED) {
                esp_mqtt_client_publish(client, MQTT_CUSTOM_STATE_TOPIC, std::to_string(customLockActions::UNLOCK).c_str(), 0, 0, false);
              }
            }
            else if (MQTT_HOMEKEY_ALWAYS_LOCK) {
              lockCurrentState->setVal(lockStates::LOCKED);
              lockTargetState->setVal(lockStates::LOCKED);
              esp_mqtt_client_publish(client, MQTT_STATE_TOPIC, std::to_string(lockStates::LOCKED).c_str(), 1, 1, true);
              if (MQTT_CUSTOM_STATE_ENABLED) {
                esp_mqtt_client_publish(client, MQTT_CUSTOM_STATE_TOPIC, std::to_string(customLockActions::LOCK).c_str(), 0, 0, false);
              }
            }
            else {
              if (MQTT_CUSTOM_STATE_ENABLED) {
                int currentState = lockCurrentState->getVal();
                if (currentState == lockStates::UNLOCKED) {
                esp_mqtt_client_publish(client, MQTT_CUSTOM_STATE_TOPIC, std::to_string(customLockActions::LOCK).c_str(), 0, 0, false);
                }
                else if(currentState == lockStates::LOCKED) {
                esp_mqtt_client_publish(client, MQTT_CUSTOM_STATE_TOPIC, std::to_string(customLockActions::UNLOCK).c_str(), 0, 0, false);
                }
              }
            }
          }
          else LOG(W, "MQTT Client not initialized, cannot publish message");

          auto stopTime = std::chrono::high_resolution_clock::now();
          LOG(I, "Total Time (from detection to mqtt publish): %lli ms", std::chrono::duration_cast<std::chrono::milliseconds>(stopTime - startTime).count());
        }
        else {
          LOG(W, "We got status FlowFailed, mqtt untouched!");
        }
      }
      else {
        json_options options;
        options.byte_string_format(byte_string_chars_format::base16);
        json payload;
        uint8_t atqa_u8[2];
        atqa_u8[0] = static_cast<uint8_t>((atqa[0] & 0xFF00) >> 8);
        atqa_u8[1] = static_cast<uint8_t>(atqa[0] & 0xFF00);
        payload["atqa"] = byte_string(atqa_u8, 2);
        payload["sak"] = byte_string(sak, 1);
        payload["uid"] = byte_string(uid, uidLen);
        payload["homekey"] = false;
        if (client != nullptr) {
          esp_mqtt_client_publish(client, MQTT_AUTH_TOPIC, payload.as<std::string>().c_str(), 0, 0, false);
        } else LOG(W, "MQTT Client not initialized, cannot publish message");
      }
      delay(500);
      nfc.inRelease();
      nfc.setPassiveActivationRetries(10);
      int counter = 50;
      bool deviceStillInField = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen);
      LOG(D, "Target still present: %d", deviceStillInField);
      while (deviceStillInField) {
        if (counter == 0) break;
        delay(300);
        nfc.inRelease();
        deviceStillInField = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen);
        --counter;
        LOG(D, "Target still present: %d counter=%d", deviceStillInField, counter);
      }
      nfc.inRelease();
      nfc.setPassiveActivationRetries(0);
    }
  }
};

struct NFCAccess : Service::NFCAccess
{
  SpanCharacteristic* configurationState;
  SpanCharacteristic* nfcControlPoint;
  SpanCharacteristic* nfcSupportedConfiguration;
  const char* TAG = "NFCAccess";

  NFCAccess() : Service::NFCAccess() {
    LOG(I, "Configuring NFCAccess"); // initialization message
    configurationState = new Characteristic::ConfigurationState();
    nfcControlPoint = new Characteristic::NFCAccessControlPoint();
    nfcSupportedConfiguration = new Characteristic::NFCAccessSupportedConfiguration();
  }


  boolean update() {
    LOG(D, "PROVISIONED READER KEY: %s", utils::bufToHexString(readerData.reader_pk, sizeof(readerData.reader_pk)).c_str());
    LOG(D, "READER GROUP IDENTIFIER: %s", utils::bufToHexString(readerData.reader_group_id, sizeof(readerData.reader_group_id)).c_str());
    LOG(D, "READER UNIQUE IDENTIFIER: %s", utils::bufToHexString(readerData.reader_id, sizeof(readerData.reader_id)).c_str());

    // char* dataConfState = configurationState->getNewString(); // Underlying functionality currently unknown
    char* dataNfcControlPoint = nfcControlPoint->getNewString();
    LOG(D, "NfcControlPoint Length: %d", strlen(dataNfcControlPoint));
    std::vector<uint8_t> decB64 = utils::decodeB64(dataNfcControlPoint);
    if (decB64.size() == 0)
      return false;
    LOG(D, "Decoded data: %s", utils::bufToHexString(decB64.data(), decB64.size()).c_str());
    LOG(D, "Decoded data length: %d", decB64.size());
    HK_HomeKit hkCtx(readerData, savedData, "READERDATA");
    std::vector<uint8_t> result = hkCtx.processResult(decB64);
    if (strlen((const char*)readerData.reader_group_id) > 0) {
      memcpy(ecpData + 8, readerData.reader_group_id, sizeof(readerData.reader_group_id));
      with_crc16(ecpData, 16, ecpData + 16);
    }
    TLV8 res(NULL, 0);
    res.unpack(result.data(), result.size());
    nfcControlPoint->setTLV(res, false);
    return true;
  }

};

void deleteReaderData(const char* buf) {
  memset(&readerData, 0, sizeof(readerData));
  esp_err_t erase_nvs = nvs_erase_key(savedData, "READERDATA");
  esp_err_t commit_nvs = nvs_commit(savedData);
  LOG(D,"*** NVS W STATUS");
  LOG(D,"ERASE: %s", esp_err_to_name(erase_nvs));
  LOG(D,"COMMIT: %s", esp_err_to_name(commit_nvs));
  LOG(D,"*** NVS W STATUS");
}

void pairCallback() {
  if (HAPClient::nAdminControllers() == 0) {
    deleteReaderData(NULL);
    return;
  }
  for (auto it=homeSpan.controllerListBegin(); it!=homeSpan.controllerListEnd(); ++it) {
      std::vector<uint8_t> id = utils::getHashIdentifier(it->getLTPK(), 32, true);
      LOG(D, "Found allocated controller - Hash: %s", utils::bufToHexString(id.data(), 8).c_str());
      HomeKeyData_KeyIssuer* foundIssuer = nullptr;
      for (auto* issuer = readerData.issuers; issuer != (readerData.issuers + readerData.issuers_count); ++issuer) {
        if (!memcmp(issuer->issuer_id, id.data(), 8)) {
          LOG(D, "Issuer %s already added, skipping", utils::bufToHexString(issuer->issuer_id, 8).c_str());
          foundIssuer = issuer;
          break;
        }
      }
      if (foundIssuer == nullptr) {
        LOG(D, "Adding new issuer - ID: %s", utils::bufToHexString(id.data(), 8).c_str());
        memcpy(readerData.issuers[readerData.issuers_count].issuer_id, id.data(), 8);
        memcpy(readerData.issuers[readerData.issuers_count].issuer_pk, it->getLTPK(), 32);
        readerData.issuers_count++;
      }
  }
  save_to_nvs();
}

void setFlow(const char* buf) {
  switch (buf[1]) {
  case '0':
    hkFlow = KeyFlow::kFlowFAST;
    Serial.println("FAST Flow");
    break;

  case '1':
    hkFlow = KeyFlow::kFlowSTANDARD;
    Serial.println("STANDARD Flow");
    break;
  case '2':
    hkFlow = KeyFlow::kFlowATTESTATION;
    Serial.println("ATTESTATION Flow");
    break;

  default:
    Serial.println("0 = FAST flow, 1 = STANDARD Flow, 2 = ATTESTATION Flow");
    break;
  }
}

void setLogLevel(const char* buf) {
  esp_log_level_t level = esp_log_level_get("*");
  if (strncmp(buf + 1, "E", 1) == 0) {
    level = ESP_LOG_ERROR;
    Serial.println("ERROR");
  }
  else if (strncmp(buf + 1, "W", 1) == 0) {
    level = ESP_LOG_WARN;
    Serial.println("WARNING");
  }
  else if (strncmp(buf + 1, "I", 1) == 0) {
    level = ESP_LOG_INFO;
    Serial.println("INFO");
  }
  else if (strncmp(buf + 1, "D", 1) == 0) {
    level = ESP_LOG_DEBUG;
    Serial.println("DEBUG");
  }
  else if (strncmp(buf + 1, "V", 1) == 0) {
    level = ESP_LOG_VERBOSE;
    Serial.println("VERBOSE");
  }
  else if (strncmp(buf + 1, "N", 1) == 0) {
    level = ESP_LOG_NONE;
    Serial.println("NONE");
  }

  esp_log_level_set("HKAuthCtx", level);
  esp_log_level_set("HKFastAuth", level);
  esp_log_level_set("HKStdAuth", level);
  esp_log_level_set("HKAttestAuth", level);
  esp_log_level_set("PN532", level);
  esp_log_level_set("PN532_SPI", level);
  esp_log_level_set("ISO18013_SC", level);
}

void print_issuers(const char* buf) {
  const char* TAG = "print_issuers";
  for (auto* issuer = readerData.issuers; issuer != (readerData.issuers + readerData.issuers_count); ++issuer) {
    LOG(D, "Issuer ID: %s, Public Key: %s", utils::bufToHexString(issuer->issuer_id, sizeof(issuer->issuer_id)).c_str(), utils::bufToHexString(issuer->issuer_pk, sizeof(issuer->issuer_pk)).c_str());
    for (auto* endpoint = issuer->endpoints; endpoint != (issuer->endpoints + issuer->endpoints_count); ++endpoint) {
      LOG(D, "Endpoint ID: %s, Public Key: %s", utils::bufToHexString(endpoint->ep_id, sizeof(endpoint->ep_id)).c_str(), utils::bufToHexString(endpoint->ep_pk, sizeof(endpoint->ep_pk)).c_str());
    }
  }
}

/**
 * The function `set_custom_state_handler` translate the custom states to their HomeKit counterpart
 * updating the state of the lock and publishes the new state to the `MQTT_STATE_TOPIC` MQTT topic.
 *
 * @param client The `client` parameter in the `set_custom_state_handler` function is of type
 * `esp_mqtt_client_handle_t`, which is a handle to the MQTT client object for this event. This
 * parameter is used to interact with the MQTT client
 * @param state The `state` parameter in the `set_custom_state_handler` function represents the
 *  received custom state value
 */
void set_custom_state_handler(esp_mqtt_client_handle_t client, int state) {
  switch (state)
  {
    case customLockStates::C_UNLOCKING:
      lockTargetState->setVal(lockStates::UNLOCKED);
      esp_mqtt_client_publish(client, MQTT_STATE_TOPIC, std::to_string(lockStates::UNLOCKING).c_str(), 0, 1, true);
      break;
    case customLockStates::C_LOCKING:
      lockTargetState->setVal(lockStates::LOCKED);
      esp_mqtt_client_publish(client, MQTT_STATE_TOPIC, std::to_string(lockStates::LOCKING).c_str(), 0, 1, true);
      break;
    case customLockStates::C_UNLOCKED:
      lockCurrentState->setVal(lockStates::UNLOCKED);
      esp_mqtt_client_publish(client, MQTT_STATE_TOPIC, std::to_string(lockStates::UNLOCKED).c_str(), 0, 1, true);
      break;
    case customLockStates::C_LOCKED:
      lockCurrentState->setVal(lockStates::LOCKED);
      esp_mqtt_client_publish(client, MQTT_STATE_TOPIC, std::to_string(lockStates::LOCKED).c_str(), 0, 1, true);
      break;
    case customLockStates::C_JAMMED:
      lockCurrentState->setVal(lockStates::JAMMED);
      esp_mqtt_client_publish(client, MQTT_STATE_TOPIC, std::to_string(lockStates::JAMMED).c_str(), 0, 1, true);
      break;
    case customLockStates::C_UNKNOWN:
      lockCurrentState->setVal(lockStates::UNKNOWN);
      esp_mqtt_client_publish(client, MQTT_STATE_TOPIC, std::to_string(lockStates::UNKNOWN).c_str(), 0, 1, true);
    default:
      LOG(D, "Update state failed! Recv value not valid");
      break;
  }
}

void set_state_handler(esp_mqtt_client_handle_t client, int state) {
  switch (state)
  {
    case lockStates::UNLOCKED:
      lockTargetState->setVal(state);
      lockCurrentState->setVal(state);
      esp_mqtt_client_publish(client, MQTT_STATE_TOPIC, std::to_string(lockStates::UNLOCKED).c_str(), 0, 1, true);
      if (MQTT_CUSTOM_STATE_ENABLED) {
        esp_mqtt_client_publish(client, MQTT_CUSTOM_STATE_TOPIC, std::to_string(customLockActions::UNLOCK).c_str(), 0, 0, false);
      }
      break;
    case lockStates::LOCKED:
      lockTargetState->setVal(state);
      lockCurrentState->setVal(state);
      esp_mqtt_client_publish(client, MQTT_STATE_TOPIC, std::to_string(lockStates::LOCKED).c_str(), 0, 1, true);
      if (MQTT_CUSTOM_STATE_ENABLED) {
        esp_mqtt_client_publish(client, MQTT_CUSTOM_STATE_TOPIC, std::to_string(customLockActions::LOCK).c_str(), 0, 0, false);
      }
      break;
    case lockStates::JAMMED:
    case lockStates::UNKNOWN:
      lockCurrentState->setVal(state);
      esp_mqtt_client_publish(client, MQTT_STATE_TOPIC, std::to_string(state).c_str(), 0, 1, true);
      break;
    default:
      LOG(D, "Update state failed! Recv value not valid");
      break;
  }
}

/**
 * The `mqtt_event_handler` function handles MQTT events, including connection, message reception, and
 * publishing of device discovery information.
 * 
 * @param event_data The `event_data` parameter in the `mqtt_event_handler` function is a handle to the
 * MQTT event data structure. It contains information about the event that occurred during the MQTT
 * communication, such as the event type, client handle, topic, data, and other relevant details. The
 * function processes different MQTT
 * 
 * @return The function `mqtt_event_handler` is returning an `esp_err_t` type, specifically `ESP_OK`.
 */
esp_err_t mqtt_event_handler(esp_mqtt_event_handle_t event_data)
{
  ESP_LOGD(TAG, "Event dispatched from callback type=%d", event_data->event_id);
  esp_mqtt_client_handle_t client = event_data->client;
  const esp_app_desc_t* app_desc = esp_ota_get_app_description();
  std::string app_version = app_desc->version;
  std::string topic(event_data->topic, event_data->topic + event_data->topic_len);
  std::string data(event_data->data, event_data->data + event_data->data_len);
  if (event_data->event_id == MQTT_EVENT_CONNECTED) {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char macStr[18] = { 0 };
    sprintf(macStr, "%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3]);
    std::string serialNumber = "HK-";
    serialNumber.append(macStr);
    LOG(D, "MQTT connected");
    if (DISCOVERY) {
      json payload;
      payload["topic"] = MQTT_AUTH_TOPIC;
      payload["value_template"] = "{{ value_json.uid }}";
      json device;
      device["name"] = "Lock";
      char identifier[18];
      sprintf(identifier, "%.2s%.2s%.2s%.2s%.2s%.2s", HAPClient::accessory.ID, HAPClient::accessory.ID + 3, HAPClient::accessory.ID + 6, HAPClient::accessory.ID + 9, HAPClient::accessory.ID + 12, HAPClient::accessory.ID + 15);
      std::string id = identifier;
      device["identifiers"] = json_array_arg;
      device["identifiers"].push_back(id);
      device["identifiers"].push_back(serialNumber);
      device["manufacturer"] = "rednblkx";
      device["model"] = "HomeKey-ESP32";
      device["sw_version"] = app_version.c_str();
      device["serial_number"] = serialNumber;
      payload["device"] = device;
      std::string bufferpub = payload.as<std::string>();
      esp_mqtt_client_publish(client, ("homeassistant/tag/" MQTT_CLIENTID "/rfid/config"), bufferpub.c_str(), bufferpub.length(), 1, true);
      payload = json();
      payload["topic"] = MQTT_AUTH_TOPIC;
      payload["value_template"] = "{{ value_json.issuerId }}";
      payload["device"] = device;
      bufferpub = payload.as<std::string>();
      esp_mqtt_client_publish(client, ("homeassistant/tag/" MQTT_CLIENTID "/hkIssuer/config"), bufferpub.c_str(), bufferpub.length(), 1, true);
      payload = json();
      payload["topic"] = MQTT_AUTH_TOPIC;
      payload["value_template"] = "{{ value_json.endpointId }}";
      payload["device"] = device;
      bufferpub = payload.as<std::string>();
      esp_mqtt_client_publish(client, ("homeassistant/tag/" MQTT_CLIENTID "/hkEndpoint/config"), bufferpub.c_str(), bufferpub.length(), 1, true);
      payload = json();
      payload["name"] = NAME;
      payload["state_topic"] = MQTT_STATE_TOPIC;
      payload["command_topic"] = MQTT_SET_STATE_TOPIC;
      payload["payload_lock"] = "1";
      payload["payload_unlock"] = "0";
      payload["state_locked"] = "1";
      payload["state_unlocked"] = "0";
      payload["state_unlocking"] = "4";
      payload["state_locking"] = "5";
      payload["state_jammed"] = "2";
      payload["availability_topic"] = MQTT_CLIENTID"/status";
      payload["unique_id"] = id;
      payload["device"] = device;
      payload["retain"] = "false";
      bufferpub = payload.as<std::string>();
      esp_mqtt_client_publish(client,("homeassistant/lock/" MQTT_CLIENTID "/lock/config"), bufferpub.c_str(), bufferpub.length(), 1, true);
      LOG(D, "MQTT PUBLISHED DISCOVERY");
    }
    esp_mqtt_client_publish(client, MQTT_CLIENTID"/status", "online", 6, 1, true);
    if (MQTT_CUSTOM_STATE_ENABLED) {
      esp_mqtt_client_subscribe(client, MQTT_CUSTOM_STATE_CTRL_TOPIC, 0);
    }
    esp_mqtt_client_subscribe(client, MQTT_SET_STATE_TOPIC, 0);
    esp_mqtt_client_subscribe(client, MQTT_SET_CURRENT_STATE_TOPIC, 0);
    esp_mqtt_client_subscribe(client, MQTT_SET_TARGET_STATE_TOPIC, 0);
  }
  else if (event_data->event_id == MQTT_EVENT_DATA) {
    LOG(D, "Received message in topic \"%s\": %s", topic.c_str(), data.c_str());
    int state = atoi(data.c_str());
    if (!strcmp(MQTT_CUSTOM_STATE_CTRL_TOPIC, topic.c_str())) {
      set_custom_state_handler(client, state);
    }
    else if (!strcmp(MQTT_SET_STATE_TOPIC, topic.c_str())) {
      set_state_handler(client, state);
    }
    else if (!strcmp(MQTT_SET_TARGET_STATE_TOPIC, topic.c_str())) {
        if (state == lockStates::UNLOCKED || state == lockStates::LOCKED) {
          lockTargetState->setVal(state);
          esp_mqtt_client_publish(client, MQTT_STATE_TOPIC, state == lockStates::UNLOCKED ? std::to_string(lockStates::UNLOCKING).c_str() : std::to_string(lockStates::LOCKING).c_str(), 0, 1, true);
        }
    }
    else if (!strcmp(MQTT_SET_CURRENT_STATE_TOPIC, topic.c_str())) {
      if (state == lockStates::UNLOCKED || state == lockStates::LOCKED || state == lockStates::JAMMED || state == lockStates::UNKNOWN) {
        lockCurrentState->setVal(state);
        esp_mqtt_client_publish(client, MQTT_STATE_TOPIC, std::to_string(lockCurrentState->getVal()).c_str(), 0, 1, true);
      }
    }
  }
  return ESP_OK;
}

/**
 * The function `mqtt_app_start` initializes and starts an MQTT client with specified configuration
 * parameters.
 */
static void mqtt_app_start(void)
{
  esp_mqtt_client_config_t mqtt_cfg = { };
  mqtt_cfg.host = MQTT_HOST;
  mqtt_cfg.port = MQTT_PORT;
  mqtt_cfg.client_id = MQTT_CLIENTID;
  mqtt_cfg.username = MQTT_USERNAME;
  mqtt_cfg.password = MQTT_PASSWORD;
  mqtt_cfg.lwt_topic = MQTT_CLIENTID"/status";
  mqtt_cfg.lwt_msg = "offline";
  mqtt_cfg.lwt_qos = 1;
  mqtt_cfg.lwt_retain = 1;
  mqtt_cfg.lwt_msg_len = 7;
  mqtt_cfg.event_handle = mqtt_event_handler;
  client = esp_mqtt_client_init(&mqtt_cfg);
  esp_mqtt_client_start(client);
}

void wifiCallback() {
  mqtt_app_start();
}

void setup() {
  Serial.begin(115200);
  const esp_app_desc_t* app_desc = esp_ota_get_app_description();
  std::string app_version = app_desc->version;
  size_t len;
  const char* TAG = "SETUP";
  nvs_open("SAVED_DATA", NVS_READWRITE, &savedData);
  if (!nvs_get_blob(savedData, "READERDATA", NULL, &len)) {
    uint8_t savedBuf[len];
    pb_istream_t istream;
    nvs_get_blob(savedData, "READERDATA", savedBuf, &len);
    LOG(I, "NVS DATA LENGTH: %d", len);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, savedBuf, len, ESP_LOG_DEBUG);
    istream = pb_istream_from_buffer(savedBuf, len);
    bool decodeStatus = pb_decode(&istream, &HomeKeyData_ReaderData_msg, &readerData);
    LOG(I, "PB DECODE STATUS: %d", decodeStatus);
    if (!decodeStatus) {
      LOG(I, "PB ERROR: %s", PB_GET_ERROR(&istream));
    }
  }
  nfc.begin();
  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    ESP_LOGE("NFC_SETUP", "Didn't find PN53x board");
  }
  else {
    ESP_LOGI("NFC_SETUP", "Found chip PN5%x", (versiondata >> 24) & 0xFF);
    ESP_LOGI("NFC_SETUP", "Firmware ver. %d.%d", (versiondata >> 16) & 0xFF, (versiondata >> 8) & 0xFF);
    nfc.SAMConfig();
    nfc.setPassiveActivationRetries(0);
    nfc.writeRegister(0x633d, 0);
    ESP_LOGI("NFC_SETUP", "Waiting for an ISO14443A card");
  }
  homeSpan.setControlPin(CONTROL_PIN);
  homeSpan.setStatusPin(LED_PIN);
  homeSpan.setStatusAutoOff(15);
  homeSpan.reserveSocketConnections(2);
  homeSpan.setPairingCode(HK_CODE);
  homeSpan.setLogLevel(0);
  homeSpan.setSketchVersion(app_version.c_str());

  LOG(I, "READER GROUP ID (%d): %s", sizeof(readerData.reader_group_id), utils::bufToHexString(readerData.reader_group_id, sizeof(readerData.reader_group_id)).c_str());
  LOG(I, "READER UNIQUE ID (%d): %s", sizeof(readerData.reader_id), utils::bufToHexString(readerData.reader_id, sizeof(readerData.reader_id)).c_str());

  LOG(I, "HOMEKEY ISSUERS: %d", readerData.issuers_count);
  for (auto* issuer = readerData.issuers; issuer != (readerData.issuers + readerData.issuers_count); ++issuer) {
    LOG(D, "Issuer ID: %s, Public Key: %s", utils::bufToHexString(issuer->issuer_id, sizeof(issuer->issuer_id)).c_str(), utils::bufToHexString(issuer->issuer_pk, sizeof(issuer->issuer_pk)).c_str());
  }
  homeSpan.enableOTA(OTA_PWD);
  homeSpan.begin(Category::Locks, NAME);

  new SpanUserCommand('D', "Delete Home Key Data", deleteReaderData);
  new SpanUserCommand('L', "Set Log Level", setLogLevel);
  new SpanUserCommand('F', "Set HomeKey Flow", setFlow);
  new SpanUserCommand('P', "Print Issuers", print_issuers);
  new SpanUserCommand('R', "Remove Endpoints", [](const char*) {
    for (auto&& issuer : readerData.issuers) {
      memset(issuer.endpoints, 0, sizeof(issuer.endpoints));
      issuer.endpoints_count = 0;
    }
    save_to_nvs();
  });


  new SpanAccessory();
  new Service::AccessoryInformation();
  new Characteristic::Identify();
  new Characteristic::Manufacturer("rednblkx");
  new Characteristic::Model("HomeKey-ESP32");
  new Characteristic::Name(NAME);
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char macStr[18] = { 0 };
  sprintf(macStr, "%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3]);
  std::string serialNumber = "HK-";
  serialNumber.append(macStr);
  new Characteristic::SerialNumber(serialNumber.c_str());
  new Characteristic::FirmwareRevision(app_version.c_str());
  new Characteristic::HardwareFinish();

  new LockManagement();
  new LockMechanism();
  new NFCAccess();
  new Service::HAPProtocolInformation();
  new Characteristic::Version();
  homeSpan.setControllerCallback(pairCallback);
  homeSpan.setWifiCallback(wifiCallback);

}

//////////////////////////////////////

void loop() {
  homeSpan.poll();
}
