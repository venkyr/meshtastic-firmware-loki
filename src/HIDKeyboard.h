#pragma once

#include "MeshModule.h"
#include "mesh/generated/meshtastic/mesh.pb.h"
#include "USB.h"
#include "USBHIDKeyboard.h"
#include "class/hid/hid.h"  // For HID_KEY_ constants

// Global USB HID Keyboard object (like in working standalone project)
extern USBHIDKeyboard Keyboard;

class HIDKeyboardModule : public MeshModule
{
  public:
    HIDKeyboardModule();
    virtual ~HIDKeyboardModule() {}

    bool wantPacket(const meshtastic_MeshPacket *p) override;
    bool init();

  protected:
    virtual ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;

  private:
    bool isReady();
    bool isHostConnected();
    void convertTextToKeystrokes(const String &text);
    void executeLoKeyScript(const String &script);
    void executeLoKeyCommand(const String &command);
    uint8_t stringToKeyCode(const String &key);
    void pressKeyCombo(uint8_t modifier, uint8_t key);
    void pressKeyComboMulti(uint8_t modifier1, uint8_t modifier2, uint8_t key);
    void pressEnter();
    void pressTab();
    void pressBackspace();
    void pressSpace();
    void pressLeft();
    void pressRight();

    bool hidInitialized = false;
    bool hidReady = false;
};

extern HIDKeyboardModule *hidKeyboardModule;