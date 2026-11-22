// Flipper Zero Wi-Fi Devboard: carries the snake pad's presses from the Flipper's UART to the
// server's /ws socket, and the server's board frames back. The protocol on both sides is
// described in flipper/wifi_link.c and server/src/pad.rs.

#include <WiFi.h>
#include <WebSocketsClient.h>

// What the board tells the Flipper, one byte each.
const uint8_t BOARD_WAITING = 0xA0;
const uint8_t BOARD_JOINING = 0xA1;
const uint8_t BOARD_HOTSPOT_NOT_FOUND = 0xA2;
const uint8_t BOARD_HOTSPOT_REFUSED = 0xA3;
const uint8_t BOARD_ON_WIFI = 0xA4;
const uint8_t BOARD_SERVER_REFUSED = 0xA5;
const uint8_t BOARD_CONNECTED = 0xA6;
const uint8_t BOARD_ECHOED = 0xA7;

// CloudFront serves *.cloudfront.net under this root. SHA-256
// 8E:CD:E6:88:4F:3D:87:B1:12:5B:A3:1A:C3:FC:B1:3D:70:16:DE:7F:57:CC:90:4F:E1:CB:97:C6:AE:98:19:6E
const char AMAZON_ROOT_CA_1[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDQTCCAimgAwIBAgITBmyfz5m/jAo54vB4ikPmljZbyjANBgkqhkiG9w0BAQsF\n"
    "ADA5MQswCQYDVQQGEwJVUzEPMA0GA1UEChMGQW1hem9uMRkwFwYDVQQDExBBbWF6\n"
    "b24gUm9vdCBDQSAxMB4XDTE1MDUyNjAwMDAwMFoXDTM4MDExNzAwMDAwMFowOTEL\n"
    "MAkGA1UEBhMCVVMxDzANBgNVBAoTBkFtYXpvbjEZMBcGA1UEAxMQQW1hem9uIFJv\n"
    "b3QgQ0EgMTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBALJ4gHHKeNXj\n"
    "ca9HgFB0fW7Y14h29Jlo91ghYPl0hAEvrAIthtOgQ3pOsqTQNroBvo3bSMgHFzZM\n"
    "9O6II8c+6zf1tRn4SWiw3te5djgdYZ6k/oI2peVKVuRF4fn9tBb6dNqcmzU5L/qw\n"
    "IFAGbHrQgLKm+a/sRxmPUDgH3KKHOVj4utWp+UhnMJbulHheb4mjUcAwhmahRWa6\n"
    "VOujw5H5SNz/0egwLX0tdHA114gk957EWW67c4cX8jJGKLhD+rcdqsq08p8kDi1L\n"
    "93FcXmn/6pUCyziKrlA4b9v7LWIbxcceVOF34GfID5yHI9Y/QCB/IIDEgEw+OyQm\n"
    "jgSubJrIqg0CAwEAAaNCMEAwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMC\n"
    "AYYwHQYDVR0OBBYEFIQYzIU07LwMlJQuCFmcx7IQTgoIMA0GCSqGSIb3DQEBCwUA\n"
    "A4IBAQCY8jdaQZChGsV2USggNiMOruYou6r4lK5IpDB/G/wkjUu0yKGX9rbxenDI\n"
    "U5PMCCjjmCXPI6T53iHTfIUJrU6adTrCC2qJeHZERxhlbI1Bjjt/msv0tadQ1wUs\n"
    "N+gDS63pYaACbvXy8MWy7Vu33PqUXHeeE6V/Uq2V8viTO96LXFvKWlJbYK8U90vv\n"
    "o/ufQJVtMVT8QtPHRh8jrdkPSHCa2XV4cdFyQzR1bldZwgJcJmApzyMZFo6IQ6XU\n"
    "5MsI+yMRQ+hDKXJioaldXgjUkK642M4UwtBV8ob2xJNDd2ZhwLnoQdeXeGADbkpy\n"
    "rqXRfboQnoZsG4q5WTP468SQvvG5\n"
    "-----END CERTIFICATE-----\n";

WebSocketsClient webSocket;
String token;
bool settingsReceived = false;
uint32_t lastHeartbeatAt = 0;
uint32_t lastServerFrameAt = 0;
uint8_t stateSent = BOARD_WAITING;
uint8_t heartbeatCount = 0;

// Socket progress, touched only from loop() and the socket callback it runs.
bool tokenSent = false;
bool accepted = false;
bool refused = false;

// Written by WiFi events, which run on another task.
volatile uint8_t wifiState = BOARD_JOINING;
// When loop() next tries to join, if the board is still off WiFi by then.
volatile uint32_t wifiRetryAt = 0;
// The core retries the first failed join of each boot by itself, whatever the reason.
bool coreRetryUsed = false;

void setup() {
  // Serial is UART0 on GPIO44 (RX) and GPIO43 (TX) while USB CDC on boot is off, which are the
  // pins wired to the Flipper. Without a TX buffer a board frame larger than the 128-byte FIFO
  // would hold up loop(), and with it the Flipper's presses, until it was written.
  Serial.setTxBufferSize(1024);
  Serial.begin(115200);
}

void loop() {
  // UART first, so a socket call that blocked for a while cannot make the Flipper look gone.
  bool stopping = false;
  while (Serial.available() > 0) {
    uint8_t command = Serial.read();
    if (command <= 4) {
      if (accepted) webSocket.sendBIN(&command, 1);
    } else if (command == 5) {
      // Only a heartbeat shows the app is there; any other byte could be noise on the header.
      lastHeartbeatAt = millis();
      stateSent = currentState();
      Serial.write(stateSent);
      if (accepted) {
        uint8_t echo[] = {5, heartbeatCount++};
        webSocket.sendBIN(echo, 2);
      }
    } else if (command == 6) {
      stopping = true;
    } else if (command == 0x10) {
      receiveSettings();
    }
  }

  // The app left or died, and a board that stayed connected would keep the server waiting for a
  // player. Closing the socket tells the server at once; the restart drops WiFi and returns the
  // board to waiting.
  if (settingsReceived && (stopping || millis() - lastHeartbeatAt > 6000)) {
    webSocket.disconnect();
    delay(100);
    ESP.restart();
  }

  // The server echoes every heartbeat, so 6 s without a frame means the connection is dead.
  // TCP can take minutes to notice that when mobile data drops.
  if (accepted && millis() - lastServerFrameAt > 6000) webSocket.disconnect();

  // The core gives up after some failures, so joins are retried here: a second after a drop,
  // and every 15 s for a join that ended without saying so.
  if (settingsReceived && wifiState != BOARD_ON_WIFI && (int32_t)(millis() - wifiRetryAt) >= 0) {
    wifiRetryAt = millis() + 15000;
    WiFi.reconnect();
  }

  // Off WiFi the socket would only look up the server's name in vain, so it waits.
  if (wifiState == BOARD_ON_WIFI) {
    webSocket.loop();
  } else if (webSocket.isConnected()) {
    webSocket.disconnect();
  }

  uint8_t state = currentState();
  if (state != stateSent) {
    stateSent = state;
    Serial.write(state);
  }
}

uint8_t currentState() {
  if (!settingsReceived) return BOARD_WAITING;
  if (wifiState != BOARD_ON_WIFI) return wifiState;
  if (accepted) return BOARD_CONNECTED;
  return refused ? BOARD_SERVER_REFUSED : BOARD_ON_WIFI;
}

// Reads the hotspot name, password, server address and pad token, one line each. A block that
// does not finish within 300 ms is thrown away. Once settings are in, a later block is read and
// dropped too, so its characters are not taken for commands.
void receiveSettings() {
  String lines[4];
  uint8_t count = 0;
  uint32_t startedAt = millis();
  while (count < 4 && millis() - startedAt < 300) {
    int c = Serial.read();
    if (c < 0) continue;
    if (c == '\n') {
      count++;
    } else {
      lines[count] += (char)c;
    }
  }
  if (count < 4 || settingsReceived) return;

  settingsReceived = true;
  lastHeartbeatAt = millis();
  token = lines[3];

  // The settings live only until the next restart.
  WiFi.persistent(false);
  // Power save would hold presses back until the next beacon.
  WiFi.setSleep(false);
  // The core gives up after a refused password; loop() retries every failure instead.
  WiFi.setAutoReconnect(false);
  WiFi.onEvent(onWifiEvent);
  wifiRetryAt = millis() + 15000;
  // A password the radio cannot use at all, such as one under 8 characters, fails here and
  // raises no event.
  if (WiFi.begin(lines[0].c_str(), lines[1].c_str()) == WL_CONNECT_FAILED) {
    wifiState = BOARD_HOTSPOT_REFUSED;
  }

  bool tls = lines[2].startsWith("wss://");
  String hostAndPort = lines[2].substring(tls ? 6 : 5);
  // An address pasted with a trailing slash or path still works.
  int slash = hostAndPort.indexOf('/');
  if (slash >= 0) hostAndPort.remove(slash);
  int colon = hostAndPort.indexOf(':');
  String host = colon < 0 ? hostAndPort : hostAndPort.substring(0, colon);
  uint16_t port = colon < 0 ? (tls ? 443 : 80) : hostAndPort.substring(colon + 1).toInt();
  // No subprotocol: the server picks none.
  if (tls) {
    webSocket.beginSslWithCA(host.c_str(), port, "/ws", AMAZON_ROOT_CA_1, "");
  } else {
    webSocket.begin(host.c_str(), port, "/ws", "");
  }
  webSocket.onEvent(onSocketEvent);
  webSocket.setReconnectInterval(1000);
}

// Runs on the network event task: it only sets values that loop() reads.
void onWifiEvent(arduino_event_id_t event, arduino_event_info_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    wifiState = BOARD_ON_WIFI;
    return;
  }
  if (event != ARDUINO_EVENT_WIFI_STA_DISCONNECTED) return;

  switch (info.wifi_sta_disconnected.reason) {
    case WIFI_REASON_ASSOC_LEAVE:
      // The core leaves like this just before it joins again, which must not hide why the last
      // join failed. A hotspot being switched off can say the same, and only then was the board
      // on WiFi.
      if (wifiState == BOARD_ON_WIFI) {
        wifiState = BOARD_JOINING;
        wifiRetryAt = millis() + 1000;
      }
      return;
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
      wifiState = BOARD_HOTSPOT_NOT_FOUND;
      break;
    // A wrong WPA2 password ends in a handshake timeout; WPA3 reports an auth failure.
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
      wifiState = BOARD_HOTSPOT_REFUSED;
      break;
    default:
      wifiState = BOARD_JOINING;
      break;
  }

  // The core retries the first drop itself, and a second join now would clash with it.
  wifiRetryAt = millis() + (coreRetryUsed ? 1000 : 15000);
  coreRetryUsed = true;
}

// Runs inside webSocket.loop(), so it may write to the UART.
void onSocketEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED: {
      webSocket.sendTXT(token);
      tokenSent = true;
      // The Flipper draws the board from the server's compact frames, so the board asks for
      // those instead of the JSON ones. A server from before that request ignores it and sends
      // JSON, which is thrown away below.
      uint8_t board = 7;
      webSocket.sendBIN(&board, 1);
      break;
    }
    case WStype_TEXT:
      // The first text after the token is the hello. Any later text is a game frame from a server
      // that does not know the request for the board, and is not needed here.
      accepted = true;
      refused = false;
      lastServerFrameAt = millis();
      break;
    case WStype_BIN:
      lastServerFrameAt = millis();
      if (length > 0 && payload[0] == 5) Serial.write(BOARD_ECHOED);
      // A board frame or a mode byte: it starts at 0x80 or above and is for the Flipper as it is.
      if (length > 0 && payload[0] >= 0x80) Serial.write(payload, length);
      break;
    case WStype_DISCONNECTED:
      // The close code is not passed on, so a close between token and hello counts as refused.
      if (tokenSent && !accepted) refused = true;
      tokenSent = false;
      accepted = false;
      break;
    default:
      break;
  }
}
