#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "analogWrite.h"
#include <Arduino_JSON.h>
#include <time.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <base64.h>

char PADDING = '#';
String key = "prapvedmpstuubha";

TaskHandle_t Task1;
TaskHandle_t Task2;
QueueHandle_t xQueue_sensor;
QueueHandle_t xQueue_target;

struct SensorData
{
    int timestamp[100];
    float pos[100];
    int count;
    int target;
    String rn;
};

struct QueueData
{
    int target;
    String rn;
};

int IN1 = 18;
int IN2 = 23;
int PWM = 2;
int ENCA = 32;
int ENCB = 34;

volatile int posi = 0;
long prevT = 0;
float eprev = 0;
float eintegral = 0;

const char* ssid = "pratyanshu-Inspiron-5570";
const char* password = "hope1985";

String get_target_url = "http://esw-onem2m.iiit.ac.in:443/~/in-cse/in-name/Team-4/Node-1/queue_data?rcn=4";
String post_sensordata_url = "http://esw-onem2m.iiit.ac.in:443/~/in-cse/in-name/Team-4/Node-1/sensor_data";

uint wifi_delay = 5000;
uint lastTime = 0;
#define PID_TIMER 10000

HTTPClient http;


void setup() {
    // put your setup code here, to run once:
    Serial.begin(9600);

    pinMode(ENCA,INPUT);
    pinMode(ENCB,INPUT);
    attachInterrupt(digitalPinToInterrupt(ENCA),readEncoder,RISING);

    analogWriteResolution(PWM, 8);
    pinMode(IN1,OUTPUT);
    pinMode(IN2,OUTPUT);

    WiFi.begin(ssid, password);
    Serial.println("Connecting");
    while(WiFi.status() != WL_CONNECTED)
        delay(500);
    Serial.print("\nConnected to WiFi network with IP Address: ");
    Serial.println(WiFi.localIP());


    xQueue_sensor = xQueueCreate( 5, sizeof( struct SensorData ) );
    xQueue_target = xQueueCreate( 5, sizeof( struct QueueData ));


    xTaskCreatePinnedToCore(Task1code,"Task1",40000,NULL,5,&Task1,0);                         
    delay(1000); 

}

void Task1code( void * parameter ){
    Serial.print("Task1 is running on core ");
    Serial.println(xPortGetCoreID());

    String tq_rn[5] = {"", "", "", "", ""};
    unsigned char decoded[1000];
    
    for(;;){
        if(WiFi.status()== WL_CONNECTED)
        {

            // Your Domain name with URL path or IP address with path
            http.begin(get_target_url.c_str());

            http.addHeader("X-M2M-Origin", "KK382AITgR:WPN0JcTeJd");
            http.addHeader("Accept", "application/json");

            // Send HTTP GET request
            int httpResponseCode = http.GET();

            if (httpResponseCode>0) 
            {
                Serial.print("HTTP Response code: ");
                Serial.println(httpResponseCode);
                String payload = http.getString();

                JSONVar myObject = JSON.parse(payload);
                if (JSON.typeof(myObject) == "undefined") 
                {
                    Serial.println("Parsing input failed!");

                }
                else
                {

                    Serial.print("JSON.typeof(myObject) = ");
                    Serial.println(JSON.typeof(myObject)); // prints: object

                    int len = (int)myObject["m2m:cnt"]["cni"];
                    for(int i = 0; i < len; i++)
                    {
                        String rn = (const char*)myObject["m2m:cnt"]["m2m:cin"][i]["rn"];
                        int j = 0;
                        for(j = 0; j < 5; j++)
                            if(rn == tq_rn[j])
                                break;
                        if(j == 5)
                        {

                            String data_string = (const char*)myObject["m2m:cnt"]["m2m:cin"][i]["con"];
                            Serial.println(data_string);
                            String hash = "";
                            String target_string = "";
                            int len = data_string.length();
                            if (len < 66)
                              continue;
                            for(int k = 0; k < len; k++)
                              if(k < 64)
                                hash += data_string[k];
                              else if(k > 64)
                                target_string += data_string[k];

                            if(hash != calc_sha256(target_string))
                            {
                              Serial.println("Corrupted Data");
                              continue;
                            }
               
                            size_t outputLength;
                            mbedtls_base64_decode(decoded, 1000, &outputLength, (const unsigned char *)target_string.c_str(), target_string.length());
                            
                            String tmp = "";
                            for(int k = 0; k < outputLength; k++)
                                tmp += (char)decoded[k];
                            target_string = decrypt(tmp, key);
                            int target = target_string.toInt();
                            struct QueueData qd;
                            qd.target = target;
                            qd.rn = rn;
                            
                            xQueueSend(xQueue_target, &qd, portMAX_DELAY);


                            struct SensorData sensor_data;
                            while(true)
                            {
                                if (xQueueReceive( xQueue_sensor, &sensor_data, portMAX_DELAY ) == pdPASS)
                                {
                                    Serial.print( "Received = ");
                                    Serial.println(sensor_data.count);
                                    if(sensor_data.count == -1)
                                        break;
                                    else
                                        sendSensorData(sensor_data);
                                }
                            }
                        }
                    }

                    for(int k = 0; k < len; k++)
                        tq_rn[k] = (const char*)myObject["m2m:cnt"]["m2m:cin"][k]["rn"];

                }        
            }
            else {
                Serial.print("Error code: ");
                Serial.println(httpResponseCode);
            }
            // Free resources
            http.end();
        }
        else {
            Serial.println("WiFi Disconnected");
        }
        lastTime = millis();
    }
}

void loop(){
    struct QueueData qd;
    if (xQueueReceive( xQueue_target, &qd, portMAX_DELAY ) == pdPASS)
    {
//        Serial.print( "Received = ");
//        Serial.println(target);
        

        PID_control(qd);
        setMotor(0,0,PWM,IN1,IN2); 
    }
}

void PID_control(struct QueueData qd)
{
    
    uint startTime = millis();
    uint lastTime = millis();
    struct SensorData sensor_data;
    sensor_data.rn = qd.rn;
    sensor_data.count = 0;
    sensor_data.target = qd.target;
    int target = qd.target * 46.0 * 11.0 / 360.0;
    while(millis()- startTime < PID_TIMER){

        // PID constants
        float kp = 6;
        float kd = 0.025;
        float ki = 0.5;

        // time difference
        long currT = micros();
        float deltaT = ((float) (currT - prevT))/( 1.0e6 );
        prevT = currT;

        // Read the positionL
        int pos = 0; 
        noInterrupts(); // disable interrupts temporarily while reading
        pos = posi;
        interrupts(); // turn interrupts back on

        // error
        float e = target - pos;

        // derivative
        float dedt = (e-eprev)/(deltaT);

        // integral
        if(dedt == 0)
            eintegral = eintegral + e*deltaT;
        if(eintegral > 255)
            eintegral = 255;
        if (eintegral < -255)
            eintegral = -255;

        // control signal
        float u = kp*e + kd*dedt + ki*eintegral;

        // motor power
        float pwr = fabs(u);
        if( pwr > 255 ){
            pwr = 255;
        }

        // motor direction
        int dir = 1;
        if(u<0){
            dir = -1;
        }

        // signal the motor
        setMotor(dir,pwr,PWM,IN1,IN2);


        // store previous error
        eprev = e;

        Serial.print(target * 0.71146245059);
        Serial.print(" ");
        Serial.print(pos * 0.71146245059);
        sensor_data.pos[sensor_data.count] = pos * 0.71146245059;
        sensor_data.timestamp[sensor_data.count] = millis()- startTime;
        sensor_data.count += 1;
        //  sensor_data += (String)(millis()- startTime) + ":" + (String)(pos * 0.71146245059) + ",";
        //  Serial.print(" ");
        //  Serial.print(e);
        //  Serial.print(" ");
        //  Serial.print(dedt);
        //  Serial.print(" ");
        //  Serial.print(eintegral);
        //  Serial.print(" ");
        //  Serial.print(u);
        Serial.println();

        if(millis() - lastTime > 500 || sensor_data.count >= 100)
        {
            lastTime = millis();
            //      setMotor(0,0,PWM,IN1,IN2);
            //      sendSensorData(sensor_data);
            Serial.println("Sending sensor data");
            xQueueSend( xQueue_sensor, &sensor_data, portMAX_DELAY );
            sensor_data.count = 0;
        }
    }
    xQueueSend( xQueue_sensor, &sensor_data, portMAX_DELAY );
    sensor_data.count = -1;
    xQueueSend( xQueue_sensor, &sensor_data, portMAX_DELAY );
}

void setMotor(int dir, int pwmVal, int pwm, int in1, int in2){
    if(dir == 1){
        digitalWrite(in1,HIGH);
        digitalWrite(in2,LOW);
    }
    else if(dir == -1){
        digitalWrite(in1,LOW);
        digitalWrite(in2,HIGH);
    }
    else{
        digitalWrite(in1,LOW);
        digitalWrite(in2,LOW);
    }  
    analogWrite(pwm,pwmVal);
}

void readEncoder(){
    int b = digitalRead(ENCB);
    if(b > 0){
        posi++;
    }
    else{
        posi--;
    }
}

void sendSensorData(struct SensorData sensor_data_st)
{
    if(sensor_data_st.count <= 0)
      return;
      
    Serial.println(millis());
    String sensor_data = sensor_data_st.rn + ":" + (String)sensor_data_st.target;
    for(int i = 0; i < sensor_data_st.count; i++)
        sensor_data += "," + (String)sensor_data_st.timestamp[i] + ":" + (String)sensor_data_st.pos[i];

    http.begin(post_sensordata_url.c_str());

    // Your Domain name with URL path or IP address with path

    Serial.println(millis());

    http.addHeader("X-M2M-Origin", "KK382AITgR:WPN0JcTeJd");
    http.addHeader("Content-Type", "application/json;ty=4");

    String enc = base64::encode(encrypt(sensor_data, key));
    String hashed = calc_sha256(enc) + "|" + enc;
    
    String ciRepresentation =
        "{\"m2m:cin\": {"
        "\"con\":\"" + hashed + "\""
        "}}";
    
    int httpResponseCode = http.POST(ciRepresentation);
    Serial.print("HTTP Response code: ");
    Serial.println(httpResponseCode);
//    Serial.println(ciRepresentation);
    Serial.println(millis());

    // Free resources
    http.end();
    Serial.println(millis());
}


String encrypt(String text, String key)
{
  String res = "";
  int text_len = text.length(), key_len = key.length(), i = 0, j = 0;
  int padding = (key_len - (text_len % key_len))%key_len;
  for(i = 0; i < padding; i++)
    text += PADDING;
  text_len = text.length();

  i = 0;
  while(i < text_len)
  {
    res += (char)(text[i] ^ key[j]);
    i++;
    j = (j + 1) % key_len;
  }
  return res;
}

String decrypt(String text, String key)
{
  String res = "";
  int text_len = text.length(), key_len = key.length(), i = 0, j = 0;
  
  while(i < text_len)
  {
    text[i] ^= key[j];
    if(text[i] == PADDING)
      break;
    else
      res += text[i];
    i++;
    j = (j + 1) % key_len;
  }
  return res;
}

String calc_sha256(String text)
{
  byte shaResult[32];
  
  mbedtls_md_context_t ctx;
  mbedtls_md_type_t md_type = MBEDTLS_MD_SHA256;
  
  const size_t payloadLength = text.length();         
  
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(md_type), 0);
  mbedtls_md_starts(&ctx);
  mbedtls_md_update(&ctx, (const unsigned char *) text.c_str(), payloadLength);
  mbedtls_md_finish(&ctx, shaResult);
  mbedtls_md_free(&ctx); 

  String result = "";
  for(int i= 0; i< sizeof(shaResult); i++){
      char str[3];
      sprintf(str, "%02x", (int)shaResult[i]);
      result += str;
  }
  return result;
}
