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

#include "application.h"
#include "elapsedMillis.h"
#include "PietteTech_DHT.h"
#include "FiniteStateMachine.h"

#define APP_NAME "Thermostat"
String VERSION = "Version 0.11";
/*******************************************************************************
 * changes in version 0.09:
       * reorganized code to group functions
       * added minimum time to protect on-off on the fan and the heating element
          in function heatingUpdateFunction()
 * changes in version 0.10:
       * added temperatureDifference to fix DHT measurements with existing thermostat
       * reduced END_OF_CYCLE_TIMEOUT to one sec since my HVAC controller
          takes care of running the fan for a minute to evacuate the heat/cold
          from the vents
       * added pushbullet notifications for heating on/off
       * added fan on/off setting via a cloud function
 * changes in version 0.11:
      * added more pushbullet notifications and commented out publish() in other cases
*******************************************************************************/

#define PUSHBULLET_NOTIF "pushbulletGUST"

/*******************************************************************************
 initialize FSM states with proper enter, update and exit functions
*******************************************************************************/
State initState = State( initEnterFunction, initUpdateFunction, initExitFunction );
State idleState = State( idleEnterFunction, idleUpdateFunction, idleExitFunction );
State heatingState = State( heatingEnterFunction, heatingUpdateFunction, heatingExitFunction );
State endOfCycleState = State( endOfCycleEnterFunction, endOfCycleUpdateFunction, endOfCycleExitFunction );

//initialize state machine, start in state: Idle
FSM thermostatStateMachine = FSM(initState);

//milliseconds for the init cycle, so temperature samples get stabilized
//this should be in the order of the 5 minutes: 5*60*1000==300000
//for now, I will use 1 minute
#define INIT_TIMEOUT 60000
elapsedMillis initTimer;

//milliseconds to leave the fan on when the target temp has been reached
//this evacuates the heat or the cold air from vents
#define END_OF_CYCLE_TIMEOUT 1000
elapsedMillis endOfCycleTimer;

//minimum number of milliseconds to leave the heating element on
// to protect on-off on the fan and the heating element
#define MINIMUM_ON_TIMEOUT 5000
elapsedMillis minimumOnTimer;

/*******************************************************************************
 IO mapping
*******************************************************************************/
// D0 : relay: fan
// D1 : relay: heat
// D2 : relay: cold
// D4 : DHT22
// D3, D5~D7 : unused
// A0~A7 : unused
int fan = D0;
int heat = D1;
int cold = D2;
//TESTING_HACK
int fanOutput;
int heatOutput;
int coldOutput;

/*******************************************************************************
 DHT sensor
*******************************************************************************/
#define DHTTYPE  DHT22                // Sensor type DHT11/21/22/AM2301/AM2302
#define DHTPIN   4                    // Digital pin for communications
#define DHT_SAMPLE_INTERVAL   30000   // Sample room temperature every 30 seconds
void dht_wrapper(); // must be declared before the lib initialization
PietteTech_DHT DHT(DHTPIN, DHTTYPE, dht_wrapper);
bool bDHTstarted;       // flag to indicate we started acquisition
elapsedMillis dhtSampleInterval;

/*******************************************************************************
 thermostat related declarations
*******************************************************************************/
//temperature related variables - internal
float targetTemp = 19.0;
float currentTemp = 20.0;
float currentHumidity = 0.0;
float margin = 0.25;
//DHT difference with real temperature (if none set to zero)
//use this variable to fix DHT measurements with your existing thermostat
float temperatureDifference = -1.6;

//temperature related variables - to be exposed in the cloud
String targetTempString = String(targetTemp); //String to store the target temp so it can be exposed and set
String currentTempString = String(currentTemp); //String to store the sensor's temp so it can be exposed
String currentHumidityString = String(currentHumidity); //String to store the sensor's humidity so it can be exposed

//fan status: false=off, true=on
bool fanStatus = false;

//TESTING_HACK
// this allows me to system test the project
bool testing = false;


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
  pinMode(cold, OUTPUT);
  myDigitalWrite(fan, LOW);
  myDigitalWrite(heat, LOW);
  myDigitalWrite(cold, LOW);

  //declare cloud variables
  //https://docs.particle.io/reference/firmware/photon/#particle-variable-
  //Currently, up to 10 cloud variables may be defined and each variable name is limited to a maximum of 12 characters
  if (Particle.variable("targetTemp", targetTempString)==false) {
    Particle.publish(APP_NAME, "ERROR: Failed to register variable targetTemp", 60, PRIVATE);
  }
  if (Particle.variable("currentTemp", currentTempString)==false) {
    Particle.publish(APP_NAME, "ERROR: Failed to register variable currentTemp", 60, PRIVATE);
  }
  if (Particle.variable("humidity", currentHumidityString)==false) {
    Particle.publish(APP_NAME, "ERROR: Failed to register variable humidity", 60, PRIVATE);
  }

  //declare cloud functions
  //https://docs.particle.io/reference/firmware/photon/#particle-function-
  //Currently the application supports the creation of up to 4 different cloud functions.
  // If you declare a function name longer than 12 characters the function will not be registered.
  //user functions
  if (Particle.function("setTargetTmp", setTargetTemp)==false) {
     Particle.publish(APP_NAME, "ERROR: Failed to register function setTargetTemp", 60, PRIVATE);
  }
  if (Particle.function("setFan", setFan)==false) {
     Particle.publish(APP_NAME, "ERROR: Failed to register function setFan", 60, PRIVATE);
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

}

// This wrapper is in charge of calling the DHT sensor lib
void dht_wrapper() { DHT.isrCallback(); }


/*******************************************************************************
 * Function Name  : loop
 * Description    : this function runs continuously while the project is running
 *******************************************************************************/
void loop() {

  //this function reads the temperature of the DHT sensor
  readTemperature();

  //this function updates the FSM
  // the FSM is the heart of the thermostat - all actions are defined by its states
  thermostatStateMachine.update();

}

/*******************************************************************************
 * Function Name  : setTargetTemp
 * Description    : sets the target temperature of the thermostat
                    newTargetTemp has to be a valid float value, or no new target temp will be set
 * Return         : 0, or -1 if it fails to convert the temp to float
 *******************************************************************************/
int setTargetTemp(String newTargetTemp)
{
  float tmpFloat = newTargetTemp.toFloat();
  //update the target temp only in the case the conversion to float works
  // (toFloat returns 0 if there is a problem in the conversion)
  // sorry, if you wanted to set 0 as the target temp, you can't :)
  if ( tmpFloat > 0 ) {
    targetTemp = tmpFloat;
    targetTempString = String(targetTemp);
    Particle.publish(APP_NAME, "New target temp: " + targetTempString, 60, PRIVATE);
    return 0;
  } else {
    Particle.publish(APP_NAME, "ERROR: Failed to set new target temp to " + newTargetTemp, 60, PRIVATE);
    return -1;
  }
}

/*******************************************************************************
 * Function Name  : readTemperature
 * Description    : reads the temperature of the DHT22 sensor at every DHT_SAMPLE_INTERVAL
                    if testing the app, it returns right away
 * Return         : 0
 *******************************************************************************/
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

  //still acquiring sample? go away
  if (DHT.acquiring()) {
    return 0;
  }

  //sample acquired, adjust DHT difference if any
  float tmpTemperature = (float)DHT.getCelsius();
  tmpTemperature = tmpTemperature + temperatureDifference;

  //sample acquired - go ahead and store temperature and humidity in internal variables
  publishTemperature( tmpTemperature, (float)DHT.getHumidity() );

  //reset the sample flag so we can take another
  bDHTstarted = false;

  return 0;
}

/*******************************************************************************
 * Function Name  : publishTemperature
 * Description    : the temperature/humidity passed as parameters get stored in internal variables
                    and then published
 * Return         : 0
 *******************************************************************************/
int publishTemperature( float temperature, float humidity ) {

  char currentTempChar[32];
  currentTemp = temperature;
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
  Particle.publish(APP_NAME, "Home temperature: " + currentTempString, 60, PRIVATE);
  Particle.publish(APP_NAME, "Home humidity: " + currentHumidityString, 60, PRIVATE);

  return 0;
}


/*******************************************************************************
********************************************************************************
********************************************************************************
 FINITE STATE MACHINE FUNCTIONS
********************************************************************************
********************************************************************************
*******************************************************************************/
void initEnterFunction(){
  //Particle.publish(APP_NAME, "initEnterFunction", 60, PRIVATE);
  //start the timer of this cycle
  initTimer = 0;
}
void initUpdateFunction(){
  //time is up?
  if (initTimer > INIT_TIMEOUT) {
    thermostatStateMachine.transitionTo(idleState);
  }
}
void initExitFunction(){
  //Particle.publish(APP_NAME, "initExitFunction", 60, PRIVATE);
}

void idleEnterFunction(){
  //Particle.publish(APP_NAME, "idleEnterFunction", 60, PRIVATE);
  //turn off the fan only if fan was not set on manually with setFan(on)
  if ( fanStatus == false ) {
    myDigitalWrite(fan, LOW);
  }
  myDigitalWrite(heat, LOW);
  myDigitalWrite(cold, LOW);
}
void idleUpdateFunction(){
  //set the fan output to the fanStatus ONLY in this state of the FSM
  // since other states might need the fan on
  //set it off only if it was on and fanStatus changed to false
  if ( fanStatus == false and fanOutput == HIGH ) {
    myDigitalWrite(fan, LOW);
  }
  //set it on only if it was off and fanStatus changed to true
  if ( fanStatus == true and fanOutput == LOW ) {
    myDigitalWrite(fan, HIGH);
  }

  if ( currentTemp <= (targetTemp - margin) ) {
    //Particle.publish(APP_NAME, "Starting to heat", 60, PRIVATE);
    thermostatStateMachine.transitionTo(heatingState);
  }
}
void idleExitFunction(){
  //Particle.publish(APP_NAME, "idleExitFunction", 60, PRIVATE);
}

void heatingEnterFunction(){
  //Particle.publish(APP_NAME, "heatingEnterFunction", 60, PRIVATE);
  Particle.publish(PUSHBULLET_NOTIF, "Heat on", 60, PRIVATE);
  myDigitalWrite(fan, HIGH);
  myDigitalWrite(heat, HIGH);
  myDigitalWrite(cold, LOW);

  //start the minimum timer of this cycle
  minimumOnTimer = 0;
}
void heatingUpdateFunction(){
  //is minimum time up?
  if (minimumOnTimer < MINIMUM_ON_TIMEOUT) {
    //not yet, so get out of here
    return;
  }

  if ( currentTemp >= (targetTemp + margin) ) {
    //Particle.publish(APP_NAME, "Desired temperature reached", 60, PRIVATE);
    Particle.publish(PUSHBULLET_NOTIF, "Desired temperature reached", 60, PRIVATE);
    thermostatStateMachine.transitionTo(endOfCycleState);
  }
}
void heatingExitFunction(){
  //Particle.publish(APP_NAME, "heatingExitFunction", 60, PRIVATE);
  Particle.publish(PUSHBULLET_NOTIF, "Heat off", 60, PRIVATE);
  myDigitalWrite(fan, HIGH);
  myDigitalWrite(heat, LOW);
  myDigitalWrite(cold, LOW);
}

void endOfCycleEnterFunction(){
  //Particle.publish(APP_NAME, "endOfCycleEnterFunction", 60, PRIVATE);
  myDigitalWrite(fan, HIGH);
  myDigitalWrite(heat, LOW);
  myDigitalWrite(cold, LOW);

  //start the timer of this cycle
  endOfCycleTimer = 0;
}
void endOfCycleUpdateFunction(){
  //time is up?
  if (endOfCycleTimer > END_OF_CYCLE_TIMEOUT) {
    thermostatStateMachine.transitionTo(idleState);
  }
}
void endOfCycleExitFunction(){
  //Particle.publish(APP_NAME, "endOfCycleExitFunction", 60, PRIVATE);
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
 * Description    : sets the testing variable to true - this enables me to override
                     the temperature read by the DHT sensor.
                    this is a hack that allows me to system test the project
 * Return         : 0
 *******************************************************************************/
int setTesting(String dummy)
{
  testing = true;
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
  // int cold = D2;
  return coldOutput*4 + heatOutput*2 + fanOutput*1;
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
    //Particle.publish(APP_NAME, "New current temp: " + currentTempString, 60, PRIVATE);
    Particle.publish(PUSHBULLET_NOTIF, "New current temp: " + currentTempString, 60, PRIVATE);
    return 0;
  } else {
    //Particle.publish(APP_NAME, "ERROR: Failed to set new current temp to " + newCurrentTemp, 60, PRIVATE);
    Particle.publish(PUSHBULLET_NOTIF, "ERROR: Failed to set new current temp to " + newCurrentTemp, 60, PRIVATE);
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
  }
  if (input == heat){
    heatOutput = status;
  }
  if (input == cold){
    coldOutput = status;
  }
}

/*******************************************************************************
 * Function Name  : setFan
 * Description    : sets the status of the Fan on or off
 * Return         : 0, or -1 if the parameter does not match on or off
 *******************************************************************************/
int setFan(String status)
{
  //update the fan status only in the case the status is on or off
  if ( status == "on" ) {
    fanStatus = true;
    Particle.publish(PUSHBULLET_NOTIF, "Fan on", 60, PRIVATE);
    return 0;
  }
  if ( status == "off" ) {
    fanStatus = false;
    Particle.publish(PUSHBULLET_NOTIF, "Fan off", 60, PRIVATE);
    return 0;
  }

  return -1;
}
