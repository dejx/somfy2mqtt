#include <Arduino.h>
#include <IotWebConf.h>
#include <IotWebConfUsing.h> // This loads aliases for easier class names.
#include <PubSubClient.h>
#include <Preferences.h>
#define PORT_TX 4

#ifndef IOTWEBCONF_CONFIG_START
# define IOTWEBCONF_CONFIG_START 150
#endif

    #define SIG_HIGH GPIO.out_w1ts = 1 << PORT_TX
    #define SIG_LOW  GPIO.out_w1tc = 1 << PORT_TX
    
const char thingName[] = "somfyrts2mqtt";
const char wifiInitialApPassword[] = "qwerasdf";
#define STRING_LEN 128
#define CONFIG_VERSION "somfy3"

char mqttServerValue[STRING_LEN];
char mqttUserNameValue[STRING_LEN];
char mqttUserPasswordValue[STRING_LEN];
char status_topic[STRING_LEN];
char ack_topic[STRING_LEN];

// Buttons
#define SYMBOL 640
#define HAUT 0x2    
#define STOP 0x1
#define BAS 0x4
#define PROG 0x8
byte frame[7];

DNSServer dnsServer;
WebServer server(80);
IotWebConf iotWebConf(thingName, &dnsServer, &server, wifiInitialApPassword, CONFIG_VERSION);
IotWebConfParameterGroup mqttGroup = IotWebConfParameterGroup("MQTT configuration");
IotWebConfTextParameter mqttServerParam = IotWebConfTextParameter("MQTT server", "mqttServer", mqttServerValue, STRING_LEN);
IotWebConfTextParameter mqttUserNameParam = IotWebConfTextParameter("MQTT user", "mqttUser", mqttUserNameValue, STRING_LEN);
IotWebConfPasswordParameter mqttUserPasswordParam = IotWebConfPasswordParameter("MQTT password", "mqttPass", mqttUserPasswordValue, STRING_LEN);
bool needMqttConnect = false;
bool needReset = false;
const bool reset_rolling_codes = false;
WiFiClient net;
PubSubClient mqtt(net);
Preferences preferences;
struct REMOTE {
    unsigned int id;
    char const* mqtt_topic;
    unsigned int default_rolling_code;
    uint32_t eeprom_address;
};
std::vector<REMOTE> const remotes = {{0x184623, "somfy/1/blinds",                   1,                0 } // spalnica
                                    ,{0x971547, "somfy/2/blinds",                   1,                4 } // otroska
                                    ,{0x187542, "somfy/3/blinds",                   1,                8 } // kabinet
                                    ,{0x214412, "somfy/4/blinds",                   1,               12 } // okno pri klimi
                                    ,{0x242412, "somfy/5/blinds",                   1,               16 } // naslednje okno pri klimi
                                    ,{0x244312, "somfy/6/blinds",                   1,               20 } // vrata v ložo
                                    ,{0x244512, "somfy/7/blinds",                   1,               24 } // okno od lože
                                    ,{0x244612, "somfy/8/blinds",                   1,               28 } // okno pri stolih
                                    ,{0x244472, "somfy/9/blinds",                   1,               32 } // vrata na teraso
                                    ,{0x244418, "somfy/10/blinds",                  1,               36 } // okno pri žaru
                                    ,{0x244492, "somfy/11/blinds",                  1,               40 } // daljinec za vse sobe
                                    ,{0x241012, "somfy/12/blinds",                  1,               44 } // daljinec za vse v dnevni
                                    ,{0x221412, "somfy/13/blinds",                  1,               48 } // dalljinec za vse loža
                                    ,{0x243552, "somfy/14/blinds",                  1,               52 } // daljinec za vse terasa
                                    };
void SendCommand(byte *frame, byte sync) {
    if(sync == 2) { // Only with the first frame.
        //Wake-up pulse & Silence
        SIG_HIGH;
        delayMicroseconds(9415);
        SIG_LOW;
        delayMicroseconds(89565);
    }

    // Hardware sync: two sync for the first frame, seven for the following ones.
    for (int i = 0; i < sync; i++) {
        SIG_HIGH;
        delayMicroseconds(4*SYMBOL);
        SIG_LOW;
        delayMicroseconds(4*SYMBOL);
    }

    // Software sync
    SIG_HIGH;
    delayMicroseconds(4550);
    SIG_LOW;
    delayMicroseconds(SYMBOL);

    //Data: bits are sent one by one, starting with the MSB.
    for(byte i = 0; i < 56; i++) {
        if(((frame[i/8] >> (7 - (i%8))) & 1) == 1) {
            SIG_LOW;
            delayMicroseconds(SYMBOL);
            SIG_HIGH;
            delayMicroseconds(SYMBOL);
        }
        else {
            SIG_HIGH;
            delayMicroseconds(SYMBOL);
            SIG_LOW;
            delayMicroseconds(SYMBOL);
        }
    }

    SIG_LOW;
    delayMicroseconds(30415); // Inter-frame silence
}


void BuildFrame(byte *frame, byte button, REMOTE remote) {
    unsigned int code;

    code = preferences.getUInt( (String(remote.id) + "rolling").c_str(), remote.default_rolling_code);

    frame[0] = 0xA7;            // Encryption key. Doesn't matter much
    frame[1] = button << 4;     // Which button did  you press? The 4 LSB will be the checksum
    frame[2] = code >> 8;       // Rolling code (big endian)
    frame[3] = code;            // Rolling code
    frame[4] = remote.id >> 16; // Remote address
    frame[5] = remote.id >>  8; // Remote address
    frame[6] = remote.id;       // Remote address

    Serial.print("Frame         : ");
    for(byte i = 0; i < 7; i++) {
        if(frame[i] >> 4 == 0) { //  Displays leading zero in case the most significant nibble is a 0.
            Serial.print("0");
        }
        Serial.print(frame[i],HEX); Serial.print(" ");
    }

    // Checksum calculation: a XOR of all the nibbles
    byte checksum = 0;
    for(byte i = 0; i < 7; i++) {
        checksum = checksum ^ frame[i] ^ (frame[i] >> 4);
    }
    checksum &= 0b1111; // We keep the last 4 bits only


    // Checksum integration
    frame[1] |= checksum; //  If a XOR of all the nibbles is equal to 0, the blinds will consider the checksum ok.

    Serial.println(""); Serial.print("With checksum : ");
    for(byte i = 0; i < 7; i++) {
        if(frame[i] >> 4 == 0) {
            Serial.print("0");
        }
        Serial.print(frame[i],HEX); Serial.print(" ");
    }


    // Obfuscation: a XOR of all the bytes
    for(byte i = 1; i < 7; i++) {
        frame[i] ^= frame[i-1];
    }

    Serial.println(""); Serial.print("Obfuscated    : ");
    for(byte i = 0; i < 7; i++) {
        if(frame[i] >> 4 == 0) {
            Serial.print("0");
        }
        Serial.print(frame[i],HEX); Serial.print(" ");
    }
    Serial.println("");
    Serial.print("Rolling Code  : ");
    Serial.println(code);

    
    preferences.putUInt( (String(remote.id) + "rolling").c_str(), code + 1); // Increment and store the rolling code

}

void receivedCallback(char* topic, byte* payload, unsigned int length) {
    char command = *payload; // 1st byte of payload
    bool commandIsValid = false;
    REMOTE currentRemote;

    Serial.print("MQTT message received: ");
    Serial.println(topic);

    Serial.print("Payload: ");
    for (int i = 0; i < length; i++) {
        Serial.print((char)payload[i]);
    }
    Serial.println();

    // Command is valid if the payload contains one of the chars below AND the topic corresponds to one of the remotes
    if ( length == 1 && ( command == 'u' || command == 's' || command == 'd' || command == 'p' ) ) {
        for ( REMOTE remote : remotes ) {
            if ( strcmp(remote.mqtt_topic, topic) == 0 ){
                currentRemote = remote;
                commandIsValid = true;
            }
        }
    }

    if ( commandIsValid ) {
        if ( command == 'u' ) {
            Serial.println("Monte"); // Somfy is a French company, after all.
            BuildFrame(frame, HAUT, currentRemote);
        }
        else if ( command == 's' ) {
            Serial.println("Stop");
            BuildFrame(frame, STOP, currentRemote);
        }
        else if ( command == 'd' ) {
            Serial.println("Descend");
            BuildFrame(frame, BAS, currentRemote);
        }
        else if ( command == 'p' ) {
            Serial.println("Prog");
            BuildFrame(frame, PROG, currentRemote);
        }

        Serial.println("");

        SendCommand(frame, 2);
        for ( int i = 0; i<2; i++ ) {
            SendCommand(frame, 7);
        }

        // Send the MQTT ack message
        String ackString = "id: 0x";
        ackString.concat( String(currentRemote.id, HEX) );
        ackString.concat(", cmd: ");
        ackString.concat(command);
        mqtt.publish(ack_topic, ackString.c_str());
    }
}


void wifiConnected()
{
  
    // Configure MQTT
    mqtt.setServer(mqttServerValue, 1883);
    mqtt.setCallback(receivedCallback);
  needMqttConnect = true;
}
void configSaved()
{
  needReset = true;
}


void handleRoot()
{
  // -- Let IotWebConf test and handle captive portal requests.
  if (iotWebConf.handleCaptivePortal())
  {
    // -- Captive portal request were already served.
    return;
  }
  String s = "<!DOCTYPE html><html lang=\"en\"><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1, user-scalable=no\"/>";
  s += "<title>IotWebConf 06 MQTT App</title></head><body>Somfy to mqtt";
  s += "<ul>";
  s += "<li>MQTT server: ";
  s += mqttServerValue;
  s += "</ul>";
  s += "Go to <a href='config'>configure page</a> to change values.";
  s += "</body></html>\n";

  server.send(200, "text/html", s);
}

bool mqttconnect() {
    // Loop until reconnected
    while ( !mqtt.connected() ) {
        Serial.print("MQTT connecting ...");

        // Connect to MQTT, with retained last will message "offline"
        if (mqtt.connect(thingName, mqttUserNameValue, mqttUserPasswordValue,  "smartHome/somfy-remote/status", 1, 1, "offline")) {
            Serial.println("connected");

            // Subscribe to the topic of each remote with QoS 1
            for ( REMOTE remote : remotes ) {
                mqtt.subscribe(remote.mqtt_topic, 1);
                Serial.print("Subscribed to topic: ");
                Serial.println(remote.mqtt_topic);
            }

            // Update status, message is retained
            mqtt.publish(status_topic, "online", true);
        }
        else {
            Serial.print("failed, status code =");
            Serial.print(mqtt.state());
            Serial.println("try again in 5 seconds");
            // Wait 5 seconds before retrying
            return false;
        }
    }
    return true;
}

void setup() {
  Serial.begin(9800);

  // Output to 433.42MHz transmitter
  pinMode(PORT_TX, OUTPUT);
  SIG_LOW;
  preferences.begin("somfy-remote",false);
  mqttGroup.addItem(&mqttServerParam);
  mqttGroup.addItem(&mqttUserNameParam);
  mqttGroup.addItem(&mqttUserPasswordParam);


  iotWebConf.addParameterGroup(&mqttGroup);
  iotWebConf.setConfigSavedCallback(&configSaved);

  iotWebConf.setWifiConnectionCallback(&wifiConnected);

  bool validConfig = iotWebConf.init();
  if (!validConfig)
  {
    mqttServerValue[0] = '\0';
    mqttUserNameValue[0] = '\0';
    mqttUserPasswordValue[0] = '\0';
  }
    // -- Set up required URL handlers on the web server.
  server.on("/", handleRoot);
  server.on("/config", []{ iotWebConf.handleConfig(); });
  server.onNotFound([](){ iotWebConf.handleNotFound(); });

  

  strcpy(status_topic,thingName);
  strcat(status_topic,"/status"); // Online / offline
  strcpy(ack_topic,thingName);
  strcat(ack_topic,"/ack");// Commands ack "id: 0x184623, cmd: u"

  Serial.println(status_topic);
  Serial.println(ack_topic);
}

void loop() {
  iotWebConf.doLoop();


  if (needMqttConnect)
  {
    if (mqttconnect())
    {
      needMqttConnect = false;
    }
  }
  else if ((iotWebConf.getState() == IOTWEBCONF_STATE_ONLINE) && (!mqtt.connected()))
  {
    Serial.println("MQTT reconnect");
    mqttconnect();
  }

 mqtt.loop();
  delay(100);
}