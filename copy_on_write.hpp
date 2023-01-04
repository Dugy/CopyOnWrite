#ifndef COPY_ON_WRITE_HPP
#define COPY_ON_WRITE_HPP

#include <atomic>
#include <mutex>

template <typename T>
class CopyOnWrite {

	// Eplanation:
	// Read behaves like a shared pointer with a bit less functionality, but it can't be simply copied out of the structure,
	// because an edit may happen at that point and destroy the last reference before the refcount is increased.
	//
	// This is solved using an extra counter that uses the usually unused 16 bits of the pointer, keeping the number of
	// threads currently in the state between reading the pointer and incrementing the refcount. It is increased before
	// incrementing the refcount and decreased back immediately afterwards. In case an overwrite happens, this counter
	// is copied into an extra counter where the threads decrement it if they find an overwrite took place. The overwriter
	// does not decrement the refcount and keeps it alive until the threads reduce this counter to zero.

	struct Internal {
		mutable std::atomic_size_t refcount = 1;
		T instance;

		template <typename... Args>
		Internal(Args&&... args) : instance(std::move(args)...) {}
	};

	mutable std::atomic_uint64_t addressAndCopyCounter = 0; // The 48 used bits of a 64 bit pointer plus number of dereferencers
	mutable std::atomic_int previousCopyCounter = 0; // Dereferencers left hit by overwrite (negative values are valid)
	mutable std::mutex editMutex = {}; // Editing uses a traditional lock

	constexpr static uint64_t increment = 0x0001000000000000;
	constexpr static uint64_t prefix = 0xffff000000000000;
	constexpr static uint64_t suffix = 0x0000ffffffffffff;

	static Internal* getPointer(uint64_t value) noexcept {
		if (value & 0x0000800000000000) {
			value |= prefix;
		} else {
			value &= suffix;
		}
		return reinterpret_cast<Internal*>(value);
	}

	static void getRidOfPointer(const Internal* pointer) noexcept {
		size_t refcountLeft = --pointer->refcount;
		if (refcountLeft == 0) {
			delete pointer;
		}
	}

	Internal* safeInstance() const noexcept {
		// Increment the pointer's counter
		uint64_t value = addressAndCopyCounter.load();
		uint64_t newValue = 0;
		do {
			newValue = value + increment;
		} while (!addressAndCopyCounter.compare_exchange_weak(value, newValue));

		// Increment the object's refcount
		Internal* obtained = getPointer(value);
		obtained->refcount++;

		// Decrement the pointer's counter
		uint64_t decrementee = addressAndCopyCounter.load();
		do {
			if ((decrementee & suffix) != (value & suffix)) {
				// This means it was overwritten, decrement the refcount instead
				previousCopyCounter--;
				break;
			}
			newValue = decrementee - increment;
		} while (!addressAndCopyCounter.compare_exchange_weak(decrementee, newValue));

		return obtained;
	}

	template <typename Creator, typename Verifier>
	bool replace(const Creator& creator, const Verifier& verifier) {
		// Can be called only with the mutex locked!!!

		Internal* original = getPointer(addressAndCopyCounter);
		if (!verifier(std::as_const(original->instance))) {
			return false; // Turned out we didn't need to modify
		}

		Internal* replacement = nullptr;
		if constexpr(std::is_invocable_v<Creator, const T&>) { // If the function wants the old copy, it can have it
			replacement = creator(std::as_const(original->instance));
		} else {
			replacement = creator();
		}
		if (!replacement) {
			return false;
		}

		uint64_t oldValue = addressAndCopyCounter.exchange(reinterpret_cast<uint64_t>(replacement)); // Expose a new version

		uint64_t abandoned = oldValue >> 48;
		previousCopyCounter += abandoned;
		do {} while (previousCopyCounter.load() != 0); // Busy wait until all accesses to the old pointer are finished
		getRidOfPointer(original);

		return true; // Did modify
	}

	struct DuplicateHolder {
		Internal* duplicate = nullptr;
		~DuplicateHolder() {
			if (duplicate) {
				delete duplicate;
			}
		}
		Internal* take() {
			Internal* taken = duplicate;
			duplicate = nullptr;
			return taken;
		}
	};

	template <typename Modifier, typename Verifier>
	bool replaceWithModifiedCopy(const Modifier& modifier, const Verifier& verifier) {
		return replace([&] (const T& old) {
			// In this case, we need to modify, so we create a copy and edit it
			DuplicateHolder duplicateHolder = { new Internal(old) };
			modifier(duplicateHolder.duplicate->instance);
			return duplicateHolder.take();
		}, verifier);
	}

	template <typename Modifier, typename Verifier, typename... ConstructorArgs>
	bool replaceWithNew(const Modifier& modifier, const Verifier& verifier, ConstructorArgs&&... constructorArgs) {
		static_assert(std::is_constructible_v<T, ConstructorArgs...>, "Object inside CopyOnWrite can't be constructed from the arguments");
		return replace([&] () {
			// In this case, we need to modify, so we create a copy and edit it
			DuplicateHolder duplicateHolder = { new Internal(std::move(constructorArgs)...) };
			modifier(duplicateHolder.duplicate->instance);
			return duplicateHolder.take();
		}, verifier);
	}

public:
	template <typename... Args>
	CopyOnWrite(Args&&... args) {
		static_assert(std::is_constructible_v<T, Args...>, "Object inside CopyOnWrite can't be constructed from the arguments");
		addressAndCopyCounter.store(reinterpret_cast<uint64_t>(new Internal(std::move(args)...)));
	}

	~CopyOnWrite() {
		Internal* lastReferenced = safeInstance();
		getRidOfPointer(lastReferenced);
		getRidOfPointer(lastReferenced);
	}

	class CopyOnWriteStateReference {
		const Internal* instance = nullptr;

	public:
		CopyOnWriteStateReference(const Internal* value) : instance(value) {}
		CopyOnWriteStateReference(const CopyOnWriteStateReference& other)
		: instance(other.instance) {
			instance->refcount++;
		}
		CopyOnWriteStateReference(CopyOnWriteStateReference&& other) {
			if (other.instance) {
				instance = other.instance;
				other.instance = nullptr;
			}
		}
		~CopyOnWriteStateReference() {
			if (instance) {
				getRidOfPointer(instance);
			}
		}

		CopyOnWriteStateReference& operator=(const CopyOnWriteStateReference& other) {
			if (instance) {
				getRidOfPointer(instance);
			}
			instance = other.instance;
			if (instance) {
				instance->refcount++;
			}
			return *this;
		}
		CopyOnWriteStateReference& operator=(CopyOnWriteStateReference&& other) {
			if (instance) {
				getRidOfPointer(instance);
			}
			instance = other.instance;
			other.instance = nullptr;
			return *this;
		}

		const T* operator->() const {
			return &instance->instance;
		}
	};

	CopyOnWriteStateReference get() const {
		return CopyOnWriteStateReference(safeInstance());
	}

	CopyOnWriteStateReference operator->() const {
		return CopyOnWriteStateReference(safeInstance());
	}

	struct AlwaysPassingVerifier {
		bool operator()(const T&) const {
			return true;
		}
	};

	template <typename... ConstructorArgs>
	bool emplace(ConstructorArgs&&... constructorArgs) {
		std::lock_guard lock(editMutex);
		static_assert(std::is_constructible_v<T, ConstructorArgs...>, "Object inside CopyOnWrite can't be constructed from the arguments");
		return replace([&] () {
			return new Internal(std::move(constructorArgs)...);
		}, AlwaysPassingVerifier());
	}

	template <typename Modifier, typename Verifier = AlwaysPassingVerifier, typename... ConstructorArgs>
	bool reset(const Modifier& modifier, const Verifier& verifier = AlwaysPassingVerifier(), ConstructorArgs&&... constructorArgs) {
		std::lock_guard lock(editMutex);
		return replaceWithNew(modifier, verifier, std::move(constructorArgs)...);
	}

	template <typename Modifier, typename Verifier = AlwaysPassingVerifier, typename... ConstructorArgs>
	bool tryReset(const Modifier& modifier, const Verifier& verifier = AlwaysPassingVerifier(), ConstructorArgs&&... constructorArgs) {
		std::unique_lock lock(editMutex, std::try_to_lock);
		if (!lock.owns_lock()) {
			return false;
		}
		return replaceWithNew(modifier, verifier, std::move(constructorArgs)...);
	}

	template <typename Modifier, typename Verifier = AlwaysPassingVerifier>
	bool edit(const Modifier& modifier, const Verifier& verifier = AlwaysPassingVerifier()) {
		std::lock_guard lock(editMutex);
		return replaceWithModifiedCopy(modifier, verifier);
	}

	template <typename Modifier, typename Verifier = AlwaysPassingVerifier>
	bool tryEdit(const Modifier& modifier, const Verifier& verifier = AlwaysPassingVerifier()) {
		std::unique_lock lock(editMutex, std::try_to_lock);
		if (!lock.owns_lock()) {
			return false;
		}
		return replaceWithModifiedCopy(modifier, verifier);
	}
};

#endif // COPY_ON_WRITE_HPP
