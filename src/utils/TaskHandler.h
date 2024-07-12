#pragma once
#include <Windows.h>
#include <thread>
#include <queue>
#include <functional>

template<typename inType, typename outType>
class TaskHandler {
private:
	std::thread thread;
	std::queue<inType> inputQueue;
	std::queue<outType> outputQueue;

	SRWLOCK inLock;
	CONDITION_VARIABLE inCV;
	SRWLOCK outLock;
	CONDITION_VARIABLE outCV;
public:
	TaskHandler(std::function<bool(inType &&, TaskHandler<inType, outType>*)> threadFunc) : thread([this, threadFunc]() {
		inType msg;
		while (PopInput(&msg, true)) {
			if (threadFunc(std::move(msg), this))
				break;
		}
	}), inLock(SRWLOCK_INIT), inCV(CONDITION_VARIABLE_INIT), outLock(SRWLOCK_INIT), outCV(CONDITION_VARIABLE_INIT), inputQueue(), outputQueue() {

	}

	void Join() {
		thread.join();
	}

	void PushInput(inType &&message) {
		AcquireSRWLockExclusive(&inLock);
		inputQueue.emplace(message);
		ReleaseSRWLockExclusive(&inLock);
		WakeAllConditionVariable(&inCV);
	}

	void PushOutput(outType&& message) {
		AcquireSRWLockExclusive(&outLock);
		outputQueue.emplace(message);
		ReleaseSRWLockExclusive(&outLock);
		WakeAllConditionVariable(&outCV);
	}

	bool PopInput(inType* in, bool wait = true) {
		AcquireSRWLockExclusive(&inLock);
		bool empty = inputQueue.empty();
		if (empty && !wait) {
			ReleaseSRWLockExclusive(&inLock);
			return false;
		}
		else if (!empty) {
			*in = inputQueue.front();
			inputQueue.pop();
			ReleaseSRWLockExclusive(&inLock);
			return true;
		}

		while (empty) {
			SleepConditionVariableSRW(&inCV, &inLock, INFINITE, !CONDITION_VARIABLE_LOCKMODE_SHARED);
			empty = inputQueue.empty();
			if (!empty) {
				*in = inputQueue.front();
				inputQueue.pop();
			}
			ReleaseSRWLockExclusive(&inLock);
		}
		return true;
	}

	bool PopOutput(outType *out, bool wait = true) {
		AcquireSRWLockExclusive(&outLock);
		bool empty = outputQueue.empty();
		if (empty && !wait) {
			ReleaseSRWLockExclusive(&outLock);
			return false;
		}
		else if (!empty) {
			*out = std::move(outputQueue.front());
			outputQueue.pop();
			ReleaseSRWLockExclusive(&outLock);
			return true;
		}

		while (empty) {
			SleepConditionVariableSRW(&outCV, &outLock, INFINITE, !CONDITION_VARIABLE_LOCKMODE_SHARED);
			empty = outputQueue.empty();
			if (!empty) {
				*out = std::move(outputQueue.front());
				outputQueue.pop();
			}
			ReleaseSRWLockExclusive(&outLock);
		}
		return true;
	}
};