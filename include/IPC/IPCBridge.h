#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>

#include "ConcurrentQueue.h"

struct IPCData {
	unsigned short version = 1;
	bool isListening = false;
	bool hasMessage = false;
	struct IPCMessage {
		enum COMMAND { DISCONNECT = 0, PING, USER } cmd{};
		int userCommand = 0;
		unsigned int messageId = 0;
		int params[3]{};
		unsigned int dataSize = 0;
		unsigned char data[300]{};
	} msg;
};

struct CallbackInfo {
	std::function<void(std::shared_ptr<IPCData::IPCMessage>)> callback;
	std::function<void(void)> timeout;
	int timeoutSecs = 5;
	std::chrono::steady_clock::time_point startTime = std::chrono::high_resolution_clock::now();
};

class IPCBridge {
private:
	std::wstring path;
	bool hasInitialized = false;
	HANDLE fileHandle = INVALID_HANDLE_VALUE;
	HANDLE fileMap = INVALID_HANDLE_VALUE;
	struct {
		IPCData outgoing;
		IPCData incoming;
	}* fileView = nullptr;
	bool isRunningLoop = false;
	ConcurrentQueue<std::shared_ptr<IPCData::IPCMessage>> outboundPacketQ;
	std::recursive_mutex callbackMapMutex;
	std::mutex accessFileViewMutex;
	std::condition_variable loopStopNotifier;
	std::map<unsigned int, CallbackInfo> callbackMap;
	bool hasActiveConnection = false;
	unsigned int curRequestId = 0;
	bool isClient = false;
	inline IPCData& getInbound() { return isClient ? fileView->outgoing : fileView->incoming; }
	inline IPCData& getOutbound() { return isClient ? fileView->incoming : fileView->outgoing; }
	std::function<void(std::shared_ptr<IPCData::IPCMessage>&)> defaultPacketHandler;
	std::thread myMessageLoop;

public:
	IPCBridge(std::wstring path);
	~IPCBridge();
	bool initBridge(bool initializeFile = false);
	void beginMessageLoop();
	void startLoopAsync();
	bool isConnected() { return this->hasActiveConnection; };
	bool isInitialized() { return this->hasInitialized; };
	void enqueuePacket(std::shared_ptr<IPCData::IPCMessage> pk, std::function<void(std::shared_ptr<IPCData::IPCMessage>)> callback,
					   std::function<void(void)> timeout, int timeoutSecs = 5);
	void enqueuePacket(std::shared_ptr<IPCData::IPCMessage> pk);
	void closeBridge();
	void setDefaultPacketHandler(std::function<void(std::shared_ptr<IPCData::IPCMessage>&)> packetHandler) { this->defaultPacketHandler = packetHandler; }
	void clearQueue();
};

extern std::shared_ptr<IPCBridge> ipcBridge;