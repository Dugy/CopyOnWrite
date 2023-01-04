# CopyOnWrite
Utility C++ class for accessing a class as lockfree and locking only when changing (at the cost of copying the whole thing so that readers can use the old copy and don't need to use the lock).

The whole thing is a small class using only few standard libraries, can be easily pasted into another project.

## Usage
Suppose we have a class we want to protect this way:
```C++
struct TestClass {
	int a = 0;
	int b = 0;

	TestClass(int a) : a(a) {}
};
```

It's not default constructible, so it's necessary to pass its arguments when constructing it.

To create a `CopyOnWrite`-protected instance, we do this:
```C++
CopyOnWrite<TestClass> safe(3);
```

This initialises it with a constructor argument 3.

To access it, we can use the `get()` method:
```C++
auto got = safe.get();
std::cout << got->a << std::endl;
```

The `get()` method returns an instance of `CopyOnWrite<TestClass>::CopyOnWriteStateReference`, which is a referece-counted copy that can be accessed only as `const`, so if it does not use anything strongly unusual, it does not need any locking.

It can also be accessed inline through the `->` operator:
```C++
std::cout << safe->b << std::endl;
```

However, one should be careful with dereferencing it inline multiple times in a function, because it requires several atomic operations to complete and may be changed between invocations.

There are several ways to modify the value. The simplest of them is `emplace()`:
```C++
tested.emplace(4);
```

This will construct a new instance with constructor argument 4 and use it to replace the old one. Reading copies are will stay valid. Nothing will happen if the constructor throws an exception.

Another possible operation is `reset()`, which will replace the value with a new one and allow to edit it before overwriting:
```C++
tested.reset([&] (TestClass& justMade) {
	justMade.b = 8;
}, [&] (const TestClass& oldCopy) {
	return !itsTooLate();
}, 7);
```

This will abort and return false if some `itsTooLate()` function return false after obtaining the lock (this may be useful to check if some change was already done by another thread), construct the object with constructor argument 7 and then set the other member to 8.

To edit a copy instead of creating a new object, there is an `edit()` method:
```C++
tested.edit([&] (TestClass& edited) {
	edited.a++;
});
```
This will copy the instance and increment its attribute. Optionally, there may be a second argument that determines whether to abort it just after obtaining the lock.

There are also variants `tryReset()` and `tryEdit()` that abort and return false if it's being edited, otherwise they behave the same.