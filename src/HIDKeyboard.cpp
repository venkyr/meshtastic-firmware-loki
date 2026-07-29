#include "HIDKeyboard.h"
#include "macros/macro_psh.h"
#include "macros/macro_psh_ele.h"
#include "macros/macro_lokimon.h"
#include "mesh/generated/meshtastic/mesh.pb.h"
#include "mesh/MeshTypes.h"
#include "Router.h"
#include "MeshService.h"
#ifdef ARCH_ESP32
#include "tusb.h"
#endif

// Global USB HID objects
USBHIDKeyboard Keyboard;
USBHIDVendor   VendorHID;

HIDKeyboardModule *hidKeyboardModule;

struct Macro {
    const char *name;
    const char *definition;
};

static const Macro macros[] = {
    {"PSH",     MACRO_PSH},
    {"PSH-ELE", MACRO_PSH_ELE},
};

struct Payload {
    const char *name;
    const char *content;
};

static const Payload payloads[] = {
    {"LOKIMON", PAYLOAD_LOKIMON},
};

HIDKeyboardModule::HIDKeyboardModule() : MeshModule("HIDKeyboard")
{
    LOG_DEBUG("HID Keyboard: Initializing HID at boot time");

    Keyboard.begin();
    VendorHID.setRxBufferSize(4096);
    VendorHID.begin();
    USB.begin();

    delay(1000);

    LOG_DEBUG("HID Keyboard: USB HID Keyboard + LokiBridge Ready!");
    LOG_DEBUG("HID Keyboard: Soldered USB: HID Keyboard + Vendor HID (GPIO 19/20)");
    LOG_DEBUG("HID Keyboard: UART Serial: Communication port");
    LOG_DEBUG("HID Keyboard: Send LoRa messages to type on HID keyboard");

    hidInitialized = true;
    hidReady = true;

    LOG_DEBUG("HID Keyboard: HID ready for keystrokes at boot");
}

bool HIDKeyboardModule::wantPacket(const meshtastic_MeshPacket *p)
{
    return p->decoded.portnum == meshtastic_PortNum_TEXT_MESSAGE_APP;
}

bool HIDKeyboardModule::init()
{
    LOG_DEBUG("HID Keyboard: init() called - HID already initialized at boot");
    return true;
}

bool HIDKeyboardModule::isReady()
{
    return hidInitialized && hidReady;
}

bool HIDKeyboardModule::isHostConnected()
{
#ifdef ARCH_ESP32
    return tud_mounted();
#else
    return isReady();
#endif
}

// ---------------------------------------------------------------------------
// LoRa message handling
// ---------------------------------------------------------------------------
ProcessMessage HIDKeyboardModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    LOG_DEBUG("HID Keyboard: Message received");

    if (!isReady()) {
        LOG_DEBUG("HID Keyboard: Not ready, skipping message");
        return ProcessMessage::CONTINUE;
    }

    if (mp.decoded.portnum != meshtastic_PortNum_TEXT_MESSAGE_APP) {
        LOG_DEBUG("HID Keyboard: Not a text message, skipping");
        return ProcessMessage::CONTINUE;
    }

    if (!isToUs(&mp)) {
        return ProcessMessage::CONTINUE;
    }

    String message = String((const char*)mp.decoded.payload.bytes, mp.decoded.payload.size);
    LOG_DEBUG("HID Keyboard: Received message: %s", message.c_str());

    String messageUpper = message;
    messageUpper.toUpperCase();
    messageUpper.trim();

    // --- Commands processed in both states ---

    // LBRESET — reset LokiBridge link and revert to LB_IDLE
    if (messageUpper == "LBRESET") {
        LOG_DEBUG("LokiBridge: LBRESET from 0x%x", mp.from);
        chunkLen = 0;
        chunkCount = 0;
        messageInProgress = false;
        awaitingResponse = false;
        shellActive = false;
        sendLokiBridgeReset();

        sendLoRaReply(mp, "LB IDLE");
        return ProcessMessage::CONTINUE;
    }

    // LBSTATUS — report LokiBridge status back over LoRa
    if (messageUpper == "LBSTATUS") {
        String status = getLokiBridgeStatus();
        sendLoRaReply(mp, status.c_str());
        return ProcessMessage::CONTINUE;
    }

    // --- State transitions ---

    // SHELL — enter LB_ACTIVE mode
    if (messageUpper == "SHELL") {
        if (!shellActive) {
            LOG_DEBUG("LokiBridge: SHELL from 0x%x, entering LB_ACTIVE", mp.from);
            shellActive = true;
        }
        sendLoRaReply(mp, "LB ACTIVE");
        return ProcessMessage::CONTINUE;
    }

    // QUIT — revert to LB_IDLE mode
    if (messageUpper == "QUIT") {
        LOG_DEBUG("LokiBridge: QUIT from 0x%x, returning to LB_IDLE", mp.from);
        shellActive = false;
        awaitingResponse = false;
        sendLoRaReply(mp, "LB IDLE");
        return ProcessMessage::CONTINUE;
    }

    // --- LB_ACTIVE: forward everything to LokiBridge ---

    if (shellActive) {
        if (awaitingResponse) {
            LOG_DEBUG("LokiBridge: Command discarded, previous still pending");
            return ProcessMessage::CONTINUE;
        }
        LOG_DEBUG("LokiBridge: Forwarding to LokiMon: %s", message.c_str());
        lokiBridgeSender = mp.from;
        sendLokiBridgeCmd(message.c_str());
        awaitingResponse = true;
        return ProcessMessage::CONTINUE;
    }

    // --- LB_IDLE: normal command processing ---

    // PING — respond with PONG/NOPE
    if (messageUpper == "PING") {
        bool hostConnected = isReady() && isHostConnected();
        sendLoRaReply(mp, hostConnected ? "PONG" : "NOPE");
        return ProcessMessage::CONTINUE;
    }

    // RUNM <name> — execute a named macro
    if (messageUpper.startsWith("RUNM ")) {
        String macroName = messageUpper.substring(5);
        macroName.trim();
        for (const auto &macro : macros) {
            if (macroName == macro.name) {
                LOG_DEBUG("HID Keyboard: RUNM '%s' from 0x%x", macro.name, mp.from);
                executeLoKeyScript(String(macro.definition));
                return ProcessMessage::CONTINUE;
            }
        }
        LOG_DEBUG("HID Keyboard: Unknown macro '%s'", macroName.c_str());
        return ProcessMessage::CONTINUE;
    }

    // LOAD <name> — type a payload line by line into the current shell
    if (messageUpper.startsWith("LOAD ")) {
        String payloadName = messageUpper.substring(5);
        payloadName.trim();
        for (const auto &payload : payloads) {
            if (payloadName == payload.name) {
                LOG_DEBUG("HID Keyboard: LOAD '%s' from 0x%x", payload.name, mp.from);
                typePayload(payload.content);
                return ProcessMessage::CONTINUE;
            }
        }
        LOG_DEBUG("HID Keyboard: Unknown payload '%s'", payloadName.c_str());
        return ProcessMessage::CONTINUE;
    }

    // LoKey Script commands (STRING, GUI, CTRL, etc.)
    if (messageUpper.startsWith("STRING ") || messageUpper.startsWith("STRINGLN ") ||
        messageUpper.startsWith("ENTER") || messageUpper.startsWith("TAB") ||
        messageUpper.startsWith("BACKSPACE") || messageUpper.startsWith("SPACE") ||
        messageUpper.startsWith("LEFT") || messageUpper.startsWith("RIGHT") ||
        messageUpper.startsWith("GUI ") || messageUpper.startsWith("ALT ") ||
        messageUpper.startsWith("CTRL ") || messageUpper.startsWith("DELAY ")) {
        String originalMessage = String((const char*)mp.decoded.payload.bytes, mp.decoded.payload.size);
        executeLoKeyScript(originalMessage);
    } else {
        sendLoRaReply(mp, "PING,GUI,ALT,CTRL,CTRL SHIFT,STRING,STRINGLN,ENTER,TAB,BACKSPACE,SPACE,LEFT,RIGHT,DELAY,RUNM,LOAD,SHELL,QUIT,LBSTATUS,LBRESET");
    }

    return ProcessMessage::CONTINUE;
}

// ---------------------------------------------------------------------------
// LokiBridge: main loop polling
// ---------------------------------------------------------------------------
void HIDKeyboardModule::lokiBridgeLoop()
{
    if (!awaitingResponse)
        return;

    while (VendorHID.available() >= (int)LB_REPORT_SIZE) {
        uint8_t pollBuf[LB_REPORT_SIZE];
        size_t n = VendorHID.read(pollBuf, LB_REPORT_SIZE);
        if (n == LB_REPORT_SIZE) {
            processLokiBridgeReport(pollBuf);
        }
    }
}


// ---------------------------------------------------------------------------
// LokiBridge: send a command to LokiMon
// ---------------------------------------------------------------------------
void HIDKeyboardModule::sendLokiBridgeCmd(const char *cmdStr)
{
    uint8_t pkt[LB_REPORT_SIZE] = {};
    pkt[0] = LB_CTRL_SHORT;
    size_t len = strlen(cmdStr);
    if (len > LB_DATA_PER_REPORT) len = LB_DATA_PER_REPORT;
    memcpy(&pkt[1], cmdStr, len);
    VendorHID.write(pkt, LB_REPORT_SIZE);
    LOG_DEBUG("LokiBridge: Sent command to LokiMon (%u bytes)", (unsigned)len);
}

// ---------------------------------------------------------------------------
// LokiBridge: send a RESET to LokiMon
// ---------------------------------------------------------------------------
void HIDKeyboardModule::sendLokiBridgeReset()
{
    uint8_t pkt[LB_REPORT_SIZE] = {};
    pkt[0] = LB_CTRL_RESET;
    VendorHID.write(pkt, LB_REPORT_SIZE);
    LOG_DEBUG("LokiBridge: Sent RESET to LokiMon");
}

// ---------------------------------------------------------------------------
// LokiBridge: process one incoming 63-byte report from LokiMon
// ---------------------------------------------------------------------------
void HIDKeyboardModule::processLokiBridgeReport(const uint8_t *buf)
{
    uint8_t ctrl = buf[0];
    const uint8_t *data = &buf[1];

    switch (ctrl) {
        case LB_CTRL_SHORT: {
            size_t dataLen = LB_DATA_PER_REPORT;
            while (dataLen > 0 && data[dataLen - 1] == 0) dataLen--;

            if (dataLen > 0) {
                chunkCount = 1;
                memcpy(chunkBuf, data, dataLen);
                chunkBuf[dataLen] = '\0';
                LOG_DEBUG("LokiBridge: Short message received (%u bytes)", (unsigned)dataLen);
                sendLoRaResponse((const char *)chunkBuf);
            }
            awaitingResponse = false;
            messageInProgress = false;
            chunkLen = 0;
            break;
        }

        case LB_CTRL_START:
            chunkLen = 0;
            chunkCount = 0;
            messageInProgress = true;
            memcpy(chunkBuf + chunkLen, data, LB_DATA_PER_REPORT);
            chunkLen += LB_DATA_PER_REPORT;
            LOG_DEBUG("LokiBridge: Multi-chunk message started");
            break;

        case LB_CTRL_CONTINUE:
            if (chunkLen + LB_DATA_PER_REPORT <= LB_CHUNK_MAX) {
                memcpy(chunkBuf + chunkLen, data, LB_DATA_PER_REPORT);
                chunkLen += LB_DATA_PER_REPORT;
            }
            break;

        case LB_CTRL_CHUNK_BOUNDARY: {
            size_t room = LB_CHUNK_MAX - chunkLen;
            size_t copyLen = min(room, (size_t)LB_DATA_PER_REPORT);
            if (copyLen > 0) {
                memcpy(chunkBuf + chunkLen, data, copyLen);
                chunkLen += copyLen;
            }
            flushChunk(false);
            break;
        }

        case LB_CTRL_END: {
            size_t room = LB_CHUNK_MAX - chunkLen;
            size_t copyLen = min(room, (size_t)LB_DATA_PER_REPORT);
            if (copyLen > 0) {
                memcpy(chunkBuf + chunkLen, data, copyLen);
                chunkLen += copyLen;
            }
            flushChunk(true);
            break;
        }

        case LB_CTRL_RESET:
            LOG_DEBUG("LokiBridge: RESET received from LokiMon, returning to LB_IDLE");
            awaitingResponse = false;
            messageInProgress = false;
            shellActive = false;
            chunkLen = 0;
            chunkCount = 0;
            break;

        default:
            LOG_WARN("LokiBridge: Unknown control byte: 0x%02X", ctrl);
            break;
    }
}

// ---------------------------------------------------------------------------
// LokiBridge: flush reassembled chunk — send back over LoRa
// ---------------------------------------------------------------------------
void HIDKeyboardModule::flushChunk(bool isFinal)
{
    while (chunkLen > 0 && chunkBuf[chunkLen - 1] == 0) chunkLen--;

    if (chunkLen > 0) {
        chunkCount++;
        chunkBuf[chunkLen] = '\0';
        LOG_DEBUG("LokiBridge: Flushing chunk %d (%u bytes) to LoRa", chunkCount, (unsigned)chunkLen);
        sendLoRaResponse((const char *)chunkBuf);
    }

    chunkLen = 0;

    if (isFinal) {
        LOG_DEBUG("LokiBridge: Message complete (%d chunks)", chunkCount);
        awaitingResponse = false;
        messageInProgress = false;
        chunkCount = 0;
    }
}

// ---------------------------------------------------------------------------
// LokiBridge: send a text response back to the LoRa sender
// ---------------------------------------------------------------------------
void HIDKeyboardModule::sendLoRaResponse(const char *text)
{
    if (lokiBridgeSender == 0) {
        LOG_WARN("LokiBridge: No sender to respond to");
        return;
    }

    size_t len = strlen(text);
    if (len > sizeof(((meshtastic_MeshPacket *)0)->decoded.payload.bytes)) {
        LOG_WARN("LokiBridge: Response too large for single LoRa packet (%u bytes), truncating", (unsigned)len);
        len = sizeof(((meshtastic_MeshPacket *)0)->decoded.payload.bytes);
    }

    LOG_DEBUG("LokiBridge: Sending response over LoRa to 0x%x (%u bytes)", lokiBridgeSender, (unsigned)len);

    meshtastic_MeshPacket *p = router->allocForSending();
    p->to = lokiBridgeSender;
    p->priority = meshtastic_MeshPacket_Priority_RELIABLE;
    p->want_ack = false;
    p->decoded.want_response = false;
    p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    memcpy(p->decoded.payload.bytes, text, len);
    p->decoded.payload.size = len;
    service->sendToMesh(p);

    LOG_DEBUG("LokiBridge: Response sent over LoRa");
}

// ---------------------------------------------------------------------------
// LokiBridge: send a reply to the sender of a LoRa packet
// ---------------------------------------------------------------------------
void HIDKeyboardModule::sendLoRaReply(const meshtastic_MeshPacket &mp, const char *text)
{
    meshtastic_MeshPacket *p = router->allocForSending();
    p->to = getFrom(&mp);
    p->priority = meshtastic_MeshPacket_Priority_RELIABLE;
    p->want_ack = false;
    p->decoded.want_response = false;
    p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    size_t len = strlen(text);
    memcpy(p->decoded.payload.bytes, text, len);
    p->decoded.payload.size = len;
    service->sendToMesh(p);
}

// ---------------------------------------------------------------------------
// LokiBridge: status string
// ---------------------------------------------------------------------------
String HIDKeyboardModule::getLokiBridgeStatus()
{
    String state = shellActive ? "LB_ACTIVE" : "LB_IDLE";
    if (awaitingResponse) {
        state += messageInProgress ? " AWAIT IN_PROG" : " AWAIT";
    }
    return state;
}

// ---------------------------------------------------------------------------
// HID Keyboard: payload typing (LOAD command)
// ---------------------------------------------------------------------------
void HIDKeyboardModule::typePayload(const char *content)
{
    const char *p = content;
    int lineNum = 0;

    while (*p) {
        const char *lineStart = p;
        while (*p && *p != '\n') p++;

        size_t lineLen = p - lineStart;
        if (*p == '\n') p++;

        if (lineLen == 0) {
            Keyboard.println();
        } else {
            String line(lineStart, lineLen);
            Keyboard.println(line);
        }
        lineNum++;
        delay(50);
    }

    LOG_DEBUG("HID Keyboard: LOAD complete, typed %d lines", lineNum);
}

// ---------------------------------------------------------------------------
// HID Keyboard: text and LoKey script execution
// ---------------------------------------------------------------------------
void HIDKeyboardModule::convertTextToKeystrokes(const String &text)
{
    LOG_DEBUG("HID Keyboard: Converting text: %s", text.c_str());
    Keyboard.print(text);
    delay(300);
    LOG_DEBUG("HID Keyboard: Text sent as keystrokes");
}

void HIDKeyboardModule::executeLoKeyScript(const String &script)
{
    LOG_DEBUG("HID Keyboard: Executing LoKey Script");

    int startPos = 0;
    while (startPos < (int)script.length()) {
        int endPos = script.indexOf('\n', startPos);
        if (endPos == -1) {
            endPos = script.length();
        }

        String line = script.substring(startPos, endPos);
        line.trim();

        if (line.length() > 0) {
            LOG_DEBUG("HID Keyboard: Executing command: %s", line.c_str());
            executeLoKeyCommand(line);
            delay(100);
        }

        startPos = endPos + 1;
    }

    LOG_DEBUG("HID Keyboard: LoKey Script execution complete");
}

void HIDKeyboardModule::executeLoKeyCommand(const String &command)
{
    String commandUpper = command;
    commandUpper.toUpperCase();

    if (commandUpper.startsWith("STRING ")) {
        String text = command.substring(7);
        LOG_DEBUG("HID Keyboard: STRING command: %s", text.c_str());
        Keyboard.print(text);
        delay(100);
    }
    else if (commandUpper.startsWith("STRINGLN ")) {
        String text = command.substring(9);
        LOG_DEBUG("HID Keyboard: STRINGLN command: %s", text.c_str());
        Keyboard.print(text);
        delay(100);
        pressEnter();
    }
    else if (commandUpper == "ENTER") {
        LOG_DEBUG("HID Keyboard: ENTER command");
        pressEnter();
    }
    else if (commandUpper == "TAB") {
        LOG_DEBUG("HID Keyboard: TAB command");
        pressTab();
    }
    else if (commandUpper == "BACKSPACE") {
        LOG_DEBUG("HID Keyboard: BACKSPACE command");
        pressBackspace();
    }
    else if (commandUpper == "SPACE") {
        LOG_DEBUG("HID Keyboard: SPACE command");
        pressSpace();
    }
    else if (commandUpper == "LEFT") {
        LOG_DEBUG("HID Keyboard: LEFT command");
        pressLeft();
    }
    else if (commandUpper == "RIGHT") {
        LOG_DEBUG("HID Keyboard: RIGHT command");
        pressRight();
    }
    else if (commandUpper.startsWith("GUI ")) {
        String key = command.substring(4);
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
        String key = command.substring(4);
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
        String key = command.substring(11);
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
        String key = command.substring(5);
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
        String delayStr = command.substring(6);
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

    if (keyLower.startsWith("f")) {
        int fNum = keyLower.substring(1).toInt();
        if (fNum >= 1 && fNum <= 12) {
            return (HID_KEY_F1 + fNum - 1);
        }
    }

    if (keyLower.length() == 1) {
        char c = keyLower.charAt(0);
        if (c >= 'a' && c <= 'z') {
            return (HID_KEY_A + (c - 'a'));
        }
        else if (c >= '0' && c <= '9') {
            if (c == '0') return HID_KEY_0;
            return (HID_KEY_1 + (c - '1'));
        }
    }

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
    return 0;
}

void HIDKeyboardModule::pressKeyCombo(uint8_t modifier, uint8_t key)
{
    LOG_DEBUG("HID Keyboard: Pressing key combo: modifier=0x%02X, key=0x%02X", modifier, key);
    Keyboard.pressRaw(modifier);
    delay(20);
    Keyboard.pressRaw(key);
    delay(150);
    Keyboard.releaseRaw(key);
    delay(20);
    Keyboard.releaseRaw(modifier);
    Keyboard.releaseAll();
    delay(100);
}

void HIDKeyboardModule::pressKeyComboMulti(uint8_t modifier1, uint8_t modifier2, uint8_t key)
{
    LOG_DEBUG("HID Keyboard: Pressing multi-key combo: modifier1=0x%02X, modifier2=0x%02X, key=0x%02X",
              modifier1, modifier2, key);
    Keyboard.pressRaw(modifier1);
    delay(20);
    Keyboard.pressRaw(modifier2);
    delay(20);
    Keyboard.pressRaw(key);
    delay(150);
    Keyboard.releaseRaw(key);
    delay(20);
    Keyboard.releaseRaw(modifier2);
    delay(20);
    Keyboard.releaseRaw(modifier1);
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
