#include <Arduino.h>
#include <pgmspace.h>
#include <FS.h>
#include <ESP8266WiFi.h>
#include <IOTAppStory.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Ticker.h>

extern "C" {
#include <user_interface.h>
}

ADC_MODE(ADC_VCC);

//#define DBG_PROG 

#ifdef DBG_PROG
  #define DBG_PRINTLN(x)                  {Serial.println(x); /*delay(100);*/}
  #define DBG_PRINT(x)                    Serial.print(x)
  #define DBG_PRINTP(x)                   Serial.print(F(x))
#else
  #define DBG_PRINTLN(x) 
  #define DBG_PRINT(x)
  #define DBG_PRINTP(x)
#endif

#define APPNAME "TempMon"
//version 1.2.0:    LED will now blink when TempMon is in config mode
//                  poor RSSI will be indicated with series of blinks at power up
//vesrion 1.3.0:    In case of failure to update shadow service, it will be attempted 
//                  again on next report cycle
//version 1.4.0:    Introduced short waiting period to establish WiFi connection before 
//                  sending MQTT msg
//version 1.4.1:    bug fixing and code optimization
#define VERSION "1.4.1"
#define COMPDATE __DATE__ __TIME__
#define MODEBUTTON 0    //GPIO00 (nodeMCU: D3 (FLASH))

#define LED_PIN 2       //GPIO02 (nodeMCU: D4)

#define REPORT_INTERVAL 60 //minutes

//RSSI should be above this level for reliable operation
#define RSSI_CRITICAL_LEVEL (-75)

//timeout for wifi reconnect after deep sleep (in multiples pof 500 ms)
#define WIFI_RECONNECT_TIMEOUT 2

Ticker ticker;

// Data wire is plugged into port 2 on the ESP8266
// TODO: is this the best pin to use!!!
#define ONE_WIRE_BUS 4     //GPIO04 (nodeMCU: D2) 
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature DS18B20(&oneWire);
DeviceAddress DS18B20Address;

// number of params to be defined 
const int _nrXF = 3;

//MQTT broker address
#define AWS_ENDPOINT "a1jkex5rueqh0y.iot.us-east-1.amazonaws.com"
char* AWS_endpoint;  

//AWS device name and shadow MQTT topic
//Used for MQTT client and to communicate wth AWS shadow service
#define AWS_SHADOW "$aws/things/%s/shadow/update"
#define AWS_DEFAULT_NAME "TempMon-%08X"
char* AWS_thing_name;
char* AWS_shadow;

#define AWS_SHADOW_UPDATE_PERIOD        (24 * 60)     //minutes. AWS shadow service will be updated once during this time period
#define AWS_SHADOW_UPDATE_INTERVALS     ((AWS_SHADOW_UPDATE_PERIOD/REPORT_INTERVAL))  
#define AWS_RTCMEM_MAGICBYTE            'W'
#define AWS_RTCMEM_BEGIN                (128-sizeof(rtcMemAWSDef)/4)           //at the end of rtc mem
typedef struct {
    char markerFlag;            // magic byte
    int sleepCycles;       // AWS shadow service update countdown
    //byte mode;  	            // spare
} rtcMemAWSDef __attribute__ ((aligned(4)));
rtcMemAWSDef rtcMemAWS;


//MQTT topic for the actual content
#define AWS_CONTENT_TOPIC  "MyHouse/Room1/Temperature"                                       
char* AWS_content_topic;

//global IAS object
IOTAppStory IAS(APPNAME, VERSION, COMPDATE, MODEBUTTON);
boolean firstBoot;

//prepare SSL layer
//openssl x509 -in aaaaaaaaa-certificate.pem.crt.txt -out cert.der -outform DER 
//openssl rsa -in aaaaaaaaaa-private.pem.key -out private.der -outform DER 
#define CERTIFICATE_FILE "/cert.der"
#define PRIVATE_KEY_FILE "/private.der"

// forward declaration of msg callback function. probably not needed
void callback(char* topic, byte* payload, unsigned int length);

//MQTT client
//set  MQTT port number to 8883 as per standard
WiFiClientSecure espClient;
PubSubClient mqtt(espClient); 
#define MAX_MQTT_CONNECT_RETRIES 2



void callback(char* topic, byte* payload, unsigned int length) {
    DBG_PRINTP("Message [");
    DBG_PRINT(topic);
    DBG_PRINTP("] ");
    for (int i = 0; i < length; i++) {
        DBG_PRINT((char)payload[i]);
    }
    DBG_PRINTLN();

}

boolean mqttConnectAndSend(const char * topic, const char * msg) {
    
    int retries = WIFI_RECONNECT_TIMEOUT;

    DBG_PRINTP("Trying to publish: [");
    DBG_PRINT(topic);
    DBG_PRINTP("] ");
    DBG_PRINT(msg);
    DBG_PRINTLN();
    
    retries = WIFI_RECONNECT_TIMEOUT;
    DBG_PRINTP("Connecting to WiFi AP...");
    while (!WiFi.isConnected() && retries-- > 0 ) {
		delay(500);
        DBG_PRINTP(".");
	} 
    DBG_PRINTLN();
    if (!WiFi.isConnected()) {
        DBG_PRINTLN("Unable to connect to WiFi AP!");
        return false;
    }

    retries = MAX_MQTT_CONNECT_RETRIES;
    while ( retries-- > 0){
        DBG_PRINTP("Attempting MQTT connection (");
        DBG_PRINT(MQTT_SOCKET_TIMEOUT);
        DBG_PRINTP(")...");
        if (mqtt.connect(AWS_thing_name)){
            DBG_PRINTP("connected!");
            DBG_PRINTLN();
            DBG_PRINTP("Publishing: [");
            DBG_PRINT(topic);
            DBG_PRINTP("] ");
            DBG_PRINT(msg);
            DBG_PRINTP(" (");
            DBG_PRINT(strlen(topic)+strlen(msg));
            DBG_PRINTP("/");
            DBG_PRINT(MQTT_MAX_PACKET_SIZE);
            DBG_PRINTP(") ");           
            if (mqtt.publish(topic, msg)) {
                DBG_PRINTP("-> Success.");
                DBG_PRINTLN();
                retries = 0;  
                //we need some delay to allow ESP8266 to actually send the MQTT packet
                unsigned long ts = millis();
                while(millis() < ts + 100){     
                    mqtt.loop();         
                    yield();
                }
                return true;
            }
            else{
                DBG_PRINTP(" -> Fail. Is msg too long!");
                DBG_PRINTLN(MQTT_MAX_PACKET_SIZE);                
            }
        }
        else{
            DBG_PRINTP("failed, rc=");
            DBG_PRINTLN(mqtt.state());
        }
    }
    return false;

}

bool readRTCMemAWS() {
    DBG_PRINTP("Reading AWS RTC Mem...");
    DBG_PRINTLN();
	bool ret = true;
    
	system_rtc_mem_read(AWS_RTCMEM_BEGIN, &rtcMemAWS, sizeof(rtcMemAWS));
	if (rtcMemAWS.markerFlag != AWS_RTCMEM_MAGICBYTE) {
		rtcMemAWS.markerFlag = AWS_RTCMEM_MAGICBYTE;
		rtcMemAWS.sleepCycles = 0;
		system_rtc_mem_write(AWS_RTCMEM_BEGIN, &rtcMemAWS, sizeof(rtcMemAWS));
		ret = false;
	}
	return ret;
}

void writeRTCMemAWS() {
    DBG_PRINTP("Writing AWS RTC Mem...");
    DBG_PRINTLN();
	rtcMemAWS.markerFlag = AWS_RTCMEM_MAGICBYTE;
	system_rtc_mem_write(AWS_RTCMEM_BEGIN, &rtcMemAWS, sizeof(rtcMemAWS));
}

void printRTCMemAWS() {
	DBG_PRINTP("AWS RTC Mem Status");
    DBG_PRINTLN();
    DBG_PRINTP("markerFlag: ");
    DBG_PRINTLN(rtcMemAWS.markerFlag);
	DBG_PRINTP("remaining sleep cycles: ");
	DBG_PRINTLN(rtcMemAWS.sleepCycles);
    DBG_PRINTLN();
}

void fileDump(File* f){
    while (f->available())
      Serial.print(f->read(), HEX);
    f->seek(0, SeekSet);
}

void setup() {
#ifdef DBG_PROG
    IAS.serialdebug(true,115200);      
#endif

    Serial.begin(115200);

    WiFi.begin();

    rst_info *resetInfo; 
    resetInfo = ESP.getResetInfoPtr();

    AWS_endpoint = new char[strlen_P(PSTR(AWS_ENDPOINT)) + 1]; //+1 to accomodate for the termination char
    strcpy_P(AWS_endpoint, PSTR(AWS_ENDPOINT));

    AWS_content_topic = new char[strlen_P(PSTR(AWS_CONTENT_TOPIC)) + 1];
    strcpy_P(AWS_content_topic, PSTR(AWS_CONTENT_TOPIC));

    AWS_thing_name = new char[strlen_P(PSTR(AWS_DEFAULT_NAME)) + 4 + 1]; 
    sprintf_P(AWS_thing_name, PSTR(AWS_DEFAULT_NAME), ESP.getChipId());  

    IAS.preSetConfig(AWS_thing_name, false);
    IAS.addField(AWS_thing_name, "device_name", "Device Name", 25);
    IAS.addField(AWS_endpoint, "aws_endpoint", "AWS Endpoint", 96);
    IAS.addField(AWS_content_topic, "topic", "Topic", 96);


    //set up LED blinker 
    IAS.onConfigMode([](){
        ticker.attach_ms(750,[](){
            digitalWrite(LED_PIN, !digitalRead(LED_PIN));
        });
    });

    if (resetInfo->reason == REASON_DEEP_SLEEP_AWAKE){
        Serial.println(F("Woke up from deep sleep!"));
        IAS.processField();
#ifdef DBG_PROG
        IAS.callHome();      
#endif        
    }
    else {
        DBG_PRINTP("Booting...!");
        DBG_PRINTLN();
        DBG_PRINTP("Flash real size: ");
        DBG_PRINTLN(ESP.getFlashChipRealSize());  
        DBG_PRINTP("Flash IDE size: ");
        DBG_PRINTLN(ESP.getFlashChipSize());                  
        rtcMemAWS.sleepCycles = 0;
        writeRTCMemAWS();
        DBG_PRINTP("AWS RTC Mem initialized!");
        DBG_PRINTLN();        
        pinMode(LED_PIN, OUTPUT);
        digitalWrite(LED_PIN, HIGH);
        IAS.begin(true, 'L');
        digitalWrite(LED_PIN, LOW);
        DBG_PRINTP("Waiting to enter in config mode");
        unsigned long t = millis();
        while ((millis() - t) < 3000){ //allow user to ented config mode within 3 secs after power on
            DBG_PRINT('.');
            if (IAS.buttonLoop() != ModeButtonNoPress)
                t = millis();
            delay(33);
            if (IAS.buttonLoop() == ModeButtonShortPress) //firmware update
                digitalWrite(LED_PIN, HIGH);
            delay(33);
            if (IAS.buttonLoop() == ModeButtonLongPress) //config mode
                digitalWrite(LED_PIN, LOW);                
            delay(33);
        }
        DBG_PRINTLN();
        digitalWrite(LED_PIN, HIGH);
        
        DBG_PRINTP("RSSI: ");
        DBG_PRINT(WiFi.RSSI());
        DBG_PRINTP("dB, Critical level set at: ");
        DBG_PRINT(RSSI_CRITICAL_LEVEL);
        DBG_PRINTLN();

        if (WiFi.RSSI() < RSSI_CRITICAL_LEVEL || WiFi.RSSI() == 31){
            ticker.attach_ms(150,[](){
                digitalWrite(LED_PIN, !digitalRead(LED_PIN));
            });
            t = millis();
            while ((millis() - t) < 3000)
                yield();
            ticker.detach();
            digitalWrite(LED_PIN, HIGH);
        }
    }
    
    readRTCMemAWS();

    AWS_shadow = new char[strlen_P(PSTR(AWS_SHADOW)) + strlen(AWS_thing_name) - 2 + 1];
    sprintf_P(AWS_shadow, PSTR(AWS_SHADOW), AWS_thing_name);

    // verify all parameters are ok
    DBG_PRINTP("Parameters are:");
    DBG_PRINTLN();
    DBG_PRINTLN(AWS_thing_name);
    DBG_PRINTLN(AWS_endpoint);
    DBG_PRINTLN(AWS_shadow);
    DBG_PRINTLN(AWS_content_topic);
    DBG_PRINTLN();

    mqtt.setServer(AWS_endpoint, 8883);

    DBG_PRINTP("Loading credentials for AWS IoT core from SPIFFS...");
    DBG_PRINTLN();
    if (!SPIFFS.begin()) {
        DBG_PRINTP("Failed to mount file system");
        DBG_PRINTLN();
    }
    else {
        DBG_PRINTP("SPIFFS content...");
        DBG_PRINTLN();
        Dir dir = SPIFFS.openDir("");
        while (dir.next()) {
            DBG_PRINTLN(dir.fileName());
        }        
        DBG_PRINTP("Free heap: ");
        DBG_PRINTLN(ESP.getFreeHeap());
        // Load certificate file
        File cert = SPIFFS.open(CERTIFICATE_FILE, "r"); 
        if (!cert){ 
            DBG_PRINTP("Failed to open cert file");
            DBG_PRINTLN();
        }
        else{
            DBG_PRINTP("Success to open cert file, ");
            if (espClient.loadCertificate(cert))
                DBG_PRINTP("cert loaded");
            else
                DBG_PRINTP("cert not loaded");
            DBG_PRINTLN();
        }

        // Load private key file
        File private_key = SPIFFS.open(PRIVATE_KEY_FILE, "r"); 
        if (!private_key){ 
            DBG_PRINTP("Failed to open private key file");
            DBG_PRINTLN(); 
        }
        else{
            DBG_PRINTP("Success to open private key file, "); 
            if (espClient.loadPrivateKey(private_key))
                DBG_PRINTP("private key loaded");
            else
                DBG_PRINTP("private key not loaded");    
            DBG_PRINTLN();
        }           

        SPIFFS.end();
        DBG_PRINTP("Free heap: ");
        DBG_PRINTLN(ESP.getFreeHeap());
    }
    
    float temp;
    DS18B20.getAddress(DS18B20Address, 0);
    DS18B20.setResolution(DS18B20Address,9);
    DS18B20.requestTemperatures(); 
    temp = DS18B20.getTempCByIndex(0); 
    DBG_PRINTP("Temperature: ");
    DBG_PRINTLN(temp);
    
    String s;
    StaticJsonBuffer<250> jsonBuffer; 
    JsonObject& root = jsonBuffer.createObject();
    root["sensor"] = AWS_thing_name;
    root["temperature"] = temp;
    root.printTo(s);
    mqttConnectAndSend(AWS_content_topic, s.c_str());

    // update AWS shadow service if needed
    if (rtcMemAWS.sleepCycles == 0){
        DBG_PRINTP("Time to check for new FW and to update AWS shadow service.");
        DBG_PRINTLN();
        IAS.callHome();
        String s;
        StaticJsonBuffer<400> jsonBuffer; 
        JsonObject& root = jsonBuffer.createObject();
        JsonObject& state = root.createNestedObject("state");
        JsonObject& state_reported = state.createNestedObject("reported");
        state_reported["sensor"] = AWS_thing_name;
        state_reported["topic"] = AWS_content_topic;
        state_reported["battery"] = ESP.getVcc();
        state_reported["rxlev"] = WiFi.RSSI();
        state_reported["AppName"] = APPNAME;
        state_reported["Version"] = VERSION;
        state_reported["CompileDate"] = COMPDATE;
        root.printTo(s);
        if (mqttConnectAndSend(AWS_shadow, s.c_str()))
            //restart shadow update cycle only if succesfully updated
            rtcMemAWS.sleepCycles = AWS_SHADOW_UPDATE_INTERVALS-1;
    }
    else
        rtcMemAWS.sleepCycles--;


    DBG_PRINTLN();
    writeRTCMemAWS();
    DBG_PRINTLN();    
    printRTCMemAWS();
    
    Serial.println(F("Going to deep sleep..."));

    // Connect GPIO16 to RST to allow ESP to wake up from deepSleep
    ESP.deepSleep(1e6L * 60 * REPORT_INTERVAL); 
    
}

void loop() {
    // put your main code here, to run repeatedly:
}