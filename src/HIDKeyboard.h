#pragma once

#include "MeshModule.h"
#include "mesh/generated/meshtastic/mesh.pb.h"
#include "USB.h"
#include "USBHIDKeyboard.h"
#include "USBHIDVendor.h"
#include "class/hid/hid.h"  // For HID_KEY_ constants

// Global USB HID objects
extern USBHIDKeyboard Keyboard;
extern USBHIDVendor   VendorHID;

// LokiBridge wire protocol constants
static constexpr uint8_t LB_CTRL_SHORT          = 0xFF;
static constexpr uint8_t LB_CTRL_RESET          = 0xFE;
static constexpr uint8_t LB_CTRL_START          = 0x00;
static constexpr uint8_t LB_CTRL_CONTINUE       = 0x01;
static constexpr uint8_t LB_CTRL_CHUNK_BOUNDARY = 0x02;
static constexpr uint8_t LB_CTRL_END            = 0x03;

static constexpr size_t  LB_REPORT_SIZE     = 63;
static constexpr size_t  LB_DATA_PER_REPORT = 62;
static constexpr size_t  LB_CHUNK_MAX       = LB_DATA_PER_REPORT * 3; // 186 bytes

class HIDKeyboardModule : public MeshModule
{
  public:
    HIDKeyboardModule();
    virtual ~HIDKeyboardModule() {}

    bool wantPacket(const meshtastic_MeshPacket *p) override;
    bool init();

    // Called from main loop to poll for LokiBridge responses
    void lokiBridgeLoop();

  protected:
    virtual ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;

  private:
    bool isReady();
    bool isHostConnected();
    void convertTextToKeystrokes(const String &text);
    void executeLoKeyScript(const String &script);
    void executeLoKeyCommand(const String &command);
    void typePayload(const char *content);
    uint8_t stringToKeyCode(const String &key);
    void pressKeyCombo(uint8_t modifier, uint8_t key);
    void pressKeyComboMulti(uint8_t modifier1, uint8_t modifier2, uint8_t key);
    void pressEnter();
    void pressTab();
    void pressBackspace();
    void pressSpace();
    void pressLeft();
    void pressRight();

    // LokiBridge methods
    void sendLokiBridgeCmd(const char *cmdStr);
    void sendLokiBridgeReset();
    void processLokiBridgeReport(const uint8_t *buf);
    void flushChunk(bool isFinal);
    void sendLoRaResponse(const char *text);
    void sendLoRaReply(const meshtastic_MeshPacket &mp, const char *text);
    String getLokiBridgeStatus();

    bool hidInitialized = false;
    bool hidReady = false;

    // LokiBridge state
    static constexpr size_t CHUNK_BUF_SIZE = LB_CHUNK_MAX + 1;
    uint8_t chunkBuf[CHUNK_BUF_SIZE] = {};
    size_t  chunkLen = 0;
    bool    messageInProgress = false;
    int     chunkCount = 0;
    bool    awaitingResponse = false;
    bool    shellActive = false;
    NodeNum lokiBridgeSender = 0;

};

extern HIDKeyboardModule *hidKeyboardModule;
