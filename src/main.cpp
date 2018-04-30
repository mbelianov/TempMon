//#define DBG_PROG 

#ifdef DBG_PROG
#define PROGMEM_T __attribute__((section(".irom.text.template")))
#define DEBUG_LOG_T(fmt, ...) \
    { static const char pfmt[] PROGMEM_T = fmt; Serial.printf_P(pfmt, ## __VA_ARGS__); }
#else
    #define DEBUG_LOG_T(fmt, ...)
#endif


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
#include <cert.h>
#include <private.h>

extern "C" {
#include <user_interface.h>
}

ADC_MODE(ADC_VCC);



#define APPNAME "TempMon"
//version 1.2.0:    LED will now blink when TempMon is in config mode
//                  poor RSSI will be indicated with series of blinks at power up
//vesrion 1.3.0:    In case of failure to update shadow service, it will be attempted 
//                  again on next report cycle
//version 1.4.0:    Introduced short waiting period to establish WiFi connection before 
//                  sending MQTT msg
//version 1.4.1:    bug fixing and code optimization
//version 1.5.0:    option to load cert and private key from flash memmory
#define VERSION "1.5.0"
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
const char* PROGMEM AWS_ENDPOINT="a1jkex5rueqh0y.iot.us-east-1.amazonaws.com";
char* AWS_endpoint;  

//AWS device name and shadow MQTT topic
//Used for MQTT client and to communicate wth AWS shadow service
const char* PROGMEM AWS_SHADOW="$aws/things/%s/shadow/update";
const char* PROGMEM AWS_DEFAULT_NAME="TempMon-%08X";
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
const char* PROGMEM AWS_CONTENT_TOPIC="MyHouse/Room1/Temperature";
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



boolean mqttConnectAndSend(const char * topic, const char * msg) {
    
    int retries = WIFI_RECONNECT_TIMEOUT;

    DEBUG_LOG_T("Trying to publish: [%s] %s\n\r", topic, msg);
    
    retries = WIFI_RECONNECT_TIMEOUT;
    DEBUG_LOG_T("Connecting to WiFi AP...");
    while (!WiFi.isConnected() && retries-- > 0 ) {
		delay(500);
        DEBUG_LOG_T(".");
	} 
    DEBUG_LOG_T("\n\r");
    if (!WiFi.isConnected()) {
        DEBUG_LOG_T("Unable to connect to WiFi AP!\n\r");
        return false;
    }

    retries = MAX_MQTT_CONNECT_RETRIES;
    while ( retries-- > 0){
        DEBUG_LOG_T("Attempting MQTT connection (timeout: %d s)...", MQTT_SOCKET_TIMEOUT);
        if (mqtt.connect(AWS_thing_name)){
            DEBUG_LOG_T("connected!\n\r");
            DEBUG_LOG_T("Publishing: [%s] %s (%d/%d)",topic, msg, strlen(topic)+strlen(msg), MQTT_MAX_PACKET_SIZE);         
            if (mqtt.publish(topic, msg)) {
                DEBUG_LOG_T(" -> Success.\n\r");
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
                DEBUG_LOG_T(" -> Fail. Is msg too long?");          
            }
        }
        else{
            DEBUG_LOG_T("failed, rc=%d", mqtt.state());
        }
    }
    return false;

}

bool readRTCMemAWS() {
    DEBUG_LOG_T("Reading AWS RTC Mem...\n\r");

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
    DEBUG_LOG_T("Writing AWS RTC Mem...\n\r");

	rtcMemAWS.markerFlag = AWS_RTCMEM_MAGICBYTE;
	system_rtc_mem_write(AWS_RTCMEM_BEGIN, &rtcMemAWS, sizeof(rtcMemAWS));
}

void printRTCMemAWS() {
	DEBUG_LOG_T("AWS RTC Mem Status:\n\rmarkerFlag: %c\n\rremaining sleep cycles: %d\n\r", rtcMemAWS.markerFlag, rtcMemAWS.sleepCycles);

}

void fileDump(File* f){
    while (f->available())
      Serial.print(f->read(), HEX);
    f->seek(0, SeekSet);
}

void setup() {
    Serial.begin(115200);
#ifdef DBG_PROG
//    delay (10);
    Serial.setDebugOutput(true);
    IAS.serialdebug(true,115200);      
#endif



    WiFi.begin();

    rst_info *resetInfo; 
    resetInfo = ESP.getResetInfoPtr();

    AWS_endpoint = new char[strlen_P((AWS_ENDPOINT)) + 1]; //+1 to accomodate for the termination char
    strcpy_P(AWS_endpoint, (AWS_ENDPOINT));

    AWS_content_topic = new char[strlen_P((AWS_CONTENT_TOPIC)) + 1];
    strcpy_P(AWS_content_topic, (AWS_CONTENT_TOPIC));

    AWS_thing_name = new char[strlen_P((AWS_DEFAULT_NAME)) + 4 + 1]; 
    sprintf_P(AWS_thing_name, (AWS_DEFAULT_NAME), ESP.getChipId());  

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
        Serial.println(("Woke up from deep sleep!"));
        IAS.processField();
#ifdef DBG_PROG
        IAS.callHome();      
#endif        
    }
    else {
        DEBUG_LOG_T("Booting...!\n\r");
        DEBUG_LOG_T("Flash real size: %u\n\r", ESP.getFlashChipRealSize());
        DEBUG_LOG_T("Flash IDE size:  %u\n\r", ESP.getFlashChipSize());

        rtcMemAWS.sleepCycles = 0;
        writeRTCMemAWS();
        DEBUG_LOG_T("AWS RTC Mem initialized!\n\r");
      
        pinMode(LED_PIN, OUTPUT);
        digitalWrite(LED_PIN, HIGH);
        IAS.begin(true, 'L');
        digitalWrite(LED_PIN, LOW);
        DEBUG_LOG_T("Waiting to enter in config mode");
        unsigned long t = millis();
        while ((millis() - t) < 3000){ //allow user to ented config mode within 3 secs after power on
            DEBUG_LOG_T(".");
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
        DEBUG_LOG_T("\n\r");
        digitalWrite(LED_PIN, HIGH);
        
        DEBUG_LOG_T("RSSI: %ddB, Critical level set at: %d\n\r", WiFi.RSSI(), RSSI_CRITICAL_LEVEL);

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

    AWS_shadow = new char[strlen_P((AWS_SHADOW)) + strlen(AWS_thing_name) - 2 + 1];
    sprintf_P(AWS_shadow, (AWS_SHADOW), AWS_thing_name);

    // verify all parameters are ok
    DEBUG_LOG_T("Parameters are:\n\r%s\n\r%s\n\r%s\n\r%s\n\r", AWS_thing_name, AWS_endpoint, AWS_shadow, AWS_content_topic);

    mqtt.setServer(AWS_endpoint, 8883);

    DEBUG_LOG_T("Loading credentials for AWS IoT core from SPIFFS...\n\r");

    if (!SPIFFS.begin()) {
        DEBUG_LOG_T("Failed to mount file system!\n\r");
    }
    else {
        DEBUG_LOG_T("SPIFFS content...\n\r");
        Dir dir = SPIFFS.openDir("");
        while (dir.next()) {
            DEBUG_LOG_T("%s %u\n\r", dir.fileName().c_str(), dir.fileSize());
        }        
        DEBUG_LOG_T("Free heap: %u\n\r", ESP.getFreeHeap());

        // Load certificate file
        File cert = SPIFFS.open(CERTIFICATE_FILE, "r"); 
        if (!cert){ 
            DEBUG_LOG_T("Failed to open cert file.\n\r");
        }

        if (cert && espClient.loadCertificate(cert)){ 
            DEBUG_LOG_T("cert loaded!\n\r");
        }
        else{
            DEBUG_LOG_T("Failed to load cert from SPIFFS. Trying to load from flash...");
            if (espClient.setCertificate_P(cert_der, cert_der_len)){
                DEBUG_LOG_T("cert loaded\n\r");
        }
        else{
                DEBUG_LOG_T("cert not loaded\n\r");
            }
        }

        // Load private key file
        File private_key = SPIFFS.open(PRIVATE_KEY_FILE, "r"); 
        if (!private_key){ 
            DEBUG_LOG_T("Failed to open private key file!\n\r");
        }

        if (private_key && espClient.loadPrivateKey(private_key)){
            DEBUG_LOG_T("private key loaded!\n\r"); 
        }
        else{
            DEBUG_LOG_T("Failed to load private key from SPIFFS. Trying to load from flash...");
            if (espClient.setPrivateKey_P(private_der, private_der_len)){
                DEBUG_LOG_T("private key loaded.\n\r");
        }
        else{
                DEBUG_LOG_T("private key not loaded.\n\r");
            }
        }           

        SPIFFS.end();
        DEBUG_LOG_T("Free heap: %u\n\r", ESP.getFreeHeap());
    }
    
    float temp;
    DS18B20.getAddress(DS18B20Address, 0);
    DS18B20.setResolution(DS18B20Address,9);
    DS18B20.requestTemperatures(); 
    temp = DS18B20.getTempCByIndex(0); 
    DEBUG_LOG_T("Temperature: %f\n\r", temp);
    
    String s;
    StaticJsonBuffer<250> jsonBuffer; 
    JsonObject& root = jsonBuffer.createObject();
    root["sensor"] = AWS_thing_name;
    root["temperature"] = temp;
    root.printTo(s);
    mqttConnectAndSend(AWS_content_topic, s.c_str());

    // update AWS shadow service if needed
    if (rtcMemAWS.sleepCycles == 0){
        DEBUG_LOG_T("Time to check for new FW and to update AWS shadow service.\n\r");
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

    writeRTCMemAWS();
    printRTCMemAWS();
    
    Serial.println(("Going to deep sleep..."));

    // Connect GPIO16 to RST to allow ESP to wake up from deepSleep
    ESP.deepSleep(1e6L * 60 * REPORT_INTERVAL); 
    
}

void loop() {
    // put your main code here, to run repeatedly:
}