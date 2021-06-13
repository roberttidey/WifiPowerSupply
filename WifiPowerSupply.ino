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
#define DAC4725V_ADDR	0x60 
#define DAC4725I_ADDR	0x61
#define I2C_SDA			4
#define I2C_SCL			5
#define ROTARYMAX		4095
#define ROTARYINITV		3000
#define ROTARYINITI		1000
//Vadj Rotary
#define ROTARY1A		12
#define ROTARY1B		13
//Iadj Rotary
#define ROTARY2A				2
#define ROTARY2B				14

#define FAN						15
#define DATA_EXT				".dat"
#define STARTUP_DELAY			2000
#define REFRESH_INTERVAL		500

//server commands
#define COMMAND_SETVOLTS	1
#define COMMAND_SETLIMIT	2
#define COMMAND_STARTCAP	3
#define COMMAND_STARTPLAY	4
#define COMMAND_STOP		5

int timeInterval = 10;
unsigned long elapsedTime;
unsigned long refreshInterval = REFRESH_INTERVAL;
unsigned long refreshTime;
int fanSpeed = 0;
//3 pairs of values threshold,fanSpeed
int fanControls[6] = {0,0,1000,400,2500,500};
//Gain is so rotary value 2000 = 4096 << 16
int rotaryPos[2] = {ROTARYINITV,ROTARYINITI};
String dataPrefix = "data";
String strConfig;

#define COMMAND_ST_IDLE			0
#define COMMAND_ST_STARTCAP		1
#define COMMAND_ST_ACTIVECAP	2
#define COMMAND_ST_STOPCAP		3
#define COMMAND_ST_STARTPLAY	4
#define COMMAND_ST_ACTIVEPLAY	5
#define COMMAND_ST_STOPPLAY		6
int commandState = COMMAND_ST_IDLE;
int captureDuration = 30000;
String captureFile;
String playFile;
String tempCaptureFile = "tempCapture.dat";
int dataRecord = 0;
unsigned long dataStartTime;
unsigned long nextPlayTime;
int dataFileCount;
int playPosition;

int startup = 1;

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
int adcConversion[4] = {1995,831000,6605,-230000};

//conversion values to DAC values, slope is multiplied by 10000
#define DAC_V_SLOPE		0
#define DAC_V_OFFSET	1
#define DAC_L_SLOPE		2
#define DAC_L_OFFSET	3
int dacConversion[4] = {2307,1250,6235,33};

String configNames[] = {"host","timeInterval","refreshInterval","adcVSlope","adcVOffset","adcASlope","adcAOffset","dacVSlope","dacVOffset","dacLSlope","dacLOffset","fanControl","captureDuration","a","c","d"};

SSD1306Wire display(DISPLAY_ADDR, I2C_SDA, I2C_SCL);
Adafruit_ADS1115 ads;
Adafruit_MCP4725 dac[2];

void parseFanControls(String str) {
	int i;
	int lastIndex = 0;
	int index = 0;
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

int limitVal(int val, int min, int max) {
	if(val < min) {
		val = min;
	} else if (val > max) {
		val = max;
	}
	return val;
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
					case 1: timeInterval = limitVal(line.toInt(),1,100); break;
					case 2: refreshInterval = limitVal(line.toInt(),10,5000); break;
					case 3: adcConversion[ADC_V_SLOPE] = limitVal(line.toInt(),100,5000); break;
					case 4: adcConversion[ADC_V_OFFSET] = limitVal(line.toInt(),-500,500); break;
					case 5: adcConversion[ADC_A_SLOPE] = limitVal(line.toInt(),100,5000); break;
					case 6: adcConversion[ADC_A_OFFSET] = limitVal(line.toInt(),-500,500); break;
					case 7: dacConversion[DAC_V_SLOPE] = limitVal(line.toInt(),500,10000); break;
					case 8: dacConversion[DAC_V_OFFSET] = limitVal(line.toInt(),100000,2000000); break;
					case 9: dacConversion[DAC_L_SLOPE] = limitVal(line.toInt(),500,20000); break;
					case 10: dacConversion[DAC_L_OFFSET] = limitVal(line.toInt(),-1000000,1000000); break;
					case 11: fanControlStr = line;
							parseFanControls(fanControlStr);
							break;
					case 12: captureDuration = limitVal(line.toInt(),1000,1000000000);;
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
		Serial.print(F("refreshInterval:"));Serial.println(refreshInterval);
		Serial.print(F("adcVSlope:"));Serial.println(adcConversion[ADC_V_SLOPE]);
		Serial.print(F("adcVOffset:"));Serial.println(adcConversion[ADC_V_OFFSET]);
		Serial.print(F("adcASlope:"));Serial.println(adcConversion[ADC_A_SLOPE]);
		Serial.print(F("adcAOffset:"));Serial.println(adcConversion[ADC_A_OFFSET]);
		Serial.print(F("dacVSlope:"));Serial.println(dacConversion[DAC_V_SLOPE]);
		Serial.print(F("dacVOffset:"));Serial.println(dacConversion[DAC_V_OFFSET]);
		Serial.print(F("dacLSlope:"));Serial.println(dacConversion[DAC_L_SLOPE]);
		Serial.print(F("dacLOffset:"));Serial.println(dacConversion[DAC_V_OFFSET]);
		Serial.print(F("fanControlStr:"));Serial.println(fanControlStr);
		Serial.print(F("captureDuration:"));Serial.println(captureDuration);
	} else {
		Serial.println(String(CONFIG_FILE) + " not found. Use default encoder");
	}
}

void configModeCallback (WiFiManager *myWiFiManager) {
	updateDisplay("Wifi config 120s",WiFi.softAPIP().toString(),String(myWiFiManager->getConfigPortalSSID()));
}

void addCaptureRecord() {
	File f;
	unsigned long m = millis();
	
	if(dataRecord) 
		f = FILESYS.open("/" + captureFile, "a");
	else
		f = FILESYS.open("/" + captureFile, "w");
		
	if(f) {
		f.println(String(millis() - dataStartTime) + "," + makeValString(measures[M_VOLTS]) + "," + makeValString(measures[M_AMPS]));
		dataRecord++;
		f.close();
	}
}

//check to see if play a record, updates position in file
int processPlayRecord() {
	if((millis() - dataStartTime) > nextPlayTime) {
		File f;
		String line;
		f = FILESYS.open("/" + playFile, "r");
		if(f) {
			f.seek(playPosition);
			if(f.available()) {
				line =f.readStringUntil('\n');
				line.replace("\r","");
				if(line.length() > 0 && line.charAt(0) != '#') {
					int i, val;
					int lastIndex = 0;
					int index = 0;
					index = line.indexOf(',', lastIndex);
					if(index > 0) {
						nextPlayTime = line.substring(lastIndex, index).toInt();
						lastIndex = index+1;
						index = line.indexOf(',', lastIndex);
						if(index > 0) {
							val = line.substring(lastIndex, index).toInt();
						} else {
							val = line.substring(lastIndex).toInt();
						}
						val = limitVal((val * dacConversion[DAC_V_SLOPE] + dacConversion[DAC_V_OFFSET]) / 10000, 0, ROTARYMAX);
						rotaryPos[0] = val;
						setRotaryPosition(0, val);
						setDac(0);
					}
				}
				playPosition = f.position();
				f.close();
				addCaptureRecord();
			} else {
				playPosition = -1;
				f.close();
			}
		} else {
			playPosition = -1;
		}
		
	} else {
		return playPosition;
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

void setDac(int dacN) {
	if(dacN == 0) {
		dac[0].setVoltage((ROTARYMAX - rotaryPos[0]), false);
	} else {
		dac[1].setVoltage(rotaryPos[1], false);
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
			rotaryPos[i] = limitVal(rotaryPos[i] + diff, 0, ROTARYMAX);
			setRotaryPosition(i, rotaryPos[i]);
			setDac(i);
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
	measures[M_LIMIT] = (rotaryPos[1] * 10000 - dacConversion[DAC_L_OFFSET]) / dacConversion[DAC_L_SLOPE];
	updateDisplay(makeValString(measures[M_VOLTS])+" Volts",makeValString(measures[M_AMPS])+" Amps",makeValString(measures[M_LIMIT])+" Limit");
	updateFan();
}

void refresh() {
	unsigned long m = millis();
	if((m - refreshTime) > refreshInterval) {
		refreshTime = m;
		updateControls();
		updateRealTime();
		switch(commandState) {
			case COMMAND_ST_IDLE :
				break;
			case COMMAND_ST_STARTCAP :
				commandState = COMMAND_ST_ACTIVECAP;
				dataRecord = 0;
				dataStartTime = millis();
				break;
			case COMMAND_ST_ACTIVECAP :
				addCaptureRecord();
				if((millis() - dataStartTime) >= captureDuration) {
					commandState = COMMAND_ST_STOPCAP;
				}
				break;
			case COMMAND_ST_STOPCAP :
				dataFileCount++;
				commandState = COMMAND_ST_IDLE;
				break;
			case COMMAND_ST_STARTPLAY :
				playPosition = 0;
				dataRecord = 0;
				commandState = COMMAND_ST_ACTIVEPLAY;
				break;
			case COMMAND_ST_ACTIVEPLAY :
				processPlayRecord();
				if(playPosition < 0) {
					commandState = COMMAND_ST_STOPPLAY;
				}
				break;
			case COMMAND_ST_STOPPLAY :
				commandState = COMMAND_ST_IDLE;
				if(FILESYS.exists("/" + playFile)) {
					FILESYS.remove("/" + playFile);
				}
				if(FILESYS.exists("/" + tempCaptureFile)) {
					FILESYS.rename("/" + tempCaptureFile, "/" + playFile);
				}
				break;
			
		}
	}
}

//action SetFan
void handleSetFanSpeed() {
	fanSpeed = server.arg("fanspeed").toInt();
	if(fanSpeed > 700) fanSpeed = 700;
	server.send(200, "text/html", "OK");
}

//action getdataFiles
void handleGetDataFiles() {
	String fileList;
	String filename;
	Dir dir = FILESYS.openDir("/");
	int i;
	dataFileCount = 0;
	while (dir.next()) {
		filename = dir.fileName();
		i = filename.indexOf(DATA_EXT);
		if(i > 0 &&  i == (filename.length()-4)) {
			fileList += filename.substring(1) + "<BR>";
			dataFileCount++;
		}
	}
	server.send(200, "text/html", fileList);
}

//action request to perform a command
void handleCommand() {
	int par = -1;
	int val;
	int rotary;
	String ret = "OK";
	String parameter = server.arg("parameter");
	int command = server.arg("command").toInt();
	Serial.println("handle command:" + String(command) + " parameter:" + parameter);
	switch(command) {
		case COMMAND_SETVOLTS:	par = DAC_V_SLOPE;
								rotary = 0;
								break;
		case COMMAND_SETLIMIT:	par = DAC_L_SLOPE;
								rotary = 1;
								break;
		case COMMAND_STARTCAP:	if(commandState != COMMAND_ST_IDLE) {
									ret = "already busy capturing";
								} else {
									captureFile = parameter;
									commandState = COMMAND_ST_STARTCAP;
								}
								break;
		case COMMAND_STARTPLAY:	if(commandState != COMMAND_ST_IDLE) {
									ret = "already busy playing";
								} else {
									playFile = parameter;
									captureFile = tempCaptureFile;
									commandState = COMMAND_ST_STARTPLAY;
								}
								break;
		case COMMAND_STOP:		if(commandState == COMMAND_ST_ACTIVECAP) {
									commandState = COMMAND_ST_STOPCAP;
								} else if(commandState == COMMAND_ST_ACTIVEPLAY) {
									commandState == COMMAND_ST_STOPPLAY;
								} else {
									ret = "not active";
								}
								break;
	}
	if(par >= 0) {
		val = parameter.toFloat() * 1000;
		val = (val * dacConversion[par] + dacConversion[par+1]) / 10000;
		if(val < 0) val - 0;
		if(val > 4095) val - 4095;
		rotaryPos[rotary] = val;
		setRotaryPosition(rotary, val);
		setDac(rotary);
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
	status += "Volts:" + makeValString(measures[M_VOLTS]) + "<BR>";
	status += "Amps:" + makeValString(measures[M_AMPS]) + "<BR>";
	status += "ILimit:" + makeValString(measures[M_LIMIT]) + "<BR>";
	status += "RotaryVolts:" + String(rotaryPos[0]) + "<BR>";
	status += "RotaryAmps:" + String(rotaryPos[1]) + "<BR>";
	status += "dataFileCount:" + String(dataFileCount) + "<BR>";
	status += "commandState:" + String(commandState) + "<BR>";
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
	server.on("/command", handleCommand);
	server.on("/getdatafiles", handleGetDataFiles);
	server.on("/setFanSpeed", handleSetFanSpeed);
}

void setupEnd() {
	if(WiFi.status() == WL_CONNECTED) {
		updateDisplay("Power Supply", WiFi.localIP().toString(),"");
	} else {
		updateDisplay("Power Supply", "No network","local mode");
	}
	startup = 1;
	ads.begin();
	rotaryEncoderInit(1);
	setRotaryEncoderPins(0, ROTARY1A, ROTARY1B, -1);
	setRotaryEncoderPins(1, ROTARY2A, ROTARY2B, -1);
	setRotaryLimits(0, 0, ROTARYMAX);
	setRotaryPosition(0, ROTARYINITV);
	setRotaryLimits(1, 0, ROTARYMAX);
	setRotaryPosition(1, ROTARYINITI);
	dac[0].begin(DAC4725V_ADDR);
	dac[1].begin(DAC4725I_ADDR);
	setDac(0);
	setDac(1);
}

void loop() {
	if(startup) {
		if(elapsedTime * timeInterval > STARTUP_DELAY)
			startup = 0;
	} else {
		refresh();
	}
	server.handleClient();
	delay(timeInterval);
	elapsedTime++;
}
