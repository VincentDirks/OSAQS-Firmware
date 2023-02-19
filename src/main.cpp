/*
	Licence under the HIPPOCRATIC LICENSE Version 3.0, October 2021

	Open Source Air Quality (CO2) Sensor Firmware

	ESP32 Wroom Module as MCU
	Lights up a strip of WS2812B Addressable RGB LEDs to display a scale of the ambient CO2 level
	CO2 data is from a Sensirion SCD40
	The LEDs are adjusted depending on the ambient Light data from a VEML7700
	There is also a webserver which displays graphs of CO2, Humidity and Temperature while also providing a csv download for that data

	The circuit:
	* i2c (IO21 -> SDA, IO22 ->SCL) -> SCD40 & VEML7700 (3.3V Power and Data)
	* IO2 (3.3V) -> SN74LVC2T45 Level Shifter (5v) -> WS2812B (5v Power and Data)
	* USB C power (5v Rail) -> XC6220B331MR-G -> 3.3V Rail


	Created 14.02.2023

	By @CD_FER (Chris Dirks)
*/

#include <Arduino.h>

// Wifi, Webserver and DNS
#include <DNSServer.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include "AsyncTCP.h"
#include "ESPAsyncWebServer.h"

// Onboard Flash Storage
#include <SPIFFS.h>

// Addressable LEDs
#include <NeoPixelBusLg.h>	// instead of NeoPixelBus.h (has Luminace and Gamma as default)

//I2C (CO2 & LUX)
#include <Wire.h>

//VEML7700 (LUX)
#include <DFRobot_VEML7700.h>

//SCD40/41 (CO2)
#include <SensirionI2CScd4x.h>

const char *ssid = "Kea-CO2";	// Name of the Wifi Access Point, FYI The SSID can't have a space in it.
const char *password = "";		// Password of the Wifi Access Point, leave blank for no Password
char LogFilename[] = "/Kea-CO2-Data.csv";	//name of the file which has the data in it.

const IPAddress localIP(4, 3, 2, 1);					// the IP address the webserver, Samsung requires the IP to be in public space
const IPAddress gatewayIP(4, 3, 2, 1);					// IP address of the network (should be the same as the local IP in most cases)
const String localIPURL = "http://4.3.2.1/index.html";	// URL to the webserver (Same as the local IP but as a string with http and the landing page subdomain)


// -----------------------------------------
//
//    Global Variables
//
// -----------------------------------------
uint16_t lux = 0;
uint16_t co2 = 450;

/**
 * This sets the scale of the co2 level with all leds off being CO2 MIN and 
*/
#define CO2_MAX 2000 	//top of the CO2 Scale (also when it transitions to warning Flash)
#define CO2_MAX_HUE 0.0	//top of the Scale (Red Hue) 
#define CO2_MIN 450		//bottom of the Scale
#define CO2_MIN_HUE 0.3	//bottom of the Scale (Green Hue)
#define CO2_SMOOTHING_FACTOR 100 //helps to make it look like we have continues data not a update every 5s

#define PIXEL_COUNT 9
#define PIXEL_DATA_PIN 2
#define FRAME_TIME 30  // Milliseconds between frames 30ms = ~33.3fps max

#define OFF RgbColor(0)
#define WARNING_COLOR RgbColor(HsbColor(CO2_MAX_HUE, 1.0f, 1.0f))

#define PPM_PER_PIXEL ((CO2_MAX - CO2_MIN) / PIXEL_COUNT)

float mapCO2ToHue(uint16_t ledCO2){
	return (float)(ledCO2 - (uint16_t)CO2_MIN) * ((float)CO2_MAX_HUE - (float)CO2_MIN_HUE) / (float)((uint16_t)CO2_MAX - (uint16_t)CO2_MIN) + (float) CO2_MIN_HUE;
}

void AddressableRGBLeds(void *parameter) {

	NeoPixelBusLg<NeoGrbFeature, NeoWs2812xMethod> strip(PIXEL_COUNT, PIXEL_DATA_PIN); //uses i2s silicon remapped to any pin to drive led data

	uint16_t ppmDrawn = 0;
	float hue = CO2_MIN_HUE;
	uint8_t brightness = 255;
	uint16_t ledCO2 = CO2_MIN;  // internal co2 which has been smoothed
	uint16_t rawbrightness = 0;	// ppm

	strip.Begin();
	strip.SetLuminance(brightness);	 // (0-255) - initially at full brightness
	strip.Show();
	ESP_LOGV("LED Strip", "STARTED");

	//Startup Fade IN OUT Green
	for (uint8_t i = 0; i < 255; i++) {
		strip.ClearTo(RgbColor(0,i,0));
		strip.Show();
		vTaskDelay((4500 / 255 / 2) / portTICK_PERIOD_MS);	 // vTaskDelay wants ticks, not milliseconds
	}
	for (uint8_t i = 255; i > 0; i--) {
		strip.ClearTo(RgbColor(0, i, 0));
		strip.Show();
		vTaskDelay((4500 / 255 / 2) / portTICK_PERIOD_MS);	// vTaskDelay wants ticks, not milliseconds
	}


	while (true) {
		// Convert Lux to Brightness and smooth
		// rawbrightness = sqrt(lux)*5;

		// if (rawbrightness < (255)) {
		// 	brightness = ((brightness * 49) + (rawbrightness)) / 50;
		// } else {
		// 	brightness = ((brightness * 49) + (255)) / 50;
		// }
		// strip.SetLuminance(brightness);									// Luminance is a gamma corrected Brightness

		//Smooth CO2
		if (co2 > CO2_MIN && co2 < CO2_MAX) {
			if (ledCO2 != co2){ //only update leds if co2 has changed
				ledCO2 = ((ledCO2 * (CO2_SMOOTHING_FACTOR - 1)) + co2) / CO2_SMOOTHING_FACTOR;
				hue = mapCO2ToHue(ledCO2);
				// Find Which Pixels are filled with base color, inbetween and not lit
				ppmDrawn = CO2_MIN;	 // ppmDrawn is a counter for how many ppm have been displayed by the previous pixels
				uint8_t currentPixel = 1;
				while (ppmDrawn <= (ledCO2 - PPM_PER_PIXEL)) {
					ppmDrawn += PPM_PER_PIXEL;
					currentPixel++;
				}

				strip.ClearTo(HsbColor(hue, 1.0f, 1.0f), 0, currentPixel - 1);												// apply base color to fully on pixels
				strip.SetPixelColor(currentPixel, HsbColor(hue, 1.0f, (float(ledCO2 - ppmDrawn) / float(PPM_PER_PIXEL))));	// apply the inbetween color mix for the inbetween pixel
				strip.ClearTo(OFF, currentPixel + 1, PIXEL_COUNT);															// apply black to the last few leds

				strip.Show();  // push led data to buffer
			}
		}else{
			if (co2 > CO2_MAX){
				brightness = 255;
				strip.SetLuminance(255);

				while (co2 > CO2_MAX) {
					strip.ClearTo(WARNING_COLOR);
					strip.Show();
					vTaskDelay(1000 / portTICK_PERIOD_MS);	// vTaskDelay wants ticks, not milliseconds
					strip.ClearTo(OFF);
					strip.Show();
					vTaskDelay(1000 / portTICK_PERIOD_MS);	// vTaskDelay wants ticks, not milliseconds
				}
				ledCO2 = co2;  // ledCo2 will have not been updated during CO2 waring Flash
			}
		}
		vTaskDelay(FRAME_TIME / portTICK_PERIOD_MS);  // time between frames
	}
}

void accessPoint(void *parameter) {
#define DNS_INTERVAL 10	 // ms between processing dns requests: dnsServer.processNextRequest();

#define MAX_CLIENTS 4
#define WIFI_CHANNEL 6	// 2.4ghz channel 6

	const IPAddress subnetMask(255, 255, 255, 0);

	DNSServer dnsServer;
	AsyncWebServer server(80);

	WiFi.mode(WIFI_AP);
	WiFi.softAPConfig(localIP, gatewayIP, subnetMask);	// Samsung requires the IP to be in public space
	WiFi.softAP(ssid, password, WIFI_CHANNEL, 0, MAX_CLIENTS);
	WiFi.setSleep(false);

	dnsServer.setTTL(300);				// set 5min client side cache for DNS
	dnsServer.start(53, "*", localIP);	// if DNSServer is started with "*" for domain name, it will reply with provided IP to all DNS request

	// ampdu_rx_disable android workaround see https://github.com/espressif/arduino-esp32/issues/4423
	esp_wifi_stop();
	esp_wifi_deinit();

	wifi_init_config_t my_config = WIFI_INIT_CONFIG_DEFAULT();	// We use the default config ...
	my_config.ampdu_rx_enable = false;							//... and modify only what we want.

	esp_wifi_init(&my_config);	// set the new config
	esp_wifi_start();			// Restart WiFi
	vTaskDelay(100 / portTICK_PERIOD_MS);  // this is necessary don't ask me why

	ESP_LOGV("AccessPoint", "Startup complete by %ims", (millis()));

	//======================== Webserver ========================
	// WARNING IOS (and maybe macos) WILL NOT POP UP IF IT CONTAINS THE WORD "Success" https://www.esp8266.com/viewtopic.php?f=34&t=4398
	// SAFARI (IOS) IS STUPID, G-ZIPPED FILES CAN'T END IN .GZ https://github.com/homieiot/homie-esp8266/issues/476 this is fixed by the webserver serve static function.
	// SAFARI (IOS) there is a 128KB limit to the size of the HTML. The HTML can reference external resources/images that bring the total over 128KB
	// SAFARI (IOS) popup browser has some severe limitations (javascript disabled, cookies disabled)

	server.serveStatic("/Water_Quality_Data.csv", SPIFFS, "/Water_Quality_Data.csv").setCacheControl("no-store");  // fetch data file every page reload
	server.serveStatic("/index.html", SPIFFS, "/index.html").setCacheControl("max-age=120");					   // serve html file

	// Required
	server.on("/connecttest.txt", [](AsyncWebServerRequest *request) { request->redirect("http://logout.net"); });	// windows 11 captive portal workaround
	server.on("/wpad.dat", [](AsyncWebServerRequest *request) { request->send(404); });								// Honestly don't understand what this is but a 404 stops win 10 keep calling this repeatedly and panicking the esp32 :)

	// Background responses: Probably not all are Required, but some are. Others might speed things up?
	// A Tier (commonly used by modern systems)
	server.on("/generate_204", [](AsyncWebServerRequest *request) { request->redirect(localIPURL); });		   // android captive portal redirect
	server.on("/redirect", [](AsyncWebServerRequest *request) { request->redirect(localIPURL); });			   // microsoft redirect
	server.on("/hotspot-detect.html", [](AsyncWebServerRequest *request) { request->redirect(localIPURL); });  // apple call home
	server.on("/canonical.html", [](AsyncWebServerRequest *request) { request->redirect(localIPURL); });	   // firefox captive portal call home
	server.on("/success.txt", [](AsyncWebServerRequest *request) { request->send(200); });					   // firefox captive portal call home
	server.on("/ncsi.txt", [](AsyncWebServerRequest *request) { request->redirect(localIPURL); });			   // windows call home

	// B Tier (uncommon)
	//  server.on("/chrome-variations/seed",[](AsyncWebServerRequest *request){request->send(200);}); //chrome captive portal call home
	//  server.on("/service/update2/json",[](AsyncWebServerRequest *request){request->send(200);}); //firefox?
	//  server.on("/chat",[](AsyncWebServerRequest *request){request->send(404);}); //No stop asking Whatsapp, there is no internet connection
	//  server.on("/startpage",[](AsyncWebServerRequest *request){request->redirect(localIPURL);});

	server.serveStatic("/", SPIFFS, "/").setCacheControl("max-age=120").setDefaultFile("index.html");  // serve any file on the device when requested

	server.onNotFound([](AsyncWebServerRequest *request) {
		request->redirect(localIPURL);
		ESP_LOGW("WebServer", "Page not found sent redirect to localIPURL");
		// DEBUG_SERIAL.print("onnotfound ");
		// DEBUG_SERIAL.print(request->host());       //This gives some insight into whatever was being requested on the serial monitor
		// DEBUG_SERIAL.print(" ");
		// DEBUG_SERIAL.print(request->url());
		// DEBUG_SERIAL.print(" sent redirect to " + localIPURL +"\n");
	});

	server.begin();

	ESP_LOGV("WebServer", "Startup complete by %ims",(millis()));

	while (true) {
		dnsServer.processNextRequest();
		vTaskDelay(DNS_INTERVAL / portTICK_PERIOD_MS);
	}
}

void LightSensor(void *parameter) {
	DFRobot_VEML7700 als;
	float rawLux;
	uint8_t error = 0;
	vTaskDelay(1000 / portTICK_PERIOD_MS);	// allow time for boot and wire begin

	als.begin(); //comment out wire.begin() in this function

	while (true){
		error = als.getAutoWhiteLux(rawLux);  // Get the measured ambient light value

		if (!error) {
			if (rawLux >= (65000 / 10)) {  // overflow protection for 16bit int
				lux = 65000;
			} else {
				lux = int(rawLux * 10);
			}
		}else{
			ESP_LOGW("VEML7700", "getAutoWhiteLux(): STATUS_ERROR");
		}

		vTaskDelay(500 / portTICK_PERIOD_MS);
	}
}

void CO2Sensor(void *parameter) {
	SensirionI2CScd4x scd4x;

	uint16_t error;
	uint16_t co2Raw = 0;
	float temperature = 0.0f;
	float humidity = 0.0f;
	char errorMessage[256];
	bool isDataReady = false;

	scd4x.begin(Wire);

	error = scd4x.startPeriodicMeasurement();
	if (error) {
		errorToString(error, errorMessage, 256);
		ESP_LOGD("SCD4x", "startPeriodicMeasurement(): %s", errorMessage);
	}

	Serial.print("\nCO2 (ppm),Temp (degC),Humidity (%RH)\n");

	while (true) {

		error = 0;
		isDataReady = false;
		do{
			error = scd4x.getDataReadyFlag(isDataReady);
			vTaskDelay(30 / portTICK_PERIOD_MS);	// about 5s between readings
		} while (isDataReady == false);


		if (error) {
			errorToString(error, errorMessage, 256);
			ESP_LOGW("SCD4x", "getDataReadyFlag(): %s", errorMessage);
			
		}else{
			error = scd4x.readMeasurement(co2Raw, temperature, humidity);
			if (error) {
				errorToString(error, errorMessage, 256);
				ESP_LOGW("SCD4x", "readMeasurement(): %s", errorMessage);

			} else if (co2Raw == 0) {
				ESP_LOGW("SCD4x", "CO2 = 0ppm, skipping");
			} else {
				//Serial.printf("%i,%.1f,%.1f\n", co2Raw, temperature, humidity);
				co2 = co2Raw;
			}
		}
		vTaskDelay(4750 / portTICK_PERIOD_MS);  //about 5s between readings, don't waste cpu time
	}
}

void setup() {
	Serial.setTxBufferSize(1024);
	Serial.begin(115200);
	while (!Serial);
	ESP_LOGI("OSAQS", "Compiled " __DATE__ " " __TIME__ " by CD_FER");

	if (SPIFFS.begin()) {  // Initialize SPIFFS (ESP32 SPI Flash Storage)
		if (SPIFFS.exists(LogFilename)) {
			ESP_LOGV("File System", "Initialized Correctly by %ims", millis());
		} else {
			ESP_LOGE("File System", "Can't find %s", (LogFilename));
		}
	} else {
		ESP_LOGE("File System", "Can't mount SPIFFS");
	}

	if (Wire.begin()) {  // Initialize I2C Bus (CO2 and Light Sensor)
		ESP_LOGV("I2C", "Initialized Correctly by %ims", millis());
	} else {
		ESP_LOGE("I2C", "Can't begin I2C Bus");
	}

	// 			Function, Name (for debugging), Stack size, Params, Priority, Handle
	// xTaskCreate(accessPoint, "accessPoint", 5000, NULL, 1, NULL);
	xTaskCreatePinnedToCore(AddressableRGBLeds, "AddressableRGBLeds", 5000, NULL, 1, NULL, 1);
	xTaskCreatePinnedToCore(LightSensor, "LightSensor", 5000, NULL, 1, NULL, 1);
	xTaskCreatePinnedToCore(CO2Sensor, "CO2Sensor", 5000, NULL, 1, NULL, 1);
}

void loop() {
	vTaskSuspend(NULL);
	//vTaskDelay(1);	// Keep RTOS Happy with a 1 tick delay when there is nothing to do
}