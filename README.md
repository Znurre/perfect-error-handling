# Fun with coroutines - Implementing perfect error handling in C++23 with std::expected and coroutines?

Error handling in C++ comes in many different shapes. In C++23 yet another option was added - `std::expected<>`, which allows us to have error handling that looks very much alike the error handling in Rust or Go.

Instead of using exceptions to throw and propagate errors, we can now use a more lightweight approach, where our functions can have a return type of `std::expected<T, E>`, where `T` is the type we *expect* to be returned, while `E` is the type of a possible error.

By using this return type for our function, we can return either the value itself, or `std::unexpected(...)`, where the argument to `std::unexpected` is the error. The caller can then check the object to see if it contains a value, or an error, and act accordingly, potentially propagating the error if needed.

Put simple, `std::expected` is as if `std::optional` and `std::variant` had a baby.

To better understand `std::expected` - let's create a simple example, which we will then extend as we go along.

```cpp
std::expected<FILE*, uint64_t> open_file(std::string_view file_name, std::string_view mode)
{
	if (auto file = fopen(file_name.data(), mode.data()))
	{
		return file;
	}

	return std::unexpected(errno);
}
```

The example above is a simple wrapper for the POSIX `fopen` function. If a file can be opened, we'll return the handle to the caller, otherwise we'll return the error code.

A potential caller of this function could look something like this.

```cpp
std::expected<std::string, uint64_t> read_from_file(std::string_view file_name)
{
	auto file = open_file(file_name, "r");

	if (!file)
	{
		return std::unexpected(file.error());
	}

	std::string target;

	char buffer[4096];

	for (auto size = 0; (size = fread(buffer, sizeof(*buffer), sizeof(buffer), *file)) > 0;)
	{
		target.append(buffer, buffer + size);
	}

	if (auto error = ferror(*file))
	{
		return std::unexpected(error);
	}

	return target;
}
```
In a real life scenario, we would of course use RAII for the file handle, but that's beyond the point of this exercise, so let's excuse this small leak for now.

As you can see, we have a boolean operator for `std::unexpected` which can be used to check for an error. If the object contains an error, we do an early return, propagating the error by using `std::unexpected(...)` again, as we need to wrap it in an `std::expected<string, uint64_t>` this time around.

If the object contains a value, we can access it using the dereference operator, as expected with an `std::optional`-like type.

Additionally, we check `ferror`, to see if an error occurred while reading the file, and if so, we return the error to the caller.

This is pretty neat, and gives very explicit error control, in contrast to exceptions. However, it also creates quite some boilerplate.

In another project that I worked on, we created macros to work around this. The macro would basically allow us to write code like this:

```cpp
std::expected<std::string, uint64_t> read_from_file(std::string_view file_name)
{
	auto file = UNWRAP(open_file(file_name, "r"));

	std::string target;

	char buffer[4096];

	for (auto size = 0; (size = fread(buffer, sizeof(*buffer), sizeof(buffer), file)) > 0;)
	{
		target.append(buffer, buffer + size);
	}

	if (auto error = ferror(file))
	{
		return std::unexpected(error);
	}

	return target;
}
```

Behind the scenes, `UNWRAP` would expand to the `if`-statement in the previous code example, as well as returning an rvalue reference to the extracted value in case of a non-error. A lot of hacks and magic was involved to make this work properly.

I thought to myself... could this be improved somehow? I considered multiple approaches, each one of them hackier than the previous one. Theoretically, one could create the following function...

```cpp
auto unwrap(auto&& value)
{
    if (value)
    {
        return *value;
    }
    
    // Do something here to unwind the stack
}
```

The problem, is obviously the comment in the end. How would we unwind the stack? I mean, we *could* just get the return address stored on the stack, and then unwind it manually. 

The problem would be all automatic storage duration objects, since they would not be cleaned up properly. Even if we could reclaim any allocated stack space and restore the instruction pointer, there would be no clean way to run any destructors.

That's when I thought to myself... why not use coroutines for this?

If you are new to coroutines, let me give you a brief introduction. Coroutines are basically C++ functions which can be suspended and resumed. A lot of people seem to believe that they have a 1:1 relationship with async programming (which would makes sense given the keywords such as `co_await`), but in fact they are so much more.

The important thing to know about coroutines, is that the way they work is different from regular C++ functions. In a coroutine, all local variables are stored inside a heap allocated state object, that is managed behind the scenes. This is what allows coroutines to be suspended and resumed.

Each `co_await` in a coroutine creates a "suspension point", and each suspension point has its own state.

Let's take the following example to better illustrate how it works.

```cpp
coroutine func()
{
    obj1 object1;
    obj2 object2;
    
    co_await some_other_func(object1, object2);
    
    obj3 object3;
    
    co_await yet_another_func(object3);
}
```

Internally, this would create the equivalent of two structs, let's call them `state1` and `state2`.

```cpp
struct state1
{
    obj1 object1;
    obj2 object2;
};

struct state2
{
    obj3 object3;
};
```

Any variables before the first `co_await` belong to the first suspension point and thus `state1`. Storage for them will be allocated when entering the function, and they will be initialized as per usual, as one would expect based on the rules for dynamic storage duration.

Any variables before the second `co_await` belong to the second suspension point, and thus `state2`. Storage for them will be allocated when resuming the coroutine after the first suspension point, and they will also be initialized as per usual, as one would expect based on the rules for dynamic storage duration.

So, how does this help us? Before I can answer that, we need to take a look at the last piece of the puzzle.

The `co_await` operator can be used on any object that satisfies the following conditions:

* Has an `await_resume` method
* Has an `await_suspend` method
* Has an `await_ready` method

The `await_ready` method is used as a shortcut, so that we can quickly determine if the coroutine needs to be suspended or not. If we return `true` here, the coroutine will not be suspended, and the `await_resume` method will be called immediately in order to obtain the value of the `co_await` expression. If we return `false`, the `await_suspend` method will instead be called, and this is where the fun begins.

The `await_suspend` method is used to suspend a coroutine. Here we are given a `std::coroutine_handle`, which acts as a glorified function pointer, and which can be invoked *in order to resume the coroutine*. Basically, it's our responsibility to save this handle, and invoke it whenever we want to resume execution.

This is the cool thing with coroutines, in that they do not impose any kind of scheduling. We could spin up a thread here, do something heavy, and once we are done, we invoke the handle to resume the coroutine. We could also subscribe to a signal for an operation that is queued in an event loop, and invoke the handle once signaled.

For this little experiment however, we're gonna do something way crazier.

The `std::coroutine_handle` does not only allow us to resume the coroutine. It actually also allows us to "destroy" it. Not only this - remember how the `await_ready` method could be used as a shortcut to prevent suspension? `await_suspend` actually also allows us to do the same thing, or at least resume the coroutine immediately, as it's actually suspended at this point. By returning `false` from this method, the coroutine will be immediately resumed, and `await_ready` will be called in order to obtain the value of the `co_await` expression. If we instead return `true`, we return control to the caller.

This allows us to do something devious. We could create an implementation which checks an `std::expected` value for error, and conditionally resumes or suspends the coroutine. If we suspend the coroutine, returning to the caller, we will also make use of the feature to destroy a coroutine to make sure that *all automatic storage duration objects* allocated up until this suspension point, are properly deallocated and deinitialized!

What this allows us to do in practice, is to unwind the stack, and properly cleaning up any automatic storage duration objects.

Let's have a look at what this would look like in practice. In order to make the code a bit less cluttered, I decided to limit ourselves to `uint64_t` for the errors. This means that we can only ever propagate numeric errors, but that should be fine, and easy enough to extend if one would want to.

```cpp
template <typename T>
struct promise;

template <typename T>
struct result
{
	using promise_type = promise<T>;

	result(std::shared_ptr<std::expected<T, uint64_t>> value)
		: value_(value)
	{
	}

	T await_resume()
	{
		return **value_;
	}

	template <typename U>
	bool await_suspend(std::coroutine_handle<promise<U>> handle)
	{
		if (!*value_)
		{
			auto& promise = handle.promise();

			promise.return_value(std::unexpected(value_->error()));

			handle.destroy();

			return true;
		}

		return false;
	}

	bool await_ready()
	{
		return false;
	}

	operator bool() const
	{
		return value_->has_value();
	}

	T& operator*() const
	{
		return value_->value();
	}

	uint64_t error() const
	{
		return value_->error();
	}

private:
	std::shared_ptr<std::expected<T, uint64_t>> value_;
};

template <typename T>
struct promise
{
	promise()
		: value_(std::make_shared<std::expected<T, uint64_t>>())
	{
	}

	result<T> get_return_object()
	{
		return value_;
	}

	std::suspend_never initial_suspend() noexcept
	{
		return {};
	}

	std::suspend_never final_suspend() noexcept
	{
		return {};
	}

	void unhandled_exception()
	{
	}

	void return_value(std::expected<T, uint64_t>&& value)
	{
		*value_ = value;
	}

private:
	std::shared_ptr<std::expected<T, uint64_t>> value_;
};
```

I am not gonna explain everything here, as all of it can be looked up at cppreference, but some things are worth nothing. For example, the coupling between the `promise<T>` type and `result<T>` might seem a bit unclear.

The way it works, is that we'll have our functions return the `result<T>` type. When the compiler sees that we are using the coroutine operators (`co_return`), it will automatically resolve the promise type based on the `promise_type` type alias of `result<T>`, and use that for our coroutine.

When the coroutine function is entered, our promise will be instantiated, the promise is then actually responsible for creating the `result<T>` type. The promise is also the component that the coroutine communicates with in order to, for example, return a value when using `co_return`.

In order to make the design as simple as possible, I decided to use a `std::shared_ptr` for the storage. This allows both the promise and any result objects to have a reference to the same data, while still making sure that there can be no dangling references if either of them go out of scope (which will happen).

This way the promise can simply assign the value in the `return_value` method, and it will immediately be reflected in any `result<T>` objects created from this promise.

Another thing worth nothing is the implementation of `await_suspend`. Here you can see how we are checking if the `std::expected` stored in our shared pointer contains an error, and if so, we do 3 things:

* The promise object for the suspension point is obtained, and we assign the `std::unexpected` error value using the same `return_value` method that the compiler would use. By doing this we make sure that the error is propagated to the promise of the parent coroutine.
* The coroutine is destroyed. This deallocates any storage and deinitializes any automatic storage duration objects up until this suspension point.
* We return `true`, returning to the caller, which effectively unwinds the stack.

If the `std::expected` does not contain an error, we simply return `false`, resuming execution immediately. We have also implemented `await_ready` to return the non-error value of the underlying `std::expected`, so that the value of the `co_await` expression will always be the value type of `std::expected`.

When we combine all of this, we can now transform our previous code into the following code (including example of usage)

```cpp
result<FILE*> open_file(std::string_view file_name, std::string_view mode)
{
	if (auto file = fopen(file_name.data(), mode.data()))
	{
		co_return file;
	}

	co_return std::unexpected(errno);
}

result<std::string> read_from_file(std::string_view file_name)
{
	auto file = co_await open_file(file_name, "r");

	std::string target;

	char buffer[4096];

	for (auto size = 0; (size = fread(buffer, sizeof(*buffer), sizeof(buffer), file)) > 0;)
	{
		target.append(buffer, buffer + size);
	}

	if (auto error = ferror(file))
	{
		co_return std::unexpected(error);
	}

	co_return target;
}

int main()
{
	if (auto result = read_from_file("existing_file.txt"))
	{
		std::cout << *result << std::endl;
	}
	else
	{
		std::cout << "Failed to read file with error " << result.error() << std::endl;
	}

	return 0;
}
```

All of a sudden, this is way cleaner! We have automatic stack unwinding and cleanup on error, thanks to the usage of `co_await`. It also has a bunch of other benefits compared to the macro solution. For example, unwinding could be performed even for more advanced scenarios, since the `co_await` operator can be used in `if` statements, in function arguments, etc, etc.

I hope you enjoyed this "small" article, which hopefully made you more curious about C++ coroutines!

A complete example is included in the main.cpp file.

Until next time o/
