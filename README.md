# **ProtoTracer-ESP32: The ESP32S3 port of 3D Rendering and Animation Engine**
This project is a live 3D rendering and animation engine designed for use on microcontrollers.

This port is still under the development. The ESP-NOW Remote Controller part is not finished.

The design files of screen controller and remote controller will be available soon.

## Demonstration:
As a quick way to showcase the capabilities of this software, here is a demo showing a live rendering of a rotating and textured .OBJ file:

## Platform requirements:
* ESP32-S3-WROOM-1/1U NxxR8

This codebase has been tested on an Arduino Mega (smaller .OBJ file with a 89 pixel matrix) and on a Teensy 4.0 and ESP32S3(2000 triangle scene with a 2,804 pixel matrix).

# Issues:
The rainbow spiral is not stable.

# Usage:
The following links give a detailed description on how to import files, set up controllers, manipulate objects, and render to displays:
- [Creating a controller](https://github.com/coelacant1/ProtoTracer/wiki/Creating-a-custom-controller)
- [Using an existing controller](https://github.com/coelacant1/ProtoTracer/wiki/Using-an-Existing-Controller)
- [Creating an animation](https://github.com/coelacant1/ProtoTracer/wiki/Creating-an-Animation)
  - [Adding (.OBJ) objects with UV maps](https://github.com/coelacant1/ProtoTracer/wiki/Adding-.OBJ-Objects-with-UV-Maps)
  - [Adding (.FBX) objects with blend shapes](https://github.com/coelacant1/ProtoTracer/wiki/Adding-.FBX-objects-with-Blend-Shapes)
  - [Adding image materials](https://github.com/coelacant1/ProtoTracer/wiki/Adding-Image-Materials)
  - [Adding gif materials](https://github.com/coelacant1/ProtoTracer/wiki/Adding-GIF-Materials)
  - [Creating a shader material](https://github.com/coelacant1/ProtoTracer/wiki/Creating-a-Shader-Material)
  - [Modifying 3D objects](https://github.com/coelacant1/ProtoTracer/wiki/Modifying-3D-Objects)
  - [Keyframing animations](https://github.com/coelacant1/ProtoTracer/wiki/Keyframing-Animations)

# Questions?
Any recommendations on additional information to add can be requested in the discussions tab. If you have additional questions, you can @ me(TassP0lar#1695) in Coelacant's [Discord server](https://discord.gg/YwaWnhJ) or direct message me on [Twitter](https://twitter.com/P0larTas).

# Contributing:
If you would like to keep this project going, please support Coelacant on Patreon: https://www.patreon.com/coelacant1

# License Agreement:
For this project, [AGPL-3.0](https://choosealicense.com/licenses/agpl-3.0/) is used for licensing as a means to make sure any contributions or usage of the software can benefit the community. If you use and modify this software for a product, you must make the modified code readily available as per the license agreement.
