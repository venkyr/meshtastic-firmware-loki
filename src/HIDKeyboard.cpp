#include "HIDKeyboard.h"
#include "mesh/generated/meshtastic/mesh.pb.h"
#include "mesh/MeshTypes.h"  // For isToUs() and isBroadcast()
#include "Router.h"
#include "MeshService.h"
#ifdef ARCH_ESP32
#include "tusb.h"  // For tud_mounted() to check USB host connection
#endif

// Global USB HID Keyboard object (like in working standalone project)
USBHIDKeyboard Keyboard;

HIDKeyboardModule *hidKeyboardModule;

HIDKeyboardModule::HIDKeyboardModule() : MeshModule("HIDKeyboard")
{
    LOG_DEBUG("HID Keyboard: Initializing HID at boot time");
    
    // Initialize HID at boot time (exactly like working standalone project)
    // Initialize USB with HID only (like in working setup())
    USB.begin();
    
    // Initialize USB HID Keyboard (like in working setup())
    Keyboard.begin();
    
    // Wait a moment for USB HID to initialize (like in working setup())
    delay(1000);
    
    LOG_DEBUG("HID Keyboard: USB HID Keyboard Ready!");
    LOG_DEBUG("HID Keyboard: Soldered USB: HID Keyboard (GPIO 19/20)");
    LOG_DEBUG("HID Keyboard: UART Serial: Communication port");
    LOG_DEBUG("HID Keyboard: Send LoRa messages to type on HID keyboard");
    
    hidInitialized = true;
    hidReady = true;
    
    LOG_DEBUG("HID Keyboard: HID ready for keystrokes at boot");
}

bool HIDKeyboardModule::wantPacket(const meshtastic_MeshPacket *p)
{
    // Only process text messages
    return p->decoded.portnum == meshtastic_PortNum_TEXT_MESSAGE_APP;
}

bool HIDKeyboardModule::init()
{
    LOG_DEBUG("HID Keyboard: init() called - HID already initialized at boot");
    // HID is already initialized in constructor
    return true;
}

bool HIDKeyboardModule::isReady()
{
    return hidInitialized && hidReady;
}

bool HIDKeyboardModule::isHostConnected()
{
    // Check if USB host (PC) is actually connected and the device is enumerated
    // For ESP32 with TinyUSB, use tud_mounted() to check if device is enumerated by host
    #ifdef ARCH_ESP32
        // tud_mounted() returns true when the USB device is enumerated by a host
        // Note: This may return true even when connected to a hub without a PC,
        // but it's the best available detection method for ESP32
        return tud_mounted();
    #else
        // For other platforms, fall back to basic ready check
        return isReady();
    #endif
}

ProcessMessage HIDKeyboardModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    LOG_DEBUG("HID Keyboard: Message received");
    
    // HID is already initialized at boot, just check if ready
    if (!isReady()) {
        LOG_DEBUG("HID Keyboard: Not ready, skipping message");
        return ProcessMessage::CONTINUE;
    }

    // Only process text messages
    if (mp.decoded.portnum != meshtastic_PortNum_TEXT_MESSAGE_APP) {
        LOG_DEBUG("HID Keyboard: Not a text message, skipping");
        return ProcessMessage::CONTINUE;
    }

    // Check if this is a direct message (not a broadcast)
    bool isDirectMessage = isToUs(&mp);
    bool isBroadcastMessage = isBroadcast(mp.to);
    
    LOG_DEBUG("HID Keyboard: Message type - Direct: %d, Broadcast: %d, From: 0x%x, To: 0x%x", 
              isDirectMessage, isBroadcastMessage, mp.from, mp.to);
    
    // Only process direct messages for security (ignore broadcasts)
    if (!isDirectMessage) {
        LOG_DEBUG("HID Keyboard: Not a direct message, ignoring");
        return ProcessMessage::CONTINUE;
    }
    
    // Extract message text
    String message = String((const char*)mp.decoded.payload.bytes, mp.decoded.payload.size);
    LOG_DEBUG("HID Keyboard: Received message: %s", message.c_str());
    
    // Check if message is "PING" and respond with "PONG"
    String messageUpper = message;
    messageUpper.toUpperCase();
    messageUpper.trim();
    if (messageUpper == "PING") {
        // Only respond with PONG if HID is ready AND a USB host (PC) is actually connected
        bool hostConnected = isReady() && isHostConnected();
        const char *response = hostConnected ? "PONG" : "NOPE";
        const char *logMsg = hostConnected ? "HID Keyboard: Received PING, host connected, sending PONG" 
                                           : "HID Keyboard: Received PING but host not connected, sending NOPE";
        LOG_DEBUG("%s", logMsg);
        
        meshtastic_MeshPacket *p = router->allocForSending();
        p->to = getFrom(&mp);
        p->priority = meshtastic_MeshPacket_Priority_RELIABLE;
        p->want_ack = false;
        p->decoded.want_response = false;
        p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
        memcpy(p->decoded.payload.bytes, response, strlen(response));
        p->decoded.payload.size = strlen(response);
        service->sendToMesh(p);
        
        return ProcessMessage::CONTINUE;
    }
    
    // Check if message is LoKey Script (starts with STRING, STRINGLN, ENTER, TAB, BACKSPACE, SPACE, LEFT, RIGHT, GUI, ALT, CTRL, or DELAY)
    // Case-insensitive check for convenience
    message.toUpperCase();
    if (message.startsWith("STRING ") || message.startsWith("STRINGLN ") || 
        message.startsWith("ENTER") || message.startsWith("TAB") || 
        message.startsWith("BACKSPACE") || message.startsWith("SPACE") || 
        message.startsWith("LEFT") || message.startsWith("RIGHT") || 
        message.startsWith("GUI ") || message.startsWith("ALT ") || 
        message.startsWith("CTRL ") || message.startsWith("DELAY ")) {
        LOG_DEBUG("HID Keyboard: Detected LoKey Script format");
        // Reconstruct original message for execution (preserve case for text content)
        String originalMessage = String((const char*)mp.decoded.payload.bytes, mp.decoded.payload.size);
        executeLoKeyScript(originalMessage);
    } else {
        // Not a LoKey Script command - ignore it (security: only explicit commands execute)
        LOG_DEBUG("HID Keyboard: Not a LoKey Script command, ignoring message");
    }
    
    return ProcessMessage::CONTINUE;
}

void HIDKeyboardModule::convertTextToKeystrokes(const String &text)
{
    LOG_DEBUG("HID Keyboard: Converting text: %s", text.c_str());
    
    // Send the entire text string at once (like in working loop())
    LOG_DEBUG("HID Keyboard: Sending text as keystrokes");
    Keyboard.print(text);
    
    // Add a delay to ensure all keystrokes are processed
    delay(300);
    LOG_DEBUG("HID Keyboard: Text sent as keystrokes");
}

void HIDKeyboardModule::executeLoKeyScript(const String &script)
{
    LOG_DEBUG("HID Keyboard: Executing LoKey Script");
    
    // Parse script line by line (handle both single-line and multi-line)
    int startPos = 0;
    while (startPos < script.length()) {
        int endPos = script.indexOf('\n', startPos);
        if (endPos == -1) {
            endPos = script.length();
        }
        
        String line = script.substring(startPos, endPos);
        line.trim(); // Remove leading/trailing whitespace
        
        if (line.length() > 0) {
            LOG_DEBUG("HID Keyboard: Executing command: %s", line.c_str());
            executeLoKeyCommand(line);
            delay(100); // Small delay between commands
        }
        
        startPos = endPos + 1;
    }
    
    LOG_DEBUG("HID Keyboard: LoKey Script execution complete");
}

void HIDKeyboardModule::executeLoKeyCommand(const String &command)
{
    // Convert to uppercase for case-insensitive command detection
    String commandUpper = command;
    commandUpper.toUpperCase();
    
    if (commandUpper.startsWith("STRING ")) {
        // STRING <text> - Type text
        String text = command.substring(7); // Skip "STRING " (preserve original case for text)
        LOG_DEBUG("HID Keyboard: STRING command: %s", text.c_str());
        Keyboard.print(text);
        delay(100);
    }
    else if (commandUpper.startsWith("STRINGLN ")) {
        // STRINGLN <text> - Type text and press Enter
        String text = command.substring(9); // Skip "STRINGLN " (preserve original case for text)
        LOG_DEBUG("HID Keyboard: STRINGLN command: %s", text.c_str());
        Keyboard.print(text);
        delay(100);
        pressEnter();
    }
    else if (commandUpper == "ENTER") {
        // ENTER - Press Enter key
        LOG_DEBUG("HID Keyboard: ENTER command");
        pressEnter();
    }
    else if (commandUpper == "TAB") {
        // TAB - Press Tab key
        LOG_DEBUG("HID Keyboard: TAB command");
        pressTab();
    }
    else if (commandUpper == "BACKSPACE") {
        // BACKSPACE - Press Backspace key
        LOG_DEBUG("HID Keyboard: BACKSPACE command");
        pressBackspace();
    }
    else if (commandUpper == "SPACE") {
        // SPACE - Press Space key
        LOG_DEBUG("HID Keyboard: SPACE command");
        pressSpace();
    }
    else if (commandUpper == "LEFT") {
        // LEFT - Press Left Arrow key
        LOG_DEBUG("HID Keyboard: LEFT command");
        pressLeft();
    }
    else if (commandUpper == "RIGHT") {
        // RIGHT - Press Right Arrow key
        LOG_DEBUG("HID Keyboard: RIGHT command");
        pressRight();
    }
    else if (commandUpper.startsWith("GUI ")) {
        // GUI <key> - Press Windows key + key
        String key = command.substring(4); // Skip "GUI " (preserve original case for key)
        key.trim();
        LOG_DEBUG("HID Keyboard: GUI command: %s", key.c_str());
        if (key.length() > 0) {
            uint8_t keyCode = stringToKeyCode(key);
            if (keyCode != 0) {
                pressKeyCombo(HID_KEY_GUI_LEFT, keyCode);
            }
        }
    }
    else if (commandUpper.startsWith("ALT ")) {
        // ALT <key> - Press Alt key + key
        String key = command.substring(4); // Skip "ALT " (preserve original case for key)
        key.trim();
        LOG_DEBUG("HID Keyboard: ALT command: %s", key.c_str());
        if (key.length() > 0) {
            uint8_t keyCode = stringToKeyCode(key);
            if (keyCode != 0) {
                pressKeyCombo(HID_KEY_ALT_LEFT, keyCode);
            }
        }
    }
    else if (commandUpper.startsWith("CTRL SHIFT ")) {
        // CTRL SHIFT <key> - Press Ctrl + Shift + key
        String key = command.substring(11); // Skip "CTRL SHIFT " (preserve original case for key)
        key.trim();
        LOG_DEBUG("HID Keyboard: CTRL SHIFT command: %s", key.c_str());
        if (key.length() > 0) {
            uint8_t keyCode = stringToKeyCode(key);
            if (keyCode != 0) {
                pressKeyComboMulti(HID_KEY_CONTROL_LEFT, HID_KEY_SHIFT_LEFT, keyCode);
            }
        }
    }
    else if (commandUpper.startsWith("CTRL ")) {
        // CTRL <key> - Press Ctrl key + key
        String key = command.substring(5); // Skip "CTRL " (preserve original case for key)
        key.trim();
        LOG_DEBUG("HID Keyboard: CTRL command: %s", key.c_str());
        if (key.length() > 0) {
            uint8_t keyCode = stringToKeyCode(key);
            if (keyCode != 0) {
                pressKeyCombo(HID_KEY_CONTROL_LEFT, keyCode);
            }
        }
    }
    else if (commandUpper.startsWith("DELAY ")) {
        // DELAY <milliseconds> - Wait for specified milliseconds
        String delayStr = command.substring(6); // Skip "DELAY " (preserve original case)
        delayStr.trim();
        LOG_DEBUG("HID Keyboard: DELAY command: %s ms", delayStr.c_str());
        if (delayStr.length() > 0) {
            unsigned long delayMs = delayStr.toInt();
            if (delayMs > 0) {
                LOG_DEBUG("HID Keyboard: Delaying for %lu ms", delayMs);
                delay(delayMs);
            } else {
                LOG_DEBUG("HID Keyboard: Invalid delay value: %s", delayStr.c_str());
            }
        }
    }
    else {
        LOG_DEBUG("HID Keyboard: Unknown command: %s", command.c_str());
    }
}

uint8_t HIDKeyboardModule::stringToKeyCode(const String &key)
{
    String keyLower = key;
    keyLower.toLowerCase();
    
    // Handle function keys F1-F12
    if (keyLower.startsWith("f")) {
        int fNum = keyLower.substring(1).toInt();
        if (fNum >= 1 && fNum <= 12) {
            // F1 = 0x3A, F2 = 0x3B, ..., F12 = 0x45
            return (HID_KEY_F1 + fNum - 1);
        }
    }
    
    // Handle single character keys
    if (keyLower.length() == 1) {
        char c = keyLower.charAt(0);
        
        // For letters, they map directly (a-z = HID_KEY_A to HID_KEY_Z)
        if (c >= 'a' && c <= 'z') {
            return (HID_KEY_A + (c - 'a'));
        }
        else if (c >= '0' && c <= '9') {
            // Numbers map to HID_KEY_0 to HID_KEY_9
            // HID_KEY_0 = 0x27, HID_KEY_1 = 0x1E, HID_KEY_2 = 0x1F, ..., HID_KEY_9 = 0x26
            if (c == '0') return HID_KEY_0;
            return (HID_KEY_1 + (c - '1'));
        }
    }
    
    // Handle special key names
    if (keyLower == "enter" || keyLower == "return") return HID_KEY_ENTER;
    if (keyLower == "tab") return HID_KEY_TAB;
    if (keyLower == "space") return HID_KEY_SPACE;
    if (keyLower == "backspace") return HID_KEY_BACKSPACE;
    if (keyLower == "esc" || keyLower == "escape") return HID_KEY_ESCAPE;
    if (keyLower == "delete") return HID_KEY_DELETE;
    if (keyLower == "up") return HID_KEY_ARROW_UP;
    if (keyLower == "down") return HID_KEY_ARROW_DOWN;
    if (keyLower == "left") return HID_KEY_ARROW_LEFT;
    if (keyLower == "right") return HID_KEY_ARROW_RIGHT;
    
    LOG_DEBUG("HID Keyboard: Unknown key: %s", key.c_str());
    return 0; // Invalid key code
}

void HIDKeyboardModule::pressKeyCombo(uint8_t modifier, uint8_t key)
{
    // Press modifier + key simultaneously, then release both
    LOG_DEBUG("HID Keyboard: Pressing key combo: modifier=0x%02X, key=0x%02X", modifier, key);
    
    // Use pressRaw for modifier keys to ensure they work correctly
    Keyboard.pressRaw(modifier);
    delay(20); // Small delay to ensure modifier is registered
    
    // Press the key
    Keyboard.pressRaw(key);
    delay(150); // Hold both keys for a moment
    
    // Release both
    Keyboard.releaseRaw(key);
    delay(20);
    Keyboard.releaseRaw(modifier);
    
    // Ensure all keys are released
    Keyboard.releaseAll();
    delay(100);
}

void HIDKeyboardModule::pressKeyComboMulti(uint8_t modifier1, uint8_t modifier2, uint8_t key)
{
    // Press two modifiers + key simultaneously, then release all
    LOG_DEBUG("HID Keyboard: Pressing multi-key combo: modifier1=0x%02X, modifier2=0x%02X, key=0x%02X", 
              modifier1, modifier2, key);
    
    // Press first modifier
    Keyboard.pressRaw(modifier1);
    delay(20); // Small delay to ensure modifier is registered
    
    // Press second modifier
    Keyboard.pressRaw(modifier2);
    delay(20); // Small delay to ensure modifier is registered
    
    // Press the key
    Keyboard.pressRaw(key);
    delay(150); // Hold all keys for a moment
    
    // Release all
    Keyboard.releaseRaw(key);
    delay(20);
    Keyboard.releaseRaw(modifier2);
    delay(20);
    Keyboard.releaseRaw(modifier1);
    
    // Ensure all keys are released
    Keyboard.releaseAll();
    delay(100);
}

void HIDKeyboardModule::pressEnter()
{
    LOG_DEBUG("HID Keyboard: Pressing ENTER key");
    Keyboard.pressRaw(HID_KEY_ENTER);
    delay(50);
    Keyboard.releaseRaw(HID_KEY_ENTER);
    delay(100);
}

void HIDKeyboardModule::pressTab()
{
    LOG_DEBUG("HID Keyboard: Pressing TAB key");
    Keyboard.pressRaw(HID_KEY_TAB);
    delay(50);
    Keyboard.releaseRaw(HID_KEY_TAB);
    delay(100);
}

void HIDKeyboardModule::pressBackspace()
{
    LOG_DEBUG("HID Keyboard: Pressing BACKSPACE key");
    Keyboard.pressRaw(HID_KEY_BACKSPACE);
    delay(50);
    Keyboard.releaseRaw(HID_KEY_BACKSPACE);
    delay(100);
}

void HIDKeyboardModule::pressSpace()
{
    LOG_DEBUG("HID Keyboard: Pressing SPACE key");
    Keyboard.pressRaw(HID_KEY_SPACE);
    delay(50);
    Keyboard.releaseRaw(HID_KEY_SPACE);
    delay(100);
}

void HIDKeyboardModule::pressLeft()
{
    LOG_DEBUG("HID Keyboard: Pressing LEFT ARROW key");
    Keyboard.pressRaw(HID_KEY_ARROW_LEFT);
    delay(50);
    Keyboard.releaseRaw(HID_KEY_ARROW_LEFT);
    delay(100);
}

void HIDKeyboardModule::pressRight()
{
    LOG_DEBUG("HID Keyboard: Pressing RIGHT ARROW key");
    Keyboard.pressRaw(HID_KEY_ARROW_RIGHT);
    delay(50);
    Keyboard.releaseRaw(HID_KEY_ARROW_RIGHT);
    delay(100);
}

// Weak stubs for TinyUSB device class callbacks not used by HID keyboard.
// The precompiled libarduino_tinyusb.a references these but the Arduino
// core only provides implementations in source files that the linker
// doesn't pull in when only HID is used.
extern "C" {
__attribute__((weak)) void tud_dfu_runtime_reboot_to_dfu_cb(void) {}
__attribute__((weak)) uint32_t tud_dfu_get_timeout_cb(uint8_t /*alt*/, uint8_t /*state*/) { return 0; }
__attribute__((weak)) void tud_dfu_download_cb(uint8_t /*alt*/, uint16_t /*block*/, const uint8_t* /*data*/, uint16_t /*len*/) {}
__attribute__((weak)) void tud_dfu_manifest_cb(uint8_t /*alt*/) {}
__attribute__((weak)) bool tud_network_recv_cb(const uint8_t* /*src*/, uint16_t /*size*/) { return false; }
__attribute__((weak)) bool tud_msc_test_unit_ready_cb(uint8_t /*lun*/) { return false; }
__attribute__((weak)) void tud_msc_capacity_cb(uint8_t /*lun*/, uint32_t* /*block_count*/, uint16_t* /*block_size*/) {}
__attribute__((weak)) int32_t tud_msc_read10_cb(uint8_t /*lun*/, uint32_t /*lba*/, uint32_t /*offset*/, void* /*buf*/, uint32_t /*bufsize*/) { return -1; }
__attribute__((weak)) int32_t tud_msc_write10_cb(uint8_t /*lun*/, uint32_t /*lba*/, uint32_t /*offset*/, uint8_t* /*buf*/, uint32_t /*bufsize*/) { return -1; }
__attribute__((weak)) int32_t tud_msc_scsi_cb(uint8_t /*lun*/, const uint8_t /*scsi_cmd*/[16], void* /*buf*/, uint16_t /*bufsize*/) { return -1; }
}