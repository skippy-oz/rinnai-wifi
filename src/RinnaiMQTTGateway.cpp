#include <WiFi.h>
#include <ArduinoJson.h>

#include "LogStream.hpp"
#include "RinnaiMQTTGateway.hpp"

const bool REPORT_RESEARCH_FIELDS = true; // send some additional data in JSON to help us understand the protocol better
const int MQTT_REPORT_FORCED_FLUSH_INTERVAL_MS = 20000; // ms
const int STATE_JSON_MAX_SIZE = REPORT_RESEARCH_FIELDS ? 500 : 300;
const int CONFIG_JSON_MAX_SIZE = 700;
const int MAX_OVERRIDE_PERIOD_FROM_ORIGINAL_MS = 500; // ms, only send override if there was an original message lately

RinnaiMQTTGateway::RinnaiMQTTGateway(String haDeviceName, RinnaiSignalDecoder &rxDecoder, RinnaiSignalDecoder &txDecoder, MQTTClient &mqttClient, String mqttTopic, byte testPin)
	: haDeviceName(haDeviceName), rxDecoder(rxDecoder), txDecoder(txDecoder), mqttClient(mqttClient), mqttTopic(mqttTopic), mqttTopicState(String(mqttTopic) + "/state"), testPin(testPin)
{
	// set a will topic to signal that we are unavailable
	String availabilityTopic = mqttTopic + "/availability";
	mqttClient.setWill(availabilityTopic.c_str(), "offline", true, 0); // set retained will message
}

void RinnaiMQTTGateway::loop()
{
	// low level rinnai decoding monitoring
	if (logLevel == RAW)
	{
		logStream().printf("rx errors: pulse %d, bit %d, packet %d\n", rxDecoder.getPulseHandlerErrorCounter(), rxDecoder.getBitTaskErrorCounter(), rxDecoder.getPacketTaskErrorCounter());
		logStream().printf("rx pulse: waiting %d, avail %d\n", uxQueueMessagesWaiting(rxDecoder.getPulseQueue()), uxQueueSpacesAvailable(rxDecoder.getPulseQueue()));
		logStream().printf("rx bit: waiting %d, avail %d\n", uxQueueMessagesWaiting(rxDecoder.getBitQueue()), uxQueueSpacesAvailable(rxDecoder.getBitQueue()));
		logStream().printf("rx packet: waiting %d, avail %d\n", uxQueueMessagesWaiting(rxDecoder.getPacketQueue()), uxQueueSpacesAvailable(rxDecoder.getPacketQueue()));

		logStream().printf("tx errors: pulse %d, bit %d, packet %d\n", txDecoder.getPulseHandlerErrorCounter(), txDecoder.getBitTaskErrorCounter(), txDecoder.getPacketTaskErrorCounter());
		logStream().printf("tx pulse: waiting %d, avail %d\n", uxQueueMessagesWaiting(txDecoder.getPulseQueue()), uxQueueSpacesAvailable(txDecoder.getPulseQueue()));
		logStream().printf("tx bit: waiting %d, avail %d\n", uxQueueMessagesWaiting(txDecoder.getBitQueue()), uxQueueSpacesAvailable(txDecoder.getBitQueue()));
		logStream().printf("tx packet: waiting %d, avail %d\n", uxQueueMessagesWaiting(txDecoder.getPacketQueue()), uxQueueSpacesAvailable(txDecoder.getPacketQueue()));
	}
	// dump intermediate item queues for low level debug
	// might require to stop their organic consuming task in the signal decoder first
	/*
	static unsigned long lastPulseTime = 0;
	while (uxQueueMessagesWaiting(rxDecoder.getPulseQueue()))
	{
		PulseQueueItem item;
		BaseType_t ret = xQueueReceive(rxDecoder.getPulseQueue(), &item, 0); // pdTRUE=1 if an item was successfully received from the queue, otherwise pdFALSE.
		// hack delta
		unsigned long d = clockCyclesToMicroseconds(item.cycle - lastPulseTime);
		lastPulseTime = item.cycle;
		logStream().printf("rx p %d %d, q %d, r %d\n", item.newLevel, d, uxQueueMessagesWaiting(rxDecoder.getPulseQueue()), ret);
	}
	while (uxQueueMessagesWaiting(rxDecoder.getBitQueue()))
	{
		BitQueueItem item;
		BaseType_t ret = xQueueReceive(rxDecoder.getBitQueue(), &item, 0); // pdTRUE if an item was successfully received from the queue, otherwise pdFALSE.
		logStream().printf("rx b %d %d %d, q %d, r %d\n", item.bit, item.startCycle, item.misc, uxQueueMessagesWaiting(rxDecoder.getBitQueue()), ret);
	}
	*/
	while (uxQueueMessagesWaiting(rxDecoder.getPacketQueue()))
	{
		PacketQueueItem item;
		BaseType_t ret = xQueueReceive(rxDecoder.getPacketQueue(), &item, 0); // pdTRUE if an item was successfully received from the queue, otherwise pdFALSE.
		if (handleIncomingPacketQueueItem(item, true) == false)
		{
			logStream().printf("Error in rx pkt %d %02x%02x%02x %u %d %d %d, q %d, r %d\n", item.bitsPresent, item.data[0], item.data[1], item.data[2], item.startCycle, item.validPre, item.validParity, item.validChecksum, uxQueueMessagesWaiting(rxDecoder.getPacketQueue()), ret);
		}
	}

	while (uxQueueMessagesWaiting(txDecoder.getPacketQueue()))
	{
		PacketQueueItem item;
		BaseType_t ret = xQueueReceive(txDecoder.getPacketQueue(), &item, 0); // pdTRUE if an item was successfully received from the queue, otherwise pdFALSE.
		if (handleIncomingPacketQueueItem(item, false) == false)
		{
			logStream().printf("Error in tx pkt %d %02x%02x%02x %u %d %d %d, q %d, r %d\n", item.bitsPresent, item.data[0], item.data[1], item.data[2], item.startCycle, item.validPre, item.validParity, item.validChecksum, uxQueueMessagesWaiting(rxDecoder.getPacketQueue()), ret);
		}
	}

	// MQTT payload generation and flushing
	// render payload
	DynamicJsonDocument doc(STATE_JSON_MAX_SIZE);
	doc["ip"] = WiFi.localIP().toString();
	doc["testPin"] = digitalRead(testPin) == LOW ? "ON" : "OFF";
	doc["enableTemperatureSync"] = enableTemperatureSync;
	if (heaterPacketCounter)
	{
		doc["currentTemperature"] = lastHeaterPacketParsed.temperatureCelsius;
		doc["targetTemperature"] = targetTemperatureCelsius;
		doc["mode"] = lastHeaterPacketParsed.on ? "heat" : "off";
		doc["action"] = lastHeaterPacketParsed.inUse ? "heating" : (lastHeaterPacketParsed.on ? "idle" : "off");
		if (REPORT_RESEARCH_FIELDS)
		{
			doc["activeId"] = lastHeaterPacketParsed.activeId;
			doc["heaterBytes"] = RinnaiProtocolDecoder::renderPacket(lastHeaterPacketBytes);
			doc["startupState"] = lastHeaterPacketParsed.startupState;
		}
	}
	if (localControlPacketCounter && REPORT_RESEARCH_FIELDS)
	{
		doc["locControlId"] = lastLocalControlPacketParsed.myId;
		doc["locControlBytes"] = RinnaiProtocolDecoder::renderPacket(lastLocalControlPacketBytes);
	}
	String payload;
	serializeJson(doc, payload);
	// check if to send
	unsigned long now = millis();
	if (mqttClient.connected() && (now - lastMqttReportMillis > MQTT_REPORT_FORCED_FLUSH_INTERVAL_MS || payload != lastMqttReportPayload))
	{
		// now that we have decided to send, expand payload with additional fields that normally don't trigger a send on their own
		doc["rssi"] = WiFi.RSSI(); // the current RSSI /Received Signal Strength in dBm (?)
		if (REPORT_RESEARCH_FIELDS)
		{
			if (heaterPacketCounter)
			{
				doc["heaterDelta"] = lastHeaterPacketDeltaMillis;
			}
			if (localControlPacketCounter)
			{
				doc["locControlTiming"] = millisDeltaPositive(lastLocalControlPacketMillis, lastHeaterPacketMillis, lastHeaterPacketDeltaMillis);
			}
			if (remoteControlPacketCounter)
			{
				doc["remControlId"] = lastRemoteControlPacketParsed.myId;
				doc["remControlBytes"] = RinnaiProtocolDecoder::renderPacket(lastRemoteControlPacketBytes);
				doc["remControlTiming"] = millisDeltaPositive(lastRemoteControlPacketMillis, lastHeaterPacketMillis, lastHeaterPacketDeltaMillis);
			}
			if (unknownPacketCounter)
			{
				doc["unknownBytes"] = RinnaiProtocolDecoder::renderPacket(lastUnknownPacketBytes);
				doc["unknownTiming"] = millisDeltaPositive(lastUnknownPacketMillis, lastHeaterPacketMillis, lastHeaterPacketDeltaMillis);
			}
		}
		// re-serialize payload
		String payloadExpanded;
		serializeJson(doc, payloadExpanded);
		// send
		logStream().printf("Sending on MQTT channel '%s': %d/%d bytes, %s\n", mqttTopicState.c_str(), payloadExpanded.length(), STATE_JSON_MAX_SIZE, payloadExpanded.c_str());
		bool ret = mqttClient.publish(mqttTopicState, payloadExpanded, true, 0);
		if (!ret)
		{
			logStream().println("Error publishing a state MQTT message");
		}
		lastMqttReportMillis = now;
		lastMqttReportPayload = payload; // save last (restricted) payload for change detection
	}

	// delay to not over flood the serial interface
	// delay(100);
}

bool RinnaiMQTTGateway::handleIncomingPacketQueueItem(const PacketQueueItem &item, bool remote)
{
	// check packet is valid
	if (!item.validPre || !item.validParity || !item.validChecksum)
	{
		return false;
	}
	// see where the packet originates from
	RinnaiPacketSource source = RinnaiProtocolDecoder::getPacketSource(item.data, RinnaiSignalDecoder::BYTES_IN_PACKET);
	if (source == INVALID) // bad checksum, size, etc
	{
		return false;
	}
	else if (source == HEATER && remote)
	{
		RinnaiHeaterPacket packet;
		bool ret = RinnaiProtocolDecoder::decodeHeaterPacket(item.data, packet);
		if (!ret)
		{
			return false;
		}
		memcpy(&lastHeaterPacketParsed, &packet, sizeof(RinnaiHeaterPacket));
		memcpy(lastHeaterPacketBytes, item.data, RinnaiProtocolDecoder::BYTES_IN_PACKET);
		// counters and timings
		unsigned long t = item.startMillis;
		if (heaterPacketCounter > 0)
		{
			lastHeaterPacketDeltaMillis = t - lastHeaterPacketMillis; // measure cycle period
		}
		heaterPacketCounter++;
		lastHeaterPacketMillis = t;
		// init target temperature once we have reports from the heater
		if (targetTemperatureCelsius == -1)
		{
			targetTemperatureCelsius = lastHeaterPacketParsed.temperatureCelsius; 
		}
		// act on temperature info
		handleTemperatureSync();
		// log
		if (logLevel == PARSED)
		{
			logStream().printf("Heater packet: a=%d o=%d u=%d t=%d\n", packet.activeId, packet.on, packet.inUse, packet.temperatureCelsius);
		}
	}
	else if (source == CONTROL)
	{
		RinnaiControlPacket packet;
		bool ret = RinnaiProtocolDecoder::decodeControlPacket(item.data, packet);
		if (!ret)
		{
			return false;
		}
		if (remote)
		{
			memcpy(&lastRemoteControlPacketParsed, &packet, sizeof(RinnaiControlPacket));
			memcpy(lastRemoteControlPacketBytes, item.data, RinnaiProtocolDecoder::BYTES_IN_PACKET);
			remoteControlPacketCounter++;
			lastRemoteControlPacketMillis = item.startMillis;
		}
		else
		{
			memcpy(&lastLocalControlPacketParsed, &packet, sizeof(RinnaiControlPacket));
			memcpy(lastLocalControlPacketBytes, item.data, RinnaiProtocolDecoder::BYTES_IN_PACKET);
			localControlPacketCounter++;
			lastLocalControlPacketMillis = item.startMillis;
		}
		// log
		if (logLevel == PARSED)
		{
			logStream().printf("Control packet: r=%d i=%d o=%d p=%d td=%d tu=%d\n", remote, packet.myId, packet.onOffPressed, packet.priorityPressed, packet.temperatureDownPressed, packet.temperatureUpPressed);
		}
	}
	else // source == UNKNOWN || local HEATER
	{
		// save metrics for troubleshooting and research
		memcpy(lastUnknownPacketBytes, item.data, RinnaiProtocolDecoder::BYTES_IN_PACKET);
		unknownPacketCounter++;
		lastUnknownPacketMillis = item.startMillis;
	}
	return true;
}

void RinnaiMQTTGateway::handleTemperatureSync()
{
	if (heaterPacketCounter && localControlPacketCounter && targetTemperatureCelsius != -1 &&
		lastHeaterPacketParsed.temperatureCelsius != targetTemperatureCelsius && millis() - lastHeaterPacketMillis < MAX_OVERRIDE_PERIOD_FROM_ORIGINAL_MS)
	{
		override(lastHeaterPacketParsed.temperatureCelsius < targetTemperatureCelsius ? TEMPERATURE_UP : TEMPERATURE_DOWN);
	}
}

bool RinnaiMQTTGateway::override(OverrideCommand command)
{
	// check if state is valid for sending
	unsigned long originalControlPacketAge = millis() - lastLocalControlPacketMillis;
	if (originalControlPacketAge > MAX_OVERRIDE_PERIOD_FROM_ORIGINAL_MS) // if we have no recent original packet. can happen because no panel signal is available
	{
		logStream().printf("No fresh original data for override command %d, age %lu, millis %lu, lastLocal %lu\n", command, originalControlPacketAge, millis(), lastLocalControlPacketMillis);
		return false;
	}
	// logStream().printf("Attempting override command %d, age %d\n", command, originalControlPacketAge);
	// prep buffer
	byte buf[RinnaiSignalDecoder::BYTES_IN_PACKET];
	memcpy(buf, lastLocalControlPacketBytes, RinnaiSignalDecoder::BYTES_IN_PACKET);
	switch (command)
	{
	case ON_OFF:
		RinnaiProtocolDecoder::setOnOffPressed(buf);
		break;
	case PRIORITY:
		RinnaiProtocolDecoder::setPriorityPressed(buf);
		break;
	case TEMPERATURE_UP:
		RinnaiProtocolDecoder::setTemperatureUpPressed(buf);
		break;
	case TEMPERATURE_DOWN:
		RinnaiProtocolDecoder::setTemperatureDownPressed(buf);
		break;
	default:
		logStream().println("Unknown command for override");
		return false;
	}
	bool overRet = txDecoder.setOverridePacket(buf, RinnaiSignalDecoder::BYTES_IN_PACKET);
	if (overRet == false)
	{
		logStream().printf("Error setting override, command = %d\n", command); // are we hammering too fast?
		return false;
	}
	return true;
}

void RinnaiMQTTGateway::onMqttMessageReceived(String &fullTopic, String &payload)
{
	// parse topic
	String topic;
	int index = fullTopic.lastIndexOf('/');
	if (index != -1)
	{
		topic = fullTopic.substring(index + 1);
	}
	else
	{
		topic = fullTopic;
	}

	// ignore what we send
	if (topic == "config" || topic == "state" || topic == "availability")
	{
		return;
	}

	// log
	logStream().printf("Incoming: %s %s - %s\n", fullTopic.c_str(), topic.c_str(), payload.c_str());

	// handle command
	if (topic == "temp")
	{
		// parse and verify targetTemperature
		int temp = atoi(payload.c_str());
		temp = min(temp, (int)RinnaiProtocolDecoder::TEMP_C_MAX);
		temp = max(temp, (int)RinnaiProtocolDecoder::TEMP_C_MIN);
		logStream().printf("Setting %d as target temperature\n", temp);
		targetTemperatureCelsius = temp;
	}
	else if (topic == "temperature_sync")
	{
		if (payload == "on" || payload == "enable" || payload == "true" || payload == "1")
		{
			enableTemperatureSync = true;
		}
		else
		{
			enableTemperatureSync = false; // in case something breaks and you want to take control manually at the panel
		}
	}
	else if (topic == "mode")
	{
		if ((payload == "off" && lastHeaterPacketParsed.on) || (payload == "heat" && !lastHeaterPacketParsed.on))
		{
			override(ON_OFF);
		}
	}
	else if (topic == "priority")
	{
		override(PRIORITY);
	}
	else if (topic == "log_level")
	{
		if (payload == "none")
		{
			logLevel = NONE;
		}
		else if (payload == "parsed")
		{
			logLevel = PARSED;
		}
		else if (payload == "raw")
		{
			logLevel = RAW;
		}
	}
	else if (topic == "log_destination")
	{
		if (payload == "telnet")
		{
			logStream().println("Telnet log set");
			logStream.SetLogStreamTelnet();
		}
		else
		{
			logStream().println("Serial log set");
			logStream.SetLogStreamSerial();
		}
	}
	else
	{
		logStream().printf("Unknown topic: %s\n", topic.c_str());
	}
}

void RinnaiMQTTGateway::onMqttConnected()
{
	// subscribe
	bool ret = mqttClient.subscribe(mqttTopic + "/#");
	if (!ret)
	{
		logStream().println("Error doing a MQTT subscribe");
	}

	// send a '/config' topic to achieve MQTT discovery - https://www.home-assistant.io/docs/mqtt/discovery/
	DynamicJsonDocument doc(CONFIG_JSON_MAX_SIZE);
	doc["~"] = mqttTopic;
	doc["name"] = haDeviceName;
	doc["action_topic"] = "~/state";
	doc["action_template"] = "{{ value_json.action }}";
	doc["current_temperature_topic"] = "~/state";
	doc["current_temperature_template"] = "{{ value_json.currentTemperature }}";
	doc["max_temp"] = RinnaiProtocolDecoder::TEMP_C_MAX;
	doc["min_temp"] = RinnaiProtocolDecoder::TEMP_C_MIN;
	doc["initial"] = RinnaiProtocolDecoder::TEMP_C_MIN;
	doc["mode_command_topic"] = "~/mode";
	doc["mode_state_topic"] = "~/state";
	doc["mode_state_template"] = "{{ value_json.mode }}";
	doc["modes"][0] = "off";
	doc["modes"][1] = "heat";
	doc["precision"] = 1;
	doc["temperature_command_topic"] = "~/temp";
	doc["temperature_unit"] = "C";
	doc["temperature_state_topic"] = "~/state";
	doc["temperature_state_template"] = "{{ value_json.targetTemperature }}";
	doc["availability_topic"] = "~/availability";
	String payload;
	serializeJson(doc, payload);
	logStream().printf("Sending on MQTT channel '%s/config': %d/%d bytes, %s\n", mqttTopic.c_str(), payload.length(), CONFIG_JSON_MAX_SIZE, payload.c_str());
	ret = mqttClient.publish(mqttTopic + "/config", payload, true, 0);
	if (!ret)
	{
		logStream().println("Error publishing a config MQTT message");
	}
	// send an availability topic to signal that we are available
	ret = mqttClient.publish(mqttTopic + "/availability", "online", true, 0);
	if (!ret)
	{
		logStream().println("Error publishing an availability MQTT message");
	}
}

long RinnaiMQTTGateway::millisDelta(unsigned long t1, unsigned long t2)
{
	// how to do it even better to handle more edge cases, overflow of millis and unsigned/signed issues?
	if (t1 > t2)
	{
		return t1 - t2;
	}
	else
	{
		return - (t2 - t1);
	}
}

// try to calculate a positive delta in a scenario where events are expected to come in a recurring cyclic manner
long RinnaiMQTTGateway::millisDeltaPositive(unsigned long t1, unsigned long t2, unsigned long cycle)
{
	long d = millisDelta(t1, t2);
	if (d < 0)
	{
		return d + cycle;
	}
	return d;
}