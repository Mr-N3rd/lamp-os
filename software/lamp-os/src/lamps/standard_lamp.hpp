#pragma once
#ifndef STANDARD_LAMP_H
#define STANDARD_LAMP_H

#include <ArduinoJson.h>

#define LAMP_SHADE_PIN 12
#define LAMP_BASE_PIN 14
#define LAMP_MAX_BRIGHTNESS 180
#define LAMP_MAX_STRIP_PIXELS_SHADE 38
#define LAMP_MAX_STRIP_PIXELS_BASE 50

void setup();
void loop();

/**
 * - Initialize all of the lamps behaviors
 * - Initialize the animation compositor
 */
void initBehaviors();

/**
 * ArtNet DMX actions shared between the base and shade
 */
void handleArtnet();

/**
 * Handle wifi mode swaps when a stage router is present
 */
void handleStageMode();

/**
 * Whole lamp changes from the configuration tool
 */
void handleWebSocket();

/**
 * @brief Dispatch a lamp action JSON document to the appropriate handler.
 *
 * Routes the same action set used by the WebSocket protocol (bright, shade,
 * base, knockout, test_expression, test_expression_complete) to the internal
 * lamp state. Callable from any transport — WS, BLE GATT, etc.
 *
 * @param doc  ArduinoJson document with at minimum an "a" (action) key.
 * @param updateTimeMs  millis() value to record as the last-update timestamp
 *                      for the configurator behaviors (use millis()).
 */
void dispatchLampAction(JsonDocument& doc, unsigned long updateTimeMs);

#endif