## A project to control Daikin split through S21 socket.

### Daikin Model
I'm controlling three old FTXSxxG splits. Probably this works mostly with the same units as [Faikin](https://github.com/revk/ESP32-Faikin) does.

### Used hardware (under 10$):
- esp8266mini
- mini560 step down (5V output version)
- mini breadboard
- 4 dupont cables male-female

You can also get (lastly, i did) these cables to ease connection: aliexpress.com/item/1005005465484217.html

### Used software
this is a platformio project.

### Pinout
On my units the S21 port is:<br/>
1 - Seems unused<br/>
2 - TX (5V)<br/>
3 - RX (5V)<br/>
4 - VCC (14.5V)<br/>
5 - GND<br/>
Luckily, RX port accepts 3.3V levels so i did not need a level shifter.<br/>
I used D6 and D7 as serial port pins for the ESP8266.

### How to install

1. Wiring

        [S21] pin1 (unused)
        [S21] pin2 =================================>  D7 [esp8266mini]
        [S21] pin3 =================================>  D6 [esp8266mini]
        [S21] pin4 =====> IN + [mini560] + OUT =====>  5V [esp8266mini]
        [S21] pin5 =====> IN - [mini560] - OUT =====> GND [esp8266mini]


   With a voltage meter, check which end of S21 is pin 1.

2. Clone this project

3. Apply Remotedebug patches (see below)

4. Check `platformio.ini` settings on uploading to your board.

5. Compile the project and upload

6. Upload filesystem files

        pio run --target uploadfs

7. On the first boot, the device creates an access point `WiFi-daikin`. Connect to the WiFi. Any ip address should lead you to a portal that asks for credentials to your actual wifi. Enter the credentials and boot the device.

8. Find out the device ip address (e.g. http://192.168.12.345) and connect to it with a browser.

9. Connect the device to you Daikin unit S21 port. Then, in the browser you should see and be able to modify the device state.

### Home Assistant integration

1. You need an MQTT broker. You can eg. use the Home Assistant add-on, or the `eclipse-mosquitto` docker image.

2. Click `MQTT control` to enable MQTT. Prefix `testamentTopic`, `subTopic` and `pubTopic` with a device id, e.g. `livingroomDaikin/`. Change `broker` to the MQTT broker name or ip address, possibly the same as your Home Assistant ip.

3. In Home Assistant, add MQTT integration: Settings > Integrations > Add integration > MQTT. Set broker address.

4. In Home Assistant, edit `configuration.yaml`. Copy this file to the end of config: https://github.com/MassiPi/DaikinS21/blob/master/HA%20Mqtt.txt but replace:

    - the name of the device, e.g. `Living room AC`
    - if you have more than unit, use more specific names: `mydaikin` => `livingroomDaikin`

### Physical setup
Well, this also fits inside the units, seems good! <br/>
<img src="https://github.com/MassiPi/DaikinS21/assets/2384381/c33e21e2-6fc4-4717-9fac-01a2bb0648b4" width="50%"></img>

### Rationale
i did not want to use already available code since this is not fun enough, so i just wrote my code.
- I kept a functional bootstrap-based web interface<br/>
<img src="https://github.com/MassiPi/DaikinS21/assets/2384381/7394fdb5-c716-463d-a2aa-b6ca453478b6" width="50%"></img>
- i decided to keep the hardware serial functional for debugging, so i moved the control on a software serial
- i included remotedebug library https://github.com/JoaoLopesF/RemoteDebug (please check the fixes!) to be able to debug the functioning also remotely
- ota update available
- all data exchange is json-ed: via websocket, via http call and via mqtt
- commands are accepted (and data is published) in the web interface, via http call and via mqtt, same format is used.
- since the starting point was the home assistant integration, this was achieved with https://www.home-assistant.io/integrations/climate.mqtt/ . For a couple of "limits" of the integration (power and swing management), the code implements a couple of custom calls.
- wifi manager for wifi config

### Home assistant integration
As said, the integration is done through MQTT, so i also added an example of code for integration and with templates. You'll probably need to redefine lists and for sure mqtt topics.

### Fixes
Remotedebug library has some flows, please remember to:
- modify the RemoteDebugCfg.h file, line 104, to disable websockets (or it's gonna conflict with the asyncwebserver websockets server)
> #define WEBSOCKET_DISABLED true
- comment out the part of the WebSocketsClient.cpp file between lines 700 and 710, since you are not using it but it gives exceptions with recent ESP8266 core.
>/*#if (WEBSOCKETS_NETWORK_TYPE == NETWORK_ESP8266)<br/>
>    _client.tcp->setNoDelay(true);<br/>
><br/>
>    if(_client.isSSL && _fingerprint.length()) {<br/>
>        if(!_client.ssl->verify(_fingerprint.c_str(), _host.c_str())) {<br/>
>            DEBUG_WEBSOCKETS("[WS-Client] certificate mismatch\n");<br/>
>            WebSockets::clientDisconnect(&_client, 1000);<br/>
>            return;<br/>
>        }<br/>
>    }<br/>
>#endif */

### Credits
i did not even know about the S21 socket, so i NEED to thank:<br/>
https://github.com/joshbenner/esphome-daikin-s21/tree/main<br/>
https://github.com/revk/ESP32-Faikin<br/>
As you'll see, i also took pieces of code, but i wasn't fully happy about it's structure, so i rewrote it as a states-machine to reduce blocking in code.

### Disclaimer
i am NOT a programmer :) but i understand a lot of parts could be better written, and that some things could be done with higher security and bla bla bla.<br/>
I would not (and i don't) expose the controller on internet, this should clarify what i mean :)

### Why publishing
someone asked, so why not?

### Please
Since i'm not a programmer and i'm totally lost in git, please don't hesitate reporting any ANY issue in my code or anything wrong you see :)
