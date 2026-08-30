# C9M3mini 

C9M3mini is a comprehensive open-source camera project, featuring custom hardware (PCBs), 3D printable enclosures, and a C++ based camera application with UI and WiFi sharing capabilities.

## Project Overview
This repository contains everything you need to build the C9M3mini camera from scratch:
- **Software**: A Qt-based C++ application for album viewing, video HUD display, and WiFi sharing.
- **Hardware**: Gerber files and BOMs for custom PCBs (power board, battery board, keypad, etc.).
- **Mechanical**: 3D models (`.3mf`) for 3D printing the camera enclosure.

## Repository Structure
- `/C9M3mini` - The core C++ camera application source code.
- `/C9M3mini_berger` - Hardware manufacturing files. Includes `.zip` Gerber files for PCB fabrication and `.xlsx` Bill of Materials (BOM).
- `C9M3mini_modle.3mf` - 3D printable model for the camera housing (Bambu Studio compatible).

## Software Features
- **Album Viewer**: View captured photos directly on the device.
- **Video HUD**: Heads-Up Display for real-time video recording and monitoring.
- **WiFi Share & Monitor**: Generate QR codes to connect to a local WiFi hotspot and browse files via SMB. Includes real-time connection and speed monitoring.
- **Autostart Support**: Scripts provided to configure the app to run automatically on boot (optimized for Raspberry Pi).

## Prerequisites & Build Instructions (Software)

### Prerequisites
- CMake (version 3.10 or higher)
- C++ Compiler (GCC, Clang, etc.)
- Qt Framework (for UI components)
- `qrencode` (for generating WiFi sharing QR codes)
- Linux network utilities (`ip`, `iw`) for network monitoring

### Build Steps
1. Clone the repository:
   ```bash
   git clone https://github.com/Pumpkin173/C9M3mini.git
   cd C9M3mini/C9M3mini
2. Build the project using CMake:
   mkdir build && cd build
   cmake ..
   cmake --build .
Autostart Setup
To set up the application to run automatically on startup (e.g., on a Raspberry Pi):
./scripts/setup_autostart.sh
## Authors
[Pumpkin173] (Creator & Hardware/Software Engineer)
antigravity--Gemini (Co-author & Assistant)
## License
This project is licensed under the GNU General Public License v3.0 (GPLv3) - see the 
LICENSE file for details.

