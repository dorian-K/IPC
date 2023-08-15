#include <IPC/IPCBridge.h>

#ifndef dprnt
#define dprnt(...) \
	{}
#endif
#ifndef iprnt
#define iprnt(...) \
	{}
#endif

#include <fstream>

IPCBridge::IPCBridge(std::wstring path) { this->path = path; }

IPCBridge::~IPCBridge() { this->closeBridge(); }

bool IPCBridge::initBridge(bool initializeFile) {
	this->hasInitialized = false;
	std::unique_lock g(this->accessFileViewMutex);
	this->isClient = !initializeFile;
	for (int i = 0; true; i++) {
		this->fileHandle = CreateFileW(this->path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
									   FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_RANDOM_ACCESS, NULL);

		if (this->fileHandle == INVALID_HANDLE_VALUE) {
			dprnt("Could not open ipc bridge: {}", GetLastError());
			if (i > 5) return false;
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		} else
			break;
	}

	LARGE_INTEGER fileSize{};
	GetFileSizeEx(this->fileHandle, &fileSize);
	if (fileSize.QuadPart < sizeof(decltype(this->fileView))) {
		auto data = std::make_shared<IPCData>();
		memset(data.get(), 0, sizeof(IPCData));

		// reset content
		WriteFile(this->fileHandle, data.get(), sizeof(IPCData), NULL, NULL);
		OVERLAPPED op{};
		op.Offset = sizeof(IPCData);
		WriteFile(this->fileHandle, data.get(), sizeof(IPCData), NULL, &op);
	}

	// map to memory
	this->fileMap = CreateFileMappingW(this->fileHandle, NULL, PAGE_READWRITE, 0, 0, NULL);
	if (this->fileMap == INVALID_HANDLE_VALUE || this->fileMap == 0) {
		dprnt("Could not open ipc map: {}", GetLastError());
		g.unlock();
		this->closeBridge();
		return false;
	}

	this->fileView = reinterpret_cast<decltype(this->fileView)>(MapViewOfFile(this->fileMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(IPCData) * 2));
	if (this->fileView == nullptr) {
		dprnt("Could not open ipc view: {}", GetLastError());
		g.unlock();
		this->closeBridge();
		return false;
	}
	this->getOutbound().version = 1;
	this->getOutbound().isListening = false;
	this->getInbound().isListening = false;
	this->hasInitialized = true;
	return true;
}

void IPCBridge::beginMessageLoop() {
	if (this->fileView == nullptr) {
		dprnt("invalid file view");
		throw std::exception("invalid_file_view");
	}
	dprnt("IPC Message loop started");
	std::unique_lock g1(this->accessFileViewMutex);

	this->isRunningLoop = true;
	this->getOutbound().version = 1;
	float boredom = 0;
	auto lastInboundMessage = std::chrono::high_resolution_clock::now();
	auto lastOutboundMessage = std::chrono::high_resolution_clock::now() - std::chrono::milliseconds(500);
	auto disconnectedSince = std::chrono::high_resolution_clock::now();
	while (this->isRunningLoop) {
		this->getInbound().isListening = true;
		if (!this->getOutbound().isListening) {
			// injector not connected
			if (hasActiveConnection) {
				disconnectedSince = std::chrono::high_resolution_clock::now();
				dprnt("Disconnected from IPC");
			}

			hasActiveConnection = false;
			if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - disconnectedSince).count() < 1000)
				this->loopStopNotifier.wait_for(g1, std::chrono::milliseconds(2));
			else
				this->loopStopNotifier.wait_for(g1, std::chrono::milliseconds(10));
			continue;
		} else {
			if (!hasActiveConnection) {
				lastInboundMessage = std::chrono::high_resolution_clock::now();
				iprnt("Connected to IPC");
				boredom = 0;
			}

			hasActiveConnection = true;
		}

		if (hasActiveConnection &&
			std::chrono::duration_cast<std::chrono::seconds>(std::chrono::high_resolution_clock::now() - lastInboundMessage).count() > 3) {
			// Timeout
			dprnt("IPC-Timeout");
			this->getOutbound().isListening = false;
			continue;
		}

		bool hadSomethingToDo = false;
		if (!this->getOutbound().hasMessage) {
			if (!this->outboundPacketQ.empty()) {
				hadSomethingToDo = true;
				std::shared_ptr<IPCData::IPCMessage> msg;
				this->outboundPacketQ.pop(msg);
				memcpy(&this->getOutbound().msg, msg.get(), sizeof(IPCData::IPCMessage));
				this->getOutbound().hasMessage = true;
				lastOutboundMessage = std::chrono::high_resolution_clock::now();
				if (msg->cmd != IPCData::IPCMessage::PING && false) dprnt("Ipcbridge: sent package");
			} else if (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::high_resolution_clock::now() - lastOutboundMessage).count() > 1) {
				auto msg = std::make_shared<IPCData::IPCMessage>();
				msg->cmd = IPCData::IPCMessage::PING;
				this->enqueuePacket(msg);  // sending ping
			}
		}

		if (this->getInbound().hasMessage) {
			if (this->getInbound().msg.cmd != IPCData::IPCMessage::PING && false) dprnt("ipcbridge: got package: {}", this->getInbound().msg.cmd);
			hadSomethingToDo = true;
			lastInboundMessage = std::chrono::high_resolution_clock::now();
			// if (this->getInbound().msg.cmd != IPCData::IPCMessage::PING)
			//	dprnt("inbound message! {}", this->getInbound().msg.cmd);
			this->getInbound().msg.dataSize = std::min((unsigned int) sizeof(this->getInbound().msg.data), this->getInbound().msg.dataSize);
			auto msgId = this->getInbound().msg.messageId;
			if (msgId != 0) {
				// call callback
				std::lock_guard g2(this->callbackMapMutex);
				if (this->callbackMap.count(msgId) > 0) {
					auto ptr = std::make_shared<IPCData::IPCMessage>(this->getInbound().msg);
					auto start = std::chrono::high_resolution_clock::now();
					this->callbackMap[msgId].callback(ptr);
					auto end = std::chrono::high_resolution_clock::now();
					auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
					if (elapsed > 50)
						dprnt(
							"IPC callback took {} to return, keeping the ipc "
							"loop inactive for too long will reduce "
							"throughput! cmd={}",
							elapsed, this->getInbound().msg.cmd);
					this->callbackMap.erase(msgId);
				} else if (this->defaultPacketHandler) {
					auto e = std::make_shared<IPCData::IPCMessage>(this->getInbound().msg);
					this->defaultPacketHandler(e);
				} else
					dprnt("no callback for msg id {}", msgId);
			} else if (this->defaultPacketHandler) {
				auto e = std::make_shared<IPCData::IPCMessage>(this->getInbound().msg);
				this->defaultPacketHandler(e);
			}

			this->getInbound().hasMessage = false;
		}

		if (!hadSomethingToDo && boredom > 5) {	 // Remove all callbacks which received a timeout
			std::lock_guard g(this->callbackMapMutex);
			auto it = this->callbackMap.begin();
			while ((it = std::find_if(it, this->callbackMap.end(), [](std::pair<const unsigned int, CallbackInfo>& entr) {
						return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::high_resolution_clock::now() - entr.second.startTime).count() >
							   entr.second.timeoutSecs;
					})) != this->callbackMap.end()) {
				it->second.timeout();
				this->callbackMap.erase(it++);
			}
		}

		if (hadSomethingToDo)
			boredom = 0;
		else {
			if (boredom < 10) boredom += 0.1f;
			this->loopStopNotifier.wait_for(g1, std::chrono::milliseconds((int) boredom + 1));
		}
	}
	if (this->fileView != nullptr) this->getInbound().isListening = false;
	g1.unlock();
	dprnt("IPC Message loop quit!");
}

void IPCBridge::enqueuePacket(std::shared_ptr<IPCData::IPCMessage> pk, std::function<void(std::shared_ptr<IPCData::IPCMessage>)> callback,
							  std::function<void(void)> timeout, int timeoutSecs) {
	std::lock_guard g(this->callbackMapMutex);
	this->curRequestId++;
	pk->messageId = this->curRequestId;
	if (timeoutSecs <= 0) timeoutSecs = 9999999;
	this->callbackMap[pk->messageId] = {callback, timeout, timeoutSecs};
	// dprnt("Enqueuing packet with cmd=%d id=%d", pk->cmd, pk->messageId);
	this->enqueuePacket(pk);
}

void IPCBridge::enqueuePacket(std::shared_ptr<IPCData::IPCMessage> pk) { this->outboundPacketQ.push(pk); }

void IPCBridge::closeBridge() {
	this->hasInitialized = false;
	bool didRunLoop = this->isRunningLoop;
	this->isRunningLoop = false;
	std::unique_lock g1(this->accessFileViewMutex);
	if (didRunLoop) {
		this->getInbound() = {};  // Reset all data
		this->getInbound().isListening = false;

		this->loopStopNotifier.notify_all();
		g1.unlock();
		if (this->myMessageLoop.joinable()) this->myMessageLoop.join();
		g1.lock();
	}

	if (this->fileView != nullptr) {
		UnmapViewOfFile(this->fileView);
		this->fileView = nullptr;
	}

	if (this->fileMap != INVALID_HANDLE_VALUE) {
		CloseHandle(this->fileMap);
		this->fileMap = INVALID_HANDLE_VALUE;
	}

	if (this->fileHandle != INVALID_HANDLE_VALUE) {
		CloseHandle(this->fileHandle);
		this->fileHandle = INVALID_HANDLE_VALUE;
	}
}
void IPCBridge::startLoopAsync() {
	this->myMessageLoop = std::thread([this]() { this->beginMessageLoop(); });
}
void IPCBridge::clearQueue() { this->outboundPacketQ.clear(); }