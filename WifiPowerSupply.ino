/*
*	WifiPowerSupply.ino
*	Copyright (c) 2021 Bob Tidey
*
*	Lab power supply using laptop adapter + XL4015 buck converter
*   Variable 1.25V -> 19V at 4A, plus fixed 3.3 /5.0V 1A
*	Rotary encoders to set voltage and current limit
*	SSD1306 to show voltage, current and current limit
*	browser based access for advanced functions
*	Uses XL4015,ESP8255,ADS1105,SSD1306,MCP4725,mini buck converter
*/

#define ESP8266
#include "BaseConfig.h"
#define FONT_SELECT
#define INC_FONT_ArialMT_Plain_16
#include "SSD1306Wire.h"
#include "Adafruit_ADS1X15.h"
#include "RotaryEncoderArray.h"
#include "Adafruit_MCP4725.h"

#define DISPLAY_ADDR	0X3C
#define DAC4725V_ADDR	0x62 
#define DAC4725I_ADDR	0x63
#define I2C_SDA			4
#define I2C_SCL			5
#define ROTARYMAX		4095
#define ROTARYINIT		500
//Vadj Rotary
#define ROTARY1A		12
#define ROTARY1B		13
//Iadj Rotary
#define ROTARY2A		14
#define ROTARY2B		2

#define FAN				15
#define CAPTURE_EXT ".dat"
#define STATE_STARTUP	0
#define STATE_NORMAL	1
#define STATE_CAPTURE	2
#define STARTUP_DELAY			2000
#define DISPLAY_INTERVAL		500

int timeInterval = 10;
unsigned long elapsedTime;
unsigned long displayInterval = DISPLAY_INTERVAL;
unsigned long displayTime;
int fanSpeed = 0;
//3 pairs of values threshold,fanSpeed
int fanControls[6] = {0,0,1000,400,2500,500};
//Gain is so rotary value 2000 = 4096 << 16
int VadjGain = 134184;
int IadjGain = 134184;
int VadjOffset = 0;
int IadjOffset = 0;
int rotaryPos[2] = {ROTARYINIT,ROTARYINIT};
String dataPrefix = "data";
String strConfig;
String captureFile;

int captureRecord = 0;
int captureFileCount;
int state = 0;

//measures volts, amps, limit
#define M_VOLTS	0
#define M_AMPS	1
#define M_LIMIT	2
int measures[3]; // mV or mA

//conversion parameters ADC
#define ADC_V_SLOPE		0
#define ADC_V_OFFSET	1
#define ADC_A_SLOPE		2
#define ADC_A_OFFSET	3
int adcConversion[4] = {1100,0,2000,160};

//conversion values to DAC values, slope is multiplied by 10000
#define DAC_V_SLOPE		0
#define DAC_V_OFFSET	1
#define DAC_L_SLOPE		2
#define DAC_L_OFFSET	3
int dacConversion[4] = {2307,1250,6235,33};

String configNames[] = {"host","timeInterval","displayInterval","adcVSlope","adcVOffset","adcASlope","adcAOffset","dacVSlope","dacVOffset","dacLSlope","dacLOffset","fanControl","a","a","c","d"};

SSD1306Wire display(DISPLAY_ADDR, I2C_SDA, I2C_SCL);
Adafruit_ADS1115 ads;
Adafruit_MCP4725 dac[2];

void parseFanControls(String str) {
	int i;
	int lastIndex = 0;
	int index = 0;
	String par;
	for(i=0; i<6; i++) {
		index = str.indexOf(',', lastIndex);
		if(index > 0) {
			fanControls[i] = str.substring(lastIndex, index).toInt();
			lastIndex = index+1;
		}
		else {
			fanControls[i] = str.substring(lastIndex).toInt();
			break;
		}
	}
}

/*
  load config
*/
void loadConfig() {
	String line = "";
	String fanControlStr;
	int config = 0;
	File f = FILESYS.open(CONFIG_FILE, "r");
	if(f) {
		strConfig = "";
		while(f.available()) {
			line =f.readStringUntil('\n');
			line.replace("\r","");
			if(line.length() > 0 && line.charAt(0) != '#') {
				switch(config) {
					case 0: host = line;break;
					case 1: timeInterval = line.toInt(); break;
					case 2: displayInterval = line.toInt(); break;
					case 3: adcConversion[ADC_V_SLOPE] = line.toInt(); break;
					case 4: adcConversion[ADC_V_OFFSET] = line.toInt(); break;
					case 5: adcConversion[ADC_A_SLOPE] = line.toInt(); break;
					case 6: adcConversion[ADC_A_OFFSET] = line.toInt(); break;
					case 7: dacConversion[DAC_V_SLOPE] = line.toInt(); break;
					case 8: dacConversion[DAC_V_OFFSET] = line.toInt(); break;
					case 9: dacConversion[DAC_L_SLOPE] = line.toInt(); break;
					case 10: dacConversion[DAC_L_OFFSET] = line.toInt(); break;
					case 11: fanControlStr = line;
							parseFanControls(fanControlStr);
							Serial.println(F("Config loaded from file OK"));
							break;
				}
				strConfig += configNames[config] + ":" + line + "<BR>";
				config++;
			}
		}
		f.close();
		Serial.println("Config loaded");
		Serial.print(F("host:"));Serial.println(host);
		Serial.print(F("timeInterval:"));Serial.println(timeInterval);
		Serial.print(F("displayInterval:"));Serial.println(displayInterval);
		Serial.print(F("adcVSlope:"));Serial.println(adcConversion[ADC_V_SLOPE]);
		Serial.print(F("adcVOffset:"));Serial.println(adcConversion[ADC_V_OFFSET]);
		Serial.print(F("adcASlope:"));Serial.println(adcConversion[ADC_A_SLOPE]);
		Serial.print(F("adcAOffset:"));Serial.println(adcConversion[ADC_A_OFFSET]);
		Serial.print(F("dacVSlope:"));Serial.println(dacConversion[DAC_V_SLOPE]);
		Serial.print(F("dacVOffset:"));Serial.println(dacConversion[DAC_V_OFFSET]);
		Serial.print(F("dacLSlope:"));Serial.println(dacConversion[DAC_L_SLOPE]);
		Serial.print(F("dacLOffset:"));Serial.println(dacConversion[DAC_V_OFFSET]);
		Serial.print(F("fanControlStr:"));Serial.println(fanControlStr);
	} else {
		Serial.println(String(CONFIG_FILE) + " not found. Use default encoder");
	}
}

void configModeCallback (WiFiManager *myWiFiManager) {
	updateDisplay("Wifi config 120s",WiFi.softAPIP().toString(),String(myWiFiManager->getConfigPortalSSID()));
}

void setCaptureFile(String filePrefix) {
	int fn;
	for(fn = 1; fn < 100; fn++) {
		captureFile = filePrefix + String(fn) + CAPTURE_EXT;
		if(!FILESYS.exists("/" + captureFile)) return;
	}
	captureFile = filePrefix + CAPTURE_EXT;
}

void saveCapture(int type) {
	File f;
	int i = 0;
	int v;
	unsigned long m = millis();
	
	if(captureRecord) 
		f = FILESYS.open("/" + captureFile, "a");
	else
		f = FILESYS.open("/" + captureFile, "w");
		
	if(f) {
		f.close();
	}
}

String  makeValString(unsigned long val) {
	String digits = String(val/1000);
	if(digits.length() < 2) {
		digits = " " + digits;
	}
	String decimal = "00" + String(val % 1000);
	decimal = decimal.substring(decimal.length()-3);
	return digits + "." + decimal;
}

void updateDisplay(String line1, String line2, String line3) {
	display.clear();
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.setFont(ArialMT_Plain_16);
    display.drawString(0, 0, line1);
    display.drawString(0, 22, line2);
    display.drawString(0, 44, line3);
	display.display();
}

void updateFan() {
	if(fanSpeed) {
		analogWrite(FAN, fanSpeed);
	} else {
		if(measures[M_AMPS] > fanControls[4]) {
			analogWrite(FAN, fanControls[5]);
		} else if(measures[M_AMPS] > fanControls[2]) {
			analogWrite(FAN, fanControls[3]);
		} else {
			analogWrite(FAN, fanControls[1]);
		}
	}
}

void setDac(int dacN, int val) {
	if(dacN == 0) {
		//dac[0].setVoltage((ROTARYMAX - rotaryPos[i]), false);
	} else {
		//dac[1].setVoltage(rotaryPos[i], false);
	}
}

void updateControls() {
	int diff;
	int acc;
	int i;
	for(i = 0; i < 2; i++) {
		diff = getRotaryPosition(i) - rotaryPos[i];
		if(diff ) {
			acc = 1 << (abs(diff) / 2);
			if(acc > 64) acc = 64;
			diff *= acc;
			rotaryPos[i] += diff;
			if(rotaryPos[i] < 0) {
				rotaryPos[i] = 0;
			} else if(rotaryPos[i] > ROTARYMAX) {
				rotaryPos[i] = ROTARYMAX;
			}
			setRotaryPosition(i, rotaryPos[i]);
			setDac(i, rotaryPos[i]);
		}
	}
}

void updateRealTime() {
	ads.setGain(GAIN_TWO); // 2.048V = 32768
	measures[M_VOLTS] = ((adcConversion[ADC_V_SLOPE] * (ads.readADC_Differential_0_1() - adcConversion[ADC_V_OFFSET])) >> 4) / 100;
	if(measures[M_VOLTS] < 0) measures[M_VOLTS] = 0;
	ads.setGain(GAIN_EIGHT); //0.512V = 32768
	measures[M_AMPS] = ((adcConversion[ADC_A_SLOPE] * (ads.readADC_Differential_2_3() - adcConversion[ADC_A_OFFSET])) >> 6) / 100;
	if(measures[M_AMPS] < 0) measures[M_AMPS] = 0;
	updateDisplay(makeValString(measures[M_VOLTS])+" Volts",makeValString(measures[M_AMPS])+" Amps",String(rotaryPos[0]) + " Pos");
	measures[M_LIMIT] = rotaryPos[1] * 10000 / dacConversion[DAC_L_SLOPE] + dacConversion[DAC_L_OFFSET];
	updateFan();
}

void stateMachine() {
	unsigned long m = millis();
	switch(state) {
		case STATE_STARTUP :
			if(elapsedTime * timeInterval > STARTUP_DELAY)
				state = STATE_NORMAL;
			break;
		case STATE_NORMAL : 
			if((m - displayTime) > displayInterval) {
				displayTime = m;
				updateControls();
				updateRealTime();
			}
			break;
		case STATE_CAPTURE :
			break;
	}
}

//action SetFan
void handleSetFanSpeed() {
	fanSpeed = server.arg("fanspeed").toInt();
	if(fanSpeed > 700) fanSpeed = 700;
	server.send(200, "text/html", "OK");
}

//action SetValue volts or limit
void handleSetValue() {
	int par = -1;
	int val;
	int rotary;
	if(server.arg("parameter") == "volts") {
		par = DAC_V_SLOPE;
		rotary = 0;
	} else if(server.arg("parameter") == "limit") {
		par = DAC_L_SLOPE;
		rotary = 1;
	}
	if(par >= 0) {
		val = server.arg("value").toInt();
		val = (val - dacConversion[par+1]) * dacConversion[par] / 10000;
		if(val < 0) val - 0;
		if(val > 4095) val - 4095;
		rotaryPos[rotary] = val;
		setRotaryPosition(rotary, val);
		setDac(rotary, val);
	}
	server.send(200, "text/html", "OK");
}

//action getcapturefiles
void handleGetCaptureFiles() {
	String fileList;
	String filename;
	Dir dir = FILESYS.openDir("/");
	int i;
	captureFileCount = 0;
	while (dir.next()) {
		filename = dir.fileName();
		i = filename.indexOf(CAPTURE_EXT);
		if(i > 0 &&  i == (filename.length()-8)) {
			fileList += filename.substring(1) + "<BR>";
			captureFileCount++;
		}
	}
	server.send(200, "text/html", fileList);
}

//action request to capture data
void handleCapture() {
	String ret;
	captureFile = server.arg("filename");
	int captureType = server.arg("capturetype").toInt();
	Serial.println("handle capture:" + captureFile + " type:" + String(captureType));
	
	if(state == STATE_NORMAL) {
		state = STATE_CAPTURE;
	} else {
		ret = "already busy capturing";
	}
	server.send(200, "text/plain", ret);
}

//action request to reload config
void handleLoadConfig() {
	loadConfig();
	server.send(200, "text/html", strConfig);
}

//action request to save config
void handleSaveConfig() {
	File f;
	String config = server.arg("config");
	config.replace("<BR>","\n");
	f = FILESYS.open(CONFIG_FILE, "w");
	if(f) {
		f.print(config);
		f.close();
		loadConfig();
		server.send(200, "text/plain", "config saved");
	} else {
		server.send(200, "text/plain", "error saving config");
	}
}

//send response to status request
void handleStatus() {
	String status = "<head><link rel='shortcut icon' href='about:blank'></head><body>";
	status += "Volts:" + makeValString(measures[M_VOLTS]) + " V<BR>";
	status += "Amps:" + makeValString(measures[M_AMPS]) + " A<BR>";
	status += "I Limit:" + makeValString(measures[M_LIMIT]) + " A<BR>";
	status += "RotaryVolts:" + String(rotaryPos[0]) + "<BR>";
	status += "RotaryAmps:" + String(rotaryPos[1]) + "<BR>";
	status += "FreeHeap:" + String(ESP.getFreeHeap()) + "<BR></body>";
	server.send(200, "text/html", status);
}

void setupStart() {
	analogWrite(FAN, 0);
	display.init();
	display.flipScreenVertically();
	updateDisplay("Power Supply", "Connecting","Up to 180s");
	wifiManager.setAPCallback(configModeCallback);
}

void extraHandlers() {
	server.on("/status",  handleStatus);
	server.on("/loadconfig", handleLoadConfig);
	server.on("/saveconfig", handleSaveConfig);
	server.on("/capture", handleCapture);
	server.on("/getcapturefiles", handleGetCaptureFiles);
	server.on("/setFanSpeed", handleSetFanSpeed);
	server.on("/setValue", handleSetValue);
}

void setupEnd() {
	if(WiFi.status() == WL_CONNECTED) {
		updateDisplay("Power Supply", WiFi.localIP().toString(),"");
	} else {
		updateDisplay("Power Supply", "No network","local mode");
	}
	state = STATE_STARTUP;
	ads.begin();
	rotaryEncoderInit(1);
	setRotaryEncoderPins(0, ROTARY1A, ROTARY1B, -1);
	setRotaryEncoderPins(1, ROTARY2A, ROTARY2B, -1);
	setRotaryLimits(0, 0, ROTARYMAX);
	setRotaryPosition(0, ROTARYINIT);
	setRotaryLimits(1, 0, ROTARYMAX);
	setRotaryPosition(1, ROTARYINIT);
	dac[0].begin(DAC4725V_ADDR);
	dac[1].begin(DAC4725I_ADDR);
}

void loop() {
	stateMachine();
	server.handleClient();
	delay(timeInterval);
	elapsedTime++;
}
