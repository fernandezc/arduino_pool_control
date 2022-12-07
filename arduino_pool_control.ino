// **************************************************************************
// Pool filtration controller 
//
// The pump is running for a time = T/2 except in wintering mode when T/3.
// Below an air temperature of 1°C , the pump is running to avoid freezing.
// 
// The running time is dispached in 6 hours period
// **************************************************************************

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
const char *TSENSOR[2] = {"water", "air"};
#define FROST_TEMPERATURE 1.0
#define MAX_WINTERING_TEMPERATURE 13.0
#define MIN_ELECTROLYSIS_TEMPERATURE 15.0
#define MIN_PH_METER_TEMPERATURE 12.0

#define INDEX_WATER_SENSOR 0
#define INDEX_AIR_SENSOR 1
#define UPDATE_INTERVAL 10000     // update contoller every 1 min


// Pin connections for relays(do not use pin 4)
#define PUMP_RELAY 3         // Pin 3 for pump relay
#define ELECTROLYSE_RELAY 5  // Pin 5 for electrolysis relay
#define PH_RELAY 6           // Pin 6 for ph relay
const char* RELAY[7] = { "none", "none", "none", "PUMP_RELAY", "none", "ELECTROLYSE_RELAY", "PH_RELAY" };
const char* STATUS[2] = {"Stopped", "Running"};
#define RUNNING HIGH
#define STOPPED LOW

// Other variables and constants

#define MS_DAY  86400000
#define MS_HOUR 3600000
#define NPERIODS 480                       // number of periods for filtration during 24h

unsigned int mode= 1;                      // operating mode = auto (0=on, 2=off)

unsigned long previous_measured=0;         // store last time temperature was measured
unsigned long previous_pump_start=0;       // store last time pump was running
unsigned long pump_working_time=0;         // will store the cumulative working time of the pump
unsigned long starting_update_time;        // will store the starting time for updating measurments
unsigned long starting_time;               // will store the starting time of each period of the day
                                           // (we do not have the actual time so the start is arbitrary)
unsigned long operating_time;              // store the pump operating time to execute

bool frost_protection=false;               // Should we protect against freezing
bool wintering=false;                      // Should we set up active wintering 
float water_temperature;
float air_temperature;

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

  // Mock temperature
  if (index==0) 
  {
    temperature=16.;
  }
  else
  {
    temperature=4.;
  }

  if (display)
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

    Serial.print(F("Enter wintering mode as water temperature < "));
    Serial.print(MAX_WINTERING_TEMPERATURE);
    Serial.println(F(" degC"));
  } 
  else if (temperature >= MAX_WINTERING_TEMPERATURE && wintering)
  {
    // exit wintering mode
    Serial.print(F("Exit wintering mode as water temperature >= "));
    Serial.print(MAX_WINTERING_TEMPERATURE);
    Serial.println(F(" degC"));
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

  if (air_T < FROST_TEMPERATURE)
  {
    operating_time = MS_DAY;
    frost_protection = true;
    if (!old_frost_flag)              // frost protection flag just set - printing only once. 
    {
      Serial.println(F("FROST PROTECTION MODE - FORCE CONTINUOUS PUMP CIRCULATION!"));
    }
  }
  else
  {
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
    operating_time = (unsigned long) (water_T*MS_HOUR)/(divider);        // Divide temperature by 2 (or 3 during winter)
    frost_protection = false;
    if (old_frost_flag)      // frost protection flag just set - printing only once. 
    {
      Serial.println(F("EXIT FROST PROTECTION MODE - RETURN TO NORMAL MODE!"));
    }    
  }

  Serial.print(F("Total operating time per period of 24h is set to "));
  Serial.print((float)operating_time/MS_HOUR);
  Serial.print("h (");
  Serial.print((float)operating_time*100/MS_DAY);
  Serial.println("% of the time)");
  
  return operating_time/NPERIODS;
}

void stopAllRelays(void)
{
  stop(PUMP_RELAY);
  stop(ELECTROLYSE_RELAY);
  stop(PH_RELAY);
}

unsigned int stop(int relay) {
  Serial.print(F("Stop "));
  Serial.print(RELAY[relay]);
  Serial.print(F(" ... "));
  digitalWrite(relay, STOPPED);
  delay(1000);
  int state = digitalRead(relay);
  Serial.println(STATUS[state]);
}

unsigned int start(int relay) {
  Serial.print(F("Start "));
  Serial.print(RELAY[relay]);
  Serial.print(F(" ... "));
  digitalWrite(relay, RUNNING);
  delay(1000);
  int state = digitalRead(relay);
  Serial.println(STATUS[state]);
}

void relayStatus(void)
{
  int i;
  int state; 

  Serial.println("\nRelay's status :");
  for (i=3; i<7; i++)
  {
    if (RELAY[i] != "none")
    {
      Serial.print(RELAY[i]);
      Serial.print(F(" -> "));
      state = digitalRead(i);
      Serial.println(STATUS[state]);
    }
  }
  Serial.println("");
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
  client.println(F("Access-Control-Allow-Origin: *\n"));
  
  // Puis on commence notre JSON par une accolade ouvrante
  client.println(F("{"));

  // On envoie la première clé : "uptime"
  client.print(F("\t\"uptime (ms)\": "));
  // Puis la valeur de l'uptime
  client.print(millis());
  //Une petite virgule pour séparer les clés
  client.println(F(","));
  // Et on envoie la clé nommée "water temperature"
  client.print(F("\t\"water temperature (degC)\": "));
  client.print(water_temperature);
  //Une autre virgule pour séparer les clés
  client.println(F(","));
  // Et on envoie la clé nommée "air temperature"
  client.print(F("\t\"air temperature (degC)\": "));
  client.println(air_temperature);

  // Et enfin on termine notre JSON par une accolade fermante
  client.println(F("}"));
}

void checkConnectedClient(void)
{
  // Look for a connected client
  EthernetClient client = server.available();

  if (client) 
  {
    
    String url = "";
    
    while(client.connected()) 
    { // While the client is connected
    
      if(client.available()) 
      { // Something to say ? 
        
        char c = client.read(); //read it!
        
        if(c != '\n') 
        { 
          url += c;
        } 
        else 
        {
          url += '\0'; // end of string

          // Try to interpret the string
          interpreter(url); 

          // In all case, send a feedback to the client
          sendFeedback(client);
          delay(10);     
          client.stop();    
          break;
        }
            
      } 
    } 
    
  }
}

boolean interpreter(String url) {
  // TO BE COMPLETED WITH ADDITIONAL COMMAND 

  // mode?
  if (url.indexOf("mode=") > 0)
  {
    mode = url.substring(url.indexOf("mode=")+5, url.indexOf("mode=")+6).toInt();
    Serial.print(F("mode = "));
    Serial.println(mode);
    return true;
  }

  return false;
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
    
    Serial.print(F("\n* Starting a new period "));
    Serial.println(F(" ...\n"));
  }
  if (pump_working_time < operating_time)
  {
    pump_state = digitalRead(PUMP_RELAY);
    
    if (pump_state == STOPPED)
    { 
      Serial.print(F("Pump must be working for a minimum duration of "));
      printDuration(operating_time, period);
      
      if (digitalRead(PUMP_RELAY) == STOPPED)
      {
        pump_state = start(PUMP_RELAY);   
        Serial.println(F("Pump is now running."));
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
  if (water_temperature <= MIN_ELECTROLYSIS_TEMPERATURE || pump_state == STOPPED) 
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
  if (water_temperature <= MIN_PH_METER_TEMPERATURE || pump_state == STOPPED) 
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

  Serial.println(F("POOL CONTROLLER"));
  Serial.println(F("***************\n"));

  // Initialize the ethernet card 
  Serial.println(F("Ethernet card init..."));
  byte mac[] = { 0x90, 0xA2, 0xDA, 0x0E, 0xA5, 0x7E };   // ethernet card MAC address of the shield (must be unique)
  char erreur = 0;
  erreur = Ethernet.begin(mac);
  
  if (erreur == 0) 
  {
    Serial.println("DHCP initialisation failed. Try a static address.");
    IPAddress ip(192,168,0,105);               
    Ethernet.begin(mac, ip);
  }

  // let's wait some time to finish initialisation (1s)
  delay(1000);

  // Start the server (Listening on port 4200)
  Serial.print(F("Start server with IP  "));
  Serial.print(Ethernet.localIP());
  Serial.println(F(":4200"));
  server.begin();
    
  // Setup relays
  Serial.println(F("\nInit relay state ..."));
  pinMode(PUMP_RELAY, OUTPUT);
  pinMode(ELECTROLYSE_RELAY, OUTPUT);
  pinMode(PH_RELAY, OUTPUT); 
  // Be sure that all relays are stopped
  // stopAllRelays();

  // Start up the sensor library 
  Serial.println(F("\nStarting temperature measurements ..."));
  sensors.begin(); 

  // Read initial temperature
  water_temperature = getTemperature(INDEX_WATER_SENSOR, true);
  air_temperature = getTemperature(INDEX_AIR_SENSOR, true);

  // Check wintering mode 
  winteringModeChecking(water_temperature);
  
  // Initialize operating time
  operating_time = operatingTime(water_temperature, air_temperature);
  Serial.print(operating_time);

  // Initialise start time for measurement interval
  starting_update_time = millis();

  // Initialize start time for fitration period
  starting_time = millis();
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



