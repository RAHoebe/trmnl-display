#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <PNGdec.h>
#include <Preferences.h>
#include <StreamString.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <esp_system.h>
#include <memory>
#include <new>
#include <time.h>

#include "roots.h"

namespace {
constexpr char CurrentScreenUrl[] = "https://trmnl.com/api/display/current";
constexpr char ImagePath[] = "/mirror.tmp";
constexpr unsigned PollMs = 30000;
constexpr unsigned MinimumPollMs = 12000;
constexpr unsigned SmallBody = 4096;
constexpr unsigned MaxPngBytes = 512 * 1024;
constexpr int MaxPngDimension = 1200;
constexpr int PanelWidth = 320;
constexpr int PanelHeight = 240;
constexpr int BootPin = 0;

TFT_eSPI display;
WiFiManager wifi;
Preferences preferences;
String deviceKey;
String lastFilename;
bool storageReady;
bool hasFrame;
bool portalWasActive;
bool preferencesReady;
uint32_t nextPoll;
uint32_t lastPoll;
uint32_t bootPressedAt;
bool bootHandled;
bool attemptedPoll;
char usbLine[176];
size_t usbLength;
bool usbOverflow;
char apPassword[13];

void showStatus(const char *message, const char *detail = "") {
    display.fillScreen(TFT_BLACK);
    display.setTextDatum(MC_DATUM);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.drawString("TRMNL Mirror", PanelWidth / 2, 76, 4);
    display.drawString(message, PanelWidth / 2, 123, 2);
    display.drawString(detail, PanelWidth / 2, 149, 2);
}

void report(const char *message, const char *detail = "") {
    Serial.printf("TRMNL %s\n", message);
    if (!hasFrame) showStatus(message, detail);
}

// The HTTPClient stream API handles chunked responses. The bound applies even
// when the server omits the content length.
class LimitedSink final : public Stream {
public:
    LimitedSink(Print &target, size_t limit) : target(target), remaining(limit) {}

    size_t write(uint8_t byte) override { return write(&byte, 1); }
    size_t write(const uint8_t *data, size_t length) override {
        if (length > remaining) return 0;
        const size_t accepted = target.write(data, length);
        remaining -= accepted;
        return accepted;
    }

    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }

private:
    Print &target;
    size_t remaining;
};

void setHttpOptions(HTTPClient &http, WiFiClientSecure &client) {
    client.setCACert(TRMNL_ROOTS);
    client.setHandshakeTimeout(10);
    http.setUserAgent("TRMNL ESP32 Mirror/1.0");
    http.setConnectTimeout(6000);
    http.setTimeout(8000);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
}

bool fetchCurrent(String &url, String &filename, unsigned &retryMs) {
    WiFiClientSecure client;
    HTTPClient http;
    setHttpOptions(http, client);
    if (!http.begin(client, CurrentScreenUrl)) return false;
    const char *headers[] = {"Retry-After"};
    http.collectHeaders(headers, 1);
    http.addHeader("access-token", deviceKey);
    http.addHeader("Accept", "application/json");
    const int status = http.GET();
    if (status != HTTP_CODE_OK) {
        if (status == 429) {
            const long seconds = http.header("Retry-After").toInt();
            retryMs = seconds > 0 ? constrain(seconds, 30L, 3600L) * 1000UL : 60000UL;
        }
        if (status == 401 || status == 403) report("Apparaatsleutel geweigerd");
        Serial.printf("TRMNL screen HTTP %d\n", status);
        return false;
    }

    StreamString body;
    LimitedSink sink(body, SmallBody);
    if (http.getSize() > static_cast<int>(SmallBody) || http.writeToStream(&sink) <= 0) {
        return false;
    }
    StaticJsonDocument<96> filter;
    filter["image_url"] = true;
    filter["filename"] = true;
    filter["status"] = true;
    DynamicJsonDocument response(SmallBody);
    if (deserializeJson(response, body.c_str(), DeserializationOption::Filter(filter))) return false;

    const int apiStatus = response["status"] | 0;
    if (apiStatus != 200) {
        Serial.printf("TRMNL screen status %d\n", apiStatus);
        if (apiStatus == 404) report("Wacht op bronbeeld");
        else if (apiStatus == 500) report("Controleer de apparaatsleutel");
        return false;
    }

    url = response["image_url"] | "";
    filename = response["filename"] | "";
    return url.startsWith("https://") && url.length() <= 3072;
}

bool downloadImage(String url) {
    LittleFS.remove(ImagePath);
    for (unsigned hop = 0; hop <= 4 && url.startsWith("https://"); ++hop) {
        WiFiClientSecure client;
        HTTPClient http;
        setHttpOptions(http, client);
        const int pathStart = url.indexOf('/', 8);
        const String host = url.substring(8, pathStart < 0 ? url.length() : pathStart);
        if (!http.begin(client, url)) return false;
        const int status = http.GET();
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            String next = http.getLocation();
            if (next.startsWith("/") && !next.startsWith("//")) {
                next = (pathStart < 0 ? url : url.substring(0, pathStart)) + next;
            }
            url = next;
            continue;
        }
        if (status != HTTP_CODE_OK || http.getSize() > static_cast<int>(MaxPngBytes)) {
            char tlsError[120] = {};
            const int tlsStatus = status < 0 ? client.lastError(tlsError, sizeof(tlsError)) : 0;
            Serial.printf("TRMNL image host %s, HTTP %d, TLS %d %s, largest heap block %u\n",
                          host.c_str(), status, tlsStatus, tlsError, ESP.getMaxAllocHeap());
            return false;
        }
        File pngFile = LittleFS.open(ImagePath, FILE_WRITE);
        if (!pngFile) return false;
        LimitedSink sink(pngFile, MaxPngBytes);
        const int size = http.writeToStream(&sink);
        pngFile.flush();
        const size_t saved = pngFile.size();
        pngFile.close();
        const bool complete = size >= 33 && static_cast<size_t>(size) == saved;
        if (!complete) Serial.printf("TRMNL image body %d, saved %u bytes\n", status == HTTP_CODE_OK ? size : status, unsigned(saved));
        return complete;
    }
    return false;
}

File input;
void *openPng(const char *path, int32_t *size) {
    input = LittleFS.open(path);
    if (!input) return nullptr;
    *size = input.size();
    return &input;
}

void closePng(void *handle) {
    static_cast<File *>(handle)->close();
}

int32_t readPng(PNGFILE *file, uint8_t *data, int32_t length) {
    return static_cast<File *>(file->fHandle)->read(data, length);
}

int32_t seekPng(PNGFILE *file, int32_t position) {
    return static_cast<File *>(file->fHandle)->seek(position) ? position : 0;
}

// Four bits per panel pixel keep a complete candidate frame in RAM. The TFT is
// updated only after the PNG passes validation, so a bad image leaves it intact.
uint8_t shades[PanelWidth * PanelHeight / 2];

struct PanelScaler {
    PNG &png;
    int sourceWidth;
    int sourceHeight;
    int cropLeft;
    int cropWidth;
    uint16_t *rgb;
    int sourceRows = 0;
    int outputRows = 0;
    uint32_t sums[PanelWidth]{};

    PanelScaler(PNG &decoder, int width, int height, int left, int right, uint16_t *rowBuffer)
        : png(decoder), sourceWidth(width), sourceHeight(height), cropLeft(left),
          cropWidth(width - left - right), rgb(rowBuffer) {}
};

unsigned luma(uint16_t pixel) {
    const unsigned red = (pixel >> 11) * 255 / 31;
    const unsigned green = ((pixel >> 5) & 63) * 255 / 63;
    const unsigned blue = (pixel & 31) * 255 / 31;
    return (77 * red + 150 * green + 29 * blue + 128) >> 8;
}

unsigned graySample(const PNGDRAW &row, int column) {
    const unsigned bits = row.iBpp;
    const unsigned mask = (1U << bits) - 1;
    const unsigned bit = column * bits;
    return ((row.pPixels[bit / 8] >> (8 - bits - bit % 8)) & mask) * 255 / mask;
}

bool prepareRow(PNG &png, PNGDRAW *row, uint16_t *rgb) {
    if (row->iPixelType == PNG_PIXEL_GRAYSCALE) {
        return row->iBpp == 1 || row->iBpp == 2 || row->iBpp == 4 || row->iBpp == 8;
    }
    png.getLineAsRGB565(row, rgb, PNG_RGB565_LITTLE_ENDIAN, 0xFFFFFF);
    return true;
}

unsigned sampleLuma(const PNGDRAW &row, const uint16_t *rgb, int column) {
    return row.iPixelType == PNG_PIXEL_GRAYSCALE ? graySample(row, column) : luma(rgb[column]);
}

struct SideMargins {
    PNG &png;
    uint16_t *rgb;
    int width;
    int firstInk;
    int pastLastInk = 0;
    int rows = 0;

    SideMargins(PNG &decoder, uint16_t *rowBuffer, int imageWidth)
        : png(decoder), rgb(rowBuffer), width(imageWidth), firstInk(imageWidth) {}
};

int scanSideMargins(PNGDRAW *row) {
    auto &margins = *static_cast<SideMargins *>(row->pUser);
    if (row->iWidth != margins.width || row->y != margins.rows ||
        !prepareRow(margins.png, row, margins.rgb)) return 0;

    for (int x = 0; x < margins.firstInk; ++x) {
        if (sampleLuma(*row, margins.rgb, x) < 255) {
            margins.firstInk = x;
            break;
        }
    }
    for (int x = margins.width - 1; x >= margins.pastLastInk; --x) {
        if (sampleLuma(*row, margins.rgb, x) < 255) {
            margins.pastLastInk = x + 1;
            break;
        }
    }
    ++margins.rows;
    return 1;
}

int makeScaledRow(PNGDRAW *row) {
    auto &scaler = *static_cast<PanelScaler *>(row->pUser);
    if (row->iWidth != scaler.sourceWidth || row->y != scaler.sourceRows ||
        row->y >= scaler.sourceHeight || scaler.outputRows >= PanelHeight ||
        !prepareRow(scaler.png, row, scaler.rgb)) return 0;

    // Weight each source pixel by its overlap with the target pixel. This
    // keeps thin strokes even when a pixel spans a fractional number of samples.
    uint8_t scaledRow[PanelWidth];
    for (int x = 0; x < PanelWidth; ++x) {
        const int left = x * scaler.cropWidth;
        const int right = left + scaler.cropWidth;
        unsigned sum = 0;
        for (int sx = left / PanelWidth; sx <= (right - 1) / PanelWidth; ++sx) {
            const int pixelLeft = sx * PanelWidth;
            const int weight = min(right, pixelLeft + PanelWidth) - max(left, pixelLeft);
            const unsigned shade = sampleLuma(*row, scaler.rgb, scaler.cropLeft + sx);
            sum += shade * weight;
        }
        scaledRow[x] = (sum + scaler.cropWidth / 2) / scaler.cropWidth;
    }

    const int sourceTop = row->y * PanelHeight;
    const int sourceBottom = sourceTop + PanelHeight;
    while (scaler.outputRows * scaler.sourceHeight < sourceBottom) {
        const int top = scaler.outputRows * scaler.sourceHeight;
        const int bottom = top + scaler.sourceHeight;
        const int weight = min(bottom, sourceBottom) - max(top, sourceTop);
        for (int x = 0; x < PanelWidth; ++x) scaler.sums[x] += scaledRow[x] * weight;
        if (bottom > sourceBottom) break;

        uint8_t *output = shades + scaler.outputRows * (PanelWidth / 2);
        const unsigned step = 17 * scaler.sourceHeight;
        for (int x = 0; x < PanelWidth; x += 2) {
            const unsigned high = (scaler.sums[x] + step / 2) / step;
            const unsigned low = (scaler.sums[x + 1] + step / 2) / step;
            output[x / 2] = (high << 4) | low;
        }
        memset(scaler.sums, 0, sizeof(scaler.sums));
        ++scaler.outputRows;
    }
    ++scaler.sourceRows;
    return 1;
}

bool renderImage() {
    auto png = std::unique_ptr<PNG>(new (std::nothrow) PNG());
    if (!png) {
        Serial.printf("TRMNL PNG memory: %u bytes, largest %u\n",
                      ESP.getFreeHeap(), ESP.getMaxAllocHeap());
        return false;
    }

    const int opened = png->open(ImagePath, openPng, closePng, readPng, seekPng, scanSideMargins);
    if (opened != PNG_SUCCESS) {
        input.close();
        Serial.printf("TRMNL PNG open error %d\n", opened);
        return false;
    }
    const int width = png->getWidth();
    const int height = png->getHeight();
    if (width < 1 || width > MaxPngDimension || height < 1 || height > MaxPngDimension) {
        png->close();
        return false;
    }

    uint16_t rgb[MaxPngDimension];
    SideMargins margins(*png, rgb, width);
    const int scanned = png->decode(&margins, PNG_CHECK_CRC);
    png->close();
    if (scanned != PNG_SUCCESS || margins.rows != height) {
        Serial.printf("TRMNL PNG margin scan error %d (%d rows)\n", scanned, margins.rows);
        return false;
    }

    // Limit zoom on sparse screens. A completely white frame has no crop.
    const int maxTrim = width / 8;
    const bool hasInk = margins.firstInk < margins.pastLastInk;
    const int leftTrim = hasInk ? min(maxTrim, margins.firstInk) : 0;
    const int rightTrim = hasInk ? min(maxTrim, width - margins.pastLastInk) : 0;
    const int reopened = png->open(ImagePath, openPng, closePng, readPng, seekPng, makeScaledRow);
    if (reopened != PNG_SUCCESS || png->getWidth() != width || png->getHeight() != height) {
        input.close();
        Serial.printf("TRMNL PNG reopen error %d\n", reopened);
        return false;
    }
    PanelScaler scaler(*png, width, height, leftTrim, rightTrim, rgb);
    const int decoded = png->decode(&scaler, PNG_CHECK_CRC);
    png->close();
    if (decoded != PNG_SUCCESS || scaler.sourceRows != height || scaler.outputRows != PanelHeight) {
        Serial.printf("TRMNL PNG decode error %d (%d source rows, %d output rows)\n",
                      decoded, scaler.sourceRows, scaler.outputRows);
        return false;
    }
    png.reset();

    alignas(4) uint16_t row[PanelWidth];
    uint32_t fingerprint = 2166136261U;
    display.startWrite();
    display.setAddrWindow(0, 0, PanelWidth, PanelHeight);
    for (int y = 0; y < PanelHeight; ++y) {
        const uint8_t *packed = shades + y * (PanelWidth / 2);
        for (int x = 0; x < PanelWidth; x += 2) {
            const uint8_t pair = packed[x / 2];
            fingerprint = (fingerprint ^ pair) * 16777619U;
            const uint8_t light = (pair >> 4) * 17;
            const uint8_t dark = (pair & 15) * 17;
            row[x] = display.color565(light, light, light);
            row[x + 1] = display.color565(dark, dark, dark);
        }
        display.pushPixels(row, PanelWidth);
    }
    display.endWrite();
    hasFrame = true;
    Serial.printf("TRMNL FRAME %dx%d -> %dx%d, trim L%d R%d, pixels %08lx\n",
                  width, height, PanelWidth, PanelHeight, leftTrim, rightTrim,
                  static_cast<unsigned long>(fingerprint));
    return true;
}

unsigned updateMirror() {
    unsigned retryMs = PollMs;
    String url;
    String filename;
    if (!fetchCurrent(url, filename, retryMs)) {
        return retryMs;
    }
    if (hasFrame && !filename.isEmpty() && filename == lastFilename) {
        Serial.println("TRMNL unchanged");
        return PollMs;
    }

    const bool rendered = downloadImage(url) && renderImage();
    LittleFS.remove(ImagePath);
    if (rendered) lastFilename = filename;
    else report("Beeld niet geladen", "Nieuwe poging over 30 sec");
    return PollMs;
}

bool keyIsValid(const char *value) {
    const size_t length = strlen(value);
    if (length < 8 || length > 160) return false;
    for (const char *p = value; *p; ++p) {
        if (!isalnum(static_cast<unsigned char>(*p)) && !strchr("_.~+/=-", *p)) return false;
    }
    return true;
}

void scheduleRefresh() {
    const uint32_t now = millis();
    const uint32_t earliest = lastPoll + MinimumPollMs;
    nextPoll = attemptedPoll && static_cast<int32_t>(now - earliest) < 0 ? earliest : now;
}

void handleUsbCommand(const char *command) {
    if (!strcmp(command, "TRMNL PING")) {
        Serial.println("TRMNL READY");
    } else if (!strncmp(command, "TRMNL KEY ", 10)) {
        const char *value = command + 10;
        if (preferencesReady && keyIsValid(value) && preferences.putString("deviceKey", value)) {
            deviceKey = value;
            scheduleRefresh();
            if (!hasFrame && WiFi.isConnected()) showStatus("Wacht op TRMNL");
            Serial.println("TRMNL KEY SAVED");
        } else {
            Serial.println("TRMNL KEY REJECTED");
        }
    } else if (!strcmp(command, "TRMNL STATUS")) {
        Serial.printf("TRMNL STATUS wifi=%s key=%s frame=%s clock=%s storage=%s\n",
                      WiFi.isConnected() ? "yes" : "no", deviceKey.isEmpty() ? "no" : "yes",
                      hasFrame ? "yes" : "no", time(nullptr) > 1700000000 ? "yes" : "no",
                      storageReady ? "yes" : "no");
    } else if (!strcmp(command, "TRMNL REFRESH")) {
        scheduleRefresh();
    }
}

void pollUsb() {
    while (Serial.available()) {
        const int byte = Serial.read();
        if (byte == '\r') continue;
        if (byte == '\n') {
            usbLine[usbLength] = '\0';
            if (!usbOverflow) handleUsbCommand(usbLine);
            memset(usbLine, 0, sizeof(usbLine));
            usbLength = 0;
            usbOverflow = false;
        } else if (usbLength + 1 < sizeof(usbLine) && byte >= 32 && byte <= 126) {
            usbLine[usbLength++] = static_cast<char>(byte);
        } else {
            usbOverflow = true;
        }
    }
}

void pollBootButton() {
    if (digitalRead(BootPin) == LOW) {
        if (!bootPressedAt) bootPressedAt = millis();
        if (!bootHandled && millis() - bootPressedAt >= 2500) {
            bootHandled = true;
            if (!wifi.getConfigPortalActive()) wifi.startConfigPortal("TRMNL-Mirror", apPassword);
        }
    } else if (bootPressedAt) {
        if (!bootHandled && millis() - bootPressedAt >= 50) scheduleRefresh();
        bootPressedAt = 0;
        bootHandled = false;
    }
}
}

void setup() {
    Serial.begin(115200);
    pinMode(BootPin, INPUT_PULLUP);
    for (int pin : {5, 33, 15, 4, 16, 17, TFT_BL}) {
        pinMode(pin, OUTPUT);
        digitalWrite(pin, HIGH);
    }

    display.init();
    display.setRotation(1);
    display.setSwapBytes(true);
    showStatus("Wifi verbinden");

    preferencesReady = preferences.begin("trmnl-mirror", false);
    if (preferencesReady) deviceKey = preferences.getString("deviceKey");
    storageReady = LittleFS.begin(true);
    if (storageReady) LittleFS.remove(ImagePath);

    snprintf(apPassword, sizeof(apPassword), "%08lX%04X",
             static_cast<unsigned long>(esp_random()), unsigned(esp_random() & 0xffff));
    WiFi.mode(WIFI_STA);
    WiFi.setHostname("trmnl-mirror");
    WiFi.setAutoReconnect(true);
    wifi.setDebugOutput(false);
    wifi.setConfigPortalBlocking(false);
    wifi.setConnectTimeout(15);
    wifi.setConfigPortalTimeout(180);
    wifi.setAPCallback([](WiFiManager *) {
        showStatus("Wifi: TRMNL-Mirror", apPassword);
    });
    wifi.autoConnect("TRMNL-Mirror", apPassword);
    configTime(0, 0, "time.cloudflare.com", "pool.ntp.org");

    Serial.println("TRMNL READY");
    if (!storageReady) report("Beeldopslag niet beschikbaar");
    else if (WiFi.isConnected()) {
        if (deviceKey.isEmpty()) report("Apparaatsleutel nodig", "Instellen via USB");
        else showStatus("Wacht op TRMNL");
    }
}

void loop() {
    pollUsb();
    pollBootButton();
    wifi.process();
    const bool portalActive = wifi.getConfigPortalActive();
    if (portalWasActive && !portalActive) {
        if (hasFrame) {
            hasFrame = false;
            lastFilename.clear();
        }
        if (deviceKey.isEmpty()) showStatus("Apparaatsleutel nodig", "Instellen via USB");
        else showStatus("Wacht op TRMNL");
        scheduleRefresh();
    }
    portalWasActive = portalActive;

    if (!portalActive && storageReady && WiFi.isConnected() && !deviceKey.isEmpty() &&
        static_cast<int32_t>(millis() - nextPoll) >= 0) {
        if (time(nullptr) > 1700000000) {
            lastPoll = millis();
            attemptedPoll = true;
            nextPoll = millis() + updateMirror();
        } else {
            report("Wacht op tijdsynchronisatie");
            nextPoll = millis() + 5000;
        }
    }
    delay(10);
}
