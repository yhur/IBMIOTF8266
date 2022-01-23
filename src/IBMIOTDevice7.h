// IBM IOT Device
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <PubSubClient.h>
#include <ESP8266httpUpdate.h>
#include <iotWIFIDevice.h>

const char*         publishTopic  = "iot-2/evt/status/fmt/json";
const char*         infoTopic     = "iot-2/evt/info/fmt/json";
const char*         commandTopic  = "iot-2/cmd/+/fmt/+";
const char*         responseTopic = "iotdm-1/response";
const char*         manageTopic   = "iotdevice-1/mgmt/manage";
const char*         updateTopic   = "iotdm-1/device/update";
const char*         rebootTopic   = "iotdm-1/mgmt/initiate/device/reboot";
const char*         resetTopic    = "iotdm-1/mgmt/initiate/device/factory_reset";


ESP8266WebServer    server(80);
WiFiClientSecure    espClient;
PubSubClient        client(espClient);
char                iot_server[100];
char                msgBuffer[JSON_BUFFER_LENGTH];
int                 cmdBaseLen = 10;
unsigned long       pubInterval;

char                fpFile[] = "/fingerprint.txt";
String              fingerprint = "B3 B7 C3 0D 9D 32 E6 A2 8A FC FD BA 11 BB 05 5E E1 D9 9E F7";

unsigned long       lastWiFiConnect;

bool subscribeTopic(const char* topic) {
    if (client.subscribe(topic)) {
        Serial.printf("Subscription to %s OK\n", topic);
        return true;
    } else {
        Serial.printf("Subscription to %s Failed\n", topic);
        return false;
    }
}

void initDevice() {
    iotInitDevice();
    if (LittleFS.exists(fpFile)) {
        File f = LittleFS.open(fpFile, "r");
        fingerprint = f.readString();
        fingerprint.trim();
        f.close();
    }
    espClient.setFingerprint(fingerprint.c_str());
}

void iot_connect() {

    while (!client.connected()) {
        sprintf(msgBuffer,"d:%s:%s:%s", (const char*)cfg["org"], (const char*)cfg["devType"], (const char*)cfg["devId"]);
        if (client.connect(msgBuffer,"use-token-auth",cfg["token"])) {
            Serial.println("MQ connected");
        } else {
            if( digitalRead(RESET_PIN) == 0 ) {
                reboot();
            }
            if(WiFi.status() == WL_CONNECTED) {
                lastWiFiConnect = millis();
                Serial.printf("MQ Connection fail RC = %d, try again in 5 seconds\n", client.state());
                delay(5000);
            } else {
                Serial.println("Reconnecting to WiFi");
                WiFi.begin();
                while (WiFi.status() != WL_CONNECTED) {
                    if(lastWiFiConnect + 3600000 < millis()) {
                        reboot();
                    } else {
                        delay(5000);
                        Serial.print("*");
                    }
                }
            }
        }
    }
    if (!subscribeTopic(responseTopic)) return;
    if (!subscribeTopic(rebootTopic)) return;
    if (!subscribeTopic(resetTopic)) return;
    if (!subscribeTopic(updateTopic)) return;
    if (!subscribeTopic(commandTopic)) return;
    JsonObject meta = cfg["meta"];
    StaticJsonDocument<512> root;
    JsonObject d = root.createNestedObject("d");
    JsonObject metadata = d.createNestedObject("metadata");
    for (JsonObject::iterator it=meta.begin(); it!=meta.end(); ++it) {
        metadata[it->key().c_str()] = it->value();
    }
    JsonObject supports = d.createNestedObject("supports");
    supports["deviceActions"] = true;
    serializeJson(root, msgBuffer);
    Serial.printf("publishing device metadata: %s\n", msgBuffer);
    if (client.publish(manageTopic, msgBuffer)) {
        serializeJson(d, msgBuffer);
        String info = String("{\"info\":") + String(msgBuffer) + String("}");
        client.publish(infoTopic, info.c_str());
    }
}

void publishError(char *msg) {
    String payload = "{\"info\":{\"error\":";
    payload += "\"" + String(msg) + "\"}}";
    client.publish(infoTopic, (char*) payload.c_str());
    Serial.println(payload);
}

void update_progress(int cur, int total) {
    Serial.printf("CALLBACK:  HTTP update process at %d of %d bytes...\n", cur, total);
}

void update_error(int err) {
    Serial.printf("CALLBACK:  HTTP update fatal error code %d\n", err);
}

void handleIOTCommand(char* topic, JsonDocument* root) {
    JsonObject d = (*root)["d"];

    if (!strcmp(responseTopic, topic)) {        // strcmp return 0 if both string matches
        return;                                 // just print of response for now
    } else if (!strcmp(rebootTopic, topic)) {   // rebooting
        reboot();
    } else if (!strcmp(resetTopic, topic)) {    // clear the configuration and reboot
        reset_config();
        ESP.restart();
    } else if (!strcmp(updateTopic, topic)) {
        JsonArray fields = d["fields"];
        for(JsonArray::iterator it=fields.begin(); it!=fields.end(); ++it) {
            DynamicJsonDocument field = *it;
            const char* fieldName = field["field"];
            if (strcmp (fieldName, "metadata") == 0) {
                JsonObject fieldValue = field["value"];
                cfg.remove("meta");
                JsonObject meta = cfg.createNestedObject("meta");
                for (JsonObject::iterator fv=fieldValue.begin(); fv!=fieldValue.end(); ++fv) {
                    meta[(char*)fv->key().c_str()] = fv->value();
                }
                save_config_json();
            }
        }
        pubInterval = cfg["meta"]["pubInterval"];
    } else if (!strncmp(commandTopic, topic, cmdBaseLen)) {
        if (d.containsKey("upgrade")) {
            JsonObject upgrade = d["upgrade"];
            String response = "{\"OTA\":{\"status\":";
            if(upgrade.containsKey("server") && 
                        upgrade.containsKey("port") && 
                        upgrade.containsKey("uri")) {
		        Serial.println("firmware upgrading");
	            const char *fw_server = upgrade["server"];
	            int fw_server_port = atoi(upgrade["port"]);
	            const char *fw_uri = upgrade["uri"];
                ESPhttpUpdate.onProgress(update_progress);
                ESPhttpUpdate.onError(update_error);
                client.publish(infoTopic,"{\"info\":{\"upgrade\":\"Device will be upgraded.\"}}" );
	            t_httpUpdate_return ret = ESPhttpUpdate.update(espClient, fw_server, fw_server_port, fw_uri);
	            switch(ret) {
		            case HTTP_UPDATE_FAILED:
                        response += "\"[update] Update failed. http://" + String(fw_server);
                        response += ":"+ String(fw_server_port) + String(fw_uri) +"\"}}";
                        client.publish(infoTopic, (char*) response.c_str());
                        Serial.println(response);
		                break;
		            case HTTP_UPDATE_NO_UPDATES:
                        response += "\"[update] Update no Update.\"}}";
                        client.publish(infoTopic, (char*) response.c_str());
                        Serial.println(response);
		                break;
		            case HTTP_UPDATE_OK:
		                Serial.println("[update] Update ok."); // may not called we reboot the ESP
		                break;
	            }
            } else {
                response += "\"OTA Information Error\"}}";
                client.publish(infoTopic, (char*) response.c_str());
                Serial.println(response);
            }
        } else if (d.containsKey("config")) {
            char maskBuffer[JSON_BUFFER_LENGTH];
            maskConfig(maskBuffer);
            String info = String("{\"config\":") + String(maskBuffer) + String("}");
            client.publish(infoTopic, info.c_str());
        }
    }
}
/* FW Upgrade informaiton 
 * var evt1 = { 'd': { 
 *   'upgrade' : {
 *       'server':'192.168.0.9',
 *       'port':'3000',
 *       'uri' : '/file/IOTPurifier4GW.ino.nodemcu.bin'
 *       }
 *   }
 * };
*/
