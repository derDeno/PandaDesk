# PandaDesk
This project develops a **universal ESP32-based controller for height-adjustable desks**. The goal is to support different desk manufacturers and communication protocols with a single hardware platform, even when the pinout or signal configuration is initially unknown.

The controller connects transparently between the desk and its original control interface and uses a configurable hardware architecture to identify, monitor, and interact with the desk’s communication lines. An **ESP32-C6**, level shifting, switchable pull-ups, signal protection, and GPIO expansion allow the same PCB to be adapted through software to different desk models.

The long-term objective is to create a flexible platform where support for new desks can primarily be added through **firmware configuration instead of redesigning the hardware**.
