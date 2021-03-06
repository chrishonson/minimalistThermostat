//The MIT License (MIT)
//Copyright (c) 2016 Gustavo Gonnet
//
//Permission is hereby granted, free of charge, to any person obtaining a copy of this software
// and associated documentation files (the "Software"), to deal in the Software without restriction,
// including without limitation the rights to use, copy, modify, merge, publish, distribute,
// sublicense, and/or sell copies of the Software, and to permit persons to whom the Software
// is furnished to do so, subject to the following conditions:
//
//The above copyright notice and this permission notice shall be included in all copies
// or substantial portions of the Software.
//
//THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
// INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
// PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
// WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
// OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
// github: https://github.com/gusgonnet/minimalistThermostat
// hackster: https://www.hackster.io/gusgonnet/the-minimalist-thermostat-bb0410

#include "application.h"
#include "blynkAuthToken.h"
// This #include statement was automatically added by the Particle IDE.
#include "SparkJson/SparkJson.h"

// This #include statement was automatically added by the Particle IDE.
#include "MQTT/MQTT.h"

// This #include statement was automatically added by the Particle IDE.
#include "blynk/blynk.h"

// This #include statement was automatically added by the Particle IDE.
#include "FiniteStateMachine/FiniteStateMachine.h"

// This #include statement was automatically added by the Particle IDE.
#include "PietteTech_DHT/PietteTech_DHT.h"

// This #include statement was automatically added by the Particle IDE.
#include "elapsedMillis/elapsedMillis.h"

#define APP_NAME "Thermostat"
String VERSION = "Version 0.25";
/*******************************************************************************
 * changes in version 0.09:
       * reorganized code to group functions
       * added minimum time to protect on-off on the fan and the heating element
          in function heatingUpdateFunction()
 * changes in version 0.10:
       * added temperatureCalibration to fix DHT measurements with existing thermostat
       * reduced END_OF_CYCLE_TIMEOUT to one sec since my HVAC controller
          takes care of running the fan for a minute to evacuate the heat/cool air
          from the vents
       * added pushbullet notifications for heating on/off
       * added fan on/off setting via a cloud function
 * changes in version 0.11:
      * added more pushbullet notifications and commented out publish() in other cases
 * changes in version 0.12:
           * added blynk support
           * added minimumIdleTimer, to protect fan and heating/cooling elements
              from glitches
 * changes in version 0.13:
           * removing endOfCycleState since my HVAC does not need it
           * adding time in notifications
 * changes in version 0.14:
           * debouncing target temp and fan status
 * changes in version 0.15:
           * taking few samples and averaging the temperature to improve stability
 * changes in version 0.16:
           * discarding samples below 0 celsius for those times when the reading of
              the temperature sensor goes wrong
           * adding date/time in notifications
           * leave only 2 decimals in temp notifications (19.00 instead of 19.000000)
           * improving blynk project
           * fine tunning the testing mode
           * adding Titan test scripts
              more info here: https://www.hackster.io/gusgonnet/how-to-test-your-projects-with-titan-a633f2
 * changes in version 0.17:
           * PUSHBULLET_NOTIF renamed to PUSHBULLET_NOTIF_PERSONAL
           * removed yyyy-mm-dd from notifications and left only hh:mm:ss
           * minor changes in temp/humidity reported with Particle.publish()
           * reporting homeMinTemp when desired temp is reached
 * changes in version 0.18:
           * created a pulse of heating for warming up the house a bit
           * created a function that sets the fan on and contains this code below in function myDigitalWrite()
              if (USE_BLYNK == "yes") {
           * updated the blynk app
           * created a mode variable (heating, cooling, off)
 * changes in version 0.19:
           * created blynk defined variables (for instance: BLYNK_LED_FAN)
           * updates in the fan control, making UI more responsive to user changes
           * add cooling support
           * pulses now are able to cool the house
 * changes in version 0.20:
           * create a state variable (idle, heating, cooling, off, fan on)
           * trying to fix bug where in init both cool and heat leads remain on until idle state is triggered
           * set the debounce timer to double the default for mode changes
 * changes in version 0.21:
           * store settings in eeprom
           * changing defaults to HVAC OFF (from HEATING)
           * updating the blynk cloud periodically
           * added in the blynk app the state of the thermostat (also published in the particle cloud)
           * here is the link for cloning the blynk app http://tinyurl.com/zq9lcef
           * changed all eeprom values to uint8_t to save space and bytes written
             this saves eeprom pages to be written more often than needed
             (for instance a float takes 4 bytes and an uint8_t takes only 1)
 * changes in version 0.22:
           * fixed an issue with homeMinTempString, when rebooting the photon would not show the
             temperature loaded from the eeprom
 * changes in version 0.23:
           * Swapped pushbullet notifications with google sheets on thermostat activity
                 source: https://www.hackster.io/gusgonnet/pushing-data-to-google-docs-02f9c4
 * changes in version 0.24:
           * Reverting to Heating/Cooling from Winter/Summer modes

TODO:
  * add multi thread support for photon: SYSTEM_THREAD(ENABLED);
              source for discussion: https://community.particle.io/t/the-minimalist-thermostat/19436
              source for docs: https://docs.particle.io/reference/firmware/photon/#system-thread
  * set max time for heating or cooling in 5 hours (alarm) or 6 hours (auto-shut-off)
  * #define STATE_FAN_ON "Fan On" -> the fan status should show up in the status
  * refloat BLYNK_CONNECTED()?
  * the fan goes off few seconds after the cooling is on

*******************************************************************************/

#define PUSHBULLET_NOTIF_HOME "pushbulletHOME"         //-> family group in pushbullet
#define PUSHBULLET_NOTIF_PERSONAL "pushbulletPERSONAL" //-> only my phone
const int TIME_ZONE = -6;

/*******************************************************************************
 initialize FSM states with proper enter, update and exit functions
*******************************************************************************/
State initState = State( initEnterFunction, initUpdateFunction, initExitFunction );
State idleState = State( idleEnterFunction, idleUpdateFunction, idleExitFunction );
State heatingState = State( heatingEnterFunction, heatingUpdateFunction, heatingExitFunction );
State pulseState = State( pulseEnterFunction, pulseUpdateFunction, pulseExitFunction );
State coolingState = State( coolingEnterFunction, coolingUpdateFunction, coolingExitFunction );

//initialize state machine, start in state: Idle
FSM thermostatStateMachine = FSM(initState);

/*******************************************************************************
 IO mapping
*******************************************************************************/
// D0 : relay: fan
// D1 : relay: heat
// D2 : relay: cool
// D4 : DHT22
// D3, D5~D7 : unused
// A0~A7 : unused
int fan = D1;
int heat = D2;
int cool = D3;
//TESTING_HACK
int fanOutput;
int heatOutput;
int coolOutput;

/*******************************************************************************
 thermostat related declarations
*******************************************************************************/
//temperature related variables - internal
float currentTemp = 20.0;
float currentHumidity = 0.0;
String currentTempString = String(currentTemp); //String to store the sensor's temp so it can be exposed
String currentHumidityString = String(currentHumidity); //String to store the sensor's humidity so it can be exposed

float newHomeMinTemp = 19.0;
float homeMinTemp = 19.0;
String homeMinTempString = String(homeMinTemp); 

float newAwayMinTemp = 19.0;
float awayMinTemp = 19.0;
String awayMinTempString = String(awayMinTemp); 

float newHomeMaxTemp = 19.0;
float homeMaxTemp = 19.0;
String homeMaxTempString = String(homeMaxTemp); 

float newAwayMaxTemp = 19.0;
float awayMaxTemp = 19.0;
String awayMaxTempString = String(awayMaxTemp); 

//here are the possible modes the thermostat can be in: off/heat/cool
#define MODE_OFF "Off"
#define MODE_HEAT "Heating"
#define MODE_COOL "Cooling"
String externalMode = MODE_OFF;
String internalMode = MODE_OFF;
bool modeButtonClick = false;
elapsedMillis modeButtonClickTimer;

//here are the possible states of the thermostat
#define STATE_INIT "Initializing"
#define STATE_IDLE "Idle"
#define STATE_HEATING "Heating"
#define STATE_COOLING "Cooling"
#define STATE_FAN_ON "Fan On"
#define STATE_OFF "Off"
#define STATE_PULSE_HEAT "Pulse Heat"
#define STATE_PULSE_COOL "Pulse Cool"
String state = STATE_INIT;

//TESTING_HACK
// this allows me to system test the project
bool testing = false;

/*******************************************************************************
 Here you decide if you want to use Blynk or not
 Your blynk token goes in another file to avoid sharing it by mistake
  (like I did in one of my commits some time ago)
 The file containing your blynk auth token has to be named blynkAuthToken.h and it should contain
 something like this:
  #define BLYNK_AUTH_TOKEN "1234567890123456789012345678901234567890"
 replace with your project auth token (the blynk app will give you one)
*******************************************************************************/
#define USE_BLYNK "yes"
char auth[] = BLYNK_AUTH_TOKEN;

//definitions for the blynk interface
#define BLYNK_DISPLAY_CURRENT_TEMP V0
#define BLYNK_DISPLAY_HUMIDITY V1
#define BLYNK_DISPLAY_HOME_MIN_TEMP V2
#define BLYNK_DISPLAY_HOME_MAX_TEMP V16
#define BLYNK_DISPLAY_AWAY_MIN_TEMP V15
#define BLYNK_DISPLAY_AWAY_MAX_TEMP V19

#define BLYNK_SLIDER_HOME_MIN_TEMP V10
#define BLYNK_SLIDER_HOME_MAX_TEMP V17

#define BLYNK_SLIDER_AWAY_MIN_TEMP V14
#define BLYNK_SLIDER_AWAY_MAX_TEMP V20

#define BLYNK_BUTTON_FAN V11
#define BLYNK_LED_FAN V3
#define BLYNK_LED_HEAT V4
#define BLYNK_LED_COOL V5

#define BLYNK_DISPLAY_MODE V7
#define BLYNK_BUTTON_MODE V8
#define BLYNK_LED_PULSE V6
#define BLYNK_BUTTON_PULSE V12
#define BLYNK_DISPLAY_STATE V13
#define BLYNK_DISPLAY_USER_POSITION V18

//this is the remote temperature sensor
#define BLYNK_DISPLAY_CURRENT_TEMP_UPSTAIRS V9

//this defines how often the readings are sent to the blynk cloud (millisecs)
#define BLYNK_STORE_INTERVAL 5000
elapsedMillis blynkStoreInterval;

WidgetLED fanLed(BLYNK_LED_FAN); //register led to virtual pin 3
WidgetLED heatLed(BLYNK_LED_HEAT); //register led to virtual pin 4
WidgetLED coolLed(BLYNK_LED_COOL); //register led to virtual pin 5
WidgetLED pulseLed(BLYNK_LED_PULSE); //register led to virtual pin 6

//enable the user code (our program below) to run in parallel with cloud connectivity code
// source: https://docs.particle.io/reference/firmware/photon/#system-thread
SYSTEM_THREAD(ENABLED);


void callback(char* topic, byte* payload, unsigned int length);
/**
 * if want to use IP address,
 * byte server[] = { XXX,XXX,XXX,XXX };
 * MQTT client(server, 1883, callback);
 * want to use domain name,
 * MQTT client("www.sample.com", 1883, callback);
 **/
 MQTT client("m13.cloudmqtt.com", 16619, callback);
/*******************************************************************************
 * Function Name  : setup
 * Description    : this function runs once at system boot
 *******************************************************************************/
void setup() {

  //publish startup message with firmware version
  Particle.publish(APP_NAME, VERSION, 60, PRIVATE);

  //declare and init pins
  pinMode(fan, OUTPUT);
  pinMode(heat, OUTPUT);
  pinMode(cool, OUTPUT);
  myDigitalWrite(fan, LOW);
  myDigitalWrite(heat, LOW);
  myDigitalWrite(cool, LOW);

  //declare cloud variables
  //https://docs.particle.io/reference/firmware/photon/#particle-variable-
  //Currently, up to 10 cloud variables may be defined and each variable name is limited to a maximum of 12 characters
  if (Particle.variable("homeMinTemp", homeMinTempString)==false) {
    Particle.publish(APP_NAME, "ERROR: Failed to register variable homeMinTemp", 60, PRIVATE);
  }
  if (Particle.variable("homeMaxTemp", homeMaxTempString)==false) {
    Particle.publish(APP_NAME, "ERROR: Failed to register variable homeMaxTemp", 60, PRIVATE);
  }
  if (Particle.variable("awayMaxTemp", awayMaxTempString)==false) {
    Particle.publish(APP_NAME, "ERROR: Failed to register variable awayMaxTemp", 60, PRIVATE);
  }
  if (Particle.variable("awayMinTemp", awayMaxTempString)==false) {
    Particle.publish(APP_NAME, "ERROR: Failed to register variable awayMinTemp", 60, PRIVATE);
  }
  
  if (Particle.variable("currentTemp", currentTempString)==false) {
    Particle.publish(APP_NAME, "ERROR: Failed to register variable currentTemp", 60, PRIVATE);
  }
  if (Particle.variable("humidity", currentHumidityString)==false) {
    Particle.publish(APP_NAME, "ERROR: Failed to register variable humidity", 60, PRIVATE);
  }
  if (Particle.variable("mode", externalMode)==false) {
    Particle.publish(APP_NAME, "ERROR: Failed to register variable mode", 60, PRIVATE);
  }
  if (Particle.variable("state", state)==false) {
    Particle.publish(APP_NAME, "ERROR: Failed to register variable mode", 60, PRIVATE);
  }

  //declare cloud functions
  //https://docs.particle.io/reference/firmware/photon/#particle-function-
  //Currently the application supports the creation of up to 4 different cloud functions.
  // If you declare a function name longer than 12 characters the function will not be registered.
  //user functions
  if (Particle.function("setTargetTmp", setHomeMin)==false) {
     Particle.publish(APP_NAME, "ERROR: Failed to register function setHomeMin", 60, PRIVATE);
  }
  //TESTING_HACK
  if (Particle.function("setCurrTmp", setCurrentTemp)==false) {
     Particle.publish(APP_NAME, "ERROR: Failed to register function setCurrentTemp", 60, PRIVATE);
  }
  //TESTING_HACK
  if (Particle.function("getOutputs", getOutputs)==false) {
     Particle.publish(APP_NAME, "ERROR: Failed to register function getOutputs", 60, PRIVATE);
  }
  //TESTING_HACK
  if (Particle.function("setTesting", setTesting)==false) {
     Particle.publish(APP_NAME, "ERROR: Failed to register function setTesting", 60, PRIVATE);
  }

  if (USE_BLYNK == "yes") {
    //init Blynk
    Blynk.begin(auth);
  }
 

  Time.zone(TIME_ZONE);

  resetSamplesArray();

  // connect to the server
  int retries = 0;
  while(!client.isConnected() && retries < 20){
      client.connect("m13.cloudmqtt.com", "home", "chicago");
      retries++;
       
      delay(500);
  }
  // publish/subscribe
  if (client.isConnected()) {
    //   client.publish("outTopic/message","hello world");
        client.subscribe("rxtest");
        client.subscribe("owntracks/#");
  }

  //restore settings from eeprom, if there were any saved before
  readFromEeprom();

}



/*******************************************************************************
 * Function Name  : loop
 * Description    : this function runs continuously while the project is running
 *******************************************************************************/
void loop() {
  //this function reads the temperature of the DHT sensor
  readTemperature();
 

  if (USE_BLYNK == "yes") {
    //all the Blynk magic happens here
    Blynk.run();
  }
 
  updateHomeMinTemp();
  updateAwayMinTemp();
  updateHomeMaxTemp();
  updateAwayMaxTemp();

  updateFanStatus();
  updatePulseStatus();
  updateMode();
 
  //this function updates the FSM
  // the FSM is the heart of the thermostat - all actions are defined by its states
  thermostatStateMachine.update();
 

  //publish readings to the blynk server every minute so the History Graph gets updated
  // even when the blynk app is not on (running) in the users phone
  updateBlynkCloud();
 

  //every now and then we save the settings
  saveSettings();
 

  if (client.isConnected())
      client.loop();
}

void updateUserPosition(JsonObject& mqttJson);
// {"_type":"transition","tid":"6p","acc":16.970562,"desc":"home","event":"leave",
// //"lat":41.87135338783264,"lon":-88.15792322158813,"tst":1482157097,"wtst":1482094653,"t":"c"}

//this'll be a user class
String user1 = "6p";
String user2 = "5s";
#define HOME "home"
#define AWAY "away"
#define THERMOSTAT_MQTT_USER_NAME "home"
String user1LocationStatus = HOME;
String user2LocationStatus = HOME;
void updateUserPosition(JsonObject& mqttJson){
  const char* type   = mqttJson["_type"];
  const char* tid    = mqttJson["tid"];
  const char* desc   = mqttJson["desc"];
  const char* event  = mqttJson["event"];
  // double latitude    = root["lat"];
  // double longitude   = root["lon"];
  String sType(type);
  String sTid(tid);
  if (sType.equalsIgnoreCase("transition")){
    String sDesc(desc);
    if(sDesc.equalsIgnoreCase(THERMOSTAT_MQTT_USER_NAME)){
      String sEvent(event);

      if(sEvent.equalsIgnoreCase("leave")){
        if(sTid.equalsIgnoreCase(user1)){
          user1LocationStatus = AWAY;
        }
        else if(sTid.equalsIgnoreCase(user2)){
          user2LocationStatus = AWAY;
        }
      }
      else if(sEvent.equalsIgnoreCase("enter")){
        if(sTid.equalsIgnoreCase(user1)){
          user1LocationStatus = HOME;
        }
        else if(sTid.equalsIgnoreCase(user2)){
          user2LocationStatus = HOME;
        }
      }
    }
    
  }

  String mqttSummaryMessage = getTime() + " mq:";
  mqttSummaryMessage.concat("t:");
  mqttSummaryMessage.concat(sType);
  mqttSummaryMessage.concat(" ti:");
  mqttSummaryMessage.concat(sTid);

  mqttSummaryMessage.concat(" u1:");
  mqttSummaryMessage.concat(user1LocationStatus);
  mqttSummaryMessage.concat(" u2:");
  mqttSummaryMessage.concat(user2LocationStatus);

  Particle.publish("googleDocs", "{\"my-name\":\"" + mqttSummaryMessage + "\"}", 60, PRIVATE);
}
// recieve message
void callback(char* topic, byte* payload, unsigned int length) {
  char p[length + 1];
  memcpy(p, payload, length);
  p[length] = NULL;
  // String message(p);//THIS LINE APPEARS TO MAKE THE p data look ok
  // debug1(message, NULL);

  // debug1("len", length);
  StaticJsonBuffer<500> jsonBuffer;
  JsonObject& root = jsonBuffer.parseObject(p);
  if(!root.success()){
    failDebugMessage(p, length);
    return;
  }
  updateUserPosition(root);
}


// Log message to cloud, message is a printf-formatted string
void debug1(String message, int value) {
    char msg [50];
    sprintf(msg, message.c_str(), value);
    Particle.publish("DEBUG1", msg);
}
void failDebugMessage(char* p, unsigned int length){
  String failMessage;
  // failMessage.concat("plength:");
  // failMessage.concat(String(length));
  // failMessage.concat(" ");
  for(int i =  0; i < length; i++){
  // for(int i =  length/2; i < length; i++){
  failMessage.concat(String(p[i], HEX));
  // failMessage.concat(" ");
  }
  // failMessage.concat("end");
  Particle.publish("googleDocs", "{\"my-name\":\"" + failMessage + "\"}", 60, PRIVATE);
}
#define DEBOUNCE_SETTINGS 4000
#define DEBOUNCE_SETTINGS_MODE 8000 //give more time to the MODE change

/*******************************************************************************
 * Function Name  : setHomeMin
 * Description    : sets the target temperature of the thermostat
                    newHomeMinTemp has to be a valid float value, or no new target temp will be set
 * Behavior       : the new setting will not take place right away, but moments after
                    since a timer is triggered. This is to debounce the setting and
                    allow the users to change their mind
* Return         : 0 if all is good, or -1 if the parameter does not match on or off
 *******************************************************************************/
elapsedMillis setHomeMinTimer;
int setHomeMin(String temp)
{
  float tmpFloat = temp.toFloat();
  //update the target temp only in the case the conversion to float works
  // (toFloat returns 0 if there is a problem in the conversion)
  // sorry, if you wanted to set 0 as the target temp, you can't :)
  if ( tmpFloat > 0 ) {
    //newHomeMinTemp will be copied to homeMinTemp moments after in function updateHomeMinTemp()
    // this is to 1-debounce the blynk slider I use and 2-debounce the user changing his/her mind quickly
    newHomeMinTemp = tmpFloat;
    //start timer to debounce this new setting
    setHomeMinTimer = 0;
    return 0;
  }

  //show only 2 decimals in notifications
  // Example: show 19.00 instead of 19.000000
  temp = temp.substring(0, temp.length()-4);

  //if the execution reaches here then the value was invalid
  //Particle.publish(APP_NAME, "ERROR: Failed to set new target temp to " + temp, 60, PRIVATE);
//  Particle.publish(PUSHBULLET_NOTIF_PERSONAL, "ERROR: Failed to set new target temp to " + temp + getTime(), 60, PRIVATE);
  String tempStatus = "ERROR: Failed to set new target temp to " + temp + getTime();
  Particle.publish("googleDocs", "{\"my-name\":\"" + tempStatus + "\"}", 60, PRIVATE);
  return -1;
}

elapsedMillis setHomeMaxTimer;
int setHomeMax(String temp)
{
  float tmpFloat = temp.toFloat();
  if ( tmpFloat > 0 ) {
    newHomeMaxTemp = tmpFloat;
    setHomeMaxTimer = 0;
    return 0;
  }
}
elapsedMillis setAwayMinTimer;
int setAwayMin(String temp)
{
  float tmpFloat = temp.toFloat();
  if ( tmpFloat > 0 ) {
    newAwayMinTemp = tmpFloat;
    setAwayMinTimer = 0;
    return 0;
  }
}
elapsedMillis setAwayMaxTimer;
int setAwayMax(String temp)
{
  float tmpFloat = temp.toFloat();
  if ( tmpFloat > 0 ) {
    newAwayMaxTemp = tmpFloat;
    setAwayMaxTimer = 0;
    return 0;
  }
}
/*******************************************************************************
 * Function Name  : updateHomeMinTemp
 * Description    : debounces setHomeMin
 * Return         : none
 *******************************************************************************/
void updateHomeMinTemp()
{
  //debounce the new setting
  if (setHomeMinTimer < DEBOUNCE_SETTINGS) {
    return;
  }
  //is there anything to update?
  if (homeMinTemp == newHomeMinTemp) {
    return;
  }

  homeMinTemp = newHomeMinTemp;
  homeMinTempString = float2string(homeMinTemp);

  String tempStatus = "New home min: " + homeMinTempString + "°C" + getTime();
  Particle.publish("googleDocs", "{\"my-name\":\"" + tempStatus + "\"}", 60, PRIVATE);
}
void updateAwayMinTemp()
{
  if (setAwayMinTimer < DEBOUNCE_SETTINGS) {
    return;
  }
  if (awayMinTemp == newAwayMinTemp) {
    return;
  }
  awayMinTemp = newAwayMinTemp;
  awayMinTempString = float2string(awayMinTemp);
  String tempStatus = "New Away Min temp: " + awayMinTempString + "°C" + getTime();
  Particle.publish("googleDocs", "{\"my-name\":\"" + tempStatus + "\"}", 60, PRIVATE);
}
void updateHomeMaxTemp()
{
  if (setHomeMaxTimer < DEBOUNCE_SETTINGS) {
    return;
  }
  if (homeMaxTemp == newHomeMaxTemp) {
    return;
  }
  homeMaxTemp = newHomeMaxTemp;
  homeMaxTempString = float2string(homeMaxTemp);
  String tempStatus = "New Home Max temp: " + homeMaxTempString + "°C" + getTime();
  Particle.publish("googleDocs", "{\"my-name\":\"" + tempStatus + "\"}", 60, PRIVATE);
}
void updateAwayMaxTemp()
{
  if (setAwayMaxTimer < DEBOUNCE_SETTINGS) {
    return;
  }
  if (awayMaxTemp == newAwayMaxTemp) {
    return;
  }
  awayMaxTemp = newAwayMaxTemp;
  awayMaxTempString = float2string(awayMaxTemp);
  String tempStatus = "New Away Max temp: " + awayMaxTempString + "°C" + getTime();
  Particle.publish("googleDocs", "{\"my-name\":\"" + tempStatus + "\"}", 60, PRIVATE);
}
/*******************************************************************************
 * Function Name  : float2string
 * Description    : return the string representation of the float number
                     passed as parameter with 2 decimals
 * Return         : the string
 *******************************************************************************/
String float2string( float floatNumber )
{
  String stringNumber = String(floatNumber);

  //return only 2 decimals
  // Example: show 19.00 instead of 19.000000
  stringNumber = stringNumber.substring(0, stringNumber.length()-4);

  return stringNumber;
}

bool externalFan = false;
bool internalFan = false;
bool fanButtonClick = false;
elapsedMillis fanButtonClickTimer;
/*******************************************************************************
 * Function Name  : updateFanStatus
 * Description    : updates the status of the fan moments after it was set
 * Return         : none
 *******************************************************************************/
void updateFanStatus()
{
  //if the button was not pressed, get out
  if ( not fanButtonClick ){
    return;
  }

  //debounce the new setting
  if (fanButtonClickTimer < DEBOUNCE_SETTINGS) {
    return;
  }

  //reset flag of button pressed
  fanButtonClick = false;

  //is there anything to update?
  // this code here takes care of the users having cycled the mode to the same original value
  if ( internalFan == externalFan ) {
    return;
  }

  //update the new setting from the external to the internal variable
  internalFan = externalFan;

  if ( internalFan ) {
    //Particle.publish(PUSHBULLET_NOTIF_PERSONAL, "Fan on" + getTime(), 60, PRIVATE);
    String tempStatus = "Fan on" + getTime();
    Particle.publish("googleDocs", "{\"my-name\":\"" + tempStatus + "\"}", 60, PRIVATE);
  } else {
    //Particle.publish(PUSHBULLET_NOTIF_PERSONAL, "Fan off" + getTime(), 60, PRIVATE);
    String tempStatus = "Fan off" + getTime();
    Particle.publish("googleDocs", "{\"my-name\":\"" + tempStatus + "\"}", 60, PRIVATE);
  }
}

bool externalPulse = false;
bool internalPulse = false;
bool pulseButtonClick = false;
elapsedMillis pulseButtonClickTimer;
/*******************************************************************************
 * Function Name  : updatePulseStatus
 * Description    : updates the status of the pulse of the thermostat
                    moments after it was set
 * Return         : none
 *******************************************************************************/
void updatePulseStatus()
{
  //if the button was not pressed, get out
  if ( not pulseButtonClick ){
    return;
  }

  //debounce the new setting
  if (pulseButtonClickTimer < DEBOUNCE_SETTINGS) {
    return;
  }

  //reset flag of button pressed
  pulseButtonClick = false;

  //is there anything to update?
  // this code here takes care of the users having cycled the mode to the same original value
  if ( internalPulse == externalPulse ) {
    return;
  }

  //update only in the case the FSM state is idleState (the thermostat is doing nothing)
  // or pulseState (a pulse is already running and the user wants to abort it)
  if ( not ( thermostatStateMachine.isInState(idleState) or thermostatStateMachine.isInState(pulseState) ) ) {
    // Particle.publish(PUSHBULLET_NOTIF_HOME, "ERROR: You can only start a pulse in idle state" + getTime(), 60, PRIVATE);
    pulseLed.off();
    return;
  }

  //update the new setting from the external to the internal variable
  internalPulse = externalPulse;

}

/*******************************************************************************
 * Function Name  : updateMode
 * Description    : check if the mode has changed
 * Behavior       : the new setting will not take place right away, but moments after
                    since a timer is triggered. This is to debounce the setting and
                    allow the users to change their mind
 * Return         : none
 *******************************************************************************/
void updateMode()
{
  //if the mode button was not pressed, get out
  if ( not modeButtonClick ){
    return;
  }

  //debounce the new setting
  if (modeButtonClickTimer < DEBOUNCE_SETTINGS_MODE ) {
    return;
  }

  //reset flag of button pressed
  modeButtonClick = false;

  //is there anything to update?
  // this code here takes care of the users having cycled the mode to the same original value
  if ( internalMode == externalMode ) {
    return;
  }

  //update the new mode from the external to the internal variable
  internalMode = externalMode;
  //Particle.publish(PUSHBULLET_NOTIF_PERSONAL, "Mode set to " + internalMode + getTime(), 60, PRIVATE);
  String tempStatus = "Mode set to " + internalMode + getTime();
  Particle.publish("googleDocs", "{\"my-name\":\"" + tempStatus + "\"}", 60, PRIVATE);

}
/*******************************************************************************
 DHT sensor
*******************************************************************************/
#define DHTTYPE  DHT22                // Sensor type DHT11/21/22/AM2301/AM2302
#define DHTPIN   4                    // Digital pin for communications
#define DHT_SAMPLE_INTERVAL   5000    // Sample room temperature every 5 seconds
                                      //  this is then averaged in temperatureAverage
void dht_wrapper(); // must be declared before the lib initialization
PietteTech_DHT DHT(DHTPIN, DHTTYPE, dht_wrapper);
bool bDHTstarted;       // flag to indicate we started acquisition
elapsedMillis dhtSampleInterval;
// how many samples to take and average, more takes longer but measurement is smoother
const int NUMBER_OF_SAMPLES = 10;
//const float DUMMY = -100;
//const float DUMMY_ARRAY[NUMBER_OF_SAMPLES] = { DUMMY, DUMMY, DUMMY, DUMMY, DUMMY, DUMMY, DUMMY, DUMMY, DUMMY, DUMMY };
#define DUMMY -100
#define DUMMY_ARRAY { DUMMY, DUMMY, DUMMY, DUMMY, DUMMY, DUMMY, DUMMY, DUMMY, DUMMY, DUMMY };
float temperatureSamples[NUMBER_OF_SAMPLES] = DUMMY_ARRAY;
float averageTemperature;

// This wrapper is in charge of calling the DHT sensor lib
void dht_wrapper() { DHT.isrCallback(); }

void resetSamplesArray(){
  //reset samples array to default so we fill it up with new samples
  uint8_t i;
  for (i=0; i<NUMBER_OF_SAMPLES; i++) {
    temperatureSamples[i] = DUMMY;
  }
}
/*******************************************************************************
 * Function Name  : readTemperature
 * Description    : reads the temperature of the DHT22 sensor at every DHT_SAMPLE_INTERVAL
                    if testing the app, it returns right away
 * Return         : 0
 *******************************************************************************/

//sensor difference with real temperature (if none set to zero)
//use this variable to align measurements with your existing thermostat
float temperatureCalibration = -1.35;
int readTemperature() {

  //TESTING_HACK
  //are we testing the app? then no need to acquire from the sensor
  if (testing) {
   return 0;
  }

  //time is up? no, then come back later
  if (dhtSampleInterval < DHT_SAMPLE_INTERVAL) {
   return 0;
  }

  //time is up, reset timer
  dhtSampleInterval = 0;

  // start the sample
  if (!bDHTstarted) {
    DHT.acquireAndWait(5);
    bDHTstarted = true;
  }

  //still acquiring sample? go away and come back later
  if (DHT.acquiring()) {
    return 0;
  }

  //I observed my dht22 measuring below 0 from time to time, so let's discard that sample
  if ( DHT.getCelsius() < 0 ) {
    //reset the sample flag so we can take another
    bDHTstarted = false;
    return 0;
  }

  //valid sample acquired, adjust DHT difference if any
  float tmpTemperature = (float)DHT.getCelsius();
  tmpTemperature = tmpTemperature + temperatureCalibration;

  //------------------------------------------------------------------
  //let's make an average of the measured temperature
  // by taking N samples
  uint8_t i;
  for (i=0; i< NUMBER_OF_SAMPLES; i++) {
    //store the sample in the next available 'slot' in the array of samples
    if ( temperatureSamples[i] == DUMMY ) {
      temperatureSamples[i] = tmpTemperature;
      break;
    }
  }

  //is the samples array full? if not, exit and get a new sample
  if ( temperatureSamples[NUMBER_OF_SAMPLES-1] == DUMMY ) {
    return 0;
  }

  // average all the samples out
  averageTemperature = 0;
  for (i=0; i<NUMBER_OF_SAMPLES; i++) {
    averageTemperature += temperatureSamples[i];
  }
  averageTemperature /= NUMBER_OF_SAMPLES;

  //reset samples array to default so we fill it up again with new samples
  for (i=0; i<NUMBER_OF_SAMPLES; i++) {
    temperatureSamples[i] = DUMMY;
  }
  //------------------------------------------------------------------

  //sample acquired and averaged - go ahead and store temperature and humidity in internal variables
  publishTemperature( averageTemperature, (float)DHT.getHumidity() );

  //reset the sample flag so we can take another
  bDHTstarted = false;

  return 0;
}
#ifndef USE_FERENHEIGHT
#define USE_FERENHEIGHT false
#endif
float userUnits(float temperature){
  if(USE_FERENHEIGHT){
    return (1.8 * temperature) + 32;//convert to f
  }else{
    return temperature;
  }
}
/*******************************************************************************
 * Function Name  : publishTemperature
 * Description    : the temperature/humidity passed as parameters get stored in internal variables
                    and then published
 * Return         : 0
 *******************************************************************************/
int publishTemperature( float temperature, float humidity ) {


  char currentTempChar[32];
  currentTemp = userUnits(temperature);

  int currentTempDecimals = (currentTemp - (int)currentTemp) * 100;
  sprintf(currentTempChar,"%0d.%d", (int)currentTemp, currentTempDecimals);

  char currentHumidityChar[32];
  currentHumidity = humidity;
  int currentHumidityDecimals = (currentHumidity - (int)currentHumidity) * 100;
  sprintf(currentHumidityChar,"%0d.%d", (int)currentHumidity, currentHumidityDecimals);

  //publish readings into exposed variables
  currentTempString = String(currentTempChar);
  currentHumidityString = String(currentHumidityChar);

  //publish readings
  Particle.publish(APP_NAME, currentTempString + "°C " + currentHumidityString + "%", 60, PRIVATE);
  //post to thinkspeak

  char buf[1000];
    snprintf(buf, sizeof(buf), "{ \"temperature\":\"" + currentTempString + 
                               "\",\"humidity\":\"" + currentHumidityString + "\"}");
    // Particle.publish("temp", buf, 60, PRIVATE);

  String connectionStatus;
  if (client.isConnected()) {
        connectionStatus = String(" Connected ");
    }else{
        connectionStatus = String(" not Connected ");      
    }
  String tempStatus = getTime() + " " + connectionStatus + 
  " temp:" + currentTempString + " humidity:" + currentHumidityString;
  Particle.publish("googleDocs", "{\"my-name\":\"" + tempStatus + "\"}", 60, PRIVATE);

  return 0;
}


/*******************************************************************************
********************************************************************************
********************************************************************************
 FINITE STATE MACHINE FUNCTIONS
********************************************************************************
********************************************************************************
*******************************************************************************/
//milliseconds for the init cycle, so temperature samples get stabilized
//this should be in the order of the 5 minutes: 5*60*1000==300000
//for now, I will use 1 minute
#define INIT_TIMEOUT 60000
elapsedMillis initTimer;
void initEnterFunction(){
  //start the timer of this cycle
  initTimer = 0;
  //set the state
  setState(STATE_INIT);
}
void initUpdateFunction(){
  //time is up?
  if (initTimer > INIT_TIMEOUT) {
    thermostatStateMachine.transitionTo(idleState);
  }
}
void initExitFunction(){
  Particle.publish(APP_NAME, "Initialization done", 60, PRIVATE);
}
//minimum number of milliseconds to leave the system in idle state
// to protect the fan and the heating/cooling elements
#define MINIMUM_IDLE_TIMEOUT 60000
elapsedMillis minimumIdleTimer;
void idleEnterFunction(){
  //set the state
  setState(STATE_IDLE);

  //turn off the fan only if fan was not set on manually by the user
  if ( internalFan == false ) {
    myDigitalWrite(fan, LOW);
  }
  myDigitalWrite(heat, LOW);
  myDigitalWrite(cool, LOW);

  //start the minimum timer of this cycle
  minimumIdleTimer = 0;
}
bool isUsersHome(){
  if(user1LocationStatus.equalsIgnoreCase(AWAY) && user2LocationStatus.equalsIgnoreCase(AWAY)) {
    return false;
  }else{
    return true;
  }
}
float getTargetMin(){
  if(isUsersHome()){
    return homeMinTemp;
  }else{
    return awayMinTemp;
  }
}
float getTargetMax(){
  if(isUsersHome()){
    return homeMaxTemp;
  }else{
    return awayMaxTemp;
  }
}
//you can change this to your liking
// a smaller value will make your temperature more constant at the price of
//  starting the heat more times
// a larger value will reduce the number of times the HVAC comes on but will leave it on a longer time
float margin = 0.25;
void idleUpdateFunction(){
  //set the fan output to the internalFan ONLY in this state of the FSM
  // since other states might need the fan on
  //set it off only if it was on and internalFan changed to false
  if ( internalFan == false and fanOutput == HIGH ) {
    myDigitalWrite(fan, LOW);
  }
  //set it on only if it was off and internalFan changed to true
  if ( internalFan == true and fanOutput == LOW ) {
    myDigitalWrite(fan, HIGH);
  }

  //is minimum time up? not yet, so get out of here
  if (minimumIdleTimer < MINIMUM_IDLE_TIMEOUT) {
    return;
  }

  //if the thermostat is OFF, there is not much to do
  if ( internalMode == MODE_OFF ){
    if ( internalPulse ) {
      // Particle.publish(PUSHBULLET_NOTIF_HOME, "ERROR: You cannot start a pulse when the system is OFF" + getTime(), 60, PRIVATE);
      internalPulse = false;
    }
    return;
  }

  //are we heating?
  if ( internalMode == MODE_HEAT ){
    //if the temperature is lower than the target, transition to heatingState
    if ( currentTemp <= (getTargetMin() - margin) ) {
      thermostatStateMachine.transitionTo(heatingState);
    }
    if ( internalPulse ) {
      thermostatStateMachine.transitionTo(pulseState);
    }
  }

  //are we cooling?
  if ( internalMode == MODE_COOL ){
    //if the temperature is higher than the target, transition to coolingState
    if ( currentTemp > (getTargetMax() + margin) ) {
      thermostatStateMachine.transitionTo(coolingState);
    }
    if ( internalPulse ) {
      thermostatStateMachine.transitionTo(pulseState);
    }
  }

}
void idleExitFunction(){
}
//minimum number of milliseconds to leave the heating element on
// to protect on-off on the fan and the heating/cooling elements
#define MINIMUM_ON_TIMEOUT 60000
elapsedMillis minimumOnTimer;
void heatingEnterFunction(){
  //set the state
  setState(STATE_HEATING);

  //Particle.publish(PUSHBULLET_NOTIF_PERSONAL, "Heat on" + getTime(), 60, PRIVATE);
  String tempStatus = "Heat on" + getTime();
  Particle.publish("googleDocs", "{\"my-name\":\"" + tempStatus + "\"}", 60, PRIVATE);
  myDigitalWrite(fan, HIGH);
  myDigitalWrite(heat, HIGH);
  myDigitalWrite(cool, LOW);

  //start the minimum timer of this cycle
  minimumOnTimer = 0;
}

String getTargetMinTempString(){
  if(isUsersHome()){
    return homeMinTempString;
  }else{
    return awayMinTempString;
  }
}
String getTargetMaxTempString(){
  if(isUsersHome()){
    return homeMaxTempString;
  }else{
    return awayMaxTempString;
  }
}

void heatingUpdateFunction(){
  //is minimum time up?
  if (minimumOnTimer < MINIMUM_ON_TIMEOUT) {
    //not yet, so get out of here
    return;
  }

  if ( currentTemp >= (getTargetMin() + margin) ) {
    //Particle.publish(PUSHBULLET_NOTIF_PERSONAL, "Desired temperature reached: " + homeMinTempString + "°C" + getTime(), 60, PRIVATE);
    String tempStatus = "Desired temperature reached: " + getTargetMinTempString() + "°C" + getTime();
    Particle.publish("googleDocs", "{\"my-name\":\"" + tempStatus + "\"}", 60, PRIVATE);
    thermostatStateMachine.transitionTo(idleState);
  }

  //was the mode changed by the user? if so, go back to idleState
  if ( internalMode != MODE_HEAT ){
    thermostatStateMachine.transitionTo(idleState);
  }

}
void heatingExitFunction(){
  String tempStatus = "Heat off" + getTime();
  Particle.publish("googleDocs", "{\"my-name\":\"" + tempStatus + "\"}", 60, PRIVATE);
  myDigitalWrite(fan, LOW);
  myDigitalWrite(heat, LOW);
  myDigitalWrite(cool, LOW);
}

/*******************************************************************************
 * FSM state Name : pulseState
 * Description    : turns the HVAC on for a certain time
                    comes in handy when you want to warm up/cool down the house a little bit
 *******************************************************************************/
//milliseconds to pulse on the heating = 600 seconds = 10 minutes
// turns the heating on for a certain time
// comes in handy when you want to warm up the house a little bit
#define PULSE_TIMEOUT 600000
elapsedMillis pulseTimer;
void pulseEnterFunction(){
  //Particle.publish(PUSHBULLET_NOTIF_PERSONAL, "Pulse on" + getTime(), 60, PRIVATE);
  String tempStatus = "Pulse on" + getTime();
  Particle.publish("googleDocs", "{\"my-name\":\"" + tempStatus + "\"}", 60, PRIVATE);
  if ( internalMode == MODE_HEAT ){
    myDigitalWrite(fan, HIGH);
    myDigitalWrite(heat, HIGH);
    myDigitalWrite(cool, LOW);
    //set the state
    setState(STATE_PULSE_HEAT);
  } else if ( internalMode == MODE_COOL ){
    myDigitalWrite(fan, HIGH);
    myDigitalWrite(heat, LOW);
    myDigitalWrite(cool, HIGH);
    //set the state
    setState(STATE_PULSE_COOL);
  }
  //start the timer of this cycle
  pulseTimer = 0;

  //start the minimum timer of this cycle
  minimumOnTimer = 0;
}
void pulseUpdateFunction(){
  //is minimum time up? if not, get out of here
  if (minimumOnTimer < MINIMUM_ON_TIMEOUT) {
    return;
  }

  //if the pulse was canceled by the user, transition to idleState
  if (not internalPulse) {
    thermostatStateMachine.transitionTo(idleState);
  }

  //is the time up for the pulse? if not, get out of here
  if (pulseTimer < PULSE_TIMEOUT) {
    return;
  }

  thermostatStateMachine.transitionTo(idleState);
}
void pulseExitFunction(){
  internalPulse = false;
  //Particle.publish(PUSHBULLET_NOTIF_PERSONAL, "Pulse off" + getTime(), 60, PRIVATE);
  String tempStatus = "Pulse off" + getTime();
  Particle.publish("googleDocs", "{\"my-name\":\"" + tempStatus + "\"}", 60, PRIVATE);
  myDigitalWrite(fan, LOW);
  myDigitalWrite(heat, LOW);
  myDigitalWrite(cool, LOW);

  if (USE_BLYNK == "yes") {
    pulseLed.off();
  }

}

/*******************************************************************************
 * FSM state Name : coolingState
 * Description    : turns the cooling element on until the desired temperature is reached
 *******************************************************************************/
void coolingEnterFunction(){
  //set the state
  setState(STATE_COOLING);

  //Particle.publish(PUSHBULLET_NOTIF_PERSONAL, "Cool on" + getTime(), 60, PRIVATE);
  String tempStatus = "Cool on" + getTime();
  Particle.publish("googleDocs", "{\"my-name\":\"" + tempStatus + "\"}", 60, PRIVATE);
  myDigitalWrite(fan, HIGH);
  myDigitalWrite(heat, LOW);
  myDigitalWrite(cool, HIGH);

  //start the minimum timer of this cycle
  minimumOnTimer = 0;
}
void coolingUpdateFunction(){
  //is minimum time up?
  if (minimumOnTimer < MINIMUM_ON_TIMEOUT) {
    //not yet, so get out of here
    return;
  }

  if ( currentTemp <= (getTargetMax() - margin) ) {
    //Particle.publish(PUSHBULLET_NOTIF_PERSONAL, "Desired temperature reached: " + homeMinTempString + "°C" + getTime(), 60, PRIVATE);
    String tempStatus = "Desired max reached: " + getTargetMaxTempString() + "°C" + getTime();
    Particle.publish("googleDocs", "{\"my-name\":\"" + tempStatus + "\"}", 60, PRIVATE);
    thermostatStateMachine.transitionTo(idleState);
  }

  //was the mode changed by the user? if so, go back to idleState
  if ( internalMode != MODE_COOL ){
   thermostatStateMachine.transitionTo(idleState);
  }

 }
 void coolingExitFunction(){
  //Particle.publish(PUSHBULLET_NOTIF_PERSONAL, "Cool off" + getTime(), 60, PRIVATE);
  String tempStatus = "Cool off" + getTime();
  Particle.publish("googleDocs", "{\"my-name\":\"" + tempStatus + "\"}", 60, PRIVATE);
  myDigitalWrite(fan, LOW);
  myDigitalWrite(heat, LOW);
  myDigitalWrite(cool, LOW);
}

/*******************************************************************************
********************************************************************************
********************************************************************************
 TESTING HACKS
********************************************************************************
********************************************************************************
*******************************************************************************/

/*******************************************************************************
 * Function Name  : setTesting
 * Description    : allows to start testing mode
                    testing mode enables an override of the temperature read
                     by the temperature sensor.
                    this is a hack that allows system testing the project
 * Return         : 0
 *******************************************************************************/
int setTesting(String test)
{
  if ( test.equalsIgnoreCase("on") ) {
    testing = true;
  } else {
    testing = false;
  }
  return 0;
}

/*******************************************************************************
 * Function Name  : getOutputs
 * Description    : returns the outputs so we can test the program
                    this is a hack that allows me to system test the project
 * Return         : returns the outputs
 *******************************************************************************/
int getOutputs(String dummy)
{
  // int fan = D0;
  // int heat = D1;
  // int cool = D2;
  return coolOutput*4 + heatOutput*2 + fanOutput*1;
}

/*******************************************************************************
 * Function Name  : setCurrentTemp
 * Description    : sets the current temperature of the thermostat
                    newCurrentTemp has to be a valid float value, or no new current temp will be set
                    this is a hack that allows me to system test the project
* Return         : 0, or -1 if it fails to convert the temp to float
 *******************************************************************************/
int setCurrentTemp(String newCurrentTemp)
{
  float tmpFloat = newCurrentTemp.toFloat();

  //update the current temp only in the case the conversion to float works
  // (toFloat returns 0 if there is a problem in the conversion)
  // sorry, if you wanted to set 0 as the current temp, you can't :)
  if ( tmpFloat > 0 ) {
    currentTemp = tmpFloat;
    currentTempString = String(currentTemp);

    //show only 2 decimals in notifications
    // Example: show 19.00 instead of 19.000000
    currentTempString = currentTempString.substring(0, currentTempString.length()-4);

    Particle.publish(APP_NAME, "New current temp: " + currentTempString, 60, PRIVATE);
    //Particle.publish(PUSHBULLET_NOTIF_PERSONAL, "New current temp: " + currentTempString + getTime(), 60, PRIVATE);
    String tempStatus = "New current temp: " + currentTempString + getTime();
    Particle.publish("googleDocs", "{\"my-name\":\"" + tempStatus + "\"}", 60, PRIVATE);
    return 0;
  } else {
    Particle.publish(APP_NAME, "ERROR: Failed to set new current temp to " + newCurrentTemp, 60, PRIVATE);
    // Particle.publish(PUSHBULLET_NOTIF_PERSONAL, "ERROR: Failed to set new current temp to " + newCurrentTemp + getTime(), 60, PRIVATE);
    return -1;
  }
}

/*******************************************************************************
 * Function Name  : myDigitalWrite
 * Description    : writes to the pin and sets a variable to keep track
                    this is a hack that allows me to system test the project
                    and know what is the status of the outputs
 * Return         : void
 *******************************************************************************/
void myDigitalWrite(int input, int status){

  digitalWrite(input, status);

  if (input == fan){
    fanOutput = status;
    BLYNK_setFanLed(status);
  }

  if (input == heat){
    heatOutput = status;
    BLYNK_setHeatLed(status);
  }

  if (input == cool){
    coolOutput = status;
    BLYNK_setCoolLed(status);
  }
}

/*******************************************************************************
 * Function Name  : getTime
 * Description    : returns the time in the following format: 14:42:31
                    TIME_FORMAT_ISO8601_FULL example: 2016-03-23T14:42:31-04:00
 * Return         : the time
 *******************************************************************************/
String getTime() {
  String timeNow = Time.format(Time.now(), TIME_FORMAT_ISO8601_FULL);
  timeNow = timeNow.substring(11, timeNow.length()-6);
  return " " + timeNow;
}

void blynkUpdateUserPosition(){
  String position = AWAY;
  if(isUsersHome()){
    position = HOME;
  }
  Blynk.virtualWrite(BLYNK_DISPLAY_USER_POSITION, position);
}

/*******************************************************************************
 * Function Name  : setState
 * Description    : sets the state of the system
 * Return         : none
 *******************************************************************************/
void setState(String newState) {
  state = newState;
  Blynk.virtualWrite(BLYNK_DISPLAY_STATE, state);
}

/*******************************************************************************/
/*******************************************************************************/
/*******************          BLYNK FUNCTIONS         **************************/
/*******************************************************************************/
/*******************************************************************************/

/*******************************************************************************
 * Function Name  : BLYNK_READ
 * Description    : these functions are called by blynk when the blynk app wants
                     to read values from the photon
                    source: http://docs.blynk.cc/#blynk-main-operations-get-data-from-hardware
 *******************************************************************************/
BLYNK_READ(BLYNK_DISPLAY_CURRENT_TEMP) {
  //this is a blynk value display
  // source: http://docs.blynk.cc/#widgets-displays-value-display
  Blynk.virtualWrite(BLYNK_DISPLAY_CURRENT_TEMP, currentTemp);
}
BLYNK_READ(BLYNK_DISPLAY_HUMIDITY) {
  //this is a blynk value display
  // source: http://docs.blynk.cc/#widgets-displays-value-display
  Blynk.virtualWrite(BLYNK_DISPLAY_HUMIDITY, currentHumidity);
}

BLYNK_READ(BLYNK_DISPLAY_HOME_MIN_TEMP) {
  //this is a blynk value display
  // source: http://docs.blynk.cc/#widgets-displays-value-display
  Blynk.virtualWrite(BLYNK_DISPLAY_HOME_MIN_TEMP, homeMinTemp);
}
BLYNK_READ(BLYNK_DISPLAY_AWAY_MIN_TEMP) {
  //this is a blynk value display
  // source: http://docs.blynk.cc/#widgets-displays-value-display
  Blynk.virtualWrite(BLYNK_DISPLAY_AWAY_MIN_TEMP, awayMinTemp);
}
BLYNK_READ(BLYNK_DISPLAY_HOME_MAX_TEMP) {
  //this is a blynk value display
  // source: http://docs.blynk.cc/#widgets-displays-value-display
  Blynk.virtualWrite(BLYNK_DISPLAY_HOME_MAX_TEMP, homeMaxTemp);
}
BLYNK_READ(BLYNK_DISPLAY_AWAY_MAX_TEMP) {
  //this is a blynk value display
  // source: http://docs.blynk.cc/#widgets-displays-value-display
  Blynk.virtualWrite(BLYNK_DISPLAY_AWAY_MAX_TEMP, awayMaxTemp);
}

BLYNK_READ(BLYNK_LED_FAN) {
  //this is a blynk led
  // source: http://docs.blynk.cc/#widgets-displays-led
  if ( externalFan ) {
    fanLed.on();
  } else {
    fanLed.off();
  }
}
BLYNK_READ(BLYNK_LED_PULSE) {
  //this is a blynk led
  // source: http://docs.blynk.cc/#widgets-displays-led
  if ( externalPulse ) {
    pulseLed.on();
  } else {
    pulseLed.off();
  }
}
BLYNK_READ(BLYNK_LED_HEAT) {
  //this is a blynk led
  // source: http://docs.blynk.cc/#widgets-displays-led
  if ( heatOutput ) {
    heatLed.on();
  } else {
    heatLed.off();
  }
}
BLYNK_READ(BLYNK_LED_COOL) {
  //this is a blynk led
  // source: http://docs.blynk.cc/#widgets-displays-led
  if ( coolOutput ) {
    coolLed.on();
  } else {
    coolLed.off();
  }
}
BLYNK_READ(BLYNK_DISPLAY_MODE) {
  Blynk.virtualWrite(BLYNK_DISPLAY_MODE, externalMode);
}
BLYNK_READ(BLYNK_DISPLAY_STATE) {
  Blynk.virtualWrite(BLYNK_DISPLAY_STATE, state);
}
BLYNK_READ(BLYNK_DISPLAY_USER_POSITION) {
 blynkUpdateUserPosition();
}
/*******************************************************************************
 * Function Name  : BLYNK_WRITE
 * Description    : these functions are called by blynk when the blynk app wants
                     to write values to the photon
                    source: http://docs.blynk.cc/#blynk-main-operations-send-data-from-app-to-hardware
 *******************************************************************************/
BLYNK_WRITE(BLYNK_SLIDER_HOME_MIN_TEMP) {
  //this is the blynk slider
  // source: http://docs.blynk.cc/#widgets-controllers-slider
  setHomeMin(param.asStr());
  flagSettingsHaveChanged();
}
BLYNK_WRITE(BLYNK_SLIDER_AWAY_MIN_TEMP) {
  //this is the blynk slider
  // source: http://docs.blynk.cc/#widgets-controllers-slider
  setAwayMin(param.asStr());
  flagSettingsHaveChanged();
}
BLYNK_WRITE(BLYNK_SLIDER_HOME_MAX_TEMP) {
  //this is the blynk slider
  // source: http://docs.blynk.cc/#widgets-controllers-slider
  setHomeMax(param.asStr());
  flagSettingsHaveChanged();
}
BLYNK_WRITE(BLYNK_SLIDER_AWAY_MAX_TEMP) {
  //this is the blynk slider
  // source: http://docs.blynk.cc/#widgets-controllers-slider
  setAwayMax(param.asStr());
  flagSettingsHaveChanged();
}

BLYNK_WRITE(BLYNK_BUTTON_FAN) {
  //flip fan status, if it's on switch it off and viceversa
  // do this only when blynk sends a 1
  // background: in a BLYNK push button, blynk sends 0 then 1 when user taps on it
  // source: http://docs.blynk.cc/#widgets-controllers-button
  if ( param.asInt() == 1 ) {
    externalFan = not externalFan;
    //start timer to debounce this new setting
    fanButtonClickTimer = 0;
    //flag that the button was clicked
    fanButtonClick = true;
    //update the led
    if ( externalFan ) {
      fanLed.on();
    } else {
      fanLed.off();
    }

    flagSettingsHaveChanged();
  }
}

BLYNK_WRITE(BLYNK_BUTTON_PULSE) {
  //flip pulse status, if it's on switch it off and viceversa
  // do this only when blynk sends a 1
  // background: in a BLYNK push button, blynk sends 0 then 1 when user taps on it
  // source: http://docs.blynk.cc/#widgets-controllers-button
  if ( param.asInt() == 1 ) {
    externalPulse = not externalPulse;
    //start timer to debounce this new setting
    pulseButtonClickTimer = 0;
    //flag that the button was clicked
    pulseButtonClick = true;
    //update the pulse led
    if ( externalPulse ) {
      pulseLed.on();
    } else {
      pulseLed.off();
    }
  }
}

BLYNK_WRITE(BLYNK_BUTTON_MODE) {
  //mode: cycle through off->heating->cooling
  // do this only when blynk sends a 1
  // background: in a BLYNK push button, blynk sends 0 then 1 when user taps on it
  // source: http://docs.blynk.cc/#widgets-controllers-button
  if ( param.asInt() == 1 ) {
    if ( externalMode == MODE_OFF ){
      externalMode = MODE_HEAT;
    } else if ( externalMode == MODE_HEAT ){
      externalMode = MODE_COOL;
    } else if ( externalMode == MODE_COOL ){
      externalMode = MODE_OFF;
    } else {
      externalMode = MODE_OFF;
    }

    //start timer to debounce this new setting
    modeButtonClickTimer = 0;
    //flag that the button was clicked
    modeButtonClick = true;
    //update the mode indicator
    Blynk.virtualWrite(BLYNK_DISPLAY_MODE, externalMode);

    flagSettingsHaveChanged();
  }
}

/*******************************************************************************
 * Function Name  : BLYNK_setXxxLed
 * Description    : these functions are called by our program to update the status
                    of the leds in the blynk cloud and the blynk app
                    source: http://docs.blynk.cc/#blynk-main-operations-send-data-from-app-to-hardware
*******************************************************************************/
void BLYNK_setFanLed(int status) {
  if (USE_BLYNK == "yes") {
    if ( status ) {
      fanLed.on();
    } else {
      fanLed.off();
    }
  }
}

void BLYNK_setHeatLed(int status) {
  if (USE_BLYNK == "yes") {
    if ( status ) {
      heatLed.on();
    } else {
      heatLed.off();
    }
  }
}

void BLYNK_setCoolLed(int status) {
  if (USE_BLYNK == "yes") {
    if ( status ) {
      coolLed.on();
    } else {
      coolLed.off();
    }
  }
}

// BLYNK_CONNECTED() {
//   Blynk.syncVirtual(BLYNK_DISPLAY_CURRENT_TEMP);
//   Blynk.syncVirtual(BLYNK_DISPLAY_HUMIDITY);
//   Blynk.syncVirtual(BLYNK_DISPLAY_HOME_MIN_TEMP);
//   Blynk.syncVirtual(BLYNK_LED_FAN);
//   Blynk.syncVirtual(BLYNK_LED_HEAT);
//   Blynk.syncVirtual(BLYNK_LED_COOL);
//   Blynk.syncVirtual(BLYNK_LED_PULSE);
//   Blynk.syncVirtual(BLYNK_DISPLAY_MODE);
//   // BLYNK_setFanLed(fan);
//   // BLYNK_setHeatLed(heat);
//   // BLYNK_setCoolLed(cool);
//
//   //update the mode and state indicator
//   Blynk.virtualWrite(BLYNK_DISPLAY_MODE, externalMode);
//   Blynk.virtualWrite(BLYNK_DISPLAY_STATE, state);
// }

/*******************************************************************************
 * Function Name  : updateBlynkCloud
 * Description    : publish readings to the blynk server every minute so the
                    History Graph gets updated even when
                    the blynk app is not on (running) in the users phone
 * Return         : none
 *******************************************************************************/
void updateBlynkCloud() {

  //is it time to store in the blynk cloud? if so, do it
  if ( (USE_BLYNK == "yes") and (blynkStoreInterval > BLYNK_STORE_INTERVAL) ) {

    //reset timer
    blynkStoreInterval = 0;

    //do not write the temp while the thermostat is initializing
    if ( not thermostatStateMachine.isInState(initState) ) {
      Blynk.virtualWrite(BLYNK_DISPLAY_CURRENT_TEMP, currentTemp);
      Blynk.virtualWrite(BLYNK_DISPLAY_HUMIDITY, currentHumidity);
    }

    Blynk.virtualWrite(BLYNK_DISPLAY_HOME_MIN_TEMP, homeMinTemp);
    Blynk.virtualWrite(BLYNK_DISPLAY_AWAY_MIN_TEMP, awayMinTemp);
    Blynk.virtualWrite(BLYNK_DISPLAY_HOME_MAX_TEMP, homeMaxTemp);
    Blynk.virtualWrite(BLYNK_DISPLAY_AWAY_MAX_TEMP, awayMaxTemp);

    if ( externalPulse ) {
      pulseLed.on();
    } else {
      pulseLed.off();
    }

    BLYNK_setFanLed(externalFan);
    BLYNK_setHeatLed(heatOutput);
    BLYNK_setCoolLed(coolOutput);

    //update the mode and state indicator
    Blynk.virtualWrite(BLYNK_DISPLAY_MODE, externalMode);
    Blynk.virtualWrite(BLYNK_DISPLAY_STATE, state);

    blynkUpdateUserPosition();
  }

}

/*******************************************************************************/
/*******************************************************************************/
/*******************          EEPROM FUNCTIONS         *************************/
/********  https://docs.particle.io/reference/firmware/photon/#eeprom  *********/
/*******************************************************************************/
/*******************************************************************************/
bool settingsHaveChanged = false;

elapsedMillis settingsHaveChanged_timer;
/*******************************************************************************
 * Function Name  : flagSettingsHaveChanged
 * Description    : this function gets called when the user of the blynk app
                    changes a setting. The blynk app calls the blynk cloud and in turn
                    it calls the functions BLYNK_WRITE()
 * Return         : none
 *******************************************************************************/
void flagSettingsHaveChanged()
{
  settingsHaveChanged = true;
  settingsHaveChanged_timer = 0;

}
/*******************************************************************************
 structure for writing thresholds in eeprom
 https://docs.particle.io/reference/firmware/photon/#eeprom
*******************************************************************************/
//randomly chosen value here. The only thing that matters is that it's not 255
// since 255 is the default value for uninitialized eeprom
// I used 137 and 138 in version 0.21 already
#define EEPROM_VERSION 141
#define EEPROM_ADDRESS 0
struct EepromMemoryStructure {
  uint8_t version = EEPROM_VERSION;
  uint8_t homeMinTemp;
  uint8_t awayMinTemp;
  uint8_t homeMaxTemp;
  uint8_t awayMaxTemp;
  uint8_t internalFan;
  uint8_t internalMode;
};
EepromMemoryStructure eepromMemory;
/*******************************************************************************
 * Function Name  : readFromEeprom
 * Description    : retrieves the settings from the EEPROM memory
 * Return         : none
 *******************************************************************************/
void readFromEeprom()
{
  EepromMemoryStructure myObj;
  EEPROM.get(EEPROM_ADDRESS, myObj);

  //verify this eeprom was written before
  // if version is 255 it means the eeprom was never written in the first place, hence the
  // data just read with the previous EEPROM.get() is invalid and we will ignore it
  if ( myObj.version == EEPROM_VERSION ) {

    homeMinTemp = float( myObj.homeMinTemp );
    newHomeMinTemp = homeMinTemp;
    homeMinTempString = float2string(homeMinTemp);

    awayMinTemp = float( myObj.awayMinTemp );
    newAwayMinTemp = awayMinTemp;
    awayMinTempString = float2string(awayMinTemp);

    homeMaxTemp = float( myObj.homeMaxTemp );
    newHomeMaxTemp = homeMaxTemp;
    homeMaxTempString = float2string(homeMaxTemp);

    awayMaxTemp = float( myObj.awayMaxTemp );
    newAwayMaxTemp = awayMaxTemp;
    awayMaxTempString = float2string(awayMaxTemp);

    internalMode = convertIntToMode( myObj.internalMode );
    externalMode = internalMode;

    //these variables are false at boot
    if ( myObj.internalFan == 1 ) {
      internalFan = true;
      externalFan = true;
    }

    // Particle.publish(APP_NAME, "DEBUG: read settings from EEPROM: " + String(myObj.homeMinTemp)
    Particle.publish(APP_NAME, "read:" + internalMode + "-" + String(internalFan) + "-" + String(homeMinTemp), 60, PRIVATE);

  }

}

/*******************************************************************************
 * Function Name  : saveSettings
 * Description    : in this function we wait a bit to give the user time
                    to adjust the right value for them and in this way we try not
                    to save in EEPROM at every little change.
                    Remember that each eeprom writing cycle is a precious and finite resource
 * Return         : none
 *******************************************************************************/
 #define SAVE_SETTINGS_INTERVAL 10000
void saveSettings() {
  //if the thermostat is initializing, get out of here
  if ( thermostatStateMachine.isInState(initState) ) {
    return;
  }

  //if no settings were changed, get out of here
  if (not settingsHaveChanged) {
    return;
  }

  //if settings have changed, is it time to store them?
  if (settingsHaveChanged_timer < SAVE_SETTINGS_INTERVAL) {
    return;
  }

  //reset timer
  settingsHaveChanged_timer = 0;
  settingsHaveChanged = false;

  //store thresholds in the struct type that will be saved in the eeprom
  eepromMemory.version = EEPROM_VERSION;
  
  eepromMemory.homeMinTemp = uint8_t(homeMinTemp);
  eepromMemory.awayMinTemp = uint8_t(awayMinTemp);
  eepromMemory.homeMaxTemp = uint8_t(homeMaxTemp);
  eepromMemory.awayMaxTemp = uint8_t(awayMaxTemp);

  eepromMemory.internalMode = convertModeToInt(internalMode);
  eepromMemory.internalFan = 0;
  if ( internalFan ) {
    eepromMemory.internalFan = 1;
  }

  //then save
  EEPROM.put(EEPROM_ADDRESS, eepromMemory);

  // Particle.publish(APP_NAME, "stored:" + eepromMemory.internalMode + "-" + String(eepromMemory.internalFan) + "-" + String(eepromMemory.homeMinTemp) , 60, PRIVATE);
  Particle.publish(APP_NAME, "stored:" + internalMode + "-" + String(internalFan) + "-" + String(homeMinTemp), 60, PRIVATE);

}

/*******************************************************************************
 * Function Name  : convertIntToMode
 * Description    : converts the int mode (saved in the eeprom) into the String mode
 * Return         : String
 *******************************************************************************/
String convertIntToMode( uint8_t mode )
{
  if ( mode == 1 ){
    return MODE_HEAT;
  }
  if ( mode == 2 ){
    return MODE_COOL;
  }

  //in all other cases
  return MODE_OFF;

}

/*******************************************************************************
 * Function Name  : convertModeToInt
 * Description    : converts the String mode into the int mode (to be saved in the eeprom)
 * Return         : String
 *******************************************************************************/
uint8_t convertModeToInt( String mode )
{
  if ( mode == MODE_HEAT ){
    return 1;
  }
  if ( mode == MODE_COOL ){
    return 2;
  }

  //in all other cases
  return 0;

}



