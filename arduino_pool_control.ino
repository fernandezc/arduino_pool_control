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
#define TEMPERATURE_MEASUREMENT_INTERVAL 300000     // measurement every 5 min


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
#define NPERIODS 960                       // number of periods for filtration during 24h

unsigned long previous_measured=0;         // store last time temperature was measured
unsigned long previous_pump_start=0;       // store last time pump was running
unsigned long pump_working_time=0;         // will store the cumulative working time of the pump
unsigned long starting_time=0;             // will store the starting time of each period of the day
                                           // (we do not have the actual time so the start is arbitrary)
unsigned long operating_time;              // store the pump operating time to execute

bool frost_protection=false;               // Should we protect against freezing
bool wintering=false;                      // Should we set up active wintering 
float water_temperature;
float air_temperature;

EthernetServer serveur(4200);              // define a server listening to port 4200

float getTemperature(int index=0)
{
  float temperature;
  // call sensors.requestTemperatures() to issue a global temperature 
  // request to all devices on the bus 
  sensors.requestTemperatures(); // Send the command to get temperature readings 
  temperature = sensors.getTempCByIndex(index); 
  // Why "byIndex"?  
  // You can have more than one DS18B20 on the same bus.  
  // 0 refers to the first IC on the wire

  Serial.print(F("Current "));
  Serial.print(TSENSOR[index]);
  Serial.print(F(" temperature is "));
  Serial.print(temperature);
  Serial.println(F(" degC"));

  return temperature;
}

// **************************************************************************
// Functions
// **************************************************************************

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
    optime = MS_DAY;
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
    optime = (unsigned long) (water_T*MS_HOUR)/(divider);        // Divide temperature by 2 (or 3 during winter)
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

  return optime;
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

void repondre(EthernetClient client) 
{
  // La fonction prend un client en argument

  float water_temperature = getTemperature(INDEX_WATER_SENSOR);
  float air_temperature = getTemperature(INDEX_AIR_SENSOR);

  // Quelqu'un est connecté !
  Serial.println(F("\nRéponse au client !")); // debug

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


// **************************************************************************
// SETUP
// **************************************************************************

void setup(void)
{
  // Start the Serial port for debugging
  Serial.begin(115200);

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
  serveur.begin();
    
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
  water_temperature = getTemperature(INDEX_WATER_SENSOR);
  air_temperature = getTemperature(INDEX_AIR_SENSOR);

  // Check wintering mode 
  winteringModeChecking(water_temperature);
  
  // Initialize operating time
  operating_time = operatingTime(water_temperature, air_temperature);

} 

// **************************************************************************
// LOOP
// **************************************************************************

void loop(void) 
{ 

  // current time for this loop
  unsigned long current_time = millis();

  // variables used for reading message from client
  char *url = (char *)malloc(100); // array to store url chars
  char url_index = 0; 

  // Look for a connected client
  EthernetClient client = serveur.available();
  
  if (client) 
  {
    
    Serial.println(F("Client connexion !"));
    url = ""; 
    url_index = 0;

    while(client.connected()) 
    { // While the client is connected
    
      if(client.available()) 
      { // Something to say ? 
        // traitement des infos du client
        
        char carlu = client.read(); //on lit ce qu'il raconte
        Serial.print(carlu);
        /*
        if(carlu != '\n') { // On est en fin de chaîne ?
          // non ! alors on stocke le caractère
          url[url_index] = carlu;
          url_index++;
        } 
        else 
        {
          // on a fini de lire ce qui nous intéresse
          // on marque la fin de l'url (caractère de fin de chaîne)
          url[url_index] = '\0';
          // boolean ok = interpreter(); // essaie d'interpréter la chaîne
          // if(ok) {
          // tout s'est bien passé = on met à jour les broches
          //  action();
        }
        // Serial.println(url);
        */
        // et dans tout les cas on répond au client
        repondre(client);
        
        // on quitte le while
        break;
      } 
    } 
    
    // Donne le temps au client de prendre les données
    delay(100);

    // Ferme la connexion avec le client
    client.stop();
    Serial.println(F("Deconnexion !"));
  }
}

/********************************************************************/ 

/********************************************************************/ 
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

/********************************************************************/ 
boolean interpreter() {

  /*
  // On commence par mettre à zéro tous les états
  etats[0] = LOW;
  etats[1] = LOW;
  etats[2] = LOW;
  pwm = 0;

  // Puis maintenant on va chercher les caractères/marqueurs un par un.
  index = 0; // Index pour se promener dans la chaîne (commence à 4 pour enlever "GET "
  while(url[index-1] != 'b' && url[index] != '=') { // On commence par chercher le "b="
    index++; // Passe au caractère suivant
    if(index == 100) {
      // On est rendu trop loin !
      Serial.println("Oups, probleme dans la recherche de 'b='");
      return false;
    }
  }
  // Puis on lit jusqu’à trouver le '&' séparant les broches de pwm
  while(url[index] != '&') { // On cherche le '&'
    if(url[index] >= '3' && url[index] <= '5') {
      // On a trouvé un chiffre identifiant une broche
      char broche = url[index]-'0'; // On ramène ça au format décimal
      etats[broche-3] = HIGH; // Puis on met la broche dans un futur état haut
    }
    index++; // Passe au caractère suivant
    if(index == 100) {
      // On est rendu trop loin !
      Serial.println("Oups, probleme dans la lecture des broches");
      return false;
    }
    // NOTE : Les virgules séparatrices sont ignorées
  }
  // On a les broches, reste plus que la valeur de la PWM
  // On cherche le "p="
  while(url[index-1] != 'p' && url[index] != '=' && index<100) {
    index++; // Passe au caractère suivant
    if(index == 100) {
      // On est rendu trop loin !
      Serial.println("Oups, probleme dans la recherche de 'p='");
      return false;
    }
  }
  // Maintenant, on va fouiller jusqu'a trouver un espace
  while(url[index] != ' ') { // On cherche le ' ' final
    if(url[index] >= '0' && url[index] <= '9') {
      // On a trouve un chiffre !
      char val = url[index]-'0'; // On ramene ca au format decimal
      pwm = (pwm*10) + val; // On stocke dans la pwm
    }
    index++; // Passe au caractère suivant
    if(index == 100) {
      // On est rendu trop loin !
      Serial.println("Oups, probleme dans la lecture de la pwm");
      return false;
    }
    // NOTE : Les virgules séparatrices sont ignorées
  }
  */
  
  // Rendu ici, on a trouvé toutes les informations utiles !
  return true;
}

/********************************************************************/
void action() {
  // On met à jour nos broches

  /*
  digitalWrite(3, etats[0]);
  digitalWrite(4, etats[1]);
  digitalWrite(5, etats[2]);
  */
}

/********************************************************************/ 
void controle(unsigned long current_time)
{
  unsigned long period;
  unsigned int pump_state;
  unsigned int electrolyse_state;
  unsigned int ph_state;
  unsigned int previous_pump_state;
  
  // Read current water and air temperature
  float water_temperature = getTemperature(INDEX_WATER_SENSOR);
  float air_temperature = getTemperature(INDEX_AIR_SENSOR);
  
  // Determine duration for a day
  operating_time = operatingTime(water_temperature, air_temperature)/NPERIODS;

  // determine if we are in the period as in the last loop
  // else reinitialize to a new period
  period = MS_DAY/NPERIODS;
  
  if (current_time - starting_time >= period)
  {
    // Start a new day and reset pump working time
    starting_time = current_time;
    previous_pump_start = current_time;
    pump_working_time = 0;
    
    Serial.print(F("\n* Starting a new period "));
    Serial.println(F(" ...\n"));
  }
  // delay(1000);
  // Serial.print(pump_working_time); Serial.print("\t"); Serial.println(operating_time);

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
      Serial.println(F("Pump is now stopped."));
    }  
  }
  
  // Both the electrolyse and PH Meter require pump is running
  // Electrolyseur (close when temperature is too low or pump is stopped)
  if (water_temperature <= MIN_ELECTROLYSIS_TEMPERATURE || pump_state == STOPPED) 
  {
    if (digitalRead(ELECTROLYSE_RELAY) == RUNNING)
    {
      stop(ELECTROLYSE_RELAY);
      Serial.println(F("Electrolysis is now stopped."));
    }
  }
  else
  {
    if (digitalRead(ELECTROLYSE_RELAY) == STOPPED)
    {
      start(ELECTROLYSE_RELAY);
      Serial.println(F("Electrolysis is now running."));   
    } 
  }  
  
  // PH meter (close when temperature is too low or pump is stopped)
  if (water_temperature <= MIN_PH_METER_TEMPERATURE || pump_state == STOPPED) 
  {
    if (digitalRead(PH_RELAY) == RUNNING)
    {
      stop(PH_RELAY);
      Serial.println(F("Ph_Meter is now stopped."));
    }
  }
  else
  { 
    if (digitalRead(PH_RELAY) == STOPPED)
    {
      start(PH_RELAY);
      Serial.println(F("Ph_Meter is now running."));
    }
  }

}



