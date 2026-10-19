"""SwitchBot device commands, for the command picker in the web UI.

Each entry is (command, parameter hint, description), keyed by the `deviceType` that
GET /devices returns (or an IR remote's `remoteType`). The data comes from the "Control Commands"
tables of https://github.com/OpenWonderLabs/SwitchBotAPI as of 2026-10-18, via the table in
~/cbin/misc/switchbot, with the non-device entries (passcode field descriptions that the doc parser
mistook for device types) and duplicate rows removed and typographic quotes replaced. It is a
snapshot: devices newer than it have no commands listed, so the UI also lets the user type any
command by hand.
"""

from __future__ import annotations

from typing import Optional

# BEGIN COMMAND TABLES
DEVICE_COMMANDS: dict[str, list[tuple[str, str, str]]] = {
    'AI Art Frame': [
        ('next', 'default', 'Switch to the next image'),
        ('previous', 'default', 'Switch to the previous image'),
        ('uploadImage', '{"imageUrl":"https://..."} OR {"imageBase64":"data:image/jpeg;base64,..."}', 'Upload an image (choose one of imageUrl / imageBase64)'),
    ],
    'Air Purifier PM2.5': [
        ('turnOff', 'default', 'set to OFF state'),
        ('turnOn', 'default', 'set to ON state'),
        ('setMode', '{"mode": mode_int, "fanGear": fan_level_int}', 'set the mode. mode_int, 1, normal or fan mode; 2, auto mode; 3, sleep mode; 4, pet mode; fan_level_int, the fan level can only be set if mode_int is set to 1, 1~3'),
        ('setChildLock', '0 or 1', 'enable or disable child lock. 1, enable; 0, disable'),
    ],
    'Air Purifier Table PM2.5': [
        ('turnOff', 'default', 'set to OFF state'),
        ('turnOn', 'default', 'set to ON state'),
        ('setMode', '{"mode": mode_int, "fanGear": fan_level_int}', 'set the mode. mode_int, 1, normal or fan mode; 2, auto mode; 3, sleep mode; 4, pet mode; fan_level_int, the fan level can only be set if mode_int is set to 1, 1~3'),
        ('setChildLock', '0 or 1', 'enable or disable child lock. 1, enable; 0, disable'),
    ],
    'Air Purifier Table VOC': [
        ('turnOff', 'default', 'set to OFF state'),
        ('turnOn', 'default', 'set to ON state'),
        ('setMode', '{"mode": mode_int, "fanGear": fan_level_int}', 'set the mode. mode_int, 1, normal or fan mode; 2, auto mode; 3, sleep mode; 4, pet mode; fan_level_int, the fan level can only be set if mode_int is set to 1, 1~3'),
        ('setChildLock', '0 or 1', 'enable or disable child lock. 1, enable; 0, disable'),
    ],
    'Air Purifier VOC': [
        ('turnOff', 'default', 'set to OFF state'),
        ('turnOn', 'default', 'set to ON state'),
        ('setMode', '{"mode": mode_int, "fanGear": fan_level_int}', 'set the mode. mode_int, 1, normal or fan mode; 2, auto mode; 3, sleep mode; 4, pet mode; fan_level_int, the fan level can only be set if mode_int is set to 1, 1~3'),
        ('setChildLock', '0 or 1', 'enable or disable child lock. 1, enable; 0, disable'),
    ],
    'Battery Circulator Fan': [
        ('turnOff', 'default', 'Set to OFF state'),
        ('turnOn', 'default', 'Set to ON state'),
        ('setNightLightMode', 'off, 1, or 2', 'off, turn off nightlight, 1, bright 2, dim'),
        ('setWindMode', 'direct, natural, sleep, or baby', 'Set fan mode. direct: direct mode. natural: natural mode. sleep: sleep mode. baby: ultra quiet mode'),
        ('setWindSpeed', '{1-100} e.g. 10', 'Set fan speed.1~100'),
        ('closeDelay', '{1-36000} e.g. 10', 'Set fan close time'),
    ],
    'Battery Circulator Fan 2 Pro': [
        ('turnOff', 'default', 'set to OFF state'),
        ('turnOn', 'default', 'set to ON state'),
        ('setNightLightMode', 'off, 0, or 1', 'off, turn off; 0, bright; 1, soft'),
        ('setWindMode', 'direct, natural, sleep, or hurricane', 'set fan mode'),
        ('setWindSpeed', '{1-100}, e.g. 10', 'set fan speed'),
    ],
    'Blind Tilt': [
        ('setPosition', 'direction;position e.g. up;60', 'direction: up/down position: 0~100 (0 means closed, 100 means open, it MUST be set to a multiple of 2. e.g. up;48 or up; 36)'),
        ('fullyOpen', 'default', 'Set the position of Blind Tilt to open, equivalent to setting the position to up;100 or down;100'),
        ('closeUp', 'default', 'Set the position of Blind Tilt to closed up, equivalent to setting the position to up;0'),
        ('closeDown', 'default', 'Set the position of Blind Tilt to closed down, equivalent to setting the position to down;0'),
    ],
    'Bot': [
        ('turnOff', 'default', 'set to OFF state'),
        ('turnOn', 'default', 'set to ON state'),
        ('press', 'default', 'trigger press'),
    ],
    'Candle Warmer Lamp': [
        ('turnOn', 'default', 'set to ON state'),
        ('turnOff', 'default', 'set to OFF state'),
        ('toggle', 'default', 'toggle state'),
        ('setBrightness', '{0-100}', 'set brightness'),
    ],
    'Ceiling Light': [
        ('turnOn', 'default', 'set to ON state'),
        ('turnOff', 'default', 'set to OFF state'),
        ('toggle', 'default', 'toggle state'),
        ('setBrightness', '{1-100}', 'set brightness'),
        ('setColorTemperature', '{2700-6500}', 'set the color temperature'),
    ],
    'Ceiling Light Pro': [
        ('turnOn', 'default', 'set to ON state'),
        ('turnOff', 'default', 'set to OFF state'),
        ('toggle', 'default', 'toggle state'),
        ('setBrightness', '{1-100}', 'set brightness'),
        ('setColorTemperature', '{2700-6500}', 'set the color temperature'),
    ],
    'Circulator Fan': [
        ('turnOff', 'default', 'Set to OFF state'),
        ('turnOn', 'default', 'Set to ON state'),
        ('setNightLightMode', 'off, 1, or 2', 'off, turn off nightlight, 1, bright 2, dim'),
        ('setWindMode', 'direct, natural, sleep, or baby', 'Set fan mode. direct: direct mode. natural: natural mode. sleep: sleep mode. baby: ultra quiet mode'),
        ('setWindSpeed', '{1-100} e.g. 10', 'Set fan speed.1~100'),
        ('closeDelay', '{1-36000} e.g. 10', 'Set fan close time'),
    ],
    'Color Bulb': [
        ('turnOn', 'default', 'set to ON state'),
        ('turnOff', 'default', 'set to OFF state'),
        ('toggle', 'default', 'toggle state'),
        ('setBrightness', '{1-100}', 'set brightness'),
        ('setColor', '"{0-255}:{0-255}:{0-255}"', 'set RGB color value'),
        ('setColorTemperature', '{2700-6500}', 'set color temperature'),
    ],
    'Curtain': [
        ('setPosition', 'index0,mode0,position0 e.g. 0,ff,80', 'mode: 0 (Performance Mode), 1 (Silent Mode), ff (default mode) position: 0~100 (0 means open, 100 means closed)'),
        ('turnOff', 'default', 'equivalent to set position to 100'),
        ('turnOn', 'default', 'equivalent to set position to 0'),
        ('pause', 'default', 'set to PAUSE state'),
    ],
    'Curtain 3': [
        ('setPosition', 'index0,mode0,position0 e.g. 0,ff,80', 'mode: 0 (Performance Mode), 1 (Silent Mode), ff (default mode) position: 0~100 (0 means open, 100 means closed)'),
        ('turnOff', 'default', 'equivalent to set position to 100'),
        ('turnOn', 'default', 'equivalent to set position to 0'),
        ('pause', 'default', 'set to PAUSE state'),
    ],
    'Floor Cleaning Robot S10': [
        ('startClean', '{"action": clean_mode_str, "param": {"fanLevel": fan_level_int, "waterLevel": water_level_int, "times": clean_cycle_int}}', 'start cleaning. action, the cleaning mode, sweep or sweep_mop. fanLevel, the vacuum level, 1-4. waterLevel, the mop moisture level, 1-2. times, the number of cycles, 1-2639999, in theory.'),
        ('addWaterForHumi', 'default', 'refill the mind blowing Evaporative Humidifier (Auto-refill).'),
        ('pause', 'default', 'pause.'),
        ('dock', 'default', 'return to Auto-empty Station and charge.'),
        ('setVolume', '0-100', 'set volume, 1-100'),
        ('selfClean', '1 or 2 or 3', 'mode 1, wash the mop. mode 2, dry itself. mode 3, terminate.'),
        ('changeParam', '{"fanLevel": fan_level_int, "waterLevel": water_level_int, "times": clean_cycle_int}', 'fanLevel, the vacuum level, 1-4. waterLevel, the mop moisture level, 1-2. times, the number of cycles, 1-2639999, in theory.'),
    ],
    'Floor Lamp': [
        ('turnOn', 'default', 'set to ON state'),
        ('turnOff', 'default', 'set to OFF state'),
        ('toggle', 'default', 'toggle state'),
        ('setBrightness', '{0-100}', 'set brightness'),
        ('setColor', '"{0-255}:{0-255}:{0-255}"', 'set RGB color value'),
        ('setColorTemperature', '{2700-6500}', 'set color temperature'),
    ],
    'Garage Door Opener': [
        ('turnOn', 'default', 'set to ON state'),
        ('turnOff', 'default', 'set to OFF state'),
    ],
    'Humidifier': [
        ('turnOff', 'default', 'set to OFF state'),
        ('turnOn', 'default', 'set to ON state'),
        ('setMode', 'auto or 101 or 102 or 103 or {0~100}', 'auto, set to Auto Mode, 101, set atomization efficiency to 34%, 102, set atomization efficiency to 67%, 103, set atomization efficiency to 100%'),
    ],
    'Humidifier2': [
        ('turnOff', 'default', 'set to OFF state'),
        ('turnOn', 'default', 'set to ON state'),
        ('setMode', '{"mode": mode_int, "targetHumidify": humidity_int}', 'set the mode. mode_int, 1, level 4; 2, level 3; 3, level 2; 4, level 1; 5, humidity mode; 6, sleep mode; 7, auto mode; 8, drying mode; targetHumidify, the target humidity level in percentage, 0~100.'),
        ('setChildLock', 'true or false', 'enable or disable child lock. true, enable; false, disable'),
    ],
    'K10+': [
        ('start', 'default', 'start vacuuming'),
        ('stop', 'default', 'stop vacuuming'),
        ('dock', 'default', 'return to charging dock'),
        ('PowLevel', '{0-3}', 'set suction power level: 0 (Quiet), 1 (Standard), 2 (Strong), 3 (MAX)'),
    ],
    'K10+ Pro': [
        ('start', 'default', 'start vacuuming'),
        ('stop', 'default', 'stop vacuuming'),
        ('dock', 'default', 'return to charging dock'),
        ('PowLevel', '{0-3}', 'set suction power level: 0 (Quiet), 1 (Standard), 2 (Strong), 3 (MAX)'),
    ],
    'K11+': [
        ('startClean', '{"action": action_str, "param": {"fanLevel": fan_level_int, "times": clean_cycle_int}}', 'start cleaning. action_str, the cleaning mode, sweep or mop. fanLevel, the vacuum level, 1-4. times, the number of cycles, 1-2639999, in theory.'),
        ('pause', 'default', 'pause.'),
        ('dock', 'default', 'return to Auto-empty Station and charge.'),
        ('setVolume', '0-100', 'set volume, 1-100'),
        ('changeParam', '{"fanLevel": fan_level_int, "waterLevel": water_level_int, "times": clean_cycle_int}', 'fanLevel, the vacuum level, 1-4. waterLevel, the mop moisture level, 1-2. times, the number of cycles, 1-2639999, in theory.'),
    ],
    'K20+ Pro': [
        ('startClean', '{"action": action_str, "param": {"fanLevel": fan_level_int, "times": clean_cycle_int}}', 'start cleaning. action_str, the cleaning mode, sweep or mop. fanLevel, the vacuum level, 1-4. times, the number of cycles, 1-2639999, in theory.'),
        ('pause', 'default', 'pause cleaning'),
        ('dock', 'default', 'return to charging dock'),
        ('setVolume', '{0-100}', 'set the robot volume'),
        ('changeParam', '{"fanLevel": fan_level_int, "waterLevel": water_level_int, "times": clean_cycle_int}', 'change clean parameters. fan_level_int, the vacuum level, 1-4; water_level_int, the mop moisture level, 1-2; times, the number of cycles, 1-2639999, in theory.'),
    ],
    'Kata Friends': [
        ('mode', 'Normal, Standby, or Sleep', 'set mode'),
        ('childLock', 'on or off', 'set child lock state'),
        ('backHome', 'default', 'return home'),
        ('pictureTaking', 'on or off', 'set album photo-taking switch'),
        ('snapshots', 'on or off', 'set random snapshot switch'),
        ('talk', 'on or off', 'set talk feature switch'),
    ],
    'Keypad': [
        ('createKey', '{ "name": passcode _name_str, "type": passcode_type_str, "password": passcode_str, "startTime": valid_from_long, "endTime": valid_to_long }', 'create a new passcode'),
        ('deleteKey', '{ "id": passcode_id_int }', 'delete an existing passcode'),
    ],
    'Keypad Touch': [
        ('createKey', '{ "name": passcode _name_str, "type": passcode_type_str, "password": passcode_str, "startTime": valid_from_long, "endTime": valid_to_long }', 'create a new passcode'),
        ('deleteKey', '{ "id": passcode_id_int }', 'delete an existing passcode'),
    ],
    'Keypad Vision': [
        ('createKey', '{ "name": passcode _name_str, "type": passcode_type_str, "password": passcode_str, "startTime": valid_from_long, "endTime": valid_to_long }', 'create a new passcode'),
        ('deleteKey', '{ "id": passcode_id_int }', 'delete an existing passcode'),
    ],
    'Keypad Vision Pro': [
        ('createKey', '{ "name": passcode _name_str, "type": passcode_type_str, "password": passcode_str, "startTime": valid_from_long, "endTime": valid_to_long }', 'create a new passcode'),
        ('deleteKey', '{ "id": passcode_id_int }', 'delete an existing passcode'),
    ],
    'Lock': [
        ('lock', 'default', 'rotate to locked position'),
        ('unlock', 'default', 'rotate to unlocked position'),
        ('deadbolt', 'default', 'disengage deadbolt or latch'),
    ],
    'Lock Lite': [
        ('lock', 'default', 'rotate to locked position'),
        ('unlock', 'default', 'rotate to unlocked position'),
        ('deadbolt', 'default', 'disengage deadbolt or latch'),
    ],
    'Lock Pro': [
        ('lock', 'default', 'rotate to locked position'),
        ('unlock', 'default', 'rotate to unlocked position'),
        ('deadbolt', 'default', 'disengage deadbolt or latch'),
    ],
    'Lock Ultra': [
        ('lock', 'default', 'rotate to locked position'),
        ('unlock', 'default', 'rotate to unlocked position'),
        ('deadbolt', 'default', 'disengage deadbolt or latch'),
    ],
    'Lock Vision': [
        ('lock', 'default', 'rotate to locked position'),
        ('unlock', 'default', 'rotate to unlocked position'),
        ('deleteKey', '{"id": ""}', 'delete a passcode by id'),
        ('createKey', '{"name":"","type":"","password":"","startTime":0,"endTime":0}', 'create a passcode'),
    ],
    'Lock Vision Pro': [
        ('lock', 'default', 'rotate to locked position'),
        ('unlock', 'default', 'rotate to unlocked position'),
        ('deleteKey', '{"id": ""}', 'delete a passcode by id'),
        ('createKey', '{"name":"","type":"","password":"","startTime":0,"endTime":0}', 'create a passcode'),
    ],
    'Permanent Outdoor Lights': [
        ('turnOn', 'default', 'set to ON state'),
        ('turnOff', 'default', 'set to OFF state'),
        ('toggle', 'default', 'toggle state'),
        ('setBrightness', '{0-100}', 'set brightness'),
        ('setColorTemperature', '{2700-6500}', 'set color temperature'),
        ('setColor', '"{0-255}:{0-255}:{0-255}"', 'set RGB color value'),
    ],
    'Plug': [
        ('turnOn', 'default', 'set to ON state'),
        ('turnOff', 'default', 'set to OFF state'),
    ],
    'Plug Mini (EU)': [
        ('turnOn', 'default', 'set to ON state'),
        ('turnOff', 'default', 'set to OFF state'),
        ('toggle', 'default', 'toggle state'),
    ],
    'Plug Mini (JP)': [
        ('turnOn', 'default', 'set to ON state'),
        ('turnOff', 'default', 'set to OFF state'),
        ('toggle', 'default', 'toggle state'),
    ],
    'Plug Mini (US)': [
        ('turnOn', 'default', 'set to ON state'),
        ('turnOff', 'default', 'set to OFF state'),
        ('toggle', 'default', 'toggle state'),
    ],
    'Relay Switch 1': [
        ('turnOn', 'default', 'set to ON state'),
        ('turnOff', 'default', 'set to OFF state'),
        ('toggle', 'default', 'toggle state'),
        ('setMode', '0~3', 'set the switch mode. 0, toggle mode; 1, edge switch mode; 2, detached switch mode; 3, momentary switch mode'),
    ],
    'Relay Switch 1PM': [
        ('turnOn', 'default', 'set to ON state'),
        ('turnOff', 'default', 'set to OFF state'),
        ('toggle', 'default', 'toggle state'),
        ('setMode', '0~3', 'set the switch mode. 0, toggle mode; 1, edge switch mode; 2, detached switch mode; 3, momentary switch mode'),
    ],
    'Relay Switch 2PM': [
        ('turnOn', '"1" or "2"', 'set to ON state. "1" is channel 1, "2" is channel 2'),
        ('turnOff', '"1" or "2"', 'set to OFF state. "1" is channel 1, "2" is channel 2'),
        ('toggle', '"1" or "2"', 'toggle state. "1" is channel 1, "2" is channel 2'),
        ('setMode', 'channel;mode e.g 1;0', 'The first item represents the switching of channel number, 1 represents channel number 1 and 2 represents channel number 2. The second item represents set the switch mode. 0, toggle mode; 1, edge switch mode; 2, detached switch mode; 3, momentary switch mode'),
        ('setPosition', '0~100', 'Set roller blind opening and closing percentage. 0, Open All; 100, Close All'),
    ],
    'RGBIC Neon Rope Light': [
        ('turnOn', 'default', 'set to ON state'),
        ('turnOff', 'default', 'set to OFF state'),
        ('toggle', 'default', 'toggle state'),
        ('setBrightness', '{0-100}', 'set brightness'),
        ('setColor', '"{0-255}:{0-255}:{0-255}"', 'set RGB color value'),
    ],
    'RGBIC Neon Wire Rope Light': [
        ('turnOn', 'default', 'set to ON state'),
        ('turnOff', 'default', 'set to OFF state'),
        ('toggle', 'default', 'toggle state'),
        ('setBrightness', '{0-100}', 'set brightness'),
        ('setColor', '"{0-255}:{0-255}:{0-255}"', 'set RGB color value'),
    ],
    'RGBICWW Ceiling Light': [
        ('turnOn', 'default', 'turn on all lights'),
        ('turnOff', 'default', 'turn off all lights'),
        ('toggle', 'default', 'toggle all lights'),
        ('turnOnMainLight', 'default', 'turn on main light'),
        ('turnOffMainLight', 'default', 'turn off main light'),
        ('turnOnColorLight', 'default', 'turn on color light'),
        ('turnOffColorLight', 'default', 'turn off color light'),
        ('setMainLightBrightness', '{1-100}', 'set main light brightness'),
        ('setMainLightColorTemp', '{2700-6500}', 'set main light color temp'),
        ('setColorLightBrightness', '{1-100}', 'set color light brightness'),
        ('setColorLightRGB', '"{0-255}:{0-255}:{0-255}"', 'set color light RGB value'),
    ],
    'RGBICWW Floor Lamp': [
        ('turnOn', 'default', 'set to ON state'),
        ('turnOff', 'default', 'set to OFF state'),
        ('toggle', 'default', 'toggle state'),
        ('setBrightness', '{0-100}', 'set brightness'),
        ('setColorTemperature', '{2700-6500}', 'set color temperature'),
        ('setColor', '"{0-255}:{0-255}:{0-255}"', 'set RGB color value'),
    ],
    'RGBICWW Strip Light': [
        ('turnOn', 'default', 'set to ON state'),
        ('turnOff', 'default', 'set to OFF state'),
        ('toggle', 'default', 'toggle state'),
        ('setBrightness', '{0-100}', 'set brightness'),
        ('setColorTemperature', '{2700-6500}', 'set color temperature'),
        ('setColor', '"{0-255}:{0-255}:{0-255}"', 'set RGB color value'),
    ],
    'Robot Vacuum Cleaner K10+ Pro Combo': [
        ('startClean', '{"action": action_str, "param": {"fanLevel": fan_level_int, "times": clean_cycle_int}}', 'start cleaning. action_str, the cleaning mode, sweep or mop. fanLevel, the vacuum level, 1-4. times, the number of cycles, 1-2639999, in theory.'),
        ('pause', 'default', 'pause cleaning'),
        ('dock', 'default', 'return to charging dock'),
        ('setVolume', '{0-100}', 'set the robot volume'),
        ('changeParam', '{"fanLevel": fan_level_int, "times": clean_cycle_int}', 'change clean parameters. fan_level_int, the vacuum level, 1-4; times, the number of cycles, 1-2639999, in theory.'),
    ],
    'Robot Vacuum Cleaner S1': [
        ('start', 'default', 'start vacuuming'),
        ('stop', 'default', 'stop vacuuming'),
        ('dock', 'default', 'return to charging dock'),
        ('PowLevel', '{0-3}', 'set suction power level: 0 (Quiet), 1 (Standard), 2 (Strong), 3 (MAX)'),
    ],
    'Robot Vacuum Cleaner S1 Plus': [
        ('start', 'default', 'start vacuuming'),
        ('stop', 'default', 'stop vacuuming'),
        ('dock', 'default', 'return to charging dock'),
        ('PowLevel', '{0-3}', 'set suction power level: 0 (Quiet), 1 (Standard), 2 (Strong), 3 (MAX)'),
    ],
    'Roller Shade': [
        ('setPosition', '0~100', '0, open; 100, closed'),
    ],
    'S20': [
        ('startClean', '{"action": clean_mode_str, "param": {"fanLevel": fan_level_int, "waterLevel": water_level_int, "times": clean_cycle_int}}', 'start cleaning. action, the cleaning mode, sweep or sweep_mop. fanLevel, the vacuum level, 1-4. waterLevel, the mop moisture level, 1-2. times, the number of cycles, 1-2639999, in theory.'),
        ('addWaterForHumi', 'default', 'refill the mind blowing Evaporative Humidifier (Auto-refill).'),
        ('pause', 'default', 'pause.'),
        ('dock', 'default', 'return to Auto-empty Station and charge.'),
        ('setVolume', '0-100', 'set volume, 1-100'),
        ('selfClean', '1 or 2 or 3', 'mode 1, wash the mop. mode 2, dry itself. mode 3, terminate.'),
        ('changeParam', '{"fanLevel": fan_level_int, "waterLevel": water_level_int, "times": clean_cycle_int}', 'fanLevel, the vacuum level, 1-4. waterLevel, the mop moisture level, 1-2. times, the number of cycles, 1-2639999, in theory.'),
    ],
    'Smart Lock Pro Wifi': [
        ('lock', 'default', 'rotate to locked position'),
        ('unlock', 'default', 'rotate to unlocked position'),
        ('nightLatchUnlock', 'default', 'unlock night latch (EU)'),
        ('deadbolt', 'default', 'disengage deadbolt or latch'),
    ],
    'Smart Radiator Thermostat': [
        ('turnOn', 'default', 'set to ON state'),
        ('turnOff', 'default', 'set to OFF state'),
        ('setMode', '0~5', 'set the switch mode. 0,0, schedule mode; 1,manual mode; 2, power off mode; 3, energy saving mode; 4, comfort mode;5, quick heating mode'),
        ('setManualModeTemperature', '4~35', 'set the radiator thermostat temperature. Temperature range: 4–35 °C (inclusive)'),
    ],
    'Standing Fan': [
        ('turnOff', 'default', 'Set to OFF state'),
        ('turnOn', 'default', 'Set to ON state'),
        ('setNightLightMode', 'off, 1, or 2', 'off, turn off nightlight, 1, bright 2, dim'),
        ('setWindMode', 'direct, natural, sleep, or baby', 'Set fan mode. direct: direct mode. natural: natural mode. sleep: sleep mode. baby: ultra quiet mode'),
        ('setWindSpeed', '{1-100} e.g. 10', 'Set fan speed.1~100'),
        ('closeDelay', '{1-36000} e.g. 10', 'Set fan close time'),
    ],
    'Strip Light': [
        ('turnOn', 'default', 'set to ON state'),
        ('turnOff', 'default', 'set to OFF state'),
        ('toggle', 'default', 'toggle state'),
        ('setBrightness', '{1-100}', 'set brightness'),
        ('setColor', '"{0-255}:{0-255}:{0-255}"', 'set RGB color value'),
    ],
    'Strip Light 3': [
        ('turnOn', 'default', 'set to ON state'),
        ('turnOff', 'default', 'set to OFF state'),
        ('toggle', 'default', 'toggle state'),
        ('setBrightness', '{0-100}', 'set brightness'),
        ('setColor', '"{0-255}:{0-255}:{0-255}"', 'set RGB color value'),
        ('setColorTemperature', '{2700-6500}', 'set color temperature'),
    ],
    'Video Doorbell': [
        ('enableMotionDetection', 'default', 'Set to enabled state'),
        ('disableMotionDetection', 'default', 'Set to disabled state'),
    ],
    'Weather Station': [
        ('customQuote', 'custom text, e.g. "Oh sea, you have so much water!"', 'Set a custom quote'),
        ('cancelCustom', 'default', 'Cancel the custom quote and revert to default'),
        ('customPage', 'custom text, e.g. "Oh, the ocean is so huge!"', 'Set a custom page text'),
    ],
}

IR_COMMON_COMMANDS: list[tuple[str, str, str]] = [
    ('turnOn', 'default', 'every home appliance can be turned on by default'),
    ('turnOff', 'default', 'every home appliance can be turned off by default'),
]

IR_COMMANDS: dict[str, list[tuple[str, str, str]]] = {
    'Air Conditioner': [
        ('setAll', '{temperature},{mode},{fan speed},{power state} e.g. 26,1,3,on', 'the unit of temperature is in celsius; modes include 0/1 (auto), 2 (cool), 3 (dry), 4 (fan), 5 (heat); fan speed includes 1 (auto), 2 (low), 3 (medium), 4 (high); power state includes on and off'),
    ],
    'DVD': [
        ('setMute', 'default', 'mute/unmute'),
        ('FastForward', 'default', 'fast forward'),
        ('Rewind', 'default', 'rewind'),
        ('Next', 'default', 'next track'),
        ('Previous', 'default', 'last track'),
        ('Pause', 'default', 'pause'),
        ('Play', 'default', 'play/resume'),
        ('Stop', 'default', 'stop'),
    ],
    'DVD Player': [
        ('setMute', 'default', 'mute/unmute'),
        ('FastForward', 'default', 'fast forward'),
        ('Rewind', 'default', 'rewind'),
        ('Next', 'default', 'next track'),
        ('Previous', 'default', 'last track'),
        ('Pause', 'default', 'pause'),
        ('Play', 'default', 'play/resume'),
        ('Stop', 'default', 'stop'),
    ],
    'Fan': [
        ('swing', 'default', 'swing'),
        ('timer', 'default', 'set timer'),
        ('lowSpeed', 'default', 'set fan speed to low'),
        ('middleSpeed', 'default', 'set fan speed to medium'),
        ('highSpeed', 'default', 'set fan speed to high'),
    ],
    'IPTV': [
        ('SetChannel', '{channel number}, e.g. 15', 'set the TV channel to switch to'),
        ('volumeAdd', 'default', 'volume up'),
        ('volumeSub', 'default', 'volume down'),
        ('channelAdd', 'default', 'next channel'),
        ('channelSub', 'default', 'previous channel'),
    ],
    'Light': [
        ('brightnessUp', 'default', 'brightness up'),
        ('brightnessDown', 'default', 'brightness down'),
    ],
    'Set Top Box': [
        ('SetChannel', '{channel number}, e.g. 15', 'set the TV channel to switch to'),
        ('volumeAdd', 'default', 'volume up'),
        ('volumeSub', 'default', 'volume down'),
        ('channelAdd', 'default', 'next channel'),
        ('channelSub', 'default', 'previous channel'),
    ],
    'Speaker': [
        ('setMute', 'default', 'mute/unmute'),
        ('FastForward', 'default', 'fast forward'),
        ('Rewind', 'default', 'rewind'),
        ('Next', 'default', 'next track'),
        ('Previous', 'default', 'last track'),
        ('Pause', 'default', 'pause'),
        ('Play', 'default', 'play/resume'),
        ('Stop', 'default', 'stop'),
        ('volumeAdd', 'default', 'volume up'),
        ('volumeSub', 'default', 'volume down'),
    ],
    'Streamer': [
        ('SetChannel', '{channel number}, e.g. 15', 'set the TV channel to switch to'),
        ('volumeAdd', 'default', 'volume up'),
        ('volumeSub', 'default', 'volume down'),
        ('channelAdd', 'default', 'next channel'),
        ('channelSub', 'default', 'previous channel'),
    ],
    'TV': [
        ('SetChannel', '{channel number}, e.g. 15', 'set the TV channel to switch to'),
        ('volumeAdd', 'default', 'volume up'),
        ('volumeSub', 'default', 'volume down'),
        ('channelAdd', 'default', 'next channel'),
        ('channelSub', 'default', 'previous channel'),
    ],
}
# END COMMAND TABLES


def commands_for(device_type: str, remote_type: Optional[str] = None
                 ) -> tuple[list[tuple[str, str, str]], list[str]]:
    """Returns the (command, parameter hint, description) rows for a device, plus notes.

    `remote_type` is set for IR remotes (virtual devices), whose commands depend on the appliance
    type; for everything else the `device_type` from GET /devices decides.
    """
    if remote_type is None:
        rows = DEVICE_COMMANDS.get(device_type)
        if rows is None:
            return [], [f"No commands are listed for device type {device_type!r}; it may only "
                        "support status, or be newer than this table. You can still type a "
                        "command by hand."]
        return list(rows), []

    notes = ["Buttons added in the app can be sent by name with the command type "
             '"customize".']
    if remote_type == "Others":
        return [], ['"Others" remotes only have custom buttons. ' + notes[0]]
    # "DIY" remotes are learned button by button; their commands follow the base appliance type.
    base = remote_type.removeprefix("DIY ")
    if base != remote_type:
        notes.append("This is a learned (DIY) remote; buttons it hasn't learned won't work, "
                     "and learned buttons may need the command type \"customize\".")
    return list(IR_COMMON_COMMANDS) + list(IR_COMMANDS.get(base, [])), notes
