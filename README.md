# WPSD Live Caller Display for ESP32

#  READ CHANGELOG FOR V2 IMPROVEMENTS AND CHANGES 

A wireless compact, real-time transmission dashboard for amateur radio operators running WPSD (WPSD Hotspot / MMDVMHost). 
This project polls your local WPSD-based hotspot server, parses active live caller metadata, and renders a clean visual display complete with user info, transmission stats, and country flag rendering.

Designed primarily for 2.8" ESP32 SPI display boards like the **ESP32-2432S028R** (popularly known as the *Cheap Yellow Display* or *CYD*).

<img width="2016" height="1512" alt="IMG_1041" src="https://github.com/user-attachments/assets/158b5fc0-4a0c-43d8-9c7f-f196ff1647db" />
<img width="2016" height="1512" alt="IMG_1040" src="https://github.com/user-attachments/assets/c81b974c-5138-4eca-9e23-6d44444c7f74" />

---

## Features

* **Real-Time WPSD Monitoring:** Periodically polls the WPSD `live_caller_backend.php` endpoint for active transmissions.
* **Country Flag Rendering:** Downloads and decodes PNG country flag icons on the fly using `PNGdec`.
* **RadioID.net Integration:** Automatically falls back to querying `radioid.net` via HTTPS to retrieve missing DMR/Radio IDs if WPSD doesn't provide them.
* **Smart UI Updates:** Only re-draws the display when transmission data or call duration changes to avoid screen flicker.
* **Custom Typography:** Utilizes smooth `TFT_eSPI` FreeFonts for high readability.

---

## Hardware Requirements

**ESP32-2432S028R** (Cheap Yellow Display / CYD). _TESTED BOARD_

---

## Configuration & Setup

1. **Install Arduino IDE:**

  Instructions available via the [Official Arduino Docs](https://docs.arduino.cc/software/ide-v2/tutorials/getting-started/ide-v2-downloading-and-installing/).

2. **Install the necessary Boards and Libraries via the managers present within the IDE:**

  This project relies on standard ESP32 core libraries along with three key external graphics libraries:

  - [Board Manager](https://docs.arduino.cc/software/ide-v2/tutorials/ide-v2-board-manager/):

  | Board | Author | Purpose | Source |
  | :--- | :--- | :--- | :--- |
  | esp32 | Espressif Systems | ESP32 Board Modules | [GitHub](https://github.com/espressif/arduino-esp32) |

  - [Library Manager](https://docs.arduino.cc/software/ide-v2/tutorials/ide-v2-installing-a-library/):
   
  | Library | Author | Purpose | Source |
  | :--- | :--- | :--- | :--- |
  | **`TFT_eSPI`** | Bodmer | Fast SPI display driver & font rendering | [GitHub](https://github.com/Bodmer/TFT_eSPI) / Library Manager |
  | **`PNGdec`** | Larry Bank | Embedded PNG image decoder | [GitHub](https://github.com/bitbank2/PNGdec) / Library Manager |
  | **`XPT2046_Touchscreen`** | Paul Stoffregen | XPT2046 resistive touchscreen controllers | [GitHub](https://github.com/PaulStoffregen/XPT2046_Touchscreen) / Library Manager |
  | **`WiFi`** | Built-in | ESP32 Wi-Fi network connection | ESP32 Arduino Core |
  | **`HTTPClient`** | Built-in | Local HTTP polling (WPSD) | ESP32 Arduino Core |
  | **`WiFiClientSecure`** | Built-in | TLS/HTTPS requests (RadioID API) | ESP32 Arduino Core |

3. **Configure `TFT_eSPI` User Setup:**

  Ensure your `User_Setup.h` or `User_Setup_Select.h` in the `TFT_eSPI` library folder is configured for your specific display hardware. For the **ESP32-2432S028R (CYD)**, use the standard ILI9341/ST7789 pinout settings:
  * `TFT_MOSI = 13`
  * `TFT_SCLK = 14`
  * `TFT_CS = 15`
  * `TFT_DC = 2`
  * `TFT_RST = 12`
  * `TFT_BL = 21`

4. **Update Wi-Fi & WPSD Network Details and Board Details:**

  Open the `.ino` sketch and update the following configuration lines:
  ```cpp
  #define SSID     "YOUR_WIFI_SSID"
  #define PASSWORD "YOUR_WIFI_PASSWORD"
  // Replace 192.168.1.50 with your WPSD hotspot IP address
  #define HOST_URL   "[http://123.123.123.123](http://123.123.123.123)"
  #define SERVER_URL "[http://122.123.123.123/mmdvmhost/live_caller_backend.php](http://123.123.123.123/mmdvmhost/live_caller_backend.php)"
  // Adapt to your screen size
  #define SCREEN_W 320 // Screen Width
  #define SCREEN_H 320 // Screen Height
  ```

5. **Verify, Upload, Debug:**

  Once the board is plugged into the USB, the Arduino IDE should recognize the default TTY of the board.
  Depending on the nature of your ESP32 chip, you might have to select the specific board from the ESP32 dropdown menu in the board selection.
  Generic ESP32 Dev Modules are available for development and testing purposes.

