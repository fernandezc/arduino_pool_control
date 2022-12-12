// **************************************************************************
// Pool filtration controller 
//
// The pump is running for a time = T/2 except in wintering mode when T/3.
// Below an air temperature of 1°C , the pump is running to avoid freezing.
// 
// The running time is dispached in 6 hours period
// **************************************************************************

#define DEBUG 1
#define MOCK 0 // This is to mock eth response for test without the eth card attached 

// This two libraries are needed for the temperature sensors
#include <OneWire.h> 
#include <DallasTemperature.h>

// This two libraries are needed for the ethernet shield
#include <SPI.h>
#include <Ethernet.h>

// DS18B20 temperature sensor setting 
#define ONE_WIRE_BUS  7      // pin 7 for Temperature data wire
OneWire oneWire(ONE_WIRE_BUS); 
DallasTemperature sensors(&oneWire);
const char *TSENSOR[] = {"water", "air"};
#define TEMPERATURE_PRECISION 11
#define FROST_TEMPERATURE 1.0
#define MAX_WINTERING_TEMPERATURE 13.0
#define MIN_ELECTROLYSIS_TEMPERATURE 15.0
#define MIN_PH_METER_TEMPERATURE 12.0

#define INDEX_WATER_SENSOR 0
#define INDEX_AIR_SENSOR 1
#define UPDATE_INTERVAL 10000     // update contoller every 1 sec

// Pin connections for relays(do not use pin 4)
#define PUMP_RELAY 3         // Pin 3 for pump relay
#define ELECTROLYSE_RELAY 5  // Pin 5 for electrolysis relay
#define PH_RELAY 6           // Pin 6 for ph relay
const char *RELAY[] = { "none", "none", "none", "pump_relay", "none", "electrolyse_relay", "ph_relay" };
const char *STATUS[] = {"off", "on"};
const char *MODES[] = {"Off", "Auto", "Forcé"};
#define RUNNING HIGH
#define STOPPED LOW

// Other variables and constants

#define MS_DAY  86400000
#define MS_HOUR 3600000
#define NPERIODS 8                         // number of periods for filtration during 24h

unsigned int mode=1;                       // operating mode = AUTO (0=OFF, 2=FORCED)

unsigned long previous_measured=0;         // store last time temperature was measured
unsigned long previous_pump_start=0;       // store last time pump was running
unsigned long pump_working_time=0;         // will store the cumulative working time of the pump
unsigned long starting_update_time;        // will store the starting time for updating measurments
unsigned long starting_time;               // will store the starting time of each period of the day
                                           // (we do not have the actual time so the start is arbitrary)
unsigned long operating_time;              // store the pump operating time to execute

bool frost_protection=false;               // Should we protect against freezing
bool wintering=false;                      // Should we set up active wintering 
float water_temperature = 5;
float air_temperature = 5;

EthernetServer server(4200);              // define a server listening to port 4200

// **************************************************************************
// Functions
// **************************************************************************

float getTemperature(int index, boolean display)
{
  float temperature;
  // call sensors.requestTemperatures() to issue a global temperature 
  // request to all devices on the bus 
  sensors.requestTemperatures(); // Send the command to get temperature readings 
  temperature = sensors.getTempCByIndex(index); 
  // Why "byIndex"?  
  // You can have more than one DS18B20 on the same bus.  
  // 0 refers to the first IC on the wire

  if (temperature == DEVICE_DISCONNECTED_C)
  {
    if (display && DEBUG) Serial.println(F("Sensor disconnnected."));
    if (index==0) temperature = water_temperature;
    if (index==1) temperature = air_temperature;
  }

  if (display && DEBUG)
  {  
    Serial.print(F("Current "));
    Serial.print(TSENSOR[index]);
    Serial.print(F(" temperature is "));
    Serial.print(temperature);
    Serial.println(F(" degC"));
  }

  return temperature;
}

// Wintering mode checking 
// -----------------------
void winteringModeChecking(float temperature)
{
  if (temperature < MAX_WINTERING_TEMPERATURE && !wintering) 
  {
    // Pass automatically in wintering mode if not already
    wintering = true; 

    if (DEBUG) Serial.print(F("Enter wintering mode as water temperature < "));
    if (DEBUG) Serial.print(MAX_WINTERING_TEMPERATURE);
    if (DEBUG) Serial.println(F(" degC"));
  } 
  else if (temperature >= MAX_WINTERING_TEMPERATURE && wintering)
  {
    // exit wintering mode
    if (DEBUG) Serial.print(F("Exit wintering mode as water temperature >= "));
    if (DEBUG) Serial.print(MAX_WINTERING_TEMPERATURE);
    if (DEBUG) Serial.println(F(" degC"));
    wintering = false;
  }
  else if (temperature >= MAX_WINTERING_TEMPERATURE)
  {
    wintering = false;    
  }
}

unsigned long operatingTime(float water_T, float air_T)
{
  float divider;
  unsigned long optime;
  bool old_frost_flag; 
  
  old_frost_flag = frost_protection;  // get the current frost protection flag

  if (water_T > MAX_WINTERING_TEMPERATURE)
  {
    divider = 2;
  }
  else 
  {
    if (water_T > MAX_WINTERING_TEMPERATURE/2)
    {
      divider = 3;
    }
    else
    {
      divider = 4;
    }
  }
  optime = (unsigned long) (water_T*MS_HOUR)/(divider);        // Divide temperature by 2 (or 3 during winter)
    
  // special situation during frozing air temperature
  if (air_T < FROST_TEMPERATURE)
  {
    optime = MS_DAY;
    frost_protection = true;
    if (!old_frost_flag)              // frost protection flag just set - printing only once. 
    {
      if (DEBUG) Serial.println(F("FROST PROTECTION MODE - FORCE CONTINUOUS PUMP CIRCULATION!"));
    }
  }
  else
  { 
    frost_protection = false;
    if (old_frost_flag)      // frost protection flag just set - printing only once. 
    {
      if (DEBUG) Serial.println(F("EXIT FROST PROTECTION MODE - RETURN TO NORMAL MODE!"));
    }    
  }

  // look if the mode is not auto
  if (!frost_protection)
  {
    if (mode == 0) // OFF
    {
      // Stopped
      if (optime != 0)
      {      
        optime = 0;
      }
    }
    else if (mode == 2) // CONTINUOUS
    {
      if (operating_time != 2)
      { 
        optime = MS_DAY;
      }
    }    
  }

  if (DEBUG) {
    Serial.print(F("Total operating time per period of 24h is set to "));
    Serial.print((float)optime/MS_HOUR);
    Serial.print("h (");
    Serial.print((float)optime*100/MS_DAY);
    Serial.println("% of the time)");
  }
  return optime;
}

void stopAllRelays(void)
{
  stop(PUMP_RELAY);
  stop(ELECTROLYSE_RELAY);
  stop(PH_RELAY);
}

unsigned int stop(int relay) {
  if (DEBUG) Serial.print(F("Stop "));
  if (DEBUG) Serial.print(RELAY[relay]);
  if (DEBUG) Serial.print(F(" ... "));
  if (DEBUG) digitalWrite(relay, STOPPED);
  delay(1000);
  int state = digitalRead(relay);
  if (DEBUG) Serial.println(STATUS[state]);
  return state;
}

unsigned int start(int relay) {
  if (DEBUG) Serial.print(F("Start "));
  if (DEBUG) Serial.print(RELAY[relay]);
  if (DEBUG) Serial.print(F(" ... "));
  if (DEBUG) digitalWrite(relay, RUNNING);
  delay(1000);
  int state = digitalRead(relay);
  if (DEBUG) Serial.println(STATUS[state]);
  return state;
}

void sendFeedback(EthernetClient client) 
{
  // La fonction prend un client en argument

  // Quelqu'un est connecté !
  // On fait notre en-tête
  // Tout d'abord le code de réponse 200 = réussite
  // Puis le type mime du contenu renvoyé, du json
  // Et on autorise le cross origin
  // On envoie une ligne vide pour signaler la fin du header

  client.println(F("HTTP/1.1 200 OK"));
  client.println(F("Content-Type: application/json; charset: utf-8"));
  client.println("Connection: close");  // the connection will be closed after completion of the response
  client.println("Refresh: 5");
  client.println(F("Access-Control-Allow-Origin: *"));
  client.println("");
  
  // Puis on commence notre JSON par une accolade ouvrante
  client.println(F("{"));

  // key mode
  client.print(F("\t\"mode\": \""));
  client.print(MODES[mode]);
  client.println(F("\","));

  // key water_temp
  client.print(F("\t\"water_temperature\": "));
  client.print(water_temperature);
  client.println(F(","));
  
  // key air_temp
  client.print(F("\t\"air_temperature\": "));
  client.print(air_temperature);
  client.println(F(","));

  // Relay's status 
  int i;
  int state; 
  for (i=3; i<7; i++)
  {
    if (RELAY[i] != "none")
    {
      client.print(F("\t\""));
      client.print(RELAY[i]);
      client.print(F("\": \""));
      state = digitalRead(i);
      client.print(STATUS[state]);
      client.println(F("\","));
    }
  }

  // key opratingtime
  client.print(F("\t\"operating_time\": "));
  client.print(operating_time);
  client.println(F(","));

  // key periods
  client.print(F("\t\"n_periods\": "));
  client.print(NPERIODS);
  client.println(F(","));

  // key wintering
  client.print(F("\t\"wintering\": "));
  client.print(wintering);
  client.println(F(","));

  // key frost
  client.print(F("\t\"frost_protection\": "));
  client.println(frost_protection);
  
  // Et enfin on termine notre JSON par une accolade fermante
  client.println(F("}"));
}

void checkConnectedClient(void)
{
  char* url = (char *)malloc(100);
  int index;

  // Look for a connected client
  EthernetClient client = server.available();

  if (client) 
  {
    
    url = "";
    index = 0;

    while(client.connected()) 
    { // While the client is connected
    
      if(client.available()) 
      { // Something to say ? 
        
        char c = client.read(); //read it!
        
        if(c != '\n') 
        { 
          url[index] = c;
          index++;
        } 
        else 
        {
          url[index]= '\0'; // end of string

          // Try to interpret the string
          interpreter(url); 

          // In all case, send a feedback to the client
          sendFeedback(client);   
          break;
        }
            
      } 
    }
    delay(10);     
    client.stop();  

  }
/*  else {
    if (MOCK) {
      url = " GET /?mode=2 HTTP/1.1";
      Serial.println(url);
      interpreter(url);
      delay(10000);
    }  
  } */
}

boolean interpreter(char url[]) {
  // TO BE COMPLETED WITH ADDITIONAL COMMAND 

  int index = 10; // Commence à 4 pour enlever GET 
  bool found = false;

  while (!(strcmp(url[index-4], "m") && strcmp(url[index],"=")))
  {
    index++;
    // mode was not found  
    if (index > 100) return false;
  }

  if (index < 100) {
    // found
    char mode = url[index+1]-'0'; 
    if (DEBUG) {
        Serial.print(F("mode = "));
        Serial.println(MODES[mode]);
    }
    // recompute operating time
    operating_time = operatingTime(water_temperature, air_temperature);
    return true;    
  } 



}

void action() {

}

void printDuration(unsigned long optime, unsigned long period)
{
    if (period >= 3600000)
    {
      Serial.print((float)optime/3600/1000);
      Serial.print(F("h per period of "));
      Serial.print((float)period/3600/1000);
      Serial.println(F(" h"));
    }
    
    if (period < 3600000)
    { // minutes
      Serial.print((float)optime/60/1000);
      Serial.print(F(" min per period of "));
      Serial.print((float)period/60/1000);
      Serial.println(F(" min"));
    }

    if (period < 60000)
    { // seconds
      Serial.print((float)optime/1000);
      Serial.print(F("s per period of "));
      Serial.print((float)period/1000);
      Serial.println(F(" s"));
    }   
}

void updatePoolControl(unsigned long current_time)
{
  unsigned long period;
  unsigned int pump_state;
  unsigned int electrolyse_state;
  unsigned int ph_state;
  unsigned int previous_pump_state;
  
  // store last temperatures
  float last_water_temperature = water_temperature;
  float last_air_temperature = air_temperature;
  
  // Read current water and air temperature
  water_temperature = getTemperature(INDEX_WATER_SENSOR, false);  // Do not display temperature if there is no change
  air_temperature = getTemperature(INDEX_AIR_SENSOR, false);
  
  // Determine duration for a day
  // We do this only if temperature have changed more than 0.2 degC or if air temperature go below 1degC
  if (abs(water_temperature - last_water_temperature) > 0.2 || air_temperature < FROST_TEMPERATURE)
  {
    water_temperature = getTemperature(INDEX_WATER_SENSOR, true);   
    air_temperature = getTemperature(INDEX_AIR_SENSOR, true);
    operating_time = operatingTime(water_temperature, air_temperature);
  }
  
  // determine if we are in the period as in the last loop
  // else reinitialize to a new period
  period = MS_DAY/NPERIODS;

  if (current_time - starting_time >= period)
  {
    // Start a new period of the day and reset pump working time
    starting_time = current_time;
    previous_pump_start = current_time;
    pump_working_time = 0;
    if (DEBUG)  Serial.print(F("\n\n* Starting a new period ... "));
  }
  if (pump_working_time < operating_time/NPERIODS)
  {
    pump_state = digitalRead(PUMP_RELAY);
    
    if (pump_state == STOPPED)
    { 
      if (DEBUG) {
        Serial.print(F("Pump must be working for a minimum duration of "));
        printDuration(operating_time/NPERIODS, period);
        Serial.println(F(""));
      }
      if (digitalRead(PUMP_RELAY) == STOPPED)
      {
        pump_state = start(PUMP_RELAY);   
      }
    }
    previous_pump_state = pump_state;
    pump_working_time += current_time - previous_pump_start;
    previous_pump_start = current_time;
  }
  else
  {
    if (digitalRead(PUMP_RELAY) == RUNNING && !frost_protection)
    {
      pump_state = stop(PUMP_RELAY);
      previous_pump_state = pump_state;
    }  
  }
  
  // Both the electrolyse and PH Meter require pump is running
  // Electrolyseur (close when temperature is too low or pump is stopped)
  if (water_temperature <= MIN_ELECTROLYSIS_TEMPERATURE || pump_state == STOPPED || frost_protection) 
  {
    if (digitalRead(ELECTROLYSE_RELAY) == RUNNING)
    {
      stop(ELECTROLYSE_RELAY);
    }
  }
  else
  {
    if (digitalRead(ELECTROLYSE_RELAY) == STOPPED)
    {
      start(ELECTROLYSE_RELAY);
    } 
  }  
  
  // PH meter (close when temperature is too low or pump is stopped)
  if (water_temperature <= MIN_PH_METER_TEMPERATURE || pump_state == STOPPED || frost_protection) 
  {
    if (digitalRead(PH_RELAY) == RUNNING)
    {
      stop(PH_RELAY);
    }
  }
  else
  { 
    if (digitalRead(PH_RELAY) == STOPPED)
    {
      start(PH_RELAY);
    }
  }
}

// **************************************************************************
// SETUP
// **************************************************************************

void setup(void)
{
  // Start the Serial port for debugging
  Serial.begin(9600);

  // Initialize the ethernet card 
  byte mac[] = { 0x90, 0xA2, 0xDA, 0x0E, 0xA5, 0x7E };   // ethernet card MAC address of the shield (must be unique)
  char erreur = 0;
  erreur = Ethernet.begin(mac);
  
  if (erreur == 0) 
  {
    Serial.println("DHCP initialisation failed. Try a static address.");
    IPAddress ip(192,168,0,143);               
    Ethernet.begin(mac, ip);
  }

  // let's wait some time to finish initialisation (1s)
  delay(1000);

  // Start the server (Listening on port 4200)
  if (DEBUG) {
    Serial.print(F("Start server with IP  "));
    Serial.print(Ethernet.localIP());
    Serial.println(F(":4200"));
  }
  server.begin();
    
  // Setup relays
  pinMode(PUMP_RELAY, OUTPUT);
  pinMode(ELECTROLYSE_RELAY, OUTPUT);
  pinMode(PH_RELAY, OUTPUT); 
  // Be sure that all relays are stopped
  // stopAllRelays();

  // Start up the sensor library 
  sensors.begin(); 
  sensors.setResolution(TEMPERATURE_PRECISION);
		
  // Read initial temperature
  water_temperature = getTemperature(INDEX_WATER_SENSOR, true);
  air_temperature = getTemperature(INDEX_AIR_SENSOR, true);

  // Check wintering mode 
  winteringModeChecking(water_temperature);
  
  // Initialize operating time
  operating_time = operatingTime(water_temperature, air_temperature);

  // Initialise start time for measurement interval
  starting_update_time = millis();

  // Initialize start time for fitration period
  starting_time = millis();

  if (DEBUG) Serial.print(F("\n\n* Starting a first period ... "));  
  
} 

// **************************************************************************
// LOOP
// **************************************************************************

void loop(void) 
{ 

  // current time for this loop
  unsigned long current_time = millis();
  
  // Check for a connected client to get or send information
  checkConnectedClient();
 
  // Update the controller periodically (every minutes)
  if ((current_time - starting_update_time) > UPDATE_INTERVAL)
  {
     starting_update_time = current_time; 
     updatePoolControl(current_time);
  }

}
