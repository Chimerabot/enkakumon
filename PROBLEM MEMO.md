**CS370 Term Project Problem Memo**

Gideon Rank & Navin Mani

[GitHub](https://github.com/Chimerabot/enkakumon)

AI has not yet been utilized for this project (no transcripts to submit)

**Pitch**: This product is called “Enkakumon”, a wordplay on the Japanese term for “remote monitoring”. It is a digital pet-style device: pocket-sized, highly portable, and battery powered, with a small screen. It has a suite of sensors that determine local environmental hazards, and conveys them both through interactions with the pet (simulated illness, for example), and more detailed measurements (easily-accessible trend graphs, tables, and so on).

1. **User**

   The intended user of this product is someone interested in real-time monitoring of their environment (via factors like air quality, UV index, temperature, etc.), for the purpose of preserving and improving their health. It can also act as a Tamagotchi-like “digital pet”, with fun features that elevate its useful functions.

2. **Problem**

   The ever-busy modern world can make it difficult to focus on health, particularly with regard to environmental factors; it is far easier to ignore bad air quality than it is to ignore a subpar diet, for example. Cheap and accessible tools for discerning a variety of health risks in one’s local environment are somewhat uncommon.

3. **Device Purpose**

   Creating a highly portable, always-on device that can monitor local environmental health risks lets users determine patterns over time and can help inform choices that may be able to improve their health. For example, placing the device in sunlight for a whole day to measure UV index could produce a helpful pattern for when to use sunscreen. Apps are often not able to solve such issues, due to phones’ limited environmental sensing capabilities.

4. **Sensors**

   *Required*: [air quality sensor](https://www.adafruit.com/product/4829), [temperature/humidity/pressure/gas sensor](https://www.adafruit.com/product/3660). Together, these inexpensive, compact sensors will be able to provide a fairly complete picture of local air quality

   *Nice to have*: [UV index sensor](https://www.adafruit.com/product/1918?gad_source=1&gad_campaignid=23986111167&gbraid=0AAAAADx9JvSjPABja3spqFZgDV9VmACUR&gclid=Cj0KCQjw8c3VBhCsARIsAA_xJ935e2XW0QWMoPlOFroUDCMIeaBZSaY1Z0xdaR33AZdV6hG8fcInDXoaAhHnEALw_wcB), [proximity/color/light sensor](https://www.adafruit.com/product/6461), [microphone](https://www.adafruit.com/product/3421). These sensors, similarly cheap and compact to the ones above, could give the device both greater environmental awareness and more enjoyable interactivity.

   To ensure a compact form factor, a Raspberry Pi Zero 2 W will likely be the ideal choice of controller for these sensors.

5. **Mechanisms**

   Mechanisms D (custom storage layer) and E (multi-process architecture) seem the most relevant to this product. It will be essential for the device to store and convey past stored information, and to consistently and concurrently monitor the inputs from multiple different sensors, in order to fulfill its intended purpose.

6. **Risks**

   Attempting to make a highly portable (and thus, battery-powered) device with a compact footprint and suitably complex sensor suite will require a lot of careful design. The most major risk of this concept is probably whatever happens when a battery dies.
