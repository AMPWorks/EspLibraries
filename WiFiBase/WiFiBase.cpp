/*
 * Author: Adam Phelps
 * License: MIT
 * Copyright: 2018
 */

#include <Arduino.h>
#include <WiFi.h>

#ifndef DEBUG_LEVEL
  #define DEBUG_LEVEL DEBUG_HIGH
#endif
#include <Debug.h>

#include "WiFiBase.h"

/**
 * Create a default WifiBase object
 */
WiFiBase::WiFiBase(boolean useStored) {
  _background = true;
  _APSsid = NULL;
  _APPasswd = NULL;
  _running = false;
  _accessPointEnabled = false;
  _accessPointActive = false;

  _numKnownNetworks = 0;
  _allocatedKnownNetworks = 0;
  _knownNetworks = NULL;

  _connectionTimeoutMs = DEFAULT_CONNECT_TIMEOUT;
  _connectedIndex = INDEX_DISCONNECTED;

  /*
   * If there was a previously connected WiFi, add it as the default known
   * network.
   */
  if (useStored && WiFi.SSID()) {
    DEBUG4_VALUELN("WFB: adding default network ", WiFi.SSID());
    addKnownNetwork("\0", "\0");
  }

  DEBUG5_PRINTLN("WFB: Created");
}

WiFiBase::~WiFiBase() {
  DEBUG4_PRINTLN("WFB: freeing");
  WiFi.disconnect();
  for (int i = 0; i < _numKnownNetworks; i++) {
    free(_knownNetworks[i].ssid);
    free(_knownNetworks[i].passwd);
  }
}

/*******************************************************************************
 * Configuration functions
 */

bool WiFiBase::configBackground(bool background) {
  if (_running) {
    DEBUG_ERR("WFB: already running");
    return false;
  }
  _background = background;
  return true;
}

bool WiFiBase::configureAccessPoint(const char *ssid, const char *passwd) {
  if (_accessPointActive) {
    DEBUG_ERR("WFB: access point is active");
    return false;
  }
  DEBUG3_VALUE("WFB: config AP ", ssid);
  DEBUG3_VALUELN(" ", passwd);
  _APSsid = ssid;
  _APPasswd = passwd;
  _accessPointEnabled = true;
  return true;
}

bool WiFiBase::disableAccessPoint() {
  if (_accessPointActive) {
    /* Shutdown the access point */
    if (!_shutdownAccessPoint()) {
      DEBUG_ERR("WFB: Failed to disable AP");
      return false;
    }
  }
  _accessPointEnabled = false;
  return true;
}

/**
 * Add a known network to the known network array, reallocating the array as
 * needed.
 *
 * @param ssid
 * @param passwd
 * @return
 */
bool WiFiBase::addKnownNetwork(const char *ssid, const char *passwd) {
  if (!_allocatedKnownNetworks) {
    _allocatedKnownNetworks = 2;
    _numKnownNetworks = 0;
    DEBUG3_VALUELN("WFB: allocate known ", _allocatedKnownNetworks);
    _knownNetworks = (struct network *)malloc(sizeof(struct network) * _allocatedKnownNetworks);
  } else {
    /* Check if the network is already listed */
    if (hasKnownNetwork(ssid)) {
      DEBUG4_VALUELN("WFB: re-added known ", ssid);
      return true;
    }

    /* Check if reallocation is necessary */
    if (_numKnownNetworks == _allocatedKnownNetworks) {
      if (_numKnownNetworks >= MAX_KNOWN_NETWORKS) {
        DEBUG_ERR("WFB: Hit maximum networks")
        return false;
      }

      /* Increase the size, allocate a new array, and copy from the old array */
      uint16_t newAlloc = _allocatedKnownNetworks * 2 ;
      DEBUG3_VALUELN("WFB: realloc known ", newAlloc);
      struct network *newNetworks = (struct network *)malloc(sizeof(struct network) * newAlloc);
      memcpy(newNetworks, _knownNetworks,
             sizeof(struct network) * _numKnownNetworks);
      free(_knownNetworks);

      /* Swap to the new copied array */
      _knownNetworks = newNetworks;
      _allocatedKnownNetworks = newAlloc;
    }
  }

  DEBUG4_VALUE("WFB: known ", _numKnownNetworks);

  _knownNetworks[_numKnownNetworks].ssid = strdup(ssid);
  _knownNetworks[_numKnownNetworks].passwd = strdup(passwd);

  DEBUG4_VALUE(" ", _knownNetworks[_numKnownNetworks].ssid);
  DEBUG4_VALUELN(" ", _knownNetworks[_numKnownNetworks].passwd);

  _numKnownNetworks++;

  return true;
}

/**
 * @return number of known networks
 */
int WiFiBase::numKnownNetworks() {
  return _numKnownNetworks;
}

/**
 * Lookup the index of a known network
 * @param ssid
 * @return index of network or INDEX_DISCONNECTED
 */
int WiFiBase::lookupKnownNetwork(const char *ssid) {
  for (int i = 0; i < _numKnownNetworks; i++) {
    if (strcmp(_knownNetworks[i].ssid, ssid) == 0) {
      return i;
    }
  }

  return INDEX_DISCONNECTED;
}

/**
 * Check if a given ssid is included in the known networks list
 * @param ssid  Name of network to lookup
 * @return      Whether the network is known
 */
bool WiFiBase::hasKnownNetwork(const char *ssid) {
  return (lookupKnownNetwork(ssid) != INDEX_DISCONNECTED);
}

bool WiFiBase::setConnectTimeoutMs(unsigned long ms) {
  _connectionTimeoutMs = ms;
  return true;
}

/*******************************************************************************
 * Operational functions
 */

/**
 *
 * @return
 */
bool WiFiBase::startup() {
  if (_background) {
    /* TODO: Set this to running in the background */
  }

  if (_connectToNetwork()) {
    return true;
  }

  if (_accessPointEnabled) {
    /* Failed to connect, launch in AP mode with a config portal */
    if (_startupAccessPoint()) {
      return true;
    }
  }

  return false;
}

/**
 * Wait for connect to succeed or fail
 * @return True if connected
 */
bool WiFiBase::_connectWait() {
  uint8_t status;
  DEBUG4_PRINTLN("WFB: _connectWait");
  unsigned long start = millis();
  while (true) {
    status = WiFi.status();
    if (status == WL_CONNECTED) {
      DEBUG4_PRINTLN("WFB: connect succeeded");
      return true;
    }
    if (status == WL_CONNECT_FAILED) {
      DEBUG4_VALUELN("WFB: connect failed ", status);
      return false;
    }
    if (millis() - start > _connectionTimeoutMs) {
      DEBUG4_PRINTLN("WFB: connect timeout")
      esp_wifi_disconnect();
      return false;
    }
    delay(100);
  };
}

/**
 * @return Whether WiFiBase is connected to a network
 */
bool WiFiBase::connected() {
  return (_connectedIndex != INDEX_DISCONNECTED);
}

void WiFiBase::_setConnected(uint8_t index) {
  _connectedIndex = index;
}

void WiFiBase::_setDisconnected() {
  _connectedIndex = INDEX_DISCONNECTED;
}

/**
 * Iterate over any known networks and connect to the first one possible.
 *
 * @return Whether this connected to a known network
 */
bool WiFiBase::_connectToNetwork() {
  int index = 0;

  if (WiFi.status() == WL_CONNECTED) {
    DEBUG3_PRINTLN("WFB: already connected");
    return true;
  }

  if (_numKnownNetworks) {
    if (_knownNetworks[index].ssid[index] == '\0') {
      /* This indicates to try the ssid stored via the Esp SDK */
      DEBUG3_PRINTLN("WFB: attempting stored network");
      WiFi.begin();
      if (_connectWait()) {
        _setConnected(index);
        return true;
      }

      index++;
    }

    /* Iterate over remaining networks and attempt connections */
    for (; index < _numKnownNetworks; index++) {
      DEBUG3_VALUELN("WFB: Connect ", _knownNetworks[index].ssid);
      WiFi.begin(_knownNetworks[index].ssid, _knownNetworks[index].passwd);
      if (_connectWait()) {
        _setConnected(index);
        return true;
      }
    }
  }

  DEBUG3_PRINTLN("WFB: Failed connect");
  _setDisconnected();
  return false;
}

/**
 * Start as access point with a config portal to allow manual network
 * configuration
 *
 * TODO: It would be good to switch to something simpler than WiFiManager for
 * this, or to write a custom variant.
 *
 * @return Whether a connection was configured via the config portal
 */
bool WiFiBase::_startupAccessPoint() {
  DEBUG3_PRINTLN("WFB: starting AP")
  WiFiManager wifiManager;
  if (!wifiManager.startConfigPortal(_APSsid, _APPasswd)) {
    DEBUG_ERR("WFB: Config portal failed");
    return false;
  }

  DEBUG3_VALUE("WFB: Config connected ", wifiManager.getSSID());
  DEBUG3_VALUELN(" ", wifiManager.getPassword());

  /* Check if the connected SSID is in the known list, if not then add it */
  int index = lookupKnownNetwork(wifiManager.getSSID().c_str());
  if (index == INDEX_DISCONNECTED) {
    addKnownNetwork(wifiManager.getSSID().c_str(),
                    wifiManager.getPassword().c_str());
    index = _numKnownNetworks - 1;
  }

  _setConnected(index);

  return true;
}

bool WiFiBase::_shutdownAccessPoint() {

  return false;
}
