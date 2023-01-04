//usr/bin/g++ --std=c++17 -Wall $0 -g -o ${o=`mktemp`} && exec $o $*
#include "copy_on_write.hpp"
#include <iostream>
#include <thread>
#include <vector>

struct TestClass {
	int a = 0;
	int b = 0;

	TestClass(int a) : a(a) {}
};

int main()
{
	int errors = 0;
	int tests = 0;
	auto doATest = [&] (auto is, auto shouldBe) {
		tests++;
		if constexpr(std::is_floating_point_v<decltype(is)>) {
			if ((is > 0 && (is > shouldBe * 1.0001 || is < shouldBe * 0.9999)) ||
					(is < 0 && (is < shouldBe * 1.0001 || is > shouldBe * 0.9999))) {
				errors++;
				std::cout << "Test failed: " << is << " instead of " << shouldBe << std::endl;
			}
		} else {
			if (is != shouldBe) {
				errors++;
				std::cout << "Test failed: " << is << " instead of " << shouldBe << std::endl;
			}
		}
	};

	{
		CopyOnWrite<TestClass> tested(3);
		doATest(tested->a, 3);
		doATest(tested.tryEdit([] (TestClass& edited) {
			edited.b = 4;
		}), true);
		doATest(tested->b, 4);
	}

	{
		CopyOnWrite<TestClass> tested(4);
		CopyOnWrite<TestClass>::CopyOnWriteStateReference reference = tested.get();
		doATest(reference->a, 4);
		doATest(tested.tryReset([] (TestClass& justMade) {
			justMade.b = 4;
		}, CopyOnWrite<TestClass>::AlwaysPassingVerifier(), 3), true);
		doATest(tested->a, 3);
		doATest(tested->b, 4);
		doATest(reference->a, 4);
		CopyOnWrite<TestClass>::CopyOnWriteStateReference reference2 = reference;
		doATest(reference2->a, 4);
		CopyOnWrite<TestClass>::CopyOnWriteStateReference reference3 = std::move(reference2);
		doATest(reference3->a, 4);
	}

	{
		CopyOnWrite<TestClass> tested(3);
		doATest(tested->a, 3);
		doATest(tested.tryEdit([] (TestClass& edited) {
			edited.a = 4;
		}, [] (const TestClass& old) {
				return (old.a == 4);
		}), false);
		doATest(tested->a, 3);
		doATest(tested.edit([] (TestClass& edited) {
			edited.a = 4;
		}, [] (const TestClass& old) {
				return (old.a == 3);
		}), true);
		doATest(tested->a, 4);
	}

	{
		CopyOnWrite<TestClass> tested(5);
		doATest(tested.tryEdit([&] (TestClass& edited) {
			edited.b = 4;
			doATest(tested.tryEdit([] (TestClass& edited2) {
				edited2.b = 3;
			}), false);
		}), true);
		doATest(tested->b, 4);
		doATest(tested.reset([&] (TestClass& justMade) {
			doATest(justMade.a, 3);
			justMade.a = 4;
			doATest(tested.tryReset([&] (TestClass& justMade2) {
				doATest(justMade2.a, 7);
				justMade2.a = 4;
			}, CopyOnWrite<TestClass>::AlwaysPassingVerifier(), 7), false);
			doATest(tested->a, 5);
		}, CopyOnWrite<TestClass>::AlwaysPassingVerifier(), 3), true);
		doATest(tested->a, 4);
		doATest(tested.emplace(6), true);
		doATest(tested->a, 6);
	}

	{
		constexpr int minValue = 0;
		constexpr int maxValue = 10000;
		CopyOnWrite<TestClass> tested(minValue);
		bool badValueFound = false;
		std::thread exporter = std::thread([&] () {
			for (int i = 0; i < 1000000; i++) {
				int copy = tested->a;
				if (copy < minValue || copy > maxValue) {
					badValueFound = true;
				}
			}
		});
		while (true) {
			if (!tested.edit([] (TestClass& edited) {
				edited.a++;
			}, [&] (const TestClass& before) { return (before.a < maxValue); })) {
				break;
			}
		}
		doATest(badValueFound, false);
		doATest(tested->a, maxValue);
		exporter.join();
	}

	{
		constexpr int minValue = 0;
		constexpr int maxValue = 10000;
		constexpr int exporterCount = 4;
		CopyOnWrite<TestClass> tested(minValue);
		std::atomic_int officialValue = minValue;
		bool badValueFound = false;
		auto exporter = [&] () {
			for (int i = 0; i < 1000000; i++) {
				int starting = officialValue - 1;
				int copy = tested->a;
				int ending = officialValue;
				if (copy < starting || copy > ending) {
					badValueFound = true;
				}
			}
		};
		std::vector<std::thread> exporters = {};
		for (int i = 0; i < exporterCount; i++) {
			exporters.push_back(std::thread(exporter));
		}
		while (tested->a < maxValue) {
			int previous = tested->a;
			if (!tested.reset([&] (TestClass& made) {
				officialValue = made.a;
			}, [] (auto) { return true; }, previous + 1)) {
				break;
			}
		}
		doATest(badValueFound, false);
		doATest(tested->a, maxValue);
		for (std::thread& it : exporters) {
			it.join();
		}
	}

	std::cout << "Passed: " << (tests - errors) << " / " << tests << ", errors: " << errors << std::endl;
	return 0;
}
