// **************************************************************************
//
// Pool controller 
// ===============
//
// The pump is running for a time = T/2 except in wintering mode when T/3.
// Below an air temperature of 1°C , the pump is running to avoid freezing.
// 
// The running time is dispached in 6 hours period
//
// **************************************************************************

// First we include the needed libraries

// This two libraries are needed for the temperature sensors
#include <OneWire.h> 
#include <DallasTemperature.h>

// This two libraries are needed for the ethernet shield
#include <SPI.h>
#include <Ethernet.h>

// MAC address of the shield (must be unique)
byte mac[] = { 0x90, 0xA2, 0xDA, 0x0E, 0xA5, 0x7E };

// Static IP address of the shield (will be used if DHCP not working)
IPAddress ip(192,168,0,143);

// Initialize the server (Listening on port 4200)
EthernetServer serveur(4200);

// Pin connections
#define ONE_WIRE_BUS  7      // pin 7 for Temperature data wire
#define PUMP_RELAY 2         // Pin 2 for pump relay
#define ELECTROLYSE_RELAY 3  // Pin 3 for electrolysis relay
#define PH_RELAY 4           // Pin 4 for ph relay

// DS18B20 temperature sensor setting
// Setup a oneWire instance to communicate with any OneWire devices  
OneWire oneWire(ONE_WIRE_BUS); 

// Pass our oneWire reference to Dallas Temperature
DallasTemperature sensors(&oneWire);

// Variables and constants
const char* RELAY[3] = { "PUMP_RELAY", "ELECTROLYSE_RELAY", "PH_RELAY" };
const char* STATUS[2] = {"Stopped", "Running"};

#define RUNNING HIGH
#define STOPPED LOW

#define FROST_TEMPERATURE 1.0
#define MAX_WINTERING_TEMPERATURE 12.0
#define MIN_ELECTROLYSIS_TEMPERATURE 15.0
#define MIN_PH_METER_TEMPERATURE 13.0

#define TEMPERATURE_REPORT_INTERVAL 60000          // 1 minute interval
#define TEMPERATURE_MEASUREMENT_INTERVAL 10000     // measurement every 10s

#define MS_DAY  86400000
#define MS_HOUR 3600000
#define NPERIODS 960                               // number of periods for filtration during 24h

float temperature=0.0;                             // temperature de l'eau
float air_temperature=0.0;                         // temperature de l'air exterieur

float sum_of_temperature=0.0;
unsigned int temperature_count=0;
unsigned long previous_reported=0;         // will store last time temperature was reported
unsigned long previous_measured=0;         // will store last time temperature was rmeasured

unsigned long previous_pump_start=0;       // will store last time pump was running
unsigned long pump_working_time=0;         // will store the cumulative working time of the pump
unsigned long previous_time_of_the_day=0;  // store the pseudo starting time of the day 
                                           // (we do not have the actual time so the start is arbitrary)
unsigned long operating_time;
long period_count=1;

bool frost_protection=false;               // Should we protect against freezing
bool wintering=false;                      // Should we set up active wintering 

/********************************************************************/
void winteringModeChecking(float temperature)
{
  if (temperature < MAX_WINTERING_TEMPERATURE && !wintering) 
  {
    // Pass automatically in wintering mode
    wintering = true; 
    Serial.print(F("Enter wintering mode as T < "));
    Serial.print(MAX_WINTERING_TEMPERATURE);
    Serial.println(F(" degC"));
  } 
  else if (temperature >= MAX_WINTERING_TEMPERATURE && wintering)
  {
    Serial.print(F("Exit wintering mode as T >= "));
    Serial.print(MAX_WINTERING_TEMPERATURE);
    Serial.println(F(" degC"));
    wintering = false;
  }
  else if (temperature >= MAX_WINTERING_TEMPERATURE)
  {
    wintering = false;    
  }
}

/********************************************************************/ 
float getTemperature(int index=0)
{
  float temp;
  // call sensors.requestTemperatures() to issue a global temperature 
  // request to all devices on the bus 
  sensors.requestTemperatures(); // Send the command to get temperature readings 
  temp = sensors.getTempCByIndex(index); 
  // Why "byIndex"?  
  // You can have more than one DS18B20 on the same bus.  
  // 0 refers to the first IC on the wire 
  return temp;
}

/********************************************************************/ 
float getAverageTemperature()
{
  unsigned long elapsed;
  unsigned long current_reported=millis(); 
  unsigned long current_measured=millis();
  
  // Measurement 
  elapsed = current_measured - previous_measured; // Elapsed time since last temperature reporting
  if (elapsed>=TEMPERATURE_MEASUREMENT_INTERVAL) 
  {
    previous_measured = current_measured;
    sum_of_temperature += getTemperature(0);  // Cumul of temperature de l'eau
    temperature_count += 1;
  }

  // Reporting
  elapsed = current_reported-previous_reported; // Elapsed time since last temperature reporting
  if (elapsed>=TEMPERATURE_REPORT_INTERVAL) 
  {
    previous_reported = current_reported;

    Serial.println(F("\nComputing average water temperatures...")); 
    Serial.print(F("Temperature is "));
    temperature = sum_of_temperature /  temperature_count;  //(TEMPERATURE_REPORT_INTERVAL / TEMPERATURE_MEASUREMENT_INTERVAL);
    sum_of_temperature = 0.;
    temperature_count = 0;
    Serial.print(temperature);
    Serial.print(F(" degC; "));
    Serial.print(F("Air_temperature:"));
 	  Serial.print(getTemperature(1));
    Serial.println(F(" degC\n"));
    winteringModeChecking(temperature);
  }

  return temperature; // return the current temperature or the new average
}

/********************************************************************/ 
unsigned long operatingTime(float T, float air_T)
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
      Serial.println(F("FROST PROTECTION MODE - FORCE PUMP CIRCULATION!"));
    }
  }
  else
  {
    if (T>18)
    {
      divider = 2;
    }
    else
    {
      divider = 2.5;
    }
    optime = (unsigned long) (T*MS_HOUR)/(divider+wintering);        // Divide temperature by 2.5 (or 3.5 during winter)
    frost_protection = false;
    if (old_frost_flag)      // frost protection flag just set - printing only once. 
    {
      Serial.println(F("EXIT FROST PROTECTION MODE - RETURN TO NORMAL MODE!"));
    }    
  }
  return optime;
}

/********************************************************************/ 
unsigned int start(int relay) {
  digitalWrite(relay, RUNNING);
  // delay(1000);
  // return digitalRead(relay);
}

/********************************************************************/ 
unsigned int stop(int relay) {
  digitalWrite(relay, STOPPED);
  // delay(1000);
  // return digitalRead(relay);
}

/********************************************************************/ 
void relayStatus()
{
  int i;
  int state; 

  Serial.println("\nRelay's status :");
  for (i=2; i<=4; i++)
  {
    Serial.print(RELAY[i-2]);
    Serial.print(F(" -> "));
    state = digitalRead(i);
    Serial.println(STATUS[state]);
  }
  Serial.println("");
}

/********************************************************************/ 
void stopAllRelays()
{
  int i;
  for (i=2; i<=4; i++)
  {
    digitalWrite(i, STOPPED);
  }
} 

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
void repondre(EthernetClient client) {
  // La fonction prend un client en argument

    // Quelqu'un est connecté !
    Serial.println(F("\nRéponse au client !")); // debug

    // On fait notre en-tête
    // Tout d'abord le code de réponse 200 = réussite
    // Puis le type mime du contenu renvoyé, du json
    // Et on autorise le cross origin
    // On envoie une ligne vide pour signaler la fin du header

    client.println(F("HTTP/1.1 200 OK"));
    client.println(F("Content-Type: application/json; charset: utf-8"));
    client.println(F("Access-Control-Allow-Origin: *"));
    client.println();

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
    client.print(temperature);
    //Une autre virgule pour séparer les clés
    client.println(F(","));
    // Et on envoie la clé nommée "air temperature"
    client.print(F("\t\"air temperature (degC)\": "));
    client.println(air_temperature);

    // Et enfin on termine notre JSON par une accolade fermante
    client.println(F("}"));
  }

/********************************************************************/ 
void setup(void)
{
  // Start the Serial port for debugging
  Serial.begin(115200);

  // Now we start the pool controler
  Serial.println(F("WELCOME TO POOL CONTROLLER"));
  Serial.println(F("**************************\n")); 

  char erreur = 0;
  // On démarre le shield Ethernet SANS adresse IP (donc donnée via DHCP)
  erreur = Ethernet.begin(mac);
  
  if (erreur == 0)
  {
    Serial.println(F("Try a static IP..."));
    // If an error occurs, DHCP assignement is not working so we try to use a static IP.
    Ethernet.begin(mac, ip);
  }

  Serial.println(F("Ethernet Shied Init..."));
  // Donne une seconde au shield pour s'initialiser
  delay(1000);

  // On lance le serveur
  serveur.begin();
  Serial.print(F("Server ready with a local IP: "));
  Serial.println(Ethernet.localIP());

  // Start up the sensor library 
  Serial.println(F("\nStarting temperature measurements ..."));
  sensors.begin(); 

  // Setup relays
  pinMode(PUMP_RELAY, OUTPUT);
  pinMode(ELECTROLYSE_RELAY, OUTPUT);
  pinMode(PH_RELAY, OUTPUT); 

  // Initialize temperature
  temperature = getTemperature(0);
  air_temperature = getTemperature(1);
  Serial.print(F("\nCurrent water temperature is "));
  Serial.print(temperature);
  Serial.println(F(" degC"));
  Serial.print(F("Current air temperature is "));
  Serial.print(air_temperature);
  Serial.println(" degC");

  // Check wintering mode 
  winteringModeChecking(temperature);
  
  // Initialize operating time
  operating_time = operatingTime(temperature, air_temperature);
  Serial.print(F("Total operating time per period of 24h is set to "));
  Serial.print((float)operating_time/MS_HOUR);
  Serial.print("h (");
  Serial.print((float)operating_time*100/MS_DAY);
  Serial.println("% of the time)");

  // Be sure that all relays are stopped
  stopAllRelays();
  relayStatus();

  Serial.println("Starting the first period ...");
   
} 

/********************************************************************/ 
void loop(void) 
{ 
  unsigned long period;
  unsigned int pump_state;
  unsigned int electrolyse_state;
  unsigned int ph_state;
  unsigned int previous_pump_state;
  unsigned long current_time_of_the_day=millis();

  // variables used for reading message from client
  char *url = (char *)malloc(100); // array to store url chars
  char url_index = 0; 

  // Read current water and air temperature
  temperature = getAverageTemperature();
  air_temperature = getTemperature(1);
  
  // Determine duration for a day
  operating_time = operatingTime(temperature, air_temperature)/NPERIODS;

  // determine if we are in the period as in the last loop
  // else reinitialize to a new period
  period = MS_DAY/NPERIODS;
  
  if (current_time_of_the_day - previous_time_of_the_day >= period)
  {
    // Start a new day and reset pump working time
    previous_time_of_the_day = current_time_of_the_day;
    previous_pump_start = current_time_of_the_day;
    pump_working_time = 0;
    period_count += 1;
    Serial.print(F("\n* Starting period "));
    Serial.print(period_count);
    Serial.println(F(" ...\n"));
    // Serial.print(F("Operating time is set to "));
    // printDuration(operating_time, period);
    Serial.print(F("Current water temperature is "));
    Serial.print(temperature);
    Serial.println(F(" degC"));
    Serial.print(F("Current air temperature is "));
    Serial.print(air_temperature);
    Serial.println(F(" degC"));

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
    pump_working_time += current_time_of_the_day - previous_pump_start;
    previous_pump_start = current_time_of_the_day;
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
  if (temperature <= MIN_ELECTROLYSIS_TEMPERATURE || pump_state == STOPPED) 
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
  if (temperature <= MIN_PH_METER_TEMPERATURE || pump_state == STOPPED) 
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

  Serial.print(millis() - previous_time_of_the_day); 

  // Regarde si un client est connecté et attend une réponse
  EthernetClient client = serveur.available();
  if (client) { // Un client est là ?
  
    repondre(client);
    // Donne le temps au client de prendre les données
    delay(10);
    // Ferme la connexion avec le client
    client.stop();
  
   /*
    Serial.println("Connexion !");
    url = ""; // on remet à zéro notre chaîne tampon
    url_index = 0;
    while(client.connected()) { // Tant que le client est connecté
      Serial.println("client.connected");
      /* if(client.available()) { // A-t-il des choses à dire ?
        // Serial.print("client available");
        // traitement des infos du client
        char carlu = client.read(); //on lit ce qu'il raconte
        if(carlu != '\n') { // On est en fin de chaîne ?
          // non ! alors on stocke le caractère
          Serial.print(carlu);
          // url[url_index] = carlu;
          // url_index++;
        } else {
          // on a fini de lire ce qui nous intéresse
          // on marque la fin de l'url (caractère de fin de chaîne)
          // url[url_index] = '\0';
          // boolean ok = interpreter(); // essaie d'interpréter la chaîne
          // if(ok) {
            // tout s'est bien passé = on met à jour les broches
          //  action();
          // }
          // et dans tout les cas on répond au client
          repondre(client);
          // on quitte le while
          break;
        }
      } 
      
    } 
    
    // Donne le temps au client de prendre les données
    delay(10);
    // Ferme la connexion avec le client
    client.stop();
    Serial.println(F("Deconnexion !"));
   */
  }
}

