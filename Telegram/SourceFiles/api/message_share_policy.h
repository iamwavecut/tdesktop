#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace Api::MessageShare {

enum class Delivery { Forward, Text, Media };

struct Entry {
	Delivery delivery = Delivery::Text;
	uint64_t source = 0;
	uint64_t group = 0;
	bool ephemeral = false;
	uint64_t account = 0;
};

struct Range {
	std::size_t begin = 0;
	std::size_t end = 0;
};

inline std::vector<Range> Partition(const std::vector<Entry> &entries) {
	auto result = std::vector<Range>();
	for (auto i = std::size_t(0); i != entries.size();) {
		const auto first = i++;
		const auto &entry = entries[first];
		const auto limit = (entry.delivery == Delivery::Forward) ? 100 : 10;
		while (i != entries.size() && i - first < limit) {
			const auto &next = entries[i];
			if (entry.delivery != next.delivery
				|| entry.source != next.source
				|| entry.account != next.account
				|| entry.group != next.group
				|| entry.ephemeral != next.ephemeral
				|| entry.delivery == Delivery::Text
				|| (entry.delivery == Delivery::Media
					&& (!entry.group || entry.group != next.group))) {
				break;
			}
			++i;
		}
		result.push_back({ first, i });
	}
	return result;
}

class DeliveryProgress {
public:
	explicit DeliveryProgress(std::size_t size) : _size(size) {
	}

	std::optional<std::size_t> begin() {
		if (_busy || _blocked || _completed == _size) {
			return std::nullopt;
		}
		_busy = true;
		return _completed;
	}
	void succeed() {
		if (_busy) {
			++_completed;
			_busy = false;
		}
	}
	void fail(bool retryable) {
		_busy = false;
		_blocked = !retryable;
	}
	std::size_t completed() const {
		return _completed;
	}
	bool busy() const {
		return _busy;
	}
	bool blocked() const {
		return _blocked;
	}

private:
	std::size_t _size = 0;
	std::size_t _completed = 0;
	bool _busy = false;
	bool _blocked = false;

};

enum class Acknowledgement { Sent, Rejected, Unknown };

class DeliveryQueue final : public std::enable_shared_from_this<DeliveryQueue> {
public:
	using Completion = std::function<void(Acknowledgement)>;
	using Transport = std::function<void(std::size_t, Completion)>;
	using Observer = std::function<void(Acknowledgement, std::size_t)>;

	DeliveryQueue(std::size_t count, Transport transport, Observer observer)
	: _progress(count)
	, _transport(std::move(transport))
	, _observer(std::move(observer)) {
	}

	void start() {
		if (const auto index = _progress.begin()) {
			const auto self = shared_from_this();
			_transport(*index, [self](Acknowledgement result) {
				if (result == Acknowledgement::Sent) {
					self->_progress.succeed();
				} else {
					self->_progress.fail(result == Acknowledgement::Rejected);
				}
				self->_observer(result, self->_progress.completed());
				if (result == Acknowledgement::Sent) {
					self->start();
				}
			});
		}
	}
	bool blocked() const {
		return _progress.blocked();
	}

private:
	DeliveryProgress _progress;
	Transport _transport;
	Observer _observer;

};

} // namespace Api::MessageShare
